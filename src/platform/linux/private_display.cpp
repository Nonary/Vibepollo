/**
 * @file src/platform/linux/private_display.cpp
 * @brief Private streaming output management for Linux/KWin.
 */

#include "private_display.h"
#include "display_power.h"

#include "hdr_policy.h"
#include "private_display_mode_client.h"
#include "private_display_configuration_policy.h"
#include "private_display_mode_policy.h"
#include "private_display_restore_policy.h"
#include "private_display_resume_policy.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/framegen_policy.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/state_storage.h"
#include "src/virtual_display_scale.h"

#include <algorithm>
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
#include <virtual_display/driver/linux_control_client.h>

namespace platf::linux_private_display {
  namespace {
    using json = nlohmann::json;

    constexpr auto output_publication_timeout = std::chrono::seconds {3};
    constexpr auto output_verification_timeout = std::chrono::seconds {3};
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool preserve_for_process_shutdown {false};
    std::shared_mutex topology_mutation_gate;

    struct command_result_t {
      bool success {false};
      std::string stdout_text;
      std::string stderr_text;
    };

    struct state_t {
      std::mutex mutex;
      std::optional<json> snapshot;
      std::map<std::string, std::string> reservations;
      std::map<std::string, double> retained_scales;
      std::set<std::string> newly_connected_reservations;
      std::atomic<std::uint64_t> cleanup_generation {0};
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

      GError *error = nullptr;
      GSubprocess *process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error
      );
      if (!process) {
        if (error) {
          result.stderr_text = error->message;
          g_error_free(error);
        }
        return result;
      }
      // Admission closes once the exact helper exists. Its KScreen request is
      // deliberately allowed to drain outside the gate: shutdown must never
      // cancel a compositor transaction, but it also must not wait for an
      // unbounded external reply before publishing the shutdown event.
      if (mutation_admission.owns_lock()) {
        mutation_admission.unlock();
      }

      gchar *stdout_text = nullptr;
      gchar *stderr_text = nullptr;
      const gboolean communicated = g_subprocess_communicate_utf8(
        process,
        nullptr,
        nullptr,
        &stdout_text,
        &stderr_text,
        &error
      );
      if (stdout_text) {
        result.stdout_text = stdout_text;
        g_free(stdout_text);
      }
      if (stderr_text) {
        result.stderr_text = stderr_text;
        g_free(stderr_text);
      }
      if (!communicated && error) {
        if (!result.stderr_text.empty()) {
          result.stderr_text += ": ";
        }
        result.stderr_text += error->message;
        g_error_free(error);
      }
      result.success = communicated && g_subprocess_get_successful(process);
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

    bool disconnect_managed_output(const std::string &name);

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
      if (wait_for_output_publication(name)) {
        return true;
      }
      BOOST_LOG(error) << "Linux private display: " << name
                       << " did not publish in KScreen after broker connection.";
      // Re-enter through the normal fenced disconnect path. If shutdown was
      // requested during publication, preserve the admitted connector for the
      // successor instead of beginning a rollback mutation.
      (void) disconnect_managed_output(name);
      return false;
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
        if (process_shutdown_preserve_requested()) return false;
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
      const auto prefix = "output." + name + ".";
      std::vector<std::string> arguments {prefix + "enable"};
      const auto mode = saved.value("currentModeId", std::string {});
      if (!mode.empty()) {
        arguments.push_back(prefix + "mode." + mode);
      }
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
        const auto *present = find_output(current, name);
        const bool is_connected = present && connected(*present);
        guard_candidates.push_back({
          name,
          enabled(saved),
          is_connected,
          private_names.contains(name),
          retiring_outputs.contains(name),
        });
        if (!is_connected) {
          continue;
        }
        const auto prefix = "output." + name + ".";
        if (!enabled(saved)) {
          arguments.deactivate.push_back(prefix + "disable");
          continue;
        }
        restored_non_private = restored_non_private || !private_names.contains(name);
        auto activation = output_activation_arguments(saved, present, name);
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

    json snapshot_for_output(const json &snapshot, const std::string &name) {
      json result;
      result["outputs"] = json::array();
      for (const auto &saved : snapshot["outputs"]) {
        if (saved.value("name", std::string {}) == name) {
          result["outputs"].push_back(saved);
          break;
        }
      }
      return result;
    }

    bool wait_for_snapshot_activation(const json &snapshot, const bool final = false) {
      return wait_for_configuration([&](const json &current) {
        return restore_policy::snapshot_matches(snapshot, current, final);
      }, final);
    }

    bool wait_for_capture_publication(const std::string &name) {
      const auto deadline = std::chrono::steady_clock::now() + output_verification_timeout;
      do {
        const auto capture_outputs = platf::display_names(platf::mem_type_e::unknown);
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

    json restorable_snapshot(const json &configuration) {
      auto snapshot = configuration;
      const auto private_names = private_output_set();
      const bool has_active_physical = std::ranges::any_of(snapshot["outputs"], [&](const json &output) {
        return connected(output) && enabled(output) &&
               !private_names.contains(output.value("name", std::string {}));
      });
      if (has_active_physical) {
        for (auto &output : snapshot["outputs"]) {
          if (private_names.contains(output.value("name", std::string {}))) {
            output["enabled"] = false;
          }
        }
      }
      return snapshot;
    }

    void snapshot_configuration_if_needed(state_t &manager, const json &configuration) {
      if (!manager.snapshot) {
        manager.snapshot = restorable_snapshot(configuration);
      }
    }

    std::optional<std::string> reserve_output(
      state_t &manager,
      const std::string &identity,
      const bool no_active_sessions,
      const bool reuse_active_reservation = true
    ) {
      const auto connect = [&](const std::string &name) {
        return restore_policy::connect_with_snapshot(manager.snapshot, [&]() -> std::optional<json> {
          const auto configuration = query_configuration();
          if (!configuration) {
            BOOST_LOG(error) << "Linux private display: cannot save the desktop before connecting " << name << '.';
            return std::nullopt;
          }
          return restorable_snapshot(*configuration);
        }, [&] {
          return connect_managed_output(name);
        });
      };
      if (const auto existing = manager.reservations.find(identity); existing != manager.reservations.end()) {
        if (const auto configuration = query_configuration()) {
          if (const auto *output = find_output(*configuration, existing->second); output && connected(*output)) {
            return existing->second;
          }
        }
        if (is_managed_output(existing->second) && connect(existing->second)) {
          manager.newly_connected_reservations.insert(identity);
          return existing->second;
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
          if (connect(candidate)) {
            manager.reservations.emplace(identity, candidate);
            manager.newly_connected_reservations.insert(identity);
            return candidate;
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

    // A normal idle pool is dormant, but a restart may follow a failed
    // compositor handoff. Never hot-unplug the last framebuffer that the
    // selected capture backend can actually enumerate.
    const auto capture_outputs = platf::display_names(platf::mem_type_e::unknown);
    const std::set<std::string> capture_names {capture_outputs.begin(), capture_outputs.end()};
    const bool capture_ready_physical = std::ranges::any_of((*configuration)["outputs"], [&](const json &output) {
      const auto name = output.value("name", std::string {});
      return connected(output) && enabled(output) && !private_names.contains(name) && capture_names.contains(name);
    });

    const auto managed_outputs = discover_managed_outputs();
    const std::set<std::string> managed_names {managed_outputs.begin(), managed_outputs.end()};
    std::set<std::string> preserved_private_outputs;
    for (const auto &name : managed_outputs) {
      const auto *output = find_output(*configuration, name);
      const bool capture_ready_private =
        output && connected(*output) && enabled(*output) && capture_names.contains(name);
      const bool active_private = output && connected(*output) && enabled(*output);
      if (restore_policy::preserve_private_scanout(capture_ready_physical, capture_ready_private) || (!capture_ready_physical && active_private)) {
        preserved_private_outputs.insert(name);
        BOOST_LOG(warning) << "Linux private display: preserving capture-ready " << name
                           << " during startup because no physical capture output is ready"
                           << (capture_ready_private ? "." : " and capture enumeration is still pending.");
        continue;
      }
      if (!disconnect_managed_output(name)) {
        BOOST_LOG(error) << "Linux private display: failed to disconnect stale pool output " << name << '.';
        return false;
      }
    }

    configuration = query_configuration();
    if (!configuration) {
      return false;
    }

    bool has_active_physical = false;
    for (const auto &output : (*configuration)["outputs"]) {
      if (connected(output) && enabled(output) && !private_names.contains(output.value("name", std::string {}))) {
        has_active_physical = true;
        break;
      }
    }
    if (has_active_physical) {
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
      if (!execute_configuration(disable_stale, "startup cleanup")) {
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
    // Failed admission must not cancel cleanup of an earlier idle display.
    cancel_scheduled_revert();

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

    {
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
          (void) disconnect_managed_output(*output_name);
          result.error = "The leased Linux private display was not published by KScreen";
          session.virtual_display_failed = true;
          return result;
        }
        result.active = true;
        result.output_name = *output_name;
        session.virtual_display = true;
        session.virtual_display_device_id = *output_name;
        const auto capture_outputs = platf::display_names(platf::mem_type_e::unknown);
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
    }

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
      snapshot_configuration_if_needed(manager, *configuration);
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
      refresh = display_device::Rational {framegen::normalize_refresh_millihz(std::max(1, session.fps)), 1000};
    }

    auto mode_id = best_mode_id(*target_before, resolution, refresh);
    const bool prefer_highest = refresh && floating_point(*refresh) >= 9999.0;
    if (resolution && refresh && !prefer_highest && is_managed_output(session.virtual_display_device_id) &&
        (mode_id.empty() || !mode_matches_refresh(*target_before, mode_id, *refresh))) {
      if (!admit_requested_mode(configuration, session.virtual_display_device_id, *resolution, *refresh)) {
        return use_current_output();
      }
      target_before = find_output(*configuration, session.virtual_display_device_id);
      if (!target_before) {
        return use_current_output();
      }
      mode_id = best_mode_id(*target_before, resolution, refresh);
    }
    if (mode_id.empty()) {
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
    // were already spawned continue outside the gate and are never cancelled
    // mid-transaction; the signal thread can therefore publish shutdown
    // without waiting for an unbounded compositor reply.
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
    snapshot_configuration_if_needed(manager, *configuration);

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

    // KScreen may process one command line in connector order rather than the
    // order supplied. Activating the destination first prevents KWin from
    // publishing a transient zero-output desktop while an exclusive topology
    // replaces the physical display.
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
    if (!execute_configuration(deactivate_arguments, "Remote Monitor topology retirement")) {
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
            const auto capture_outputs = platf::display_names(platf::mem_type_e::unknown);
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
      const auto capture_outputs = platf::display_names(platf::mem_type_e::unknown);
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

  bool revert() {
    cancel_scheduled_revert();
    if (process_shutdown_preserve_requested()) {
      return true;
    }
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    std::set<std::string> reserved_outputs;
    for (const auto &[_, output_name] : manager.reservations) {
      reserved_outputs.insert(output_name);
    }
    const auto current = query_configuration();
    if (current) {
      remember_reserved_scales(manager, *current);
    }
    if (!manager.snapshot) {
      if (!reserved_outputs.empty()) {
        BOOST_LOG(warning) << "Linux private display: no saved replacement topology can guard connector release; preserving the current private scanout and releasing only process-local ownership.";
      }
      manager.reservations.clear();
      manager.newly_connected_reservations.clear();
      return true;
    }
    if (!current) {
      return false;
    }
    const auto arguments = restore_arguments(*manager.snapshot, *current, reserved_outputs);
    if (!arguments.guard_output) {
      // A headless saved baseline, or a saved private output which is itself
      // being retired, cannot authorize removing the compositor's last live
      // scanout. Release only process-local client ownership; startup can
      // retire the preserved connector after a distinct physical capture
      // source exists.
      BOOST_LOG(warning) << "Linux private display: no distinct connected saved output can guard topology restore; preserving the current private scanout.";
      manager.snapshot.reset();
      manager.reservations.clear();
      manager.newly_connected_reservations.clear();
      return true;
    }
    if (!execute_configuration(arguments.guard_activate, "topology restore guard activation")) {
      return false;
    }
    const auto guard_snapshot = snapshot_for_output(*manager.snapshot, *arguments.guard_output);
    if (!wait_for_snapshot_activation(guard_snapshot)) {
      BOOST_LOG(error) << "Linux private display: saved output " << *arguments.guard_output
                       << " did not activate as the topology restore guard; preserving the current private scanout.";
      return false;
    }
    if (!wait_for_capture_publication(*arguments.guard_output)) {
      BOOST_LOG(error) << "Linux private display: saved output " << *arguments.guard_output
                       << " activated in KScreen but did not publish to the capture backend; preserving the current private scanout.";
      return false;
    }
    BOOST_LOG(debug) << "Linux private display: capture-ready restore guard "
                     << *arguments.guard_output << " is active.";
    // Apply retirement and activation as one KScreen transaction. Activating
    // every saved output before retiring the private output can exceed the
    // compositor's max-active-output limit. The capture-ready guard above
    // ensures this transaction cannot create a transient zero-output desktop.
    std::vector<std::string> restore;
    std::ranges::copy_if(arguments.deactivate, std::back_inserter(restore), [&](const auto &argument) {
      // Keep every retiring scanout live until a distinct guard has survived
      // the complete restore and the final capture-publication check.
      return std::ranges::none_of(reserved_outputs, [&](const auto &retiring_output) {
        return argument.starts_with("output." + retiring_output + ".");
      });
    });
    const auto guard_prefix = "output." + *arguments.guard_output + ".";
    std::ranges::copy_if(arguments.activate, std::back_inserter(restore), [&](const auto &argument) {
      // The guard is already in its exact saved state and capture-verified.
      // Reissuing its mode/HDR properties here could retrain the physical link
      // between verification and connector retirement.
      return !argument.starts_with(guard_prefix);
    });
    if (!execute_configuration(restore, "topology restore")) {
      return false;
    }
    if (!wait_for_snapshot_activation(*manager.snapshot)) {
      BOOST_LOG(error) << "Linux private display: timed out activating the saved output topology.";
      return false;
    }
    if (!wait_for_capture_publication(*arguments.guard_output)) {
      BOOST_LOG(error) << "Linux private display: restore guard " << *arguments.guard_output
                       << " lost capture publication before connector retirement; preserving the current private scanout.";
      return false;
    }
    bool disconnected = true;
    for (const auto &output_name : reserved_outputs) {
      disconnected = disconnect_managed_output(output_name) && disconnected;
    }
    if (!disconnected) {
      BOOST_LOG(error) << "Linux private display: restored topology but failed to disconnect one or more released outputs.";
      return false;
    }
    // The broker acknowledgement precedes KWin's hotplug processing. Wait for
    // removal before reapplying the baseline: KWin can load a different saved
    // setup when the connected output set changes, including enabling a panel
    // which was disabled before streaming.
    if (!wait_for_configuration([&](const json &configuration) {
          return std::ranges::all_of(reserved_outputs, [&](const auto &name) {
            const auto *output = find_output(configuration, name);
            return !output || !connected(*output);
          });
        }, true)) {
      BOOST_LOG(error) << "Linux private display: timed out waiting for released outputs to disappear from KScreen.";
      return false;
    }
    const auto settled = query_configuration();
    if (!settled) {
      return false;
    }
    if (!restore_policy::snapshot_matches(*manager.snapshot, *settled, true)) {
      const auto final_arguments = restore_arguments(*manager.snapshot, *settled, reserved_outputs);
      auto final_restore = final_arguments.deactivate;
      final_restore.insert(final_restore.end(), final_arguments.activate.begin(), final_arguments.activate.end());
      if (!execute_configuration(final_restore, "post-disconnect topology restore")) {
        return false;
      }
    }
    if (!wait_for_snapshot_activation(*manager.snapshot, true)) {
      BOOST_LOG(error) << "Linux private display: the pre-stream topology did not stabilize after connector retirement.";
      return false;
    }
    manager.snapshot.reset();
    manager.reservations.clear();
    manager.newly_connected_reservations.clear();
    BOOST_LOG(info) << "Linux private display: restored the pre-stream output topology.";
    return true;
  }

  bool reset_persistence() {
    if (!revert()) {
      return false;
    }
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    manager.snapshot.reset();
    manager.reservations.clear();
    manager.retained_scales.clear();
    manager.newly_connected_reservations.clear();
    statefile::clear_virtual_display_scales();
    return true;
  }

  void schedule_revert(const std::chrono::milliseconds delay, std::string reason) {
    auto &manager = state();
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::thread([delay, generation, reason = std::move(reason)]() {
      std::this_thread::sleep_for(delay);
      auto &delayed_manager = state();
      if (delayed_manager.cleanup_generation.load(std::memory_order_acquire) == generation) {
        BOOST_LOG(info) << "Linux private display: " << reason << " elapsed; restoring outputs.";
        (void) revert();
      }
    }).detach();
  }

  void cancel_scheduled_revert() {
    state().cleanup_generation.fetch_add(1, std::memory_order_acq_rel);
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
