/**
 * @file src/platform/linux/private_display.cpp
 * @brief Private streaming output management for Linux/KWin.
 */

#include "private_display.h"
#include "private_display_capacity.h"
#include "display_restore_capture.h"

#include "display_helper_process.h"
#include "display_power.h"
#include "display_restore_dispatcher.h"
#include "hdr_policy.h"
#include "private_display_cleanup_policy.h"
#include "private_display_configuration_policy.h"
#include "private_display_emergency_policy.h"
#include "private_display_recovery_policy.h"
#include "private_display_mode_client.h"
#include "private_display_mode_policy.h"
#include "private_display_restore_policy.h"
#include "private_display_restore_transaction.h"
#include "private_display_resume_policy.h"
#include "private_display_snapshot_policy.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/remote_display_topology.h"
#include "src/rtsp.h"
#include "src/state_storage.h"
#include "src/stream.h"
#include "src/virtual_display_scale.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <display_device/json.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <shared_mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <virtual_display/driver/linux_control_client.h>

namespace platf::linux_private_display {
  namespace {
    using json = nlohmann::json;

    constexpr auto output_publication_timeout = std::chrono::seconds {3};
    constexpr auto output_verification_timeout = std::chrono::seconds {3};
    constexpr auto helper_reply_timeout = std::chrono::seconds {10};
    constexpr auto connector_reply_timeout = std::chrono::seconds {8};
    constexpr auto restore_operation_timeout = std::chrono::seconds {30};
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool preserve_for_process_shutdown {false};
    std::atomic_bool helper_completion_unknown {false};
    std::shared_mutex topology_mutation_gate;

    using command_result_t = helper_process::result_t;

    struct restore_context_t {
      std::chrono::steady_clock::time_point deadline;
      std::function<bool()> valid;
    };
    thread_local const restore_context_t *restore_context = nullptr;

    bool restore_allowed() {
      return !helper_completion_unknown.load(std::memory_order_acquire) &&
             (!restore_context ||
              (std::chrono::steady_clock::now() < restore_context->deadline && restore_context->valid()));
    }

    std::vector<std::string> capture_output_names() {
      auto deadline = std::chrono::steady_clock::now() + output_verification_timeout;
      if (restore_context) deadline = std::min(deadline, restore_context->deadline);
      return platf::display_names_for_restore(platf::mem_type_e::unknown, deadline, [] {
        return !process_shutdown_preserve_requested() && restore_allowed();
      });
    }

    bool helper_budget_available(std::chrono::steady_clock::duration budget) {
      return restore_allowed() && (!restore_context ||
             restore_context->deadline - std::chrono::steady_clock::now() >= budget);
    }

    struct state_t {
      std::mutex mutex;
      std::optional<json> snapshot;
      bool snapshot_loaded {false};
      bool snapshot_legacy_record {false};
      std::map<std::string, std::string> reservations;
      std::map<std::string, double> retained_scales;
      std::set<std::string> newly_connected_reservations;
      std::atomic<std::uint64_t> cleanup_generation {0};
      std::atomic<std::uint64_t> reset_generation {0};
      // Destroy/join the worker before its snapshot, generation and mutex.
      restore_dispatcher_t restore_dispatcher {[] {
        BOOST_LOG(error) << "Linux display helper: asynchronous restore failed with an exception.";
      }};
      std::mutex recovery_monitor_mutex;
      std::jthread recovery_monitor;
      bool recovery_monitor_stopping {false};
    };

    state_t &state() {
      static state_t value;
      return value;
    }

    bool broker_socket_ready() {
      struct stat attributes {};
      return lstat("/run/vibeshine/vkms-control.sock", &attributes) == 0 &&
             S_ISSOCK(attributes.st_mode) && attributes.st_uid == 0 &&
             (attributes.st_mode & 0007) == 0;
    }

    bool broker_set_connected(const std::string &output_name, const bool connected) {
      // The control client waits longer than the broker's mutation/stop bound.
      // Reserve that entire interval rather than shortening its safe drain.
      if (!helper_budget_available(connector_reply_timeout)) return false;
      static const virtual_display::driver::LinuxControlClient client;
      const auto result = client.set_connector(output_name, connected);
      if (!result.ok()) {
        BOOST_LOG(error) << "Linux private display: broker rejected "
                         << (connected ? "connect" : "disconnect") << " for " << output_name
                         << ": " << virtual_display::driver::to_string(result.status)
                         << (result.detail.empty() ? std::string {} : " (" + result.detail + ")");
        return false;
      }
      return true;
    }

    std::optional<std::string> doctor_path() {
      auto *path = g_find_program_in_path("kscreen-doctor");
      if (!path) {
        return std::nullopt;
      }
      std::string result {path};
      g_free(path);
      return result;
    }

    command_result_t run_doctor(const std::vector<std::string> &arguments, const bool mutating = false) {
      command_result_t result;
      if (!helper_budget_available(helper_reply_timeout)) {
        result.stderr_text = "display helper fenced, restore superseded, or insufficient reply budget";
        return result;
      }
      const auto executable = doctor_path();
      if (!executable) {
        result.stderr_text = "kscreen-doctor was not found in PATH";
        return result;
      }

      std::vector<std::string> owned_argv;
      owned_argv.reserve(arguments.size() + 2);
      if (std::getenv("VIBEPOLLO_MACHINE_HOST")) {
        owned_argv.emplace_back("/usr/libexec/vibeshine/vibepollo-session-exec");
        owned_argv.emplace_back(arguments.size() == 1 && arguments.front() == "-j" ? "display-query" : "display-apply");
      } else {
        owned_argv.push_back(*executable);
      }
      if (!std::getenv("VIBEPOLLO_MACHINE_HOST") || owned_argv.back() == "display-apply") {
        owned_argv.insert(owned_argv.end(), arguments.begin(), arguments.end());
      }
      std::vector<const gchar *> argv;
      argv.reserve(owned_argv.size() + 1);
      for (const auto &arg : owned_argv) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);

      std::shared_lock<std::shared_mutex> mutation_admission;
      if (mutating) {
        if (process_shutdown_preserve_requested()) {
          result.success = true;
          return result;
        }
        mutation_admission = std::shared_lock {topology_mutation_gate};
        if (process_shutdown_preserve_requested()) {
          result.success = true;
          return result;
        }
      }

      // Revalidate after admission: a concurrent mutation may have fenced
      // helper completion, or the queued restore was superseded meanwhile.
      if (!helper_budget_available(helper_reply_timeout)) {
        result.stderr_text = "display helper admission superseded or fenced";
        return result;
      }
      GError *spawn_error = nullptr;
      GSubprocess *process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &spawn_error
      );
      if (!process) {
        if (spawn_error) {
          result.stderr_text = spawn_error->message;
          g_error_free(spawn_error);
        }
        return result;
      }
      // Admission closes once the exact helper exists. Its KScreen request is
      // allowed to drain outside the gate up to the helper reply deadline:
      // shutdown fences later mutations without cancelling an admitted one.
      if (mutation_admission.owns_lock()) {
        mutation_admission.unlock();
      }

      auto deadline = std::chrono::steady_clock::now() + helper_reply_timeout;
      result = helper_process::communicate_until(process, deadline, std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr);
      if (mutating && result.completion_unknown) {
        std::unique_lock mutation_barrier {topology_mutation_gate};
        helper_completion_unknown.store(true, std::memory_order_release);
        BOOST_LOG(error) << "Linux display helper: completion could not be confirmed; refusing further display mutations until host restart.";
      }
      g_object_unref(process);
      return result;
    }

    std::optional<json> query_configuration() {
      const auto result = run_doctor({"-j"});
      if (!result.success) {
        BOOST_LOG(warning) << "Linux private display: unable to query KScreen: " << result.stderr_text;
        return std::nullopt;
      }
      try {
        auto value = json::parse(result.stdout_text);
        if (!value.contains("outputs") || !value["outputs"].is_array()) {
          throw std::runtime_error("KScreen response has no outputs array");
        }
        return value;
      } catch (const std::exception &error) {
        BOOST_LOG(warning) << "Linux private display: invalid KScreen JSON: " << error.what();
        return std::nullopt;
      }
    }

    template<typename Predicate>
    bool wait_for_configuration(Predicate &&predicate, const bool require_stability = false) {
      const auto deadline = std::chrono::steady_clock::now() + output_verification_timeout;
      linux_hdr::output_state_stabilizer_t stabilizer;
      do {
        if (!restore_allowed()) return false;
        const auto configuration = query_configuration();
        const bool matches = configuration && predicate(*configuration);
        if (matches && (!require_stability || stabilizer.observe(true))) {
          return true;
        }
        if (require_stability && !matches) {
          (void) stabilizer.observe(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    const json *find_output(const json &configuration, const std::string &name) {
      for (const auto &output : configuration["outputs"]) {
        if (output.value("name", std::string {}) == name) {
          return &output;
        }
      }
      return nullptr;
    }

    bool connected(const json &output) {
      return output.value("connected", false);
    }

    bool enabled(const json &output) {
      return output.value("enabled", false);
    }

    json physical_topology_signature(const json &configuration) {
      std::vector<std::pair<std::string, json>> outputs;
      const auto values = configuration.find("outputs");
      if (values == configuration.end() || !values->is_array()) return nullptr;
      for (const auto &output : *values) {
        if (!output.is_object()) continue;
        const auto name = output.value("name", std::string {});
        if (name.empty() || mode_policy::managed_connector_name(name)) continue;
        outputs.emplace_back(name, output);
      }
      std::ranges::sort(outputs, [](const auto &left, const auto &right) { return left.first < right.first; });
      json result = json::array();
      for (const auto &[_, output] : outputs) result.push_back(output);
      return result;
    }

    std::vector<std::string> discover_managed_outputs() {
      std::vector<std::string> result;
      std::error_code error;
      const std::filesystem::path drm_class {"/sys/class/drm"};
      for (std::filesystem::directory_iterator it {drm_class, error}, end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (!filename.starts_with("card")) {
          continue;
        }
        const auto separator = filename.find('-');
        if (separator == std::string::npos || separator + 1 >= filename.size()) {
          continue;
        }
        const auto resolved = std::filesystem::canonical(it->path(), error);
        if (error) {
          error.clear();
          continue;
        }
        const auto resolved_text = resolved.string();
        if (resolved_text.find("/devices/faux/vibeshine") == std::string::npos) {
          continue;
        }
        result.push_back(filename.substr(separator + 1));
      }
      std::sort(result.begin(), result.end());
      result.erase(std::unique(result.begin(), result.end()), result.end());
      return result;
    }

    std::vector<std::string> configured_outputs() {
      if (!config::video.dd.virtual_display_outputs.empty()) {
        return config::video.dd.virtual_display_outputs;
      }
      return discover_managed_outputs();
    }

    bool is_managed_output(const std::string &name) {
      const auto managed = discover_managed_outputs();
      return std::find(managed.begin(), managed.end(), name) != managed.end();
    }

    std::optional<json> wait_for_output_publication(const std::string &name) {
      const auto deadline = std::chrono::steady_clock::now() + output_publication_timeout;
      do {
        if (auto configuration = query_configuration()) {
          if (const auto *output = find_output(*configuration, name); output && connected(*output)) {
            return configuration;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } while (std::chrono::steady_clock::now() < deadline);
      return std::nullopt;
    }

    bool connect_managed_output(const std::string &name) {
      if (process_shutdown_preserve_requested()) {
        return false;
      }
      {
        std::shared_lock mutation_admission {topology_mutation_gate};
        if (process_shutdown_preserve_requested()) {
          return false;
        }
        if (!broker_set_connected(name, true)) {
          return false;
        }
      }
      return true;
    }

    bool disconnect_managed_output(const std::string &name) {
      if (process_shutdown_preserve_requested()) {
        return true;
      }
      std::shared_lock mutation_lock {topology_mutation_gate};
      if (process_shutdown_preserve_requested()) {
        return true;
      }
      return !is_managed_output(name) || broker_set_connected(name, false);
    }

    std::string connector_sysfs_path(const std::string &name) {
      std::error_code error;
      const std::filesystem::path drm_class {"/sys/class/drm"};
      const auto suffix = "-" + name;
      std::vector<std::filesystem::path> matching_connectors;
      for (std::filesystem::directory_iterator it {drm_class, error}, end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (!filename.ends_with(suffix)) {
          continue;
        }
        const auto card_name = filename.substr(0, filename.size() - suffix.size());
        if (!card_name.starts_with("card") || card_name.size() == 4 ||
            !std::ranges::all_of(card_name.substr(4), [](const unsigned char value) { return std::isdigit(value); })) {
          continue;
        }
        const auto resolved = std::filesystem::canonical(it->path(), error);
        if (!error && resolved.string().find("/devices/faux/vibeshine/") != std::string::npos) {
          return it->path().string();
        }
        error.clear();
        matching_connectors.push_back(it->path());
      }

      // A unique physical connector needs no broker ownership check. The broker
      // deliberately recognizes only Vibepollo's Virtual-N pool, so querying it
      // for HDMI/DP connectors both produces a false error and prevents restore
      // verification from observing an already-active physical output.
      if (matching_connectors.size() == 1 && !name.starts_with("Virtual-")) {
        return matching_connectors.front().string();
      }
      return {};
    }

    bool connector_is_connected(const std::string &name) {
      const auto path = connector_sysfs_path(name);
      if (path.empty()) {
        return false;
      }
      std::ifstream status {std::filesystem::path {path} / "status"};
      std::string value;
      return status >> value && value == "connected";
    }

    recovery_policy::connector_state_t recovery_connector_state(
      const std::string &name,
      const std::map<std::string, display_power::dpms_state_t> &dpms_states
    ) {
      const auto path = connector_sysfs_path(name);
      if (path.empty() || is_managed_output(name)) return {};
      recovery_policy::connector_state_t result;
      std::ifstream status {std::filesystem::path {path} / "status"};
      std::string value;
      if (!(status >> value) || (value != "connected" && value != "disconnected")) return result;
      result.known = true;
      result.connected = value == "connected";
      std::ifstream enabled {std::filesystem::path {path} / "enabled"};
      if (enabled >> value) {
        if (value == "enabled" || value == "disabled") {
          result.enabled_known = true;
          result.enabled = value == "enabled";
        }
      }
      const auto compositor_dpms = dpms_states.find(name);
      if (compositor_dpms != dpms_states.end() && compositor_dpms->second.known) {
        result.dpms_known = true;
        result.dpms_on = compositor_dpms->second.on;
      } else {
        // Some drivers expose the connector's legacy DPMS state here. Keep it
        // as a fallback observation; enabled alone never fabricates a wake.
        std::ifstream dpms {std::filesystem::path {path} / "dpms"};
        if (dpms >> value) {
          if (value == "On") {
            result.dpms_known = true;
            result.dpms_on = true;
          } else if (value == "Off" || value == "Standby" || value == "Suspend") {
            result.dpms_known = true;
            result.dpms_on = false;
          }
        }
      }
      return result;
    }

    std::map<std::string, std::string> managed_connector_identities() {
      std::map<std::string, std::string> result;
      for (const auto &name : discover_managed_outputs()) {
        const auto path = connector_sysfs_path(name);
        if (path.empty()) continue;
        std::error_code error;
        const auto resolved = std::filesystem::canonical(path, error);
        if (!error && resolved.string().find("/devices/faux/vibeshine/") != std::string::npos) {
          result.emplace(name, resolved.string());
        }
      }
      return result;
    }

    bool managed_connector_identity_matches(const std::string &name, const std::string &identity) {
      const auto path = connector_sysfs_path(name);
      if (path.empty()) return false;
      std::error_code error;
      const auto resolved = std::filesystem::canonical(path, error);
      return !error && resolved.string() == identity &&
             resolved.string().find("/devices/faux/vibeshine/") != std::string::npos;
    }

    bool managed_connector_verified_disconnected(const std::string &name, const std::string &identity) {
      std::error_code error;
      const std::filesystem::path drm_class {"/sys/class/drm"};
      const auto suffix = "-" + name;
      std::optional<std::filesystem::path> exact_node;
      for (std::filesystem::directory_iterator it {drm_class, error}, end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (!filename.starts_with("card") || !filename.ends_with(suffix)) continue;
        const auto resolved = std::filesystem::canonical(it->path(), error);
        if (error) return false;
        if (resolved.string() == identity &&
            resolved.string().find("/devices/faux/vibeshine/") != std::string::npos) {
          exact_node = it->path();
          break;
        }
      }
      if (error) return false;
      if (!exact_node) return true;
      std::ifstream status {*exact_node / "status"};
      std::string value;
      if (!(status >> value) || (value != "connected" && value != "disconnected")) return false;
      const bool kernel_disconnected = value == "disconnected";
      const auto configuration = query_configuration();
      if (!configuration) return false;
      const auto *output = find_output(*configuration, name);
      const bool compositor_inactive = !output || !connected(*output);
      return recovery_policy::managed_connector_retired(
        true, true, kernel_disconnected, compositor_inactive);
    }

    bool connector_hdr_capable(const std::string &name) {
      const auto path = connector_sysfs_path(name);
      if (path.empty()) {
        return false;
      }
      std::ifstream input {std::filesystem::path {path} / "edid", std::ios::binary};
      std::vector<std::uint8_t> edid;
      char value;
      while (input.get(value)) {
        edid.push_back(static_cast<std::uint8_t>(value));
      }
      if (linux_hdr::edid_supports_hdr10(edid)) {
        return true;
      }

      // The managed Vibepollo DRM device has a fixed HDR10 EDID. Keep its
      // capability stable while the connector is deliberately disconnected:
      // the kernel exposes no EDID bytes in that dormant state.
      std::error_code error;
      const auto resolved = std::filesystem::canonical(path, error);
      return !error && resolved.string().find("/devices/faux/vibeshine/") != std::string::npos;
    }

    bool output_hdr_capable(const json *output, const std::string &name) {
      (void) output;
      return connector_hdr_capable(name);
    }

    std::set<std::string> private_output_set() {
      const auto outputs = configured_outputs();
      return {outputs.begin(), outputs.end()};
    }

    bool execute_configuration(const std::vector<std::string> &arguments, const std::string_view operation) {
      if (arguments.empty()) {
        return true;
      }
      const auto result = run_doctor(arguments, true);
      if (!configuration_policy::command_succeeded(result.success, result.stdout_text, result.stderr_text) ||
          mode_policy::doctor_reported_failure(result.stdout_text) ||
          mode_policy::doctor_reported_failure(result.stderr_text)) {
        BOOST_LOG(error) << "Linux private display: KScreen " << operation << " failed: "
                         << result.stdout_text << result.stderr_text;
        return false;
      }
      return true;
    }

    double floating_point(const display_device::FloatingPoint &value) {
      if (const auto *number = std::get_if<double>(&value)) {
        return *number;
      }
      const auto &rational = std::get<display_device::Rational>(value);
      return rational.m_denominator == 0 ? 0.0 :
                                           static_cast<double>(rational.m_numerator) / rational.m_denominator;
    }

    double output_refresh(const json &output) {
      const auto current_id = output.value("currentModeId", std::string {});
      for (const auto &mode : output.value("modes", json::array())) {
        if (mode.value("id", std::string {}) == current_id) {
          return mode.value("refreshRate", 0.0);
        }
      }
      return 0.0;
    }

    std::string best_mode_id(
      const json &output,
      const std::optional<display_device::Resolution> &resolution,
      const std::optional<display_device::FloatingPoint> &refresh_rate
    ) {
      std::string best;
      double best_score = std::numeric_limits<double>::max();
      const bool prefer_highest = refresh_rate && floating_point(*refresh_rate) >= 9999.0;
      for (const auto &mode : output.value("modes", json::array())) {
        const auto size = mode.value("size", json::object());
        const auto width = size.value("width", 0u);
        const auto height = size.value("height", 0u);
        if (resolution && (width != resolution->m_width || height != resolution->m_height)) {
          continue;
        }
        const auto hz = mode.value("refreshRate", 0.0);
        double score = 0.0;
        if (prefer_highest) {
          score = -hz;
        } else if (refresh_rate) {
          score = std::abs(hz - floating_point(*refresh_rate));
        }
        if (score < best_score) {
          best_score = score;
          best = mode.value("id", std::string {});
        }
      }
      return best;
    }

    std::pair<std::uint32_t, std::uint32_t> mode_size(const json &output, const std::string &mode_id) {
      for (const auto &mode : output.value("modes", json::array())) {
        if (mode.value("id", std::string {}) == mode_id) {
          const auto size = mode.value("size", json::object());
          return {size.value("width", 0u), size.value("height", 0u)};
        }
      }
      const auto size = output.value("size", json::object());
      return {size.value("width", 0u), size.value("height", 0u)};
    }

    bool mode_matches_refresh(
      const json &output,
      const std::string &mode_id,
      const display_device::FloatingPoint &refresh_rate
    ) {
      for (const auto &mode : output.value("modes", json::array())) {
        if (mode.value("id", std::string {}) == mode_id) {
          return mode_policy::refresh_matches(
            mode.value("refreshRate", 0.0),
            floating_point(refresh_rate)
          );
        }
      }
      return false;
    }

    bool admit_requested_mode(
      std::optional<json> &configuration,
      const std::string &name,
      const display_device::Resolution &resolution,
      const display_device::FloatingPoint &refresh
    ) {
      const auto hz = floating_point(refresh);
      if (!std::isfinite(hz) || hz < 1.0 || hz > 1000.0) {
        BOOST_LOG(error) << "Linux private display: requested refresh is outside the managed display limits: " << hz;
        return false;
      }
      const mode_policy::requested_mode_t request {
        resolution.m_width, resolution.m_height,
        static_cast<std::uint32_t>(std::lround(hz * 1000.0)),
      };
      BOOST_LOG(info) << "Linux private display: requesting " << request.width << 'x' << request.height
                      << '@' << request.refresh_millihz << " mHz on " << name << '.';
      {
        std::shared_lock mutation_admission {topology_mutation_gate};
        if (process_shutdown_preserve_requested() || !restore_allowed()) return false;
        const auto admitted = request_managed_mode(name, request);
        if (!admitted.success) {
          BOOST_LOG(error) << "Linux private display: " << admitted.detail;
          return false;
        }
      }
      // A mode catalog hotplug is asynchronous in KWin. Do not select a nearby
      // preset or reuse a stale mode id while the exact request is publishing.
      const auto deadline = std::chrono::steady_clock::now() + output_publication_timeout;
      do {
        if (process_shutdown_preserve_requested()) return false;
        auto current = query_configuration();
        const auto *output = current ? find_output(*current, name) : nullptr;
        if (output && connected(*output)) {
          const auto candidate = best_mode_id(*output, resolution, refresh);
          if (!candidate.empty() && mode_matches_refresh(*output, candidate, refresh)) {
            configuration = std::move(current);
            return true;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
      } while (std::chrono::steady_clock::now() < deadline);
      BOOST_LOG(error) << "Linux private display: KScreen did not publish the admitted "
                       << request.width << 'x' << request.height << '@' << request.refresh_millihz
                       << " mHz mode on " << name << '.';
      return false;
    }

    std::pair<int, int> logical_size(const json &output) {
      const auto size = output.value("size", json::object());
      const auto scale = std::max(0.25, output.value("scale", 1.0));
      return {
        static_cast<int>(std::ceil(size.value("width", 0) / scale)),
        static_cast<int>(std::ceil(size.value("height", 0) / scale))
      };
    }

    struct phased_configuration_t {
      std::vector<std::string> activate;
      std::vector<std::string> deactivate;
      std::vector<std::string> guard_activate;
      std::optional<std::string> guard_output;
    };

    std::vector<std::string> output_activation_arguments(
      const json &saved,
      const json *present,
      const std::string &name
    ) {
      const auto mode = restore_policy::select_restore_mode(saved, present);
      if (!mode) {
        BOOST_LOG(warning) << "Linux private display: no current mode can verify the saved configuration for " << name << "; retaining the saved output for later recovery.";
        return {};
      }
      const auto prefix = "output." + name + ".";
      std::vector<std::string> arguments {prefix + "enable", prefix + "mode." + *mode};
      // KScreen serializes Output::Rotation as flags, while the doctor takes names.
      static const std::map<int, std::string> rotations {
        {1, "none"}, {2, "left"}, {4, "inverted"}, {8, "right"},
        {16, "flipped"}, {32, "flipped90"}, {64, "flipped180"}, {128, "flipped270"},
      };
      if (const auto rotation = rotations.find(saved.value("rotation", 1)); rotation != rotations.end()) {
        arguments.push_back(prefix + "rotation." + rotation->second);
      }
      arguments.push_back(prefix + "scale." + std::to_string(saved.value("scale", 1.0)));
      const auto pos = saved.value("pos", json::object());
      arguments.push_back(prefix + "position." + std::to_string(pos.value("x", 0)) + "," + std::to_string(pos.value("y", 0)));
      const auto priority = saved.value("priority", 0);
      if (priority > 0) {
        arguments.push_back(prefix + "priority." + std::to_string(priority));
      }
      if (saved.contains("hdr") && output_hdr_capable(present, name)) {
        arguments.push_back(prefix + "hdr." + std::string(saved.value("hdr", false) ? "enable" : "disable"));
      }
      return arguments;
    }

    phased_configuration_t restore_arguments(
      const json &snapshot,
      const json &current,
      const std::set<std::string> &retiring_outputs
    ) {
      phased_configuration_t arguments;
      const auto private_names = private_output_set();
      bool restored_non_private = false;
      std::set<std::string> snapshot_names;
      std::vector<restore_policy::candidate_t> guard_candidates;
      std::map<std::string, std::vector<std::string>> activation_by_output;

      for (const auto &saved : snapshot["outputs"]) {
        const auto name = saved.value("name", std::string {});
        if (name.empty()) {
          if (enabled(saved)) {
            guard_candidates.push_back({name, true, false, false, false});
          }
          continue;
        }
        snapshot_names.insert(name);
        if (mode_policy::managed_connector_name(name)) continue;
        const auto *present = find_output(current, name);
        const bool is_connected = present && connected(*present);
        if (!is_connected) {
          continue;
        }
        const auto prefix = "output." + name + ".";
        if (!enabled(saved)) {
          arguments.deactivate.push_back(prefix + "disable");
          continue;
        }
        auto activation = output_activation_arguments(saved, present, name);
        if (activation.empty()) continue;
        guard_candidates.push_back({name, true, true, false, retiring_outputs.contains(name)});
        restored_non_private = true;
        arguments.activate.insert(arguments.activate.end(), activation.begin(), activation.end());
        activation_by_output.emplace(name, std::move(activation));
      }

      arguments.guard_output = restore_policy::select_guard(guard_candidates);
      if (arguments.guard_output) {
        const auto activation = restore_policy::guard_activation(arguments.guard_output, activation_by_output);
        if (activation) {
          arguments.guard_activate = *activation;
        } else {
          BOOST_LOG(error) << "Linux private display: selected restore guard " << *arguments.guard_output
                           << " has no activation transaction; preserving the current private scanout.";
          arguments.guard_output.reset();
        }
      }

      if (restored_non_private) {
        for (const auto &name : private_names) {
          if (!snapshot_names.contains(name)) {
            if (const auto *present = find_output(current, name); present && connected(*present)) {
              arguments.deactivate.push_back("output." + name + ".disable");
            }
          }
        }
      }
      return arguments;
    }

    bool wait_for_snapshot_activation(const json &snapshot, const bool final = false) {
      return wait_for_configuration([&](const json &current) {
        return restore_policy::snapshot_matches(snapshot, current, final);
      }, final);
    }

    bool wait_for_capture_publication(const std::string &name) {
      const auto deadline = std::chrono::steady_clock::now() + output_verification_timeout;
      do {
        if (!restore_allowed()) return false;
        const auto capture_outputs = capture_output_names();
        if (std::find(capture_outputs.begin(), capture_outputs.end(), name) != capture_outputs.end()) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } while (std::chrono::steady_clock::now() < deadline);
      return false;
    }

    std::string reservation_identity(const rtsp_stream::launch_session_t &session, const bool shared) {
      if (shared) {
        return "shared";
      }
      if (!session.normal_vdd_owner_uuid.empty()) {
        return "client:" + session.normal_vdd_owner_uuid;
      }
      if (!session.client_uuid.empty()) {
        return "client:" + session.client_uuid;
      }
      if (!session.unique_id.empty()) {
        return "client:" + session.unique_id;
      }
      return "session:" + std::to_string(session.id);
    }

    std::string client_reservation_identity(const std::string &client_uuid) {
      return "client:" + client_uuid;
    }

    std::optional<double> retained_scale(state_t &manager, const std::string &identity) {
      if (const auto saved = manager.retained_scales.find(identity); saved != manager.retained_scales.end()) {
        return saved->second;
      }
      const auto saved = statefile::load_virtual_display_scale(identity);
      if (saved) {
        manager.retained_scales.emplace(identity, *saved);
      }
      return saved;
    }

    void remember_scale(state_t &manager, const std::string &identity, const json *output) {
      if (!output || !connected(*output) || !enabled(*output)) {
        return;
      }
      const auto scale = output->value("scale", 0.0);
      if (!std::isfinite(scale) || scale < 0.25 || scale > 5.0) {
        return;
      }
      manager.retained_scales.insert_or_assign(identity, scale);
      statefile::save_virtual_display_scale(identity, scale);
    }

    void remember_reserved_scales(state_t &manager, const json &configuration) {
      for (const auto &[identity, output_name] : manager.reservations) {
        remember_scale(manager, identity, find_output(configuration, output_name));
      }
    }

    std::string snapshot_owner() {
      const auto *uid = std::getenv("VIBEPOLLO_SESSION_UID");
      const auto *role = std::getenv("VIBEPOLLO_SESSION_ROLE");
      return std::string {uid ? uid : std::to_string(getuid())} + ":" + (role ? role : "standalone");
    }

    bool persist_snapshot(const json &snapshot, const bool restore_pending = true) {
      return statefile::save_linux_display_snapshot(json {{"version", 1}, {"owner", snapshot_owner()}, {"restore_pending", restore_pending}, {"topology", snapshot}}.dump());
    }

    void load_snapshot_if_needed(state_t &manager) {
      if (manager.snapshot_loaded) {
        return;
      }
      manager.snapshot_loaded = true;
      if (const auto saved = statefile::load_linux_display_snapshot()) {
        bool restore_pending = false;
        auto snapshot = snapshot_policy::decode<json>(*saved, snapshot_owner(), &restore_pending, &manager.snapshot_legacy_record);
        if (!snapshot) {
          BOOST_LOG(warning) << "Linux private display: ignoring unusable or different-session saved topology.";
        } else if (restore_pending) {
          manager.snapshot = std::move(snapshot);
        }
      }
    }

    bool snapshot_configuration_if_needed(state_t &manager, const json &configuration) {
      load_snapshot_if_needed(manager);
      if (!snapshot_policy::capture(manager.snapshot, configuration, private_output_set(), !manager.reservations.empty(), [](const json &snapshot) {
            return persist_snapshot(snapshot);
          })) {
        BOOST_LOG(error) << "Linux private display: cannot persist the desktop before changing outputs.";
        return false;
      }
      return true;
    }

    void restore_after_failed_preparation() {
      const auto delay = cleanup_policy::failed_preparation_restore_delay(
        proc::proc.current_app_id() > 0,
        config::video.dd.config_revert_on_disconnect,
        config::video.dd.paused_virtual_display_timeout_secs,
        config::video.dd.config_revert_delay);
      if (delay) schedule_revert(*delay, "failed display preparation");
    }

    std::optional<std::string> reserve_output(
      state_t &manager,
      const std::string &identity,
      const bool no_active_sessions,
      const bool reuse_active_reservation = true
    ) {
      const auto connect = [&](const std::string &name) {
        const auto configuration = query_configuration();
        // Refresh idle preferences and persist ownership before hotplug.
        if (!configuration || !snapshot_configuration_if_needed(manager, *configuration)) {
          BOOST_LOG(error) << "Linux private display: cannot save the desktop before connecting " << name << '.';
          return restore_policy::connection_result_e::failed;
        }
        return restore_policy::connect_with_snapshot_and_publication(manager.snapshot, [&]() {
          return manager.snapshot;
        }, [&] {
          return connect_managed_output(name);
        }, [&] {
          if (wait_for_output_publication(name)) return true;
          BOOST_LOG(error) << "Linux private display: " << name
                           << " did not publish in KScreen after broker connection; preserving the admitted connector and saved topology for guarded restoration.";
          return false;
        });
      };
      if (const auto existing = manager.reservations.find(identity); existing != manager.reservations.end()) {
        if (const auto configuration = query_configuration()) {
          if (const auto *output = find_output(*configuration, existing->second); output && connected(*output)) {
            return existing->second;
          }
        }
        if (is_managed_output(existing->second)) {
          const auto connection = connect(existing->second);
          if (connection != restore_policy::connection_result_e::failed) {
            manager.newly_connected_reservations.insert(identity);
            if (connection == restore_policy::connection_result_e::published) return existing->second;
            // Acknowledged hotplug may already be the only live scanout. Keep
            // ownership for guarded recovery and stop trying other connectors.
            return std::nullopt;
          }
        }
        manager.reservations.erase(existing);
      }

      // A Linux compositor owns one global desktop topology. While another stream is
      // active, keep capturing the already-active private desktop instead of modesetting
      // a second per-client connector and invalidating the first stream's exclusive layout.
      if (reuse_active_reservation && !no_active_sessions && !manager.reservations.empty()) {
        const auto &active_output = manager.reservations.begin()->second;
        if (const auto configuration = query_configuration()) {
          if (const auto *output = find_output(*configuration, active_output); output && connected(*output) && enabled(*output)) {
            manager.reservations.emplace(identity, active_output);
            return active_output;
          }
        }
      }

      std::set<std::string> used;
      for (const auto &[_, output] : manager.reservations) {
        used.insert(output);
      }
      for (const auto &candidate : configured_outputs()) {
        if (used.contains(candidate)) {
          continue;
        }
        if (is_managed_output(candidate)) {
          const auto connection = connect(candidate);
          if (connection != restore_policy::connection_result_e::failed) {
            manager.reservations.emplace(identity, candidate);
            manager.newly_connected_reservations.insert(identity);
            if (connection == restore_policy::connection_result_e::published) return candidate;
            return std::nullopt;
          }
          continue;
        }
        if (const auto configuration = query_configuration()) {
          if (const auto *output = find_output(*configuration, candidate); output && connected(*output)) {
            manager.reservations.emplace(identity, candidate);
            return candidate;
          }
        }
      }
      return std::nullopt;
    }
  }  // namespace

  bool initialize() {
    if (process_shutdown_preserve_requested()) {
      return false;
    }
    const auto private_names = private_output_set();
    if (private_names.empty()) {
      BOOST_LOG(info) << "Linux private display: no managed or explicitly reserved outputs are provisioned.";
      return false;
    }

    auto configuration = query_configuration();
    if (!configuration) {
      return false;
    }

    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    load_snapshot_if_needed(manager);
    const auto startup = snapshot_policy::prepare_startup(manager.snapshot, *configuration, private_names, [](const json &snapshot) {
      return persist_snapshot(snapshot, false);
    }, manager.snapshot_legacy_record);
    if (startup == snapshot_policy::startup_action_e::recover) {
      // Hot-unplug can precede KWin's final baseline reapply. The durable
      // marker owns that unfinished restore even after every private output
      // has disconnected; never refresh it from the interrupted topology.
      schedule_revert({}, "recover saved topology after host restart");
      BOOST_LOG(info) << "Linux private display: queued saved topology recovery for the current session.";
      return true;
    }
    if (startup == snapshot_policy::startup_action_e::failed) {
      return false;
    }

    // A normal idle pool is dormant, but a restart may follow a failed
    // compositor handoff. Never hot-unplug the last framebuffer that the
    // selected capture backend can actually enumerate.
    const auto managed_outputs = discover_managed_outputs();
    const std::set<std::string> managed_names {managed_outputs.begin(), managed_outputs.end()};
    std::set<std::string> preserved_private_outputs;
    for (const auto &name : managed_outputs) {
      // Each hotplug may load a different remembered KWin topology. Never
      // reuse the previous connector's guard observation for another unplug.
      configuration = query_configuration();
      if (!configuration) return false;
      const auto *output = find_output(*configuration, name);
      if (!output || !connected(*output)) continue;
      const auto capture_outputs = capture_output_names();
      std::set<std::string> physical_names;
      for (const auto &present : (*configuration)["outputs"]) {
        const auto physical_name = present.value("name", std::string {});
        if (connected(present) && enabled(present) && !private_names.contains(physical_name)) physical_names.insert(physical_name);
      }
      const auto guard = restore_policy::select_capture_ready_guard(*configuration, physical_names, capture_outputs);
      if (!guard) {
        preserved_private_outputs.insert(name);
        BOOST_LOG(warning) << "Linux private display: preserving connected " << name
                           << " during startup because no physical capture output is ready.";
        continue;
      }
      if (!restore_policy::retire_with_capture_guard(true, [&] { return guard; }, [&](const std::string &guard_name) {
            const auto current = query_configuration();
            const auto *physical = current ? find_output(*current, guard_name) : nullptr;
            return physical && connected(*physical) && enabled(*physical);
          }, [] {
            return !process_shutdown_preserve_requested() && restore_allowed();
          }, [&] { return disconnect_managed_output(name); })) {
        BOOST_LOG(error) << "Linux private display: could not safely disconnect stale pool output " << name << '.';
        return false;
      }
      if (!wait_for_configuration([&](const json &current) {
            const auto *present = find_output(current, name);
            return !present || !connected(*present);
          }, true)) {
        BOOST_LOG(error) << "Linux private display: startup hotplug did not settle for " << name << "; preserving remaining connectors.";
        return false;
      }
    }

    configuration = query_configuration();
    if (!configuration) {
      return false;
    }

    std::set<std::string> physical_names;
    for (const auto &output : (*configuration)["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (connected(output) && enabled(output) && !private_names.contains(name)) physical_names.insert(name);
    }
    if (!physical_names.empty()) {
      std::vector<std::string> disable_stale;
      for (const auto &name : private_names) {
        if (preserved_private_outputs.contains(name)) {
          continue;
        }
        // Managed connectors were already hot-unplugged through the broker.
        // KScreen can briefly retain a stale JSON object after that hotplug;
        // do not send a modeset to an output which no longer exists.
        if (managed_names.contains(name)) {
          continue;
        }
        if (const auto *output = find_output(*configuration, name); output && connected(*output) && enabled(*output)) {
          disable_stale.push_back("output." + name + ".disable");
        }
      }
      if (!disable_stale.empty() && !restore_policy::retire_with_capture_guard(true, [&]() -> std::optional<std::string> {
            const auto current = query_configuration();
            if (!current) return std::nullopt;
            const auto capture_outputs = capture_output_names();
            return restore_policy::select_capture_ready_guard(*current, physical_names, capture_outputs);
          }, [&](const std::string &guard) {
            const auto current = query_configuration();
            const auto *output = current ? find_output(*current, guard) : nullptr;
            return output && connected(*output) && enabled(*output);
          }, [] {
            return !process_shutdown_preserve_requested() && restore_allowed();
          }, [&] {
            return execute_configuration(disable_stale, "startup cleanup");
          })) {
        return false;
      }
    }

    BOOST_LOG(info) << "Linux private display: ready with " << private_names.size() << " private output(s).";
    return true;
  }

  prepare_result_t prepare_session(
    rtsp_stream::launch_session_t &session,
    const bool no_active_sessions,
    const bool allow_display_changes
  ) {
    prepare_result_t result;

    if (!session.display_power_guard) {
      session.display_power_guard = display_power::acquire();
    }
    if (!session.display_power_guard) {
      result.requested = true;
      result.error = "The desktop display could not be woken for streaming";
      session.virtual_display_failed = true;
      return result;
    }
    const auto mode = session.virtual_display_mode_override.value_or(config::video.virtual_display_mode);
    const bool config_requests_virtual = mode != config::video_t::virtual_display_mode_e::disabled;
    const bool client_requests_virtual = session.client_virtual_display_override.value_or(session.client_requests_virtual_display);
    const bool explicit_physical = session.client_virtual_display_override && !*session.client_virtual_display_override;
    const bool app_requests_virtual = session.virtual_display;
    result.requested = app_requests_virtual || client_requests_virtual || (config_requests_virtual && !explicit_physical);
    if (session.output_name_override && !session.output_name_override->empty() && !app_requests_virtual && !client_requests_virtual) {
      result.requested = false;
    }

    session.virtual_display = false;
    session.virtual_display_failed = false;
    session.virtual_display_device_id.clear();
    session.virtual_display_ready_since.reset();
    session.virtual_display_hdr_enabled.reset();
    session.virtual_display_recreated_on_demand = false;
    session.virtual_display_needs_resume_apply = false;
    if (!result.requested) {
      return result;
    }

    cleanup_policy::prepare_with_restore_on_failure(cancel_scheduled_revert, [&] {
      auto &manager = state();
      std::lock_guard lock {manager.mutex};
      const bool shared = mode == config::video_t::virtual_display_mode_e::shared;
      const auto identity = reservation_identity(session, shared);
      const auto output_name = reserve_output(manager, identity, no_active_sessions, shared);
      if (!output_name) {
        result.error = "No unreserved Linux private display could be connected";
      } else {
        const auto configuration = query_configuration();
        const auto *output = configuration ? find_output(*configuration, *output_name) : nullptr;
        if (!output || !connected(*output)) {
          manager.reservations.erase(identity);
          manager.newly_connected_reservations.erase(identity);
          // A failed query is not permission to unplug a potentially last-live
          // output. Guarded restoration discovers this released connector.
          result.error = "The leased Linux private display was not published by KScreen";
          session.virtual_display_failed = true;
          return false;
        }
        result.active = true;
        result.output_name = *output_name;
        session.virtual_display = true;
        session.virtual_display_device_id = *output_name;
        const auto capture_outputs = capture_output_names();
        const bool capture_available = std::find(capture_outputs.begin(), capture_outputs.end(), *output_name) != capture_outputs.end();
        session.virtual_display_recreated_on_demand =
          resume_policy::requires_apply(
            manager.newly_connected_reservations.contains(identity),
            enabled(*output),
            capture_available
          );
        session.virtual_display_needs_resume_apply = session.virtual_display_recreated_on_demand;
        if (enabled(*output) && !capture_available) {
          BOOST_LOG(warning) << "Linux private display: " << *output_name
                             << " is enabled in KScreen but unavailable to capture; requiring activation before resume.";
        }
        if (enabled(*output) && capture_available && (!allow_display_changes || shared)) {
          const bool requested_hdr = rtsp_stream::effective_hdr_requested(session);
          const bool current_hdr = output_hdr_capable(output, *output_name) && output->value("hdr", false);
          if (requested_hdr && !current_hdr) session.force_sdr = true;
          session.virtual_display_hdr_enabled = requested_hdr && current_hdr;
          session.virtual_display_ready_since = std::chrono::steady_clock::now();
        }
      }
      return result.active;
    }, restore_after_failed_preparation);

    if (!result.active) {
      session.virtual_display_failed = true;
      BOOST_LOG(error) << "Linux private display: " << result.error;
    } else {
      BOOST_LOG(info) << "Linux private display: reserved " << result.output_name
                      << " for client '" << session.client_name << "'.";
    }
    return result;
  }

  bool apply_session(rtsp_stream::launch_session_t &session) {
    if (!session.virtual_display || session.virtual_display_device_id.empty()) {
      return true;
    }

    const auto use_current_output = [&]() {
      // Capture availability is authoritative. A failed KScreen query or
      // preference verification must not veto a still-capturable output.
      // Keep the leased target: another client's display is not a substitute.
      if (!wait_for_capture_publication(session.virtual_display_device_id)) {
        BOOST_LOG(error) << "Linux private display: no usable current capture output on "
                         << session.virtual_display_device_id << '.';
        return false;
      }
      const auto current = query_configuration();
      const auto *output = current ? find_output(*current, session.virtual_display_device_id) : nullptr;
      if (output) {
        const bool current_hdr = output->value("hdr", false) &&
                                 output_hdr_capable(nullptr, session.virtual_display_device_id);
        if (rtsp_stream::effective_hdr_requested(session) && !current_hdr) {
          session.force_sdr = true;
        }
        session.virtual_display_hdr_enabled = current_hdr;
      } else {
        // Unknown metadata is not proof of SDR. Let capture/encoder probing
        // determine the usable format instead of claiming an applied HDR state.
        session.virtual_display_hdr_enabled.reset();
      }
      session.virtual_display_ready_since = std::chrono::steady_clock::now();
      session.virtual_display_recreated_on_demand = false;
      session.virtual_display_needs_resume_apply = true;
      BOOST_LOG(warning) << "Linux private display: continuing capture on " << session.virtual_display_device_id
                         << " with its current settings after display configuration failed.";
      return true;
    };

    auto configuration = query_configuration();
    if (!configuration) {
      return use_current_output();
    }
    const auto *target_before = find_output(*configuration, session.virtual_display_device_id);
    if (!target_before || !connected(*target_before)) {
      BOOST_LOG(error) << "Linux private display: reserved output disappeared: " << session.virtual_display_device_id;
      return use_current_output();
    }

    auto &manager = state();
    std::set<std::string> reserved_outputs;
    std::optional<double> saved_scale;
    std::string identity;
    bool newly_connected = false;
    {
      std::lock_guard lock {manager.mutex};
      if (!snapshot_configuration_if_needed(manager, *configuration)) {
        return false;
      }
      for (const auto &[_, output_name] : manager.reservations) {
        reserved_outputs.insert(output_name);
      }
      const auto mode = session.virtual_display_mode_override.value_or(config::video.virtual_display_mode);
      identity = reservation_identity(session, mode == config::video_t::virtual_display_mode_e::shared);
      newly_connected = manager.newly_connected_reservations.contains(identity);
      if (config::video.dd.virtual_display_scale_percent == 0 &&
          (newly_connected || !enabled(*target_before))) {
        saved_scale = retained_scale(manager, identity);
      }
    }


    auto effective_video = config::video;
    effective_video.output_name = session.virtual_display_device_id;
    if (session.dd_config_option_override) {
      effective_video.dd.configuration_option = *session.dd_config_option_override;
    }
    const auto parsed = display_device::parse_configuration(effective_video, session);
    if (std::holds_alternative<display_device::failed_to_parse_tag_t>(parsed)) {
      BOOST_LOG(error) << "Linux private display: failed to parse the requested display mode.";
      return use_current_output();
    }

    std::optional<display_device::Resolution> resolution;
    std::optional<display_device::FloatingPoint> refresh;
    std::optional<bool> parsed_hdr_state;
    if (const auto *request = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      resolution = request->m_resolution;
      refresh = request->m_refresh_rate;
      if (request->m_hdr_state) {
        parsed_hdr_state = *request->m_hdr_state == display_device::HdrState::Enabled;
      }
    } else {
      resolution = display_device::Resolution {
        static_cast<unsigned int>(std::max(1, session.resolution_override ? session.resolution_override->width : session.width)),
        static_cast<unsigned int>(std::max(1, session.resolution_override ? session.resolution_override->height : session.height))
      };
      refresh = display_device::Rational {static_cast<unsigned int>(std::max(1, session.fps)), 1};
    }

    auto mode_id = best_mode_id(*target_before, resolution, refresh);
    const bool prefer_highest = refresh && floating_point(*refresh) >= 9999.0;
    if (mode_policy::should_admit_requested_mode(
          resolution.has_value(),
          refresh.has_value(),
          prefer_highest,
          is_managed_output(session.virtual_display_device_id)
        )) {
      if (!admit_requested_mode(configuration, session.virtual_display_device_id, *resolution, *refresh)) {
        return use_current_output();
      }
      target_before = find_output(*configuration, session.virtual_display_device_id);
      if (!target_before) {
        return use_current_output();
      }
      mode_id = best_mode_id(*target_before, resolution, refresh);
    }
    if (mode_id.empty() || (resolution && refresh && !prefer_highest &&
                            is_managed_output(session.virtual_display_device_id) &&
                            !mode_matches_refresh(*target_before, mode_id, *refresh))) {
      BOOST_LOG(error) << "Linux private display: no compatible mode is available for "
                       << session.virtual_display_device_id;
      return use_current_output();
    }

    const auto private_names = private_output_set();
    const auto layout = session.virtual_display_layout_override.value_or(config::video.virtual_display_layout);
    // Remote Monitor peers retain their own connectors independently of the
    // normal game's exclusive preference. With no peer, preserve the ordinary
    // exclusive behavior.
    const bool has_reserved_peer = std::any_of(reserved_outputs.begin(), reserved_outputs.end(), [&](const auto &name) {
      return name != session.virtual_display_device_id;
    });
    const bool exclusive = layout == config::video_t::virtual_display_layout_e::exclusive && !has_reserved_peer;
    const bool primary = layout == config::video_t::virtual_display_layout_e::extended_primary ||
                         layout == config::video_t::virtual_display_layout_e::extended_primary_isolated;
    const bool isolated = layout == config::video_t::virtual_display_layout_e::extended_isolated ||
                          layout == config::video_t::virtual_display_layout_e::extended_primary_isolated;
    const auto target_prefix = "output." + session.virtual_display_device_id + ".";
    std::vector<std::string> arguments {
      target_prefix + "enable",
      target_prefix + "mode." + mode_id,
      target_prefix + "vrrpolicy.always",
    };

    const auto [mode_width, mode_height] = mode_size(*target_before, mode_id);
    const double target_scale = virtual_display_scale::effective_factor(
      config::video.dd.virtual_display_scale_percent,
      mode_width,
      mode_height,
      target_before->value("scale", 1.0),
      saved_scale
    );
    arguments.push_back(target_prefix + "scale." + std::to_string(target_scale));

    int right_edge = 0;
    int bottom_edge = 0;
    int last_priority = 0;
    for (const auto &output : (*configuration)["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (name == session.virtual_display_device_id || !connected(output)) {
        continue;
      }
      const auto prefix = "output." + name + ".";
      if (private_names.contains(name)) {
        if (!reserved_outputs.contains(name)) {
          arguments.push_back(prefix + "disable");
          continue;
        }
        if (enabled(output)) {
          const auto pos = output.value("pos", json::object());
          const auto [width, height] = logical_size(output);
          right_edge = std::max(right_edge, pos.value("x", 0) + width);
          bottom_edge = std::max(bottom_edge, pos.value("y", 0) + height);
          last_priority = std::max(last_priority, output.value("priority", 0));
        }
        continue;
      }
      if (exclusive) {
        arguments.push_back(prefix + "disable");
        continue;
      }
      if (enabled(output)) {
        const auto pos = output.value("pos", json::object());
        const auto [width, height] = logical_size(output);
        right_edge = std::max(right_edge, pos.value("x", 0) + width);
        bottom_edge = std::max(bottom_edge, pos.value("y", 0) + height);
        last_priority = std::max(last_priority, output.value("priority", 0));
        if (primary && output.value("priority", 0) > 0) {
          arguments.push_back(prefix + "priority." + std::to_string(output.value("priority", 0) + 1));
        }
      }
    }

    if (exclusive) {
      arguments.push_back(target_prefix + "position.0,0");
      arguments.push_back(target_prefix + "priority.1");
    } else {
      int x = right_edge;
      int y = 0;
      if (isolated) {
        const auto max_size = (*configuration).value("screen", json::object()).value("maxSize", json::object());
        const auto mode = std::find_if(target_before->at("modes").begin(), target_before->at("modes").end(), [&](const json &candidate) {
          return candidate.value("id", std::string {}) == mode_id;
        });
        const auto size = mode != target_before->at("modes").end() ? mode->value("size", json::object()) : json::object();
        x = std::max(right_edge, max_size.value("width", 64000) - static_cast<int>(size.value("width", 0) / target_scale));
        y = std::max(bottom_edge, max_size.value("height", 64000) - static_cast<int>(size.value("height", 0) / target_scale));
      }
      arguments.push_back(target_prefix + "position." + std::to_string(x) + "," + std::to_string(y));
      arguments.push_back(target_prefix + "priority." + std::to_string(primary ? 1 : std::max(1, last_priority + 1)));
    }

    const bool hdr_requested = rtsp_stream::effective_hdr_requested(session);
    const bool hdr_capable = output_hdr_capable(target_before, session.virtual_display_device_id);
    const auto hdr_policy = linux_hdr::resolve_output_state(
      parsed_hdr_state,
      hdr_capable,
      hdr_capable && target_before->value("hdr", false)
    );
    const bool separate_hdr = linux_hdr::requires_post_modeset_transaction(hdr_policy.command);
    if (hdr_policy.command && !separate_hdr) {
      arguments.push_back(target_prefix + "hdr.disable");
    }
    const std::vector<std::string> hdr_arguments = separate_hdr ?
      std::vector<std::string> {target_prefix + "hdr." + std::string(*hdr_policy.command ? "enable" : "disable")} :
      std::vector<std::string> {};
    const std::vector<std::string> hdr_rearm_arguments = linux_hdr::requires_hdr_rearm(hdr_policy.command, newly_connected) ?
      std::vector<std::string> {target_prefix + "hdr.disable"} :
      std::vector<std::string> {};
    const bool require_hdr_stability = !hdr_rearm_arguments.empty();
    arguments.insert(arguments.end(), hdr_rearm_arguments.begin(), hdr_rearm_arguments.end());
    if (!hdr_policy.command && parsed_hdr_state.value_or(false) && !hdr_capable) {
      BOOST_LOG(warning) << "Linux private display: " << session.virtual_display_device_id
                         << " does not advertise HDR10; the verified session will use SDR.";
    }

    if (!execute_configuration(arguments, "apply")) {
      if (!isolated) {
        return use_current_output();
      }
      BOOST_LOG(warning) << "Linux private display: compositor rejected isolated placement; using an adjacent private output.";
      arguments.erase(
        std::remove_if(arguments.begin(), arguments.end(), [&](const std::string &arg) {
          return arg.starts_with(target_prefix + "position.");
        }),
        arguments.end()
      );
      arguments.push_back(target_prefix + "position." + std::to_string(right_edge) + ",0");
      if (!execute_configuration(arguments, "isolated-layout fallback")) {
        return use_current_output();
      }
    }

    // A newly enabled output can expose its requested HDR bit before KWin has
    // committed the mode and color pipeline. First settle the topology, then
    // apply HDR in its own KScreen transaction so it cannot be lost behind the
    // modeset. This also leaves no unapplied composite transaction for KDE's
    // HDR calibration UI to inherit.
    const auto base_matches = [&](const json &current) {
      const auto *output = find_output(current, session.virtual_display_device_id);
      return output && connected(*output) && enabled(*output) &&
             output->value("currentModeId", std::string {}) == mode_id &&
             std::abs(output->value("scale", 1.0) - target_scale) < 0.01;
    };
    // Mode/scale publication is only an ordering barrier for the following
    // HDR transaction. Requiring three fresh kscreen-doctor processes here
    // adds seconds to every launch without strengthening the final HDR gate.
    if (separate_hdr && !wait_for_configuration([&](const json &current) {
          const auto *output = find_output(current, session.virtual_display_device_id);
          return output && linux_hdr::ready_for_hdr_activation(
                             base_matches(current), require_hdr_stability, output->value("hdr", false));
        })) {
      BOOST_LOG(error) << "Linux private display: timed out stabilizing mode/scale state on "
                       << session.virtual_display_device_id << '.';
      return use_current_output();
    }
    if (!execute_configuration(hdr_arguments, "HDR activation")) {
      return use_current_output();
    }

    bool verified_hdr_enabled = false;
    const bool verified = wait_for_configuration([&](const json &current) {
      if (!base_matches(current)) {
        return false;
      }
      const auto *output = find_output(current, session.virtual_display_device_id);
      if (hdr_policy.command && output->value("hdr", false) != *hdr_policy.command) {
        return false;
      }
      verified_hdr_enabled = hdr_capable && output->value("hdr", false);
      return true;
    }, require_hdr_stability);

    if (!verified || !wait_for_capture_publication(session.virtual_display_device_id)) {
      BOOST_LOG(error) << "Linux private display: timed out verifying mode/HDR/scale or capture state on "
                       << session.virtual_display_device_id << '.';
      return use_current_output();
    }

    if (hdr_requested && !verified_hdr_enabled) {
      session.force_sdr = true;
    }
    session.virtual_display_hdr_enabled = verified_hdr_enabled;
    session.virtual_display_ready_since = std::chrono::steady_clock::now();
    session.virtual_display_recreated_on_demand = false;
    session.virtual_display_needs_resume_apply = false;
    {
      std::lock_guard lock {manager.mutex};
      manager.newly_connected_reservations.erase(identity);
    }
    BOOST_LOG(info) << "Linux private display: applied private output " << session.virtual_display_device_id
                    << " at " << std::lround(target_scale * 100.0) << "% scale.";
    return true;
  }

  bool publish_current_session_state(rtsp_stream::launch_session_t &session) {
    if (!session.virtual_display || session.virtual_display_device_id.empty()) {
      return false;
    }
    const auto configuration = query_configuration();
    const auto *output = configuration ? find_output(*configuration, session.virtual_display_device_id) : nullptr;
    if (!output || !connected(*output) || !enabled(*output)) {
      return false;
    }
    const bool requested_hdr = rtsp_stream::effective_hdr_requested(session);
    const bool current_hdr = output_hdr_capable(output, session.virtual_display_device_id) && output->value("hdr", false);
    if (requested_hdr && !current_hdr) session.force_sdr = true;
    session.virtual_display_hdr_enabled = requested_hdr && current_hdr;
    session.virtual_display_ready_since = std::chrono::steady_clock::now();
    return true;
  }

  void request_process_shutdown_preserve() noexcept {
    preserve_for_process_shutdown.store(true, std::memory_order_release);
    // Drain only bounded mutation-admission sections. KScreen helpers which
    // were already spawned drain outside the gate to their normal deadline;
    // the signal thread can publish shutdown without waiting for their reply.
    std::unique_lock mutation_barrier {topology_mutation_gate};
  }

  bool process_shutdown_preserve_requested() noexcept {
    return preserve_for_process_shutdown.load(std::memory_order_acquire);
  }

  bool remote_create_or_reclaim(
    const std::string &client_uuid,
    const remote_display_topology::mode_t &mode
  ) {
    (void) mode;
    if (client_uuid.empty()) {
      return false;
    }
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    // Remote Monitor bypasses prepare_session(). Invalidate an earlier idle
    // restore under the same lock which publishes its connector reservation.
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    manager.restore_dispatcher.cancel(generation);
    const auto output = reserve_output(
      manager,
      client_reservation_identity(client_uuid),
      false,
      false
    );
    if (!output) {
      BOOST_LOG(error) << "Linux Remote Monitor: no private connector is available for client '"
                       << client_uuid << "'.";
      return false;
    }
    BOOST_LOG(info) << "Linux Remote Monitor: client '" << client_uuid
                    << "' owns private connector " << *output << ".";
    return true;
  }

  void remote_resolve_mode(
    const std::string &client_uuid,
    remote_display_topology::mode_t &mode
  ) {
    const auto output_name = output_for_client(client_uuid);
    const auto configuration = query_configuration();
    const auto *output = output_name && configuration ? find_output(*configuration, *output_name) : nullptr;
    const bool capable = output_name && output_hdr_capable(output, *output_name);

    // Mirror display_device::parse_configuration() on the coordinator's
    // effective copy. In particular, disabled means preserve the current host
    // state; it must not be reinterpreted as the retained client's desired HDR.
    if (rtsp_stream::rtx_hdr_enabled(config::video)) {
      mode.hdr = false;
    } else if (config::video.dd.wa.dummy_plug_hdr10) {
      mode.hdr = true;
    } else if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::disabled) {
      mode.hdr = capable && output && output->value("hdr", false);
      return;
    }

    if (mode.hdr && !capable) {
      mode.hdr = false;
      BOOST_LOG(warning) << "Linux Remote Monitor: the reserved private output for client '"
                         << client_uuid << "' cannot apply HDR; downgrading this stream to SDR.";
    }
  }

  bool remote_apply_composed_topology(
    const std::vector<remote_display_topology::node_t> &nodes
  ) {
    if (process_shutdown_preserve_requested()) {
      return true;
    }
    // An empty composition occurs when a headless final owner releases. The
    // saved pre-stream topology is the authoritative way to disable its output.
    if (nodes.empty()) {
      return revert();
    }

    auto configuration = query_configuration();
    if (!configuration) {
      return false;
    }

    struct desired_output_t {
      std::string name;
      std::string identity;
      remote_display_topology::node_t node;
      bool owned_client {false};
      std::string mode_id;
      double scale {1.0};
    };

    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    manager.restore_dispatcher.cancel(generation);
    if (!snapshot_configuration_if_needed(manager, *configuration)) {
      return false;
    }

    std::vector<desired_output_t> desired;
    std::map<std::string, std::size_t> desired_indexes;
    for (const auto &node : nodes) {
      std::string output_name;
      const bool owned_client = !node.physical && !node.preexisting;
      if (owned_client) {
        const auto reservation = manager.reservations.find(client_reservation_identity(node.id));
        if (reservation == manager.reservations.end()) {
          BOOST_LOG(error) << "Linux Remote Monitor: composed client '" << node.id
                           << "' has no private connector reservation.";
          return false;
        }
        output_name = reservation->second;
      } else {
        output_name = node.device_id.empty() ? node.id : node.device_id;
      }

      const auto *output = find_output(*configuration, output_name);
      if (!output || !connected(*output)) {
        BOOST_LOG(error) << "Linux Remote Monitor: composed output disappeared: " << output_name;
        return false;
      }

      desired_output_t entry {
        .name = output_name,
        .identity = owned_client ? client_reservation_identity(node.id) : std::string {},
        .node = node,
        .owned_client = owned_client,
      };
      if (const auto existing = desired_indexes.find(output_name); existing != desired_indexes.end()) {
        // A normal stream and Remote Monitor owned by the same paired client
        // deliberately resolve to one connector. The explicit client node owns
        // its requested mode and placement.
        if (owned_client || !desired[existing->second].owned_client) {
          desired[existing->second] = std::move(entry);
        }
      } else {
        desired_indexes.emplace(output_name, desired.size());
        desired.push_back(std::move(entry));
      }
    }

    for (auto &entry : desired) {
      if (!entry.owned_client) {
        continue;
      }
      const auto *output = find_output(*configuration, entry.name);
      if (!output || !connected(*output)) {
        BOOST_LOG(error) << "Linux Remote Monitor: output disappeared during requested-mode admission: " << entry.name;
        return false;
      }
      const auto resolution = display_device::Resolution {
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.width)),
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.height)),
      };
      const display_device::FloatingPoint refresh = display_device::Rational {
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.refresh_hz)),
        1,
      };
      entry.mode_id = best_mode_id(*output, resolution, refresh);
      if (is_managed_output(entry.name) &&
          (entry.mode_id.empty() || !mode_matches_refresh(*output, entry.mode_id, refresh))) {
        if (!admit_requested_mode(configuration, entry.name, resolution, refresh)) {
          return false;
        }
        output = find_output(*configuration, entry.name);
        if (!output) return false;
        entry.mode_id = best_mode_id(*output, resolution, refresh);
        if (entry.mode_id.empty() || !mode_matches_refresh(*output, entry.mode_id, refresh)) {
          BOOST_LOG(error) << "Linux Remote Monitor: custom mode was not accepted on " << entry.name;
          return false;
        }
      }
    }

    for (const auto &entry : desired) {
      if (entry.owned_client && entry.mode_id.empty()) {
        BOOST_LOG(error) << "Linux Remote Monitor: no " << entry.node.configured_mode.width
                         << 'x' << entry.node.configured_mode.height << '@'
                         << entry.node.configured_mode.refresh_hz << " mode is available on "
                         << entry.name << ".";
        return false;
      }
    }

    const auto min_x = std::min_element(desired.begin(), desired.end(), [](const auto &lhs, const auto &rhs) {
                         return lhs.node.x < rhs.node.x;
                       })->node.x;
    const auto min_y = std::min_element(desired.begin(), desired.end(), [](const auto &lhs, const auto &rhs) {
                         return lhs.node.y < rhs.node.y;
                       })->node.y;

    std::size_t primary_index = desired.size();
    for (std::size_t i = 0; i < desired.size(); ++i) {
      if (desired[i].node.primary) {
        primary_index = i;
        break;
      }
    }
    if (primary_index == desired.size()) {
      for (std::size_t i = 0; i < desired.size(); ++i) {
        if (const auto *output = find_output(*configuration, desired[i].name); output && output->value("priority", 0) == 1) {
          primary_index = i;
          break;
        }
      }
    }
    if (primary_index == desired.size()) {
      primary_index = 0;
    }

    std::set<std::string> desired_names;
    std::vector<std::string> activate_arguments;
    std::vector<std::string> hdr_rearm_arguments;
    std::vector<std::string> hdr_arguments;
    std::vector<std::string> deactivate_arguments;
    bool retiring_active_scanout = false;
    int next_priority = 2;
    for (std::size_t i = 0; i < desired.size(); ++i) {
      auto &entry = desired[i];
      desired_names.insert(entry.name);
      const auto prefix = "output." + entry.name + ".";
      activate_arguments.push_back(prefix + "enable");
      if (entry.owned_client) {
        activate_arguments.push_back(prefix + "mode." + entry.mode_id);
        activate_arguments.push_back(prefix + "vrrpolicy.always");
        const auto *output = find_output(*configuration, entry.name);
        const auto saved_scale =
          config::video.dd.virtual_display_scale_percent == 0 && output &&
              (manager.newly_connected_reservations.contains(entry.identity) || !enabled(*output)) ?
            retained_scale(manager, entry.identity) :
            std::nullopt;
        entry.scale = virtual_display_scale::effective_factor(
          config::video.dd.virtual_display_scale_percent,
          static_cast<std::uint32_t>(std::max(1, entry.node.configured_mode.width)),
          static_cast<std::uint32_t>(std::max(1, entry.node.configured_mode.height)),
          output ? output->value("scale", 1.0) : 1.0,
          saved_scale
        );
        activate_arguments.push_back(prefix + "scale." + std::to_string(entry.scale));
        if (const auto *output = find_output(*configuration, entry.name); output_hdr_capable(output, entry.name)) {
          if (linux_hdr::requires_hdr_rearm(
                entry.node.configured_mode.hdr,
                manager.newly_connected_reservations.contains(entry.identity)
              )) {
            hdr_rearm_arguments.push_back(prefix + "hdr.disable");
          }
          if (linux_hdr::requires_post_modeset_transaction(entry.node.configured_mode.hdr)) {
            hdr_arguments.push_back(prefix + "hdr.enable");
          } else {
            activate_arguments.push_back(prefix + "hdr.disable");
          }
        } else if (entry.node.configured_mode.hdr) {
          BOOST_LOG(error) << "Linux Remote Monitor: " << entry.name
                           << " does not advertise HDR10 capability.";
          return false;
        }
      }
      activate_arguments.push_back(prefix + "position." + std::to_string(entry.node.x - min_x) + "," + std::to_string(entry.node.y - min_y));
      activate_arguments.push_back(prefix + "priority." + std::to_string(i == primary_index ? 1 : next_priority++));
    }

    for (const auto &output : (*configuration)["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (connected(output) && !desired_names.contains(name)) {
        deactivate_arguments.push_back("output." + name + ".disable");
      }
    }

    // Activate and verify the destination before retiring previous scanouts.
    // A successful KScreen transaction alone does not prove capture readiness.
    activate_arguments.insert(activate_arguments.end(), hdr_rearm_arguments.begin(), hdr_rearm_arguments.end());
    if (!execute_configuration(activate_arguments, "Remote Monitor topology activation")) {
      return false;
    }

    const auto topology_matches = [&](const json &current) {
      return std::ranges::all_of(desired, [&](const auto &entry) {
        const auto *output = find_output(current, entry.name);
        if (!output || !connected(*output) || !enabled(*output)) {
          return false;
        }
        return !entry.owned_client ||
               (output->value("currentModeId", std::string {}) == entry.mode_id &&
                std::abs(output->value("scale", 1.0) - entry.scale) < 0.01);
      });
    };
    // This observation orders the HDR transaction after the modeset. The
    // final verification below is the phase that must remain stable.
    if (!hdr_arguments.empty() && !wait_for_configuration([&](const json &current) {
          return topology_matches(current) && std::ranges::all_of(desired, [&](const auto &entry) {
            if (!entry.owned_client || !linux_hdr::requires_hdr_rearm(
                                         entry.node.configured_mode.hdr,
                                         manager.newly_connected_reservations.contains(entry.identity)
                                       )) {
              return true;
            }
            const auto *output = find_output(current, entry.name);
            return output && !output->value("hdr", false);
          });
        })) {
      BOOST_LOG(error) << "Linux Remote Monitor: timed out stabilizing the pre-HDR output state.";
      return false;
    }
    if (!execute_configuration(hdr_arguments, "Remote Monitor HDR activation")) {
      return false;
    }
    const bool verified = wait_for_configuration([&](const json &current) {
      // Base guard admission on the post-activation observation: KWin may
      // change a previously disabled output while applying the new topology.
      retiring_active_scanout = std::ranges::any_of(current["outputs"], [&](const auto &output) {
        return connected(output) && enabled(output) &&
               std::ranges::find(deactivate_arguments, "output." + output.value("name", std::string {}) + ".disable") != deactivate_arguments.end();
      });
      return topology_matches(current) && std::ranges::all_of(desired, [&](const auto &entry) {
        if (!entry.owned_client) {
          return true;
        }
        const auto *output = find_output(current, entry.name);
        return !output_hdr_capable(output, entry.name) ||
               output->value("hdr", false) == entry.node.configured_mode.hdr;
      });
    }, !hdr_rearm_arguments.empty());
    if (!verified) {
      BOOST_LOG(error) << "Linux Remote Monitor: timed out verifying the composed mode/HDR/scale state.";
      return false;
    }
    const bool retired = restore_policy::retire_with_capture_guard(retiring_active_scanout, [&]() -> std::optional<std::string> {
      std::optional<std::string> guard;
      const bool published = wait_for_configuration([&](const json &current) {
        const auto capture_outputs = capture_output_names();
        guard = restore_policy::select_capture_ready_guard(current, desired_names, capture_outputs);
        return guard.has_value();
      });
      return published ? guard : std::nullopt;
    }, [&](const std::string &guard) {
      // Capture publication can take time. Reread KScreen immediately before
      // retiring the previous scanout rather than trusting the earlier modeset.
      const auto current = query_configuration();
      const auto *output = current ? find_output(*current, guard) : nullptr;
      return output && connected(*output) && enabled(*output);
    }, [] {
      return !process_shutdown_preserve_requested() && restore_allowed();
    }, [&] {
      return execute_configuration(deactivate_arguments, "Remote Monitor topology retirement");
    });
    if (!retired) {
      BOOST_LOG(error) << "Linux Remote Monitor: capture-ready topology retirement was not confirmed; preserving the previous outputs.";
      return false;
    }
    for (const auto &entry : desired) {
      if (entry.owned_client) {
        manager.newly_connected_reservations.erase(entry.identity);
      }
    }
    BOOST_LOG(info) << "Linux Remote Monitor: applied a " << desired.size()
                    << "-output composed desktop.";
    return true;
  }

  std::optional<std::string> remote_exact_capture_output(
    const std::string &client_uuid,
    const remote_display_topology::mode_t &mode
  ) {
    const auto owned_output = output_for_client(client_uuid);
    if (!owned_output) {
      return std::nullopt;
    }

    // KScreen's command is synchronous, but KWin's screencast output registry
    // can trail it briefly. Readiness is exact and bounded; never fall back to
    // another connector while the requested one is still publishing.
    for (int attempt = 0; attempt < 20; ++attempt) {
      const auto configuration = query_configuration();
      if (configuration) {
        if (const auto *output = find_output(*configuration, *owned_output); output && connected(*output) && enabled(*output)) {
          const auto size = output->value("size", json::object());
          const bool mode_matches =
            size.value("width", 0) == mode.width &&
            size.value("height", 0) == mode.height &&
            static_cast<int>(std::lround(output_refresh(*output))) == mode.refresh_hz &&
            output->value("hdr", false) == mode.hdr;
          if (mode_matches) {
            const auto capture_outputs = capture_output_names();
            if (std::find(capture_outputs.begin(), capture_outputs.end(), *owned_output) != capture_outputs.end()) {
              return owned_output;
            }
          }
        }
      }
      if (attempt + 1 < 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
    return std::nullopt;
  }

  bool remote_remove_owned_display(const std::string &client_uuid) {
    if (process_shutdown_preserve_requested()) {
      return true;
    }
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    const auto reservation = manager.reservations.find(client_reservation_identity(client_uuid));
    if (reservation == manager.reservations.end()) {
      return true;
    }
    const auto output_name = reservation->second;
    if (const auto configuration = query_configuration()) {
      remember_scale(manager, reservation->first, find_output(*configuration, output_name));
    }
    const bool still_reserved = std::ranges::any_of(manager.reservations, [&](const auto &entry) {
      return entry.first != reservation->first && entry.second == output_name;
    });
    if (!still_reserved) {
      const auto configuration = query_configuration();
      const auto capture_outputs = capture_output_names();
      const bool has_capture_ready_survivor = configuration &&
                                              std::ranges::any_of((*configuration)["outputs"], [&](const json &output) {
                                                const auto name = output.value("name", std::string {});
                                                return name != output_name && connected(output) && enabled(output) &&
                                                       std::ranges::find(capture_outputs, name) != capture_outputs.end();
                                              });
      if (!has_capture_ready_survivor) {
        BOOST_LOG(warning) << "Linux private display: preserving released output " << output_name
                           << " because no distinct capture-ready scanout can guard its disconnect.";
      } else if (!disconnect_managed_output(output_name)) {
        BOOST_LOG(error) << "Linux private display: failed to disconnect released output " << output_name << '.';
        return false;
      }
    }
    manager.newly_connected_reservations.erase(reservation->first);
    manager.reservations.erase(reservation);
    return true;
  }

  bool is_kernel_output(const std::string &output_name) {
    return is_managed_output(output_name);
  }

  bool is_private_output(const std::string &output_name) {
    return private_output_set().contains(output_name);
  }

  std::optional<std::string> output_for_client(const std::string &client_uuid) {
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    const auto reservation = manager.reservations.find(client_reservation_identity(client_uuid));
    return reservation == manager.reservations.end() ? std::nullopt : std::make_optional(reservation->second);
  }

  static bool revert_locked(
    state_t &manager,
    restore_transaction::result_e *failure_result = nullptr,
    const recovery_policy::target_policy_e target_policy = recovery_policy::target_policy_e::saved_baseline,
    json *observed_topology = nullptr,
    const std::map<std::string, std::string> *allowed_managed_identities = nullptr,
    const bool require_dpms_on_before_retirement = false,
    std::vector<std::string> *failed_managed_retirements = nullptr,
    json *verified_physical_topology_before_retirement = nullptr
  ) {
    if (failure_result) *failure_result = restore_transaction::result_e::cancelled;
    if (process_shutdown_preserve_requested()) {
      return false;
    }
    if (!restore_allowed()) return false;
    const auto private_names = private_output_set();
    if (!manager.snapshot && manager.reservations.empty() && private_names.empty()) {
      return true;
    }
    std::set<std::string> reserved_outputs;
    for (const auto &[_, output_name] : manager.reservations) {
      reserved_outputs.insert(output_name);
    }
    const auto current = query_configuration();
    if (current) {
      if (observed_topology) *observed_topology = *current;
      remember_reserved_scales(manager, *current);
    }
    if (!current || !restore_allowed()) {
      return false;
    }
    load_snapshot_if_needed(manager);
    auto restore_snapshot = manager.snapshot;
    const bool has_enabled_physical_output = std::ranges::any_of((*current)["outputs"], [](const auto &output) {
      return output.value("connected", false) && output.value("enabled", false) &&
             !mode_policy::managed_connector_name(output.value("name", std::string {}));
    });
    const bool preserve_live_physical_layout = recovery_policy::select_target(
      target_policy, has_enabled_physical_output) == recovery_policy::target_e::live_physical_layout;
    if (preserve_live_physical_layout) {
      // Automatic recovery must respect physical choices made while an older
      // snapshot was stranded. Keep the durable intent untouched until this
      // live baseline has completed the guarded handoff below.
      restore_snapshot = snapshot_policy::live_fallback(*current, private_names);
      if (!restore_snapshot) {
        BOOST_LOG(warning) << "Linux private display: automatic recovery has no enabled physical baseline; preserving saved topology.";
        return false;
      }
    }
    auto managed = discover_managed_outputs();
    if (allowed_managed_identities) {
      std::erase_if(managed, [&](const auto &name) {
        const auto expected = allowed_managed_identities->find(name);
        return expected == allowed_managed_identities->end() ||
               !managed_connector_identity_matches(name, expected->second);
      });
    }
    if (!snapshot_policy::restore_needed(restore_snapshot, *current, {managed.begin(), managed.end()}, !manager.reservations.empty())) {
      // Nothing acquired display ownership and no managed orphan remains.
      // The user's current idle layout is authoritative.
      return true;
    }
    const auto use_live_fallback = [&] {
      const auto fallback = snapshot_policy::live_fallback(*current, private_names);
      if (!fallback || !persist_snapshot(*fallback)) {
        return false;
      }
      manager.snapshot = *fallback;
      restore_snapshot = *fallback;
      BOOST_LOG(info) << "Linux private display: recovering with the live enabled physical topology.";
      return true;
    };
    if (!restore_snapshot && !use_live_fallback()) {
      // No historical intent remains. Do not guess which deliberately
      // disabled physical monitor should be enabled, or retire the last image.
      BOOST_LOG(warning) << "Linux private display: no saved or active physical topology can guard connector release; preserving the current private scanout.";
      manager.reservations.clear();
      manager.newly_connected_reservations.clear();
      return false;
    }
    // Restart has no process-local reservations, but its orphan connectors
    // still need retirement once a distinct saved guard is capture-ready.
    const auto remember_orphans = [&] {
      const auto orphaned = snapshot_policy::retiring_outputs(
        *restore_snapshot,
        *current,
        std::set<std::string> {managed.begin(), managed.end()}
      );
      reserved_outputs.insert(orphaned.begin(), orphaned.end());
    };
    remember_orphans();
    auto arguments = restore_arguments(*restore_snapshot, *current, reserved_outputs);
    if (!arguments.guard_output && !preserve_live_physical_layout && use_live_fallback()) {
      restore_snapshot = manager.snapshot;
      remember_orphans();
      arguments = restore_arguments(*restore_snapshot, *current, reserved_outputs);
    }
    if (!arguments.guard_output) {
      if (!restore_allowed()) return false;
      if (failure_result) *failure_result = restore_transaction::result_e::no_guard;
      BOOST_LOG(warning) << "Linux private display: no distinct connected saved output can guard topology restore; preserving the current private scanout and saved topology.";
      manager.reservations.clear();
      manager.newly_connected_reservations.clear();
      return false;
    }
    struct operations_t {
      const std::map<std::string, std::string> *allowed_managed_identities;
      bool automatic_recovery;
      bool require_dpms_on_before_retirement;
      std::optional<std::string> guard_output;
      std::vector<std::string> *failed_managed_retirements;
      json *verified_physical_topology_before_retirement;
      json verified_physical_topology;
      bool topology_deactivation_applied {false};

      phased_configuration_t plan(const json &snapshot, const json &current, const std::set<std::string> &retiring) {
        return restore_arguments(snapshot, current, retiring);
      }
      bool configure(const std::vector<std::string> &arguments, const char *phase) {
        const bool configured = execute_configuration(arguments, phase);
        if (configured && std::string_view {phase} == "topology restore deactivation") {
          topology_deactivation_applied = true;
        }
        return configured;
      }
      bool wait_snapshot(const json &snapshot, const bool final) {
        return wait_for_snapshot_activation(snapshot, final);
      }
      bool wait_capture(const std::string &name) {
        if (!wait_for_capture_publication(name)) return false;
        if (automatic_recovery && topology_deactivation_applied && verified_physical_topology.is_null()) {
          const auto fresh = query_configuration();
          if (!fresh) return false;
          verified_physical_topology = physical_topology_signature(*fresh);
          if (verified_physical_topology_before_retirement) {
            *verified_physical_topology_before_retirement = verified_physical_topology;
          }
        }
        return true;
      }
      bool disconnect(const std::string &name) {
        if (automatic_recovery) {
          if (!restore_allowed()) return false;
          const auto fresh = query_configuration();
          if (!fresh || verified_physical_topology.is_null() ||
              !recovery_policy::topology_still_matches(verified_physical_topology,
                                                        physical_topology_signature(*fresh))) return false;
          if (!guard_output || !wait_for_capture_publication(*guard_output) || !restore_allowed()) return false;
          const auto power_deadline = restore_context ? restore_context->deadline :
            std::chrono::steady_clock::now() + helper_reply_timeout;
          const auto dpms_states = display_power::query_dpms_states(power_deadline);
          const auto no_dpms_states = std::map<std::string, display_power::dpms_state_t> {};
          bool guard_power_known_on = false;
          for (const auto &output : verified_physical_topology) {
            if (!output.is_object() || !output.value("connected", false) || !output.value("enabled", false)) continue;
            const auto physical_name = output.value("name", std::string {});
            if (physical_name.empty() || mode_policy::managed_connector_name(physical_name)) continue;
            const auto observed_power = recovery_connector_state(
              physical_name, dpms_states ? *dpms_states : no_dpms_states);
            if (!observed_power.dpms_known) continue;
            if (!observed_power.dpms_on) return false;
            if (physical_name == *guard_output) guard_power_known_on = true;
          }
          if (require_dpms_on_before_retirement && !guard_power_known_on) return false;
        }
        if (allowed_managed_identities) {
          const auto expected = allowed_managed_identities->find(name);
          if (expected == allowed_managed_identities->end() ||
              !managed_connector_identity_matches(name, expected->second)) {
            if (failed_managed_retirements) failed_managed_retirements->push_back(name);
            return false;
          }
        }
        const bool disconnected = disconnect_managed_output(name);
        if (!disconnected && failed_managed_retirements) failed_managed_retirements->push_back(name);
        return disconnected;
      }
      bool wait_disconnected(const std::set<std::string> &names) {
        const bool gone = wait_for_configuration([&](const json &configuration) {
          return std::ranges::all_of(names, [&](const auto &name) {
            const auto *output = find_output(configuration, name);
            return !output || !connected(*output);
          });
        }, true);
        if (!gone && failed_managed_retirements) {
          failed_managed_retirements->insert(failed_managed_retirements->end(), names.begin(), names.end());
        }
        return gone;
      }
      std::optional<json> query() {
        return query_configuration();
      }
      bool allowed() {
        return !process_shutdown_preserve_requested() && restore_allowed();
      }
    } operations {
      allowed_managed_identities,
      target_policy == recovery_policy::target_policy_e::automatic_recovery,
      require_dpms_on_before_retirement,
      arguments.guard_output,
      failed_managed_retirements,
      verified_physical_topology_before_retirement,
    };
    const auto result = restore_transaction::perform_guarded_restore(*restore_snapshot, *current, reserved_outputs, operations);
    if (result != restore_transaction::result_e::restored) {
      if (failure_result) *failure_result = result;
      BOOST_LOG(warning) << "Linux private display: " << restore_transaction::result_name(result)
                         << "; preserving the saved topology and remaining connector ownership for recovery.";
      return false;
    }
    if (!restore_allowed() || !statefile::save_linux_display_snapshot(std::nullopt)) {
      return false;
    }
    manager.snapshot.reset();
    manager.reservations.clear();
    manager.newly_connected_reservations.clear();
    BOOST_LOG(info) << "Linux private display: restored the pre-stream output topology.";
    return true;
  }

  namespace {
    struct failed_restore_incident_t {
      recovery_policy::incident_t policy;
      std::vector<std::string> physical_outputs;
      std::map<std::string, std::string> managed_connector_identities;
    };

    enum class recovery_check_e { current, busy, stale };
    enum class recovery_enqueue_e { queued, busy, stale };

    bool recovery_owner_active() {
      // The stream query covers pending launches, startup, active capture and
      // teardown. A paused app can retain its managed identity after capture
      // ends, so automatic recovery parks until that identity is released.
      return stream::session::has_capture_runtime_owner() ||
             proc::proc.current_app_id() > 0 ||
             remote_display_topology::instance().has_live_managed_client_identity() ||
             !remote_display_topology::instance().protected_remote_monitor_client_ids().empty() ||
             nvhttp::has_remote_role_owner();
    }

    std::vector<std::string> recovery_physical_outputs(const json &snapshot, const json &observed) {
      std::vector<std::string> result;
      auto append = [&](const json &configuration, const bool enabled_only) {
        if (!configuration.is_object()) return;
        const auto outputs = configuration.find("outputs");
        if (outputs == configuration.end() || !outputs->is_array()) return;
        for (const auto &output : *outputs) {
          if (!output.is_object()) continue;
          const auto name = output.value("name", std::string {});
          if (!name.empty() && (!enabled_only || output.value("enabled", false)) &&
              !mode_policy::managed_connector_name(name)) {
            result.push_back(name);
          }
        }
      };
      append(snapshot, true);
      append(observed, false);
      std::ranges::sort(result);
      result.erase(std::unique(result.begin(), result.end()), result.end());
      return result;
    }

    void stop_recovery_monitor(state_t &manager) {
      std::jthread worker;
      {
        std::lock_guard lock {manager.recovery_monitor_mutex};
        manager.recovery_monitor_stopping = true;
        if (manager.recovery_monitor.joinable()) {
          manager.recovery_monitor.request_stop();
          worker = std::move(manager.recovery_monitor);
        }
      }
      if (worker.joinable()) worker.join();
    }

    recovery_check_e recovery_incident_current(state_t &manager, const failed_restore_incident_t &incident) {
      if (process_shutdown_preserve_requested() ||
          helper_completion_unknown.load(std::memory_order_acquire) ||
          manager.cleanup_generation.load(std::memory_order_acquire) != incident.policy.cleanup_generation ||
          snapshot_owner() != incident.policy.session_owner) {
        return recovery_check_e::stale;
      }
      std::unique_lock lock {manager.mutex, std::try_to_lock};
      if (!lock.owns_lock()) return recovery_check_e::busy;
      return manager.snapshot && manager.snapshot->dump() == incident.policy.snapshot_identity
               ? recovery_check_e::current
               : recovery_check_e::stale;
    }

    recovery_enqueue_e enqueue_failed_restore_recovery(
      state_t &manager,
      failed_restore_incident_t &incident,
      const bool dpms_wake_observed
    ) {
      // Match the normal teardown lock order and inspect the broad caller and
      // retained-monitor ownership before atomically validating the generation
      // under the display mutex. A launch after this point must acquire the
      // lifecycle gate before applying its display plan.
      std::unique_lock lifecycle_lock {nvhttp::stream_lifecycle_mutex(), std::try_to_lock};
      if (!lifecycle_lock.owns_lock()) return recovery_enqueue_e::busy;
      if (recovery_owner_active()) return recovery_enqueue_e::stale;
      std::unique_lock display_lock {manager.mutex, std::try_to_lock};
      if (!display_lock.owns_lock()) return recovery_enqueue_e::busy;
      if (!manager.snapshot || manager.snapshot->dump() != incident.policy.snapshot_identity) return recovery_enqueue_e::stale;
      if (!recovery_policy::may_start(
            incident.policy,
            manager.cleanup_generation.load(std::memory_order_acquire),
            snapshot_owner(),
            manager.snapshot->dump(),
            !helper_completion_unknown.load(std::memory_order_acquire),
            false,
            false,
            process_shutdown_preserve_requested())) {
        return recovery_enqueue_e::stale;
      }
      incident.policy.attempted = true;
      const auto generation = incident.policy.cleanup_generation;
      const auto expected_owner = incident.policy.session_owner;
      const auto expected_snapshot = incident.policy.snapshot_identity;
      const auto managed_identities = incident.managed_connector_identities;
      const bool accepted = manager.restore_dispatcher.submit(generation,
        [generation, expected_owner, expected_snapshot, managed_identities, dpms_wake_observed](std::stop_token stop) {
          auto &manager = state();
          stream::session::cleanup_reservation_t cleanup_reservation;
          static constexpr std::array<std::chrono::milliseconds, 0> no_retry_delays {};
          const auto result = cleanup_policy::run_delayed_restore_with_retries(
            nvhttp::stream_lifecycle_mutex(), manager.mutex, manager.cleanup_generation, generation,
            [] { return recovery_owner_active(); },
            [&](const std::uint64_t claimed_generation, const auto deadline) {
              const restore_context_t context {deadline, [&] {
                return !stop.stop_requested() && !process_shutdown_preserve_requested() &&
                       manager.cleanup_generation.load(std::memory_order_acquire) == claimed_generation;
              }};
              restore_context = &context;
              auto context_guard = util::fail_guard([] { restore_context = nullptr; });
              if (snapshot_owner() != expected_owner || !manager.snapshot ||
                  manager.snapshot->dump() != expected_snapshot) {
                return false;
              }
              restore_transaction::result_e failure = restore_transaction::result_e::cancelled;
              json observed_before_restore;
              json verified_physical_layout;
              std::vector<std::string> failed_retirements;
              const auto power_deadline = deadline;
              if (!revert_locked(manager, &failure,
                                 recovery_policy::target_policy_e::automatic_recovery,
                                 &observed_before_restore,
                                 &managed_identities,
                                 dpms_wake_observed,
                                 &failed_retirements,
                                 &verified_physical_layout)) {
                const bool connector_cleanup_failure =
                  recovery_policy::solely_virtual_cleanup_failed(
                    failure == restore_transaction::result_e::connector_disconnect_failed,
                    failure == restore_transaction::result_e::connector_disappearance_failed);
                std::ranges::sort(failed_retirements);
                failed_retirements.erase(std::unique(failed_retirements.begin(), failed_retirements.end()),
                                         failed_retirements.end());
                if (!connector_cleanup_failure || failed_retirements.empty() ||
                    !observed_before_restore.contains("outputs") || verified_physical_layout.is_null() || recovery_owner_active() ||
                    !restore_allowed()) return false;

                // The fallback is limited to connectors whose exact retirement
                // failed in the guarded transaction. It is allowed only while
                // KScreen still reports the same verified physical layout.
                auto fresh = query_configuration();
                if (!fresh || !recovery_policy::topology_still_matches(
                                verified_physical_layout, physical_topology_signature(*fresh))) return false;
                const auto guard = std::find_if(verified_physical_layout.begin(),
                  verified_physical_layout.end(), [](const auto &output) {
                    return output.is_object() && output.value("connected", false) &&
                           output.value("enabled", false) &&
                           !mode_policy::managed_connector_name(output.value("name", std::string {}));
                  });
                if (guard == verified_physical_layout.end()) return false;
                const auto guard_name = guard->value("name", std::string {});
                if (guard_name.empty() || !wait_for_capture_publication(guard_name) || !restore_allowed()) return false;
                auto power = display_power::query_dpms_states(power_deadline);
                const auto no_dpms_states = std::map<std::string, display_power::dpms_state_t> {};
                auto guard_power = recovery_connector_state(guard_name, power ? *power : no_dpms_states);
                if (guard_power.dpms_known && !guard_power.dpms_on) return false;
                if (dpms_wake_observed && (!guard_power.dpms_known || !guard_power.dpms_on)) {
                  return false;
                }

                for (const auto &name : failed_retirements) {
                  if (!restore_allowed() || recovery_owner_active()) return false;
                  const auto identity = managed_identities.find(name);
                  if (identity == managed_identities.end() ||
                      !managed_connector_identity_matches(name, identity->second)) return false;
                  fresh = query_configuration();
                  if (!fresh || !recovery_policy::topology_still_matches(
                                  verified_physical_layout, physical_topology_signature(*fresh)) ||
                      !wait_for_capture_publication(guard_name) || !restore_allowed()) return false;
                  power = display_power::query_dpms_states(power_deadline);
                  guard_power = recovery_connector_state(guard_name, power ? *power : no_dpms_states);
                  if (guard_power.dpms_known && !guard_power.dpms_on) return false;
                  if (dpms_wake_observed && (!guard_power.dpms_known || !guard_power.dpms_on)) {
                    return false;
                  }
                  if (!disconnect_managed_output(name)) return false;
                  const auto verify_deadline = std::chrono::steady_clock::now() + output_verification_timeout;
                  if (!wait_for_configuration([&](const json &configuration) {
                        const auto *output = find_output(configuration, name);
                        return !output || !connected(*output);
                      }, true) || std::chrono::steady_clock::now() > verify_deadline) return false;
                  if (!managed_connector_verified_disconnected(name, identity->second)) return false;
                  fresh = query_configuration();
                  if (!fresh || !recovery_policy::topology_still_matches(
                                  verified_physical_layout, physical_topology_signature(*fresh)) || !restore_allowed()) return false;
                }
                for (const auto &name : failed_retirements) {
                  const auto identity = managed_identities.find(name);
                  if (identity == managed_identities.end() ||
                      !managed_connector_verified_disconnected(name, identity->second)) return false;
                }
                if (!wait_for_capture_publication(guard_name) || !restore_allowed() || recovery_owner_active() ||
                    !statefile::save_linux_display_snapshot(std::nullopt)) return false;
                manager.snapshot.reset();
                manager.reservations.clear();
                manager.newly_connected_reservations.clear();
              }
              auto reset_generation = manager.reset_generation.load(std::memory_order_acquire);
              if (reset_generation && reset_generation <= claimed_generation && restore_allowed()) {
                if (!statefile::save_linux_display_snapshot(std::nullopt)) return false;
                manager.retained_scales.clear();
                statefile::clear_virtual_display_scales();
                (void) manager.reset_generation.compare_exchange_strong(reset_generation, 0, std::memory_order_acq_rel);
              }
              return true;
            }, [] {
              return !process_shutdown_preserve_requested() &&
                     !helper_completion_unknown.load(std::memory_order_acquire);
            }, no_retry_delays, stop, restore_operation_timeout, [](const std::uint64_t) {
              if (!process_shutdown_preserve_requested() && !stream::session::has_capture_runtime_owner()) {
                remote_display_topology::instance().complete_restored_normal_game_cleanup();
              }
            });
          if (result == cleanup_policy::result_e::restored) {
            BOOST_LOG(info) << "Linux display helper: one-shot guarded recovery completed.";
          } else {
            BOOST_LOG(warning) << "Linux display helper: one-shot guarded recovery did not complete; preserving the saved topology.";
          }
        });
      if (!accepted) {
        incident.policy.attempted = false;
        return recovery_enqueue_e::busy;
      }
      return recovery_enqueue_e::queued;
    }

    void start_failed_restore_monitor(state_t &manager, failed_restore_incident_t incident) {
      std::jthread previous;
      {
        std::lock_guard lock {manager.recovery_monitor_mutex};
        if (manager.recovery_monitor_stopping || process_shutdown_preserve_requested()) return;
        if (manager.recovery_monitor.joinable()) {
          manager.recovery_monitor.request_stop();
          previous = std::move(manager.recovery_monitor);
        }
      }
      if (previous.joinable()) previous.join();

      std::lock_guard lock {manager.recovery_monitor_mutex};
      if (manager.recovery_monitor_stopping || process_shutdown_preserve_requested()) return;
      manager.recovery_monitor = std::jthread([&manager, incident = std::move(incident)](std::stop_token stop) mutable {
        recovery_policy::run_monitor_safely([&] {
          recovery_policy::wake_event_tracker_t events;
          recovery_policy::topology_change_tracker_t<json> physical_topology_events;
          std::map<std::string, display_power::dpms_state_t> sampled_dpms;
          bool ready = false;
          bool dpms_wake_observed = false;
          display_power::observe_events([&](const display_power::observation_t &event) {
            const auto incident_state = recovery_incident_current(manager, incident);
            if (incident_state == recovery_check_e::stale) return false;
            if (event.kind == display_power::observation_t::kind_e::power) {
              sampled_dpms[event.output] = event.power;
              const auto sample = recovery_connector_state(event.output, sampled_dpms);
              if (!ready) {
                events.prime(event.output, sample);
                return true;
              }
              const auto edge = events.observe(event.output, sample);
              if (!edge) return true;
              dpms_wake_observed = dpms_wake_observed || edge.dpms_woke;
            } else if (event.kind == display_power::observation_t::kind_e::topology) {
              if (!ready || mode_policy::managed_connector_name(event.output)) return true;
            }
            const auto observed_topology = query_configuration();
            std::optional<json> observed_physical_signature;
            if (observed_topology) {
              auto signature = physical_topology_signature(*observed_topology);
              if (!signature.is_null()) observed_physical_signature = std::move(signature);
            }
            if (event.kind == display_power::observation_t::kind_e::baseline_ready) {
              ready = true;
              if (observed_physical_signature) physical_topology_events.prime(*observed_physical_signature);
              return true;
            }
            const bool topology_changed = physical_topology_events.observe(observed_physical_signature);
            if (incident_state == recovery_check_e::busy || (!dpms_wake_observed && !topology_changed)) return true;
            {
              const restore_context_t context {
                std::chrono::steady_clock::now() + helper_reply_timeout + output_verification_timeout,
                [&] {
                  return !stop.stop_requested() && !process_shutdown_preserve_requested() &&
                         manager.cleanup_generation.load(std::memory_order_acquire) == incident.policy.cleanup_generation;
                }
              };
              restore_context = &context;
              auto context_guard = util::fail_guard([] { restore_context = nullptr; });
              const auto &current = observed_topology;
              if (current) {
                const bool physical_target_verified = std::ranges::any_of(
                  current->value("outputs", json::array()), [&](const auto &output) {
                    if (!output.is_object() || !output.value("connected", false) ||
                        mode_policy::managed_connector_name(output.value("name", std::string {}))) return false;
                    const auto name = output.value("name", std::string {});
                    const auto connector = recovery_connector_state(name, sampled_dpms);
                    if (!connector.present() ||
                        (connector.dpms_known && !connector.dpms_on)) return false;
                    return !dpms_wake_observed || connector.dpms_known;
                  });
                if (physical_target_verified) {
                  const auto queued = enqueue_failed_restore_recovery(manager, incident, dpms_wake_observed);
                  if (queued == recovery_enqueue_e::queued || queued == recovery_enqueue_e::stale) return false;
                }
              }
            }
            return true;
          }, [&] {
            return !stop.stop_requested() && !process_shutdown_preserve_requested() &&
                   manager.cleanup_generation.load(std::memory_order_acquire) == incident.policy.cleanup_generation;
          });
        }, [](const std::string_view detail) {
          if (detail.empty()) {
            BOOST_LOG(error) << "Linux display recovery monitor stopped after an unknown exception; saved topology is retained.";
          } else {
            BOOST_LOG(error) << "Linux display recovery monitor stopped: " << detail << "; saved topology is retained.";
          }
        });
      });
    }
  }  // namespace

  static bool dispatch_restore(std::chrono::milliseconds delay, std::string reason, bool reset) {
    if (process_shutdown_preserve_requested()) {
      return true;
    }
    auto &manager = state();
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (reset) {
      // Coalescing later restores must retain an accepted persistence reset.
      auto previous = manager.reset_generation.load(std::memory_order_acquire);
      while (previous < generation &&
             !manager.reset_generation.compare_exchange_weak(previous, generation, std::memory_order_acq_rel)) {}
    }
    const bool accepted = manager.restore_dispatcher.submit(generation,
      [generation, reason = std::move(reason)](std::stop_token stop) {
        auto &manager = state();
        stream::session::cleanup_reservation_t cleanup_reservation;
        constexpr std::array retry_delays {std::chrono::milliseconds(250), std::chrono::milliseconds(1000)};
        unsigned attempt = 0;
        std::optional<restore_transaction::result_e> last_transaction_failure;
        std::optional<failed_restore_incident_t> failed_incident;
        std::uint64_t last_claimed_generation = 0;
        json last_observed_topology;
        const auto result = cleanup_policy::run_delayed_restore_with_retries(
          nvhttp::stream_lifecycle_mutex(), manager.mutex, manager.cleanup_generation, generation,
          [] {
            // Normal paused apps may reach their display timeout. Retained
            // Remote Monitors remain owners even without an active transport.
            return stream::session::has_capture_runtime_owner() ||
                   !remote_display_topology::instance().protected_remote_monitor_client_ids().empty();
          },
          [&](const std::uint64_t claimed_generation, const auto deadline) {
            ++attempt;
            last_claimed_generation = claimed_generation;
            last_transaction_failure.reset();
            failed_incident.reset();
            last_observed_topology = json {};
            const restore_context_t context {deadline, [&] {
              return !stop.stop_requested() && !process_shutdown_preserve_requested() &&
                     manager.cleanup_generation.load(std::memory_order_acquire) == claimed_generation;
            }};
            restore_context = &context;
            auto context_guard = util::fail_guard([] { restore_context = nullptr; });
            BOOST_LOG(info) << "Linux display helper: restoring outputs (reason=" << reason << ", attempt=" << attempt << ").";
            restore_transaction::result_e failure_result = restore_transaction::result_e::cancelled;
            json observed_topology;
            if (!revert_locked(manager, &failure_result,
                               recovery_policy::target_policy_e::saved_baseline,
                               &observed_topology)) {
              if (failure_result != restore_transaction::result_e::cancelled) {
                last_transaction_failure = failure_result;
                if (observed_topology.contains("outputs")) {
                  last_observed_topology = std::move(observed_topology);
                }
                if (!helper_completion_unknown.load(std::memory_order_acquire) &&
                    manager.snapshot && snapshot_policy::valid(*manager.snapshot)) {
                  failed_restore_incident_t candidate;
                  candidate.policy.cleanup_generation = claimed_generation;
                  candidate.policy.session_owner = snapshot_owner();
                  candidate.policy.snapshot_identity = manager.snapshot->dump();
                  candidate.policy.restore_callback_claimed = true;
                  candidate.policy.transaction_failed = true;
                  candidate.policy.helper_completion_known = true;
                  candidate.physical_outputs = recovery_physical_outputs(*manager.snapshot, last_observed_topology);
                  candidate.managed_connector_identities = managed_connector_identities();
                  if (!candidate.physical_outputs.empty() && !candidate.managed_connector_identities.empty()) {
                    failed_incident = std::move(candidate);
                  }
                }
              }
              return false;
            }
            auto reset_generation = manager.reset_generation.load(std::memory_order_acquire);
            if (reset_generation && reset_generation <= claimed_generation && restore_allowed()) {
              if (!statefile::save_linux_display_snapshot(std::nullopt)) {
                return false;
              }
              manager.retained_scales.clear();
              statefile::clear_virtual_display_scales();
              (void) manager.reset_generation.compare_exchange_strong(reset_generation, 0, std::memory_order_acq_rel);
            }
            return true;
          }, [] {
            return !process_shutdown_preserve_requested() &&
                   !helper_completion_unknown.load(std::memory_order_acquire);
          }, retry_delays, stop, restore_operation_timeout, [](const std::uint64_t) {
            // The lifecycle gate is still held and the display mutex released.
            // Only a verified restore can retire ended logical roles; live
            // capture and retained Remote Monitors remain protected.
            if (!process_shutdown_preserve_requested() && !stream::session::has_capture_runtime_owner()) {
              remote_display_topology::instance().complete_restored_normal_game_cleanup();
            }
          });
        if (result == cleanup_policy::result_e::failed) {
          BOOST_LOG(error) << "Linux display helper: restore failed; preserving saved topology (reason=" << reason << ").";
          const auto current_generation = manager.cleanup_generation.load(std::memory_order_acquire);
          if (last_transaction_failure && failed_incident && last_claimed_generation != 0 &&
              recovery_policy::failed_claim_is_current(last_claimed_generation, current_generation) &&
              !helper_completion_unknown.load(std::memory_order_acquire) &&
              !process_shutdown_preserve_requested()) {
            // The incident was bound while the claimed callback held both
            // lifecycle and display gates; avoid a second lock race that could
            // silently discard a valid recovery ticket.
            start_failed_restore_monitor(manager, std::move(*failed_incident));
          }
        } else if (result == cleanup_policy::result_e::restored) {
          BOOST_LOG(info) << "Linux display helper: restore completed (reason=" << reason << ").";
        } else {
          BOOST_LOG(debug) << "Linux display helper: restore superseded or still owned (reason=" << reason << ").";
        }
      }, std::max(delay, std::chrono::milliseconds::zero()));
    if (!accepted) BOOST_LOG(warning) << "Linux display helper: restore request was not accepted.";
    return accepted;
  }

  bool revert() {
    return dispatch_restore({}, "immediate restore", false);
  }

  bool reset_persistence() {
    return dispatch_restore({}, "reset display persistence", true);
  }

  void schedule_revert(const std::chrono::milliseconds delay, std::string reason) {
    (void) dispatch_restore(delay, std::move(reason), false);
  }

  void cancel_scheduled_revert() {
    auto &manager = state();
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    manager.restore_dispatcher.cancel(generation);
  }

  void shutdown_restore_worker(bool drain) {
    auto &manager = state();
    // A passive recovery waiter must never keep standalone shutdown's drain
    // alive. Sysfs polling checks stop every two seconds; a single in-flight
    // KScreen query can delay the join by at most its ten-second reply bound.
    stop_recovery_monitor(manager);
    if (drain) manager.restore_dispatcher.drain();
    manager.restore_dispatcher.stop();
  }

  static bool recover_emergency_physical_display() {
    if (!restore_allowed()) return false;
    auto current = query_configuration();
    if (!current || !restore_allowed()) return false;
    if (emergency_policy::physical_active(*current)) return true;
    // The explicit emergency action authorizes activating a disabled monitor.
    // Keep failed saved intent intact; this is a temporary usable desktop.
    for (const auto &candidate : emergency_policy::physical_candidates(*current)) {
      if (!restore_allowed()) return false;
      current = query_configuration();
      if (!current || !restore_allowed()) return false;
      if (emergency_policy::physical_active(*current)) return true;
      const auto name = candidate.value("name", std::string {});
      const auto *present = find_output(*current, name);
      if (!present || !connected(*present)) continue;
      const auto arguments = output_activation_arguments(candidate, present, name);
      if (arguments.empty() || !execute_configuration(arguments, "emergency physical recovery")) continue;
      if (wait_for_snapshot_activation(json {{"outputs", json::array({candidate})}})) {
        BOOST_LOG(info) << "Linux private display: emergency recovery activated physical output " << name << '.';
        return true;
      }
    }
    BOOST_LOG(warning) << "Linux private display: emergency recovery could not verify an active physical output.";
    return false;
  }

  termination_result_t terminate_all() {
    auto &manager = state();
    stream::session::cleanup_reservation_t cleanup_reservation;
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    manager.restore_dispatcher.cancel(generation);
    termination_result_t result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {90};
    const auto outcome = cleanup_policy::run_delayed_restore(
      nvhttp::stream_lifecycle_mutex(), manager.mutex, manager.cleanup_generation, generation,
      [] { return true; },
      [&] {
        const auto claimed_generation = generation + 1;
        auto valid = [&] {
          return !process_shutdown_preserve_requested() &&
                 manager.cleanup_generation.load(std::memory_order_acquire) == claimed_generation;
        };
        restore_context_t context {std::min(deadline, std::chrono::steady_clock::now() + restore_operation_timeout), valid};
        restore_context = &context;
        auto context_guard = util::fail_guard([] { restore_context = nullptr; });
        manager.reset_generation.store(0, std::memory_order_release);
        // Discover the entire driver pool, not only configured or reserved slots.
        // Never send disconnect commands for physical or other drivers' outputs.
        const auto outputs = discover_managed_outputs();
        const auto terminated = cleanup_policy::terminate_outputs(outputs,
          [&] {
            context.deadline = std::min(deadline, std::chrono::steady_clock::now() + restore_operation_timeout);
            const bool restored = revert_locked(manager);
            context.deadline = deadline;
            return restored;
          },
          [](const std::string &name) { return disconnect_managed_output(name); },
          [&](const auto &names) {
            const auto verify_deadline = std::min(deadline, std::chrono::steady_clock::now() + output_verification_timeout);
            do {
              if (!restore_allowed()) return false;
              const bool disconnected = std::ranges::all_of(names, [](const auto &name) {
                const auto path = connector_sysfs_path(name);
                if (path.empty()) return false;
                std::ifstream status {std::filesystem::path {path} / "status"};
                std::string value;
                return status >> value && value == "disconnected";
              });
              if (disconnected) return true;
              std::this_thread::sleep_for(std::chrono::milliseconds {50});
            } while (std::chrono::steady_clock::now() < verify_deadline);
            return false;
          }, [&] { return valid() && restore_allowed(); });
        result = {terminated.topology_restored, terminated.virtual_displays_removed};
        if (result.virtual_displays_removed) {
          result.physical_display_recovered = recover_emergency_physical_display();
          manager.reservations.clear();
          manager.newly_connected_reservations.clear();
        }
        return result.virtual_displays_removed;
      }, {}, deadline, [](const std::uint64_t) {
        // Drop logical owners only after verified removal and outside the display
        // lock. This avoids compositor callbacks recreating a terminated output.
        remote_display_topology::instance().shutdown(true);
      }, cleanup_policy::admission_e::override_owners);
    result.virtual_displays_removed = outcome == cleanup_policy::result_e::restored;
    BOOST_LOG(info) << "Linux private display: terminal cleanup removed=" << result.virtual_displays_removed
                    << ", restored=" << result.topology_restored << '.';
    return result;
  }

  bool capable() {
    return doctor_path().has_value() && !configured_outputs().empty();
  }

  bool ready() {
    if (!doctor_path() || !broker_socket_ready()) {
      return false;
    }
    for (const auto &name : configured_outputs()) {
      if (is_managed_output(name)) {
        // A dormant managed connector is healthy only while both its kernel
        // node and the root-owned control endpoint exist. This remains a
        // passive readiness check; actual IPC is reserved for transitions.
        if (!connector_sysfs_path(name).empty()) {
          return true;
        }
        continue;
      }
      if (connector_is_connected(name)) {
        return true;
      }
    }
    return false;
  }

  bool hdr_capable() {
    return std::ranges::any_of(configured_outputs(), connector_hdr_capable);
  }

  bool kernel_hdr_pool_available() {
    return std::ranges::any_of(configured_outputs(), connector_hdr_capable);
  }

  bool kernel_pool_available() {
    return !discover_managed_outputs().empty();
  }

  std::vector<std::string> private_output_names() {
    return configured_outputs();
  }

  std::size_t client_output_capacity() {
    return configured_client_output_capacity(configured_outputs(), discover_managed_outputs(), connector_is_connected);
  }

  std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
    const display_device::DeviceEnumerationDetail detail
  ) {
    const auto configuration = query_configuration();
    if (!configuration) {
      return std::nullopt;
    }
    display_device::EnumeratedDeviceList result;
    for (const auto &output : (*configuration)["outputs"]) {
      if (!connected(output)) {
        continue;
      }
      display_device::EnumeratedDevice device;
      device.m_device_id = output.value("name", std::string {});
      device.m_display_name = device.m_device_id;
      device.m_friendly_name = is_managed_output(device.m_device_id) ?
                                 "Vibepollo Private Display (" + device.m_device_id + ")" :
                                 device.m_device_id;
      device.m_monitor_device_path = connector_sysfs_path(device.m_device_id);
      if (detail == display_device::DeviceEnumerationDetail::Full && enabled(output)) {
        display_device::EnumeratedDevice::Info info;
        const auto size = output.value("size", json::object());
        info.m_resolution = {
          size.value("width", 0u),
          size.value("height", 0u),
        };
        info.m_resolution_scale = output.value("scale", 1.0);
        info.m_refresh_rate = output_refresh(output);
        info.m_primary = output.value("priority", 0) == 1;
        const auto pos = output.value("pos", json::object());
        info.m_origin_point = {pos.value("x", 0), pos.value("y", 0)};
        if (output_hdr_capable(&output, device.m_device_id)) {
          info.m_hdr_state = output.value("hdr", false) ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
        }
        device.m_info = info;
        std::set<unsigned int> refresh_millihz;
        for (const auto &mode : output.value("modes", json::array())) {
          refresh_millihz.insert(static_cast<unsigned int>(std::round(mode.value("refreshRate", 0.0) * 1000.0)));
        }
        for (const auto value : refresh_millihz) {
          device.m_supported_refresh_rates.push_back({value, 1000});
        }
      }
      result.push_back(std::move(device));
    }
    return result;
  }

  std::string enumerate_devices_json(const display_device::DeviceEnumerationDetail detail) {
    const auto devices = enumerate_devices(detail);
    if (!devices) {
      return "[]";
    }
    return display_device::toJson(*devices, std::nullopt);
  }
}  // namespace platf::linux_private_display
