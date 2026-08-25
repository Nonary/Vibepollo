/**
 * @file src/platform/linux/private_display.cpp
 * @brief Private streaming output management for Linux/KWin.
 */

#include "private_display.h"

#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <display_device/json.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace platf::linux_private_display {
  namespace {
    using json = nlohmann::json;

    struct command_result_t {
      bool success {false};
      std::string stdout_text;
      std::string stderr_text;
    };

    struct state_t {
      std::mutex mutex;
      std::optional<json> snapshot;
      std::map<std::string, std::string> reservations;
      std::atomic<std::uint64_t> cleanup_generation {0};
    };

    state_t &state() {
      static state_t value;
      return value;
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

    command_result_t run_doctor(const std::vector<std::string> &arguments) {
      command_result_t result;
      const auto executable = doctor_path();
      if (!executable) {
        result.stderr_text = "kscreen-doctor was not found in PATH";
        return result;
      }

      std::vector<std::string> owned_argv;
      owned_argv.reserve(arguments.size() + 1);
      owned_argv.push_back(*executable);
      owned_argv.insert(owned_argv.end(), arguments.begin(), arguments.end());
      std::vector<const gchar *> argv;
      argv.reserve(owned_argv.size() + 1);
      for (const auto &arg : owned_argv) {
        argv.push_back(arg.c_str());
      }
      argv.push_back(nullptr);

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

    std::string connector_sysfs_path(const std::string &name) {
      std::error_code error;
      const std::filesystem::path drm_class {"/sys/class/drm"};
      const auto suffix = "-" + name;
      for (std::filesystem::directory_iterator it {drm_class, error}, end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (filename.ends_with(suffix)) {
          return it->path().string();
        }
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

    std::set<std::string> private_output_set() {
      const auto outputs = configured_outputs();
      return {outputs.begin(), outputs.end()};
    }

    bool execute_configuration(const std::vector<std::string> &arguments, const std::string_view operation) {
      if (arguments.empty()) {
        return true;
      }
      const auto result = run_doctor(arguments);
      if (!result.success) {
        BOOST_LOG(error) << "Linux private display: KScreen " << operation << " failed: "
                         << (result.stderr_text.empty() ? result.stdout_text : result.stderr_text);
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

    std::pair<int, int> logical_size(const json &output) {
      const auto size = output.value("size", json::object());
      const auto scale = std::max(0.25, output.value("scale", 1.0));
      return {
        static_cast<int>(std::ceil(size.value("width", 0) / scale)),
        static_cast<int>(std::ceil(size.value("height", 0) / scale))
      };
    }

    std::vector<std::string> restore_arguments(const json &snapshot, const json &current) {
      std::vector<std::string> arguments;
      const auto private_names = private_output_set();
      bool restored_non_private = false;
      std::set<std::string> snapshot_names;

      for (const auto &saved : snapshot["outputs"]) {
        const auto name = saved.value("name", std::string {});
        if (name.empty()) {
          continue;
        }
        snapshot_names.insert(name);
        const auto *present = find_output(current, name);
        if (!present || !connected(*present)) {
          continue;
        }
        const auto prefix = "output." + name + ".";
        if (!enabled(saved)) {
          arguments.push_back(prefix + "disable");
          continue;
        }
        restored_non_private = restored_non_private || !private_names.contains(name);
        arguments.push_back(prefix + "enable");
        const auto mode = saved.value("currentModeId", std::string {});
        if (!mode.empty()) {
          arguments.push_back(prefix + "mode." + mode);
        }
        arguments.push_back(prefix + "scale." + std::to_string(saved.value("scale", 1.0)));
        const auto pos = saved.value("pos", json::object());
        arguments.push_back(prefix + "position." + std::to_string(pos.value("x", 0)) + "," + std::to_string(pos.value("y", 0)));
        const auto priority = saved.value("priority", 0);
        if (priority > 0) {
          arguments.push_back(prefix + "priority." + std::to_string(priority));
        }
        if (saved.contains("hdr") && present->contains("hdr")) {
          arguments.push_back(prefix + "hdr." + std::string(saved.value("hdr", false) ? "enable" : "disable"));
        }
      }

      if (restored_non_private) {
        for (const auto &name : private_names) {
          if (!snapshot_names.contains(name)) {
            if (const auto *present = find_output(current, name); present && connected(*present)) {
              arguments.push_back("output." + name + ".disable");
            }
          }
        }
      }
      return arguments;
    }

    std::string reservation_identity(const rtsp_stream::launch_session_t &session, const bool shared) {
      if (shared) {
        return "shared";
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

    void snapshot_configuration_if_needed(state_t &manager, const json &configuration) {
      if (manager.snapshot) {
        return;
      }
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
      manager.snapshot = std::move(snapshot);
    }

    std::optional<std::string> reserve_output(
      state_t &manager,
      const json &configuration,
      const std::string &identity,
      const bool no_active_sessions,
      const bool reuse_active_reservation = true
    ) {
      if (const auto existing = manager.reservations.find(identity); existing != manager.reservations.end()) {
        if (const auto *output = find_output(configuration, existing->second); output && connected(*output)) {
          return existing->second;
        }
        manager.reservations.erase(existing);
      }

      // A Linux compositor owns one global desktop topology. While another stream is
      // active, keep capturing the already-active private desktop instead of modesetting
      // a second per-client connector and invalidating the first stream's exclusive layout.
      if (reuse_active_reservation && !no_active_sessions && !manager.reservations.empty()) {
        const auto &active_output = manager.reservations.begin()->second;
        if (const auto *output = find_output(configuration, active_output); output && connected(*output) && enabled(*output)) {
          manager.reservations.emplace(identity, active_output);
          return active_output;
        }
      }

      std::set<std::string> used;
      for (const auto &[_, output] : manager.reservations) {
        used.insert(output);
      }
      for (const auto &candidate : configured_outputs()) {
        const auto *output = find_output(configuration, candidate);
        if (output && connected(*output) && !used.contains(candidate)) {
          manager.reservations.emplace(identity, candidate);
          return candidate;
        }
      }
      return std::nullopt;
    }
  }  // namespace

  bool initialize() {
    auto configuration = query_configuration();
    if (!configuration) {
      return false;
    }
    const auto private_names = private_output_set();
    if (private_names.empty()) {
      BOOST_LOG(info) << "Linux private display: no managed or explicitly reserved outputs are connected.";
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
    (void) allow_display_changes;
    cancel_scheduled_revert();
    prepare_result_t result;

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

    auto configuration = query_configuration();
    if (!configuration) {
      result.error = "KScreen is unavailable; a private display cannot be prepared";
    } else {
      auto &manager = state();
      std::lock_guard lock {manager.mutex};
      const bool shared = mode == config::video_t::virtual_display_mode_e::shared;
      const auto identity = reservation_identity(session, shared);
      const auto output_name = reserve_output(manager, *configuration, identity, no_active_sessions, shared);
      if (!output_name) {
        result.error = "No unreserved Linux private display is connected";
      } else {
        const auto *output = find_output(*configuration, *output_name);
        result.active = true;
        result.output_name = *output_name;
        session.virtual_display = true;
        session.virtual_display_device_id = *output_name;
        session.virtual_display_ready_since = std::chrono::steady_clock::now();
        const bool hdr_capable = output && output->contains("hdr");
        session.virtual_display_hdr_enabled = hdr_capable && rtsp_stream::effective_hdr_requested(session);
        if (!hdr_capable && rtsp_stream::effective_hdr_requested(session)) {
          session.force_sdr = true;
          BOOST_LOG(warning) << "Linux private display: " << *output_name
                             << " has no HDR capability; downgrading the session to SDR.";
        }
        session.virtual_display_recreated_on_demand = output && !enabled(*output);
        session.virtual_display_needs_resume_apply = session.virtual_display_recreated_on_demand;
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

  bool apply_session(const rtsp_stream::launch_session_t &session) {
    if (!session.virtual_display || session.virtual_display_device_id.empty()) {
      return true;
    }

    auto configuration = query_configuration();
    if (!configuration) {
      return false;
    }
    const auto *target_before = find_output(*configuration, session.virtual_display_device_id);
    if (!target_before || !connected(*target_before)) {
      BOOST_LOG(error) << "Linux private display: reserved output disappeared: " << session.virtual_display_device_id;
      return false;
    }

    auto &manager = state();
    std::set<std::string> reserved_outputs;
    {
      std::lock_guard lock {manager.mutex};
      snapshot_configuration_if_needed(manager, *configuration);
      for (const auto &[_, output_name] : manager.reservations) {
        reserved_outputs.insert(output_name);
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
      return false;
    }

    std::optional<display_device::Resolution> resolution;
    std::optional<display_device::FloatingPoint> refresh;
    if (const auto *request = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      resolution = request->m_resolution;
      refresh = request->m_refresh_rate;
    } else {
      resolution = display_device::Resolution {
        static_cast<unsigned int>(std::max(1, session.resolution_override ? session.resolution_override->width : session.width)),
        static_cast<unsigned int>(std::max(1, session.resolution_override ? session.resolution_override->height : session.height))
      };
      refresh = display_device::Rational {static_cast<unsigned int>(std::max(1, session.fps)), 1};
    }

    auto mode_id = best_mode_id(*target_before, resolution, refresh);
    const bool prefer_highest = refresh && floating_point(*refresh) >= 9999.0;
    if (mode_id.empty() && resolution && refresh && !prefer_highest && is_managed_output(session.virtual_display_device_id)) {
      const auto refresh_millihz = static_cast<unsigned int>(std::max(1.0, std::round(floating_point(*refresh) * 1000.0)));
      const auto custom_mode = "output." + session.virtual_display_device_id + ".addCustomMode." +
                               std::to_string(resolution->m_width) + "." +
                               std::to_string(resolution->m_height) + "." +
                               std::to_string(refresh_millihz) + ".reduced";
      if (!execute_configuration({custom_mode}, "custom-mode creation")) {
        return false;
      }
      configuration = query_configuration();
      if (!configuration) {
        return false;
      }
      target_before = find_output(*configuration, session.virtual_display_device_id);
      if (!target_before) {
        return false;
      }
      mode_id = best_mode_id(*target_before, resolution, refresh);
    }
    if (mode_id.empty()) {
      BOOST_LOG(error) << "Linux private display: no compatible mode is available for "
                       << session.virtual_display_device_id;
      return false;
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
    };

    double target_scale = target_before->value("scale", 1.0);
    if (config::video.dd.virtual_display_scale_percent > 0) {
      target_scale = config::video.dd.virtual_display_scale_percent / 100.0;
    } else if (config::video.dd.virtual_display_scale_percent < 0) {
      target_scale = 1.0;
    }
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
    if (target_before->contains("hdr")) {
      arguments.push_back(target_prefix + "hdr." + std::string(hdr_requested ? "enable" : "disable"));
    } else if (hdr_requested) {
      BOOST_LOG(warning) << "Linux private display: " << session.virtual_display_device_id
                         << " does not expose HDR; streaming this session in SDR.";
    }

    if (!execute_configuration(arguments, "apply")) {
      if (!isolated) {
        return false;
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
        return false;
      }
    }
    BOOST_LOG(info) << "Linux private display: applied private output " << session.virtual_display_device_id << ".";
    return true;
  }

  bool remote_create_or_reclaim(
    const std::string &client_uuid,
    const remote_display_topology::mode_t &mode
  ) {
    (void) mode;
    if (client_uuid.empty()) {
      return false;
    }
    const auto configuration = query_configuration();
    if (!configuration) {
      return false;
    }
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    const auto output = reserve_output(
      manager,
      *configuration,
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

  bool remote_apply_composed_topology(
    const std::vector<remote_display_topology::node_t> &nodes
  ) {
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
      remote_display_topology::node_t node;
      bool owned_client {false};
      std::string mode_id;
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

    std::vector<std::string> custom_modes;
    for (auto &entry : desired) {
      if (!entry.owned_client) {
        continue;
      }
      const auto *output = find_output(*configuration, entry.name);
      const auto resolution = display_device::Resolution {
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.width)),
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.height)),
      };
      const display_device::FloatingPoint refresh = display_device::Rational {
        static_cast<unsigned int>(std::max(1, entry.node.configured_mode.refresh_hz)),
        1,
      };
      entry.mode_id = best_mode_id(*output, resolution, refresh);
      if (entry.mode_id.empty() && is_managed_output(entry.name)) {
        custom_modes.push_back(
          "output." + entry.name + ".addCustomMode." +
          std::to_string(resolution.m_width) + "." +
          std::to_string(resolution.m_height) + "." +
          std::to_string(static_cast<unsigned int>(entry.node.configured_mode.refresh_hz * 1000)) +
          ".reduced"
        );
      }
    }

    if (!custom_modes.empty()) {
      if (!execute_configuration(custom_modes, "Remote Monitor custom-mode creation")) {
        return false;
      }
      configuration = query_configuration();
      if (!configuration) {
        return false;
      }
      for (auto &entry : desired) {
        if (!entry.owned_client || !entry.mode_id.empty()) {
          continue;
        }
        const auto *output = find_output(*configuration, entry.name);
        if (!output) {
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
    std::vector<std::string> arguments;
    int next_priority = 2;
    for (std::size_t i = 0; i < desired.size(); ++i) {
      auto &entry = desired[i];
      desired_names.insert(entry.name);
      const auto prefix = "output." + entry.name + ".";
      arguments.push_back(prefix + "enable");
      if (entry.owned_client) {
        arguments.push_back(prefix + "mode." + entry.mode_id);
        double scale = 1.0;
        if (config::video.dd.virtual_display_scale_percent > 0) {
          scale = config::video.dd.virtual_display_scale_percent / 100.0;
        } else if (config::video.dd.virtual_display_scale_percent == 0) {
          if (const auto *output = find_output(*configuration, entry.name)) {
            scale = output->value("scale", 1.0);
          }
        }
        arguments.push_back(prefix + "scale." + std::to_string(scale));
        if (const auto *output = find_output(*configuration, entry.name); output && output->contains("hdr")) {
          arguments.push_back(prefix + "hdr.disable");
        }
      }
      arguments.push_back(prefix + "position." + std::to_string(entry.node.x - min_x) + "," + std::to_string(entry.node.y - min_y));
      arguments.push_back(prefix + "priority." + std::to_string(i == primary_index ? 1 : next_priority++));
    }

    for (const auto &output : (*configuration)["outputs"]) {
      const auto name = output.value("name", std::string {});
      if (connected(output) && !desired_names.contains(name)) {
        arguments.push_back("output." + name + ".disable");
      }
    }

    if (!execute_configuration(arguments, "Remote Monitor topology apply")) {
      return false;
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
            static_cast<int>(std::lround(output_refresh(*output))) == mode.refresh_hz;
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

  void remote_remove_owned_display(const std::string &client_uuid) {
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    manager.reservations.erase(client_reservation_identity(client_uuid));
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
    auto &manager = state();
    std::lock_guard lock {manager.mutex};
    if (!manager.snapshot) {
      manager.reservations.clear();
      return true;
    }
    const auto current = query_configuration();
    if (!current) {
      return false;
    }
    const auto arguments = restore_arguments(*manager.snapshot, *current);
    if (!execute_configuration(arguments, "restore")) {
      return false;
    }
    manager.snapshot.reset();
    manager.reservations.clear();
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
    return true;
  }

  void schedule_revert(const std::chrono::milliseconds delay) {
    auto &manager = state();
    const auto generation = manager.cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::thread([delay, generation]() {
      std::this_thread::sleep_for(delay);
      auto &delayed_manager = state();
      if (delayed_manager.cleanup_generation.load(std::memory_order_acquire) == generation) {
        BOOST_LOG(info) << "Linux private display: paused-session timeout elapsed; restoring outputs.";
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
    if (!doctor_path()) {
      return false;
    }
    for (const auto &name : configured_outputs()) {
      if (connector_is_connected(name)) {
        return true;
      }
    }
    return false;
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
        if (output.contains("hdr")) {
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
