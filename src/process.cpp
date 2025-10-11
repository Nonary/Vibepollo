/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif
// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "display_helper_integration.h"
#include "file_handler.h"
#include "logging.h"
#include "platform/common.h"
#ifdef _WIN32
  #include "config_playnite.h"
  #include "platform/windows/ipc/misc_utils.h"
  #include "platform/windows/playnite_integration.h"
#endif
#include "httpcommon.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  namespace {
    constexpr const char *LOSSLESS_PROFILE_RECOMMENDED = "recommended";
    constexpr const char *LOSSLESS_PROFILE_CUSTOM = "custom";
    constexpr int LOSSLESS_DEFAULT_FLOW_SCALE = 50;
    constexpr int LOSSLESS_DEFAULT_RESOLUTION_SCALE = 100;
    constexpr bool LOSSLESS_DEFAULT_PERFORMANCE_MODE = true;
    constexpr int LOSSLESS_MIN_FLOW_SCALE = 0;
    constexpr int LOSSLESS_MAX_FLOW_SCALE = 100;
    constexpr int LOSSLESS_MIN_RESOLUTION_SCALE = 10;
    constexpr int LOSSLESS_MAX_RESOLUTION_SCALE = 100;
    constexpr int LOSSLESS_SHARPNESS_MIN = 1;
    constexpr int LOSSLESS_SHARPNESS_MAX = 10;

    constexpr const char *ENV_LOSSLESS_PROFILE = "SUNSHINE_LOSSLESS_SCALING_ACTIVE_PROFILE";
    constexpr const char *ENV_LOSSLESS_CAPTURE_API = "SUNSHINE_LOSSLESS_SCALING_CAPTURE_API";
    constexpr const char *ENV_LOSSLESS_QUEUE_TARGET = "SUNSHINE_LOSSLESS_SCALING_QUEUE_TARGET";
    constexpr const char *ENV_LOSSLESS_HDR = "SUNSHINE_LOSSLESS_SCALING_HDR";
    constexpr const char *ENV_LOSSLESS_FLOW_SCALE = "SUNSHINE_LOSSLESS_SCALING_FLOW_SCALE";
    constexpr const char *ENV_LOSSLESS_PERFORMANCE_MODE = "SUNSHINE_LOSSLESS_SCALING_PERFORMANCE_MODE";
    constexpr const char *ENV_LOSSLESS_RESOLUTION = "SUNSHINE_LOSSLESS_SCALING_RESOLUTION_SCALE";
    constexpr const char *ENV_LOSSLESS_FRAMEGEN_MODE = "SUNSHINE_LOSSLESS_SCALING_FRAMEGEN_MODE";
    constexpr const char *ENV_LOSSLESS_LSFG3_MODE = "SUNSHINE_LOSSLESS_SCALING_LSFG3_MODE";
    constexpr const char *ENV_LOSSLESS_SCALING_TYPE = "SUNSHINE_LOSSLESS_SCALING_SCALING_TYPE";
    constexpr const char *ENV_LOSSLESS_SHARPNESS = "SUNSHINE_LOSSLESS_SCALING_SHARPNESS";
    constexpr const char *ENV_LOSSLESS_LS1_SHARPNESS = "SUNSHINE_LOSSLESS_SCALING_LS1_SHARPNESS";
    constexpr const char *ENV_LOSSLESS_ANIME4K_TYPE = "SUNSHINE_LOSSLESS_SCALING_ANIME4K_TYPE";
    constexpr const char *ENV_LOSSLESS_ANIME4K_VRS = "SUNSHINE_LOSSLESS_SCALING_ANIME4K_VRS";

    std::string normalize_frame_generation_provider(const std::string &value) {
      std::string normalized;
      normalized.reserve(value.size());
      for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
      }
      if (normalized == "nvidia" || normalized == "smoothmotion" || normalized == "nvidiasmoothmotion") {
        return "nvidia-smooth-motion";
      }
      if (normalized == "lossless" || normalized == "losslessscaling") {
        return "lossless-scaling";
      }
      return "lossless-scaling";
    }

    struct lossless_profile_defaults_t {
      bool performance_mode;
      int flow_scale;
      int resolution_scale;
      std::string scaling_mode;
      int sharpening;
      std::string anime4k_size;
      bool anime4k_vrs;
    };

    const lossless_profile_defaults_t LOSSLESS_DEFAULTS_RECOMMENDED {
      true,
      LOSSLESS_DEFAULT_FLOW_SCALE,
      LOSSLESS_DEFAULT_RESOLUTION_SCALE,
      "off",
      5,
      "S",
      false,
    };

    const lossless_profile_defaults_t LOSSLESS_DEFAULTS_CUSTOM {
      false,
      LOSSLESS_DEFAULT_FLOW_SCALE,
      LOSSLESS_DEFAULT_RESOLUTION_SCALE,
      "off",
      5,
      "S",
      false,
    };

    const std::array<std::string, 11> &lossless_scaling_modes() {
      static const std::array<std::string, 11> modes {
        "off",
        "ls1",
        "fsr",
        "nis",
        "sgsr",
        "bcas",
        "anime4k",
        "xbr",
        "sharp-bilinear",
        "integer",
        "nearest"
      };
      return modes;
    }

    std::optional<std::string> normalize_scaling_mode(const std::string &value) {
      std::string lower = boost::algorithm::to_lower_copy(value);
      const auto &modes = lossless_scaling_modes();
      if (std::find(modes.begin(), modes.end(), lower) != modes.end()) {
        return lower;
      }
      return std::nullopt;
    }

    bool scaling_mode_requires_sharpening(const std::string &mode) {
      static const std::array<std::string, 4> sharpening_modes {"ls1", "fsr", "nis", "sgsr"};
      return std::find(sharpening_modes.begin(), sharpening_modes.end(), mode) != sharpening_modes.end();
    }

    bool scaling_mode_is_anime(const std::string &mode) {
      return mode == "anime4k";
    }

    std::optional<std::string> scaling_mode_to_lossless_value(const std::string &mode) {
      if (mode == "off") {
        return std::string("Off");
      }
      if (mode == "ls1") {
        return std::string("LS1");
      }
      if (mode == "fsr") {
        return std::string("FSR");
      }
      if (mode == "nis") {
        return std::string("NIS");
      }
      if (mode == "sgsr") {
        return std::string("SGSR");
      }
      if (mode == "bcas") {
        return std::string("BicubicCAS");
      }
      if (mode == "anime4k") {
        return std::string("Anime4k");
      }
      if (mode == "xbr") {
        return std::string("XBR");
      }
      if (mode == "sharp-bilinear") {
        return std::string("SharpBilinear");
      }
      if (mode == "integer") {
        return std::string("Integer");
      }
      if (mode == "nearest") {
        return std::string("Nearest");
      }
      return std::nullopt;
    }

    int clamp_sharpness(int value) {
      return std::clamp(value, LOSSLESS_SHARPNESS_MIN, LOSSLESS_SHARPNESS_MAX);
    }

    struct lossless_runtime_values_t {
      std::string profile;
      std::optional<bool> performance_mode;
      std::optional<int> flow_scale;
      std::optional<double> resolution_scale_factor;
      std::optional<std::string> capture_api;
      std::optional<int> queue_target;
      std::optional<bool> hdr_enabled;
      std::optional<std::string> frame_generation;
      std::optional<std::string> lsfg3_mode;
      std::optional<std::string> scaling_type;
      std::optional<int> sharpness;
      std::optional<int> ls1_sharpness;
      std::optional<std::string> anime4k_type;
      std::optional<bool> anime4k_vrs;
    };

    std::optional<bool> pt_get_optional_bool(const pt::ptree &node, const std::string &key) {
      auto child = node.get_child_optional(key);
      if (!child) {
        return std::nullopt;
      }
      try {
        return child->get_value<bool>();
      } catch (...) {
      }
      try {
        auto text = child->get_value<std::string>();
        if (text.empty()) {
          return std::nullopt;
        }
        if (boost::iequals(text, "true") || text == "1") {
          return true;
        }
        if (boost::iequals(text, "false") || text == "0") {
          return false;
        }
      } catch (...) {
      }
      return std::nullopt;
    }

    std::optional<int> pt_get_optional_int(const pt::ptree &node, const std::string &key) {
      auto child = node.get_child_optional(key);
      if (!child) {
        return std::nullopt;
      }
      try {
        return child->get_value<int>();
      } catch (...) {
      }
      try {
        auto text = child->get_value<std::string>();
        if (text.empty()) {
          return std::nullopt;
        }
        return std::stoi(text);
      } catch (...) {
      }
      return std::nullopt;
    }

    void populate_lossless_overrides(const pt::ptree &node, lossless_scaling_profile_overrides_t &target) {
      if (auto perf = pt_get_optional_bool(node, "performance-mode")) {
        target.performance_mode = *perf;
      }
      if (auto flow = pt_get_optional_int(node, "flow-scale")) {
        target.flow_scale = *flow;
      }
      if (auto res = pt_get_optional_int(node, "resolution-scale")) {
        target.resolution_scale = *res;
      }
      if (auto scaling = node.get_optional<std::string>("scaling-type")) {
        if (auto normalized = normalize_scaling_mode(*scaling)) {
          target.scaling_type = *normalized;
        }
      }
      if (auto sharp = pt_get_optional_int(node, "sharpening")) {
        target.sharpening = clamp_sharpness(*sharp);
      }
      if (auto anime = node.get_optional<std::string>("anime4k-size")) {
        std::string value = boost::algorithm::to_upper_copy(*anime);
        target.anime4k_size = std::move(value);
      }
      if (auto vrs = pt_get_optional_bool(node, "anime4k-vrs")) {
        target.anime4k_vrs = *vrs;
      }
    }

    lossless_runtime_values_t compute_lossless_runtime(const ctx_t &ctx) {
      lossless_runtime_values_t result;
      const lossless_profile_defaults_t &defaults = boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED) ?
                                                      LOSSLESS_DEFAULTS_RECOMMENDED :
                                                      LOSSLESS_DEFAULTS_CUSTOM;

      const lossless_scaling_profile_overrides_t &overrides = boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED) ?
                                                                ctx.lossless_scaling_recommended :
                                                                ctx.lossless_scaling_custom;

      if (boost::iequals(ctx.lossless_scaling_profile, LOSSLESS_PROFILE_RECOMMENDED)) {
        result.profile = LOSSLESS_PROFILE_RECOMMENDED;
        result.capture_api = "WGC";
        result.queue_target = 0;
        result.hdr_enabled = true;
        result.frame_generation = "LSFG3";
        result.lsfg3_mode = "ADAPTIVE";
      } else {
        result.profile = LOSSLESS_PROFILE_CUSTOM;
      }

      bool performance_mode = overrides.performance_mode.value_or(defaults.performance_mode);
      result.performance_mode = performance_mode;

      int flow_scale = overrides.flow_scale.value_or(defaults.flow_scale);
      flow_scale = std::clamp(flow_scale, LOSSLESS_MIN_FLOW_SCALE, LOSSLESS_MAX_FLOW_SCALE);
      result.flow_scale = flow_scale;

      std::string scaling_mode = overrides.scaling_type.has_value() ? *overrides.scaling_type : defaults.scaling_mode;
      auto normalized_mode = normalize_scaling_mode(scaling_mode);
      if (!normalized_mode) {
        normalized_mode = defaults.scaling_mode;
      }

      // Only apply resolution scaling if scaling type is not 'off'
      if (*normalized_mode != "off") {
        int resolution_scale = overrides.resolution_scale.value_or(defaults.resolution_scale);
        resolution_scale = std::clamp(resolution_scale, LOSSLESS_MIN_RESOLUTION_SCALE, LOSSLESS_MAX_RESOLUTION_SCALE);
        double factor = 100.0 / static_cast<double>(resolution_scale);
        factor = std::clamp(factor, 1.0, 10.0);
        factor = std::round(factor * 100.0) / 100.0;
        result.resolution_scale_factor = factor;
      } else {
        // When scaling is off, use unity scale factor to disable custom scaling
        result.resolution_scale_factor = 1.0;
      }

      if (auto mapped = scaling_mode_to_lossless_value(*normalized_mode)) {
        result.scaling_type = *mapped;
      }

      if (scaling_mode_requires_sharpening(*normalized_mode)) {
        int sharpness = overrides.sharpening.value_or(defaults.sharpening);
        sharpness = clamp_sharpness(sharpness);
        result.sharpness = sharpness;
        if (*normalized_mode == "ls1") {
          result.ls1_sharpness = sharpness;
        }
      }

      if (scaling_mode_is_anime(*normalized_mode)) {
        std::string anime_type = overrides.anime4k_size.has_value() ? *overrides.anime4k_size : defaults.anime4k_size;
        boost::algorithm::to_upper(anime_type);
        result.anime4k_type = anime_type;
        bool vrs = overrides.anime4k_vrs.value_or(defaults.anime4k_vrs);
        result.anime4k_vrs = vrs;
      }

      return result;
    }
  }  // namespace

  proc_t proc;

  int input_only_app_id = -1;
  std::string input_only_app_id_str;
  int terminate_app_id = -1;
  std::string terminate_app_id_str;

#ifdef _WIN32
  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
        return;
      }
    }
  }
#endif

  // Custom move operations to allow global proc replacement if ever needed
  proc_t::proc_t(proc_t &&other) noexcept:
      _app_id(other._app_id),
      _env(std::move(other._env)),
      _apps(std::move(other._apps)),
      _app(std::move(other._app)),
      _app_launch_time(other._app_launch_time),
      placebo(other.placebo),
      _process(std::move(other._process)),
      _process_group(std::move(other._process_group)),
      _pipe(std::move(other._pipe)),
      _app_prep_it(other._app_prep_it),
      _app_prep_begin(other._app_prep_begin) {
  }

  proc_t &proc_t::operator=(proc_t &&other) noexcept {
    if (this != &other) {
      std::scoped_lock lk(_apps_mutex, other._apps_mutex);
      _app_id = other._app_id;
      _env = std::move(other._env);
      _apps = std::move(other._apps);
      _app = std::move(other._app);
      _app_launch_time = other._app_launch_time;
      placebo = other.placebo;
      _process = std::move(other._process);
      _process_group = std::move(other._process_group);
      _pipe = std::move(other._pipe);
      _app_prep_it = other._app_prep_it;
      _app_prep_begin = other._app_prep_begin;
    }
    return *this;
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, const boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::launch_input_only() {
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::execute(const ctx_t &app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    if (_app_id == input_only_app_id) {
      terminate(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate(false, false);
    }

    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    allow_client_commands = app.allow_client_commands;
    launch_session->gen1_framegen_fix = _app.gen1_framegen_fix;
    launch_session->gen2_framegen_fix = _app.gen2_framegen_fix;
    launch_session->lossless_scaling_framegen = _app.lossless_scaling_framegen;
    launch_session->lossless_scaling_target_fps = _app.lossless_scaling_target_fps;
    launch_session->lossless_scaling_rtss_limit = _app.lossless_scaling_rtss_limit;
    launch_session->frame_generation_provider = _app.frame_generation_provider;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    uint32_t client_width = launch_session->width ? launch_session->width : 1920;
    uint32_t client_height = launch_session->height ? launch_session->height : 1080;

    uint32_t render_width = client_width;
    uint32_t render_height = client_height;

    int scale_factor = launch_session->scale_factor;
    if (_app.scale_factor != 100) {
      scale_factor = _app.scale_factor;
    }

    if (scale_factor != 100) {
      render_width *= ((float) scale_factor / 100);
      render_height *= ((float) scale_factor / 100);

      // Chop the last bit to ensure the scaled resolution is even numbered
      // Most odd resolutions won't work well
      render_width &= ~1;
      render_height &= ~1;
    }

    launch_session->width = render_width;
    launch_session->height = render_height;

    this->initial_display = config::video.output_name;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      // Restore to user defined output name
      config::video.output_name = this->initial_display;
      terminate();
#ifdef _WIN32
      display_helper_integration::revert();
#endif
    });

    if (!app.gamepad.empty()) {
      _saved_input_config = std::make_shared<config::input_t>(config::input);
      if (app.gamepad == "disabled") {
        config::input.controller = false;
      } else {
        config::input.controller = true;
        config::input.gamepad = app.gamepad;
      }
    }

#ifdef _WIN32
    using dd_config_option_e = config::video_t::dd_t::config_option_e;
    const auto dd_config_option = config::video.dd.configuration_option;
    const bool forced_sudavda_virtual_display = config::video.output_name == VDISPLAY::SUDOVDA_VIRTUAL_DISPLAY_SELECTION;
    const bool dd_conflicts_with_virtual_display =
      dd_config_option == dd_config_option_e::ensure_only_display &&
      dd_config_option != dd_config_option_e::disabled &&
      !config::video.headless_mode;

    if (forced_sudavda_virtual_display) {
      launch_session->virtual_display = true;
    }

    bool should_use_virtual_display =
      config::video.headless_mode  // Headless mode
      || launch_session->virtual_display  // User requested virtual display
      || _app.virtual_display  // App is configured to use virtual display
      || !video::allow_encoder_probing();  // No active display presents

    if (should_use_virtual_display && dd_conflicts_with_virtual_display && !forced_sudavda_virtual_display) {
      if (launch_session->virtual_display || _app.virtual_display) {
        BOOST_LOG(info) << "Skipping virtual display activation because display device configuration is set to ensure-only-display.";
      }
      launch_session->virtual_display = false;
      should_use_virtual_display =
        config::video.headless_mode  // Headless mode still forces virtual display
        || !video::allow_encoder_probing();  // No active display presents
    }

    // Try DD API first if enabled and not in headless mode
    bool dd_api_handled = false;
    if (!forced_sudavda_virtual_display && (!should_use_virtual_display || (dd_config_option != dd_config_option_e::disabled && !config::video.headless_mode))) {
      dd_api_handled = display_helper_integration::apply_from_session(config::video, *launch_session);
      if (dd_api_handled) {
        const bool virtual_display_requested = launch_session->virtual_display || _app.virtual_display;
        const bool still_missing_active_display = !video::allow_encoder_probing();

        if (!virtual_display_requested && !still_missing_active_display) {
          BOOST_LOG(info) << "Display configuration handled by DD API, skipping SudoVDA virtual display.";
          should_use_virtual_display = false;
        } else {
          BOOST_LOG(info) << "Display configuration handled by DD API but virtual display support remains required; keeping SudoVDA virtual display active.";
        }
      }
    }

    // Use SudoVDA as fallback only if DD API didn't handle it or is disabled
    if (should_use_virtual_display && !dd_api_handled) {
      if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        // Try init driver again
        initVDisplayDriver();
      }

      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        // Try set the render adapter matching the capture adapter if user has specified one
        if (!config::video.adapter_name.empty()) {
          VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
        }

        std::string device_name;
        std::string device_uuid_str;
        uuid_util::uuid_t device_uuid;

        if (_app.use_app_identity) {
          device_name = _app.name;
          if (_app.per_client_app_identity) {
            device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);

            // Use XOR to mix the two UUIDs
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];

            device_uuid_str = device_uuid.string();
          } else {
            device_uuid_str = _app.uuid;
            device_uuid = uuid_util::uuid_t::parse(_app.uuid);
          }
        } else {
          device_name = launch_session->device_name;
          device_uuid_str = launch_session->unique_id;
          device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        }

        memcpy(&launch_session->display_guid, &device_uuid, sizeof(GUID));

        int target_fps = launch_session->fps ? launch_session->fps : 60000;

        if (target_fps < 1000) {
          target_fps *= 1000;
        }

        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

        std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(
          device_uuid_str.c_str(),
          device_name.c_str(),
          render_width,
          render_height,
          target_fps,
          launch_session->display_guid
        );

        // No matter we get the display name or not, the virtual display might still be created.
        // We need to track it properly to remove the display when the session terminates.
        launch_session->virtual_display = true;

        if (!vdisplayName.empty()) {
          BOOST_LOG(info) << "Virtual Display created at " << vdisplayName;

          // Don't change display settings when no params are given
          if (launch_session->width && launch_session->height && launch_session->fps) {
            // Apply display settings
            VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), render_width, render_height, target_fps);
          }

          // Check the ISOLATED DISPLAY configuration setting and rearrange the displays
          if (config::video.isolated_virtual_display_option == true) {
            // Apply the isolated display settings
            VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), render_width, render_height, target_fps, true);
          }

          // Set virtual_display to true when everything went fine
          this->virtual_display = true;
          this->display_name = platf::to_utf8(vdisplayName);

          // When using virtual display, we don't care which display user configured to use.
          // So we always set output_name to the newly created virtual display as a workaround for
          // empty name when probing graphics cards.

          config::video.output_name = this->display_name;
        } else {
          BOOST_LOG(warning) << "Virtual Display creation failed, or cannot get created display name in time!";
        }
      } else {
        // Driver isn't working so we don't need to track virtual display.
        launch_session->virtual_display = false;
      }
    }

    // Call DD API again if it wasn't already called above
    if (!dd_api_handled && !this->virtual_display) {
      display_helper_integration::apply_from_session(config::video, *launch_session);
    }

    // We should not preserve display state when using virtual display.
    // It is already handled by Windows properly.
    if (this->virtual_display) {
      display_helper_integration::reset_persistence();
    }

#endif  // _WIN32

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
    if (rtsp_stream::session_count() == 0 && video::probe_encoders()) {
      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration.";
      } else {
        return 503;
      }
    }

    std::string fps_str;
    char fps_buf[8];
    snprintf(fps_buf, sizeof(fps_buf), "%.3f", (float) launch_session->fps / 1000.0f);
    fps_str = fps_buf;
    const std::string fps_scaled_str = std::to_string(launch_session->fps);

    // Add Stream-specific environment variables
    // Sunshine Compatibility
    _env["SUNSHINE_APP_ID"] = _app.id;
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["SUNSHINE_CLIENT_FPS"] = config::sunshine.envvar_compatibility_mode ? std::to_string(std::round((float) launch_session->fps / 1000.0f)) : fps_str;
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    _env["APOLLO_APP_ID"] = _app.id;
    _env["APOLLO_APP_NAME"] = _app.name;
    _env["APOLLO_APP_UUID"] = _app.uuid;
    _env["APOLLO_APP_STATUS"] = "STARTING";
    _env["APOLLO_CLIENT_UUID"] = launch_session->unique_id;
    _env["APOLLO_CLIENT_NAME"] = launch_session->device_name;
    _env["APOLLO_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["APOLLO_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["APOLLO_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["APOLLO_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["APOLLO_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["APOLLO_CLIENT_FPS"] = fps_scaled_str;
    _env["APOLLO_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["APOLLO_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["APOLLO_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["APOLLO_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        _env["APOLLO_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    _env["APOLLO_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    try {
      _env["SUNSHINE_LOSSLESS_SCALING_EXE"] = config::lossless_scaling.exe_path;
    } catch (...) {
      _env["SUNSHINE_LOSSLESS_SCALING_EXE"] = "";
    }

    auto clear_lossless_runtime_env = [&]() {
      _env[ENV_LOSSLESS_PROFILE] = "";
      _env[ENV_LOSSLESS_CAPTURE_API] = "";
      _env[ENV_LOSSLESS_QUEUE_TARGET] = "";
      _env[ENV_LOSSLESS_HDR] = "";
      _env[ENV_LOSSLESS_FLOW_SCALE] = "";
      _env[ENV_LOSSLESS_PERFORMANCE_MODE] = "";
      _env[ENV_LOSSLESS_RESOLUTION] = "";
      _env[ENV_LOSSLESS_FRAMEGEN_MODE] = "";
      _env[ENV_LOSSLESS_LSFG3_MODE] = "";
      _env[ENV_LOSSLESS_SCALING_TYPE] = "";
      _env[ENV_LOSSLESS_SHARPNESS] = "";
      _env[ENV_LOSSLESS_LS1_SHARPNESS] = "";
      _env[ENV_LOSSLESS_ANIME4K_TYPE] = "";
      _env[ENV_LOSSLESS_ANIME4K_VRS] = "";
    };

    _env["SUNSHINE_FRAME_GENERATION_PROVIDER"] = _app.lossless_scaling_framegen ? _app.frame_generation_provider : "";

    const bool using_lossless_provider = _app.lossless_scaling_framegen &&
                                         boost::iequals(_app.frame_generation_provider, "lossless-scaling");
    if (using_lossless_provider) {
      _env["SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"] = "1";
      if (_app.lossless_scaling_target_fps) {
        _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = std::to_string(*_app.lossless_scaling_target_fps);
      } else {
        _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = "";
      }
      if (_app.lossless_scaling_rtss_limit) {
        _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = std::to_string(*_app.lossless_scaling_rtss_limit);
      } else {
        _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = "";
      }

      auto runtime = compute_lossless_runtime(_app);
      auto set_string = [&](const char *key, const std::optional<std::string> &value) {
        if (value && !value->empty()) {
          _env[key] = *value;
        } else {
          _env[key] = "";
        }
      };
      auto set_int = [&](const char *key, const std::optional<int> &value) {
        if (value.has_value()) {
          _env[key] = std::to_string(*value);
        } else {
          _env[key] = "";
        }
      };
      auto set_double = [&](const char *key, const std::optional<double> &value) {
        if (value.has_value()) {
          std::ostringstream stream;
          stream.setf(std::ios::fixed);
          stream << std::setprecision(2) << *value;
          _env[key] = stream.str();
        } else {
          _env[key] = "";
        }
      };
      auto set_bool = [&](const char *key, const std::optional<bool> &value) {
        if (value.has_value()) {
          _env[key] = *value ? "1" : "0";
        } else {
          _env[key] = "";
        }
      };

      _env[ENV_LOSSLESS_PROFILE] = runtime.profile;
      set_string(ENV_LOSSLESS_CAPTURE_API, runtime.capture_api);
      set_int(ENV_LOSSLESS_QUEUE_TARGET, runtime.queue_target);
      set_bool(ENV_LOSSLESS_HDR, runtime.hdr_enabled);
      set_int(ENV_LOSSLESS_FLOW_SCALE, runtime.flow_scale);
      set_bool(ENV_LOSSLESS_PERFORMANCE_MODE, runtime.performance_mode);
      set_double(ENV_LOSSLESS_RESOLUTION, runtime.resolution_scale_factor);
      set_string(ENV_LOSSLESS_FRAMEGEN_MODE, runtime.frame_generation);
      set_string(ENV_LOSSLESS_LSFG3_MODE, runtime.lsfg3_mode);
      set_string(ENV_LOSSLESS_SCALING_TYPE, runtime.scaling_type);
      set_int(ENV_LOSSLESS_SHARPNESS, runtime.sharpness);
      set_int(ENV_LOSSLESS_LS1_SHARPNESS, runtime.ls1_sharpness);
      set_string(ENV_LOSSLESS_ANIME4K_TYPE, runtime.anime4k_type);
      set_bool(ENV_LOSSLESS_ANIME4K_VRS, runtime.anime4k_vrs);
    } else {
      _env["SUNSHINE_LOSSLESS_SCALING_FRAMEGEN"] = "";
      _env["SUNSHINE_LOSSLESS_SCALING_TARGET_FPS"] = "";
      _env["SUNSHINE_LOSSLESS_SCALING_RTSS_LIMIT"] = "";
      clear_lossless_runtime_env();
    }

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0 && ec != std::errc::permission_denied) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
        return -1;
      }
    }

    _env["APOLLO_APP_STATUS"] = "RUNNING";

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    // Playnite-backed apps: invoke via Playnite and treat as placebo (lifetime managed via Playnite status)
#ifdef _WIN32
    if (!_app.playnite_id.empty() && _app.cmd.empty()) {
      // Auto-update Playnite plugin if an update is available
      try {
        std::string installed_ver, packaged_ver;
        bool have_installed = platf::playnite::get_installed_plugin_version(installed_ver);
        bool have_packaged = platf::playnite::get_packaged_plugin_version(packaged_ver);

        if (have_installed && have_packaged) {
          // Simple version comparison: compare as strings (works for semantic versioning)
          auto normalize_ver = [](std::string s) -> std::string {
            // Strip leading 'v' if present
            if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) {
              s = s.substr(1);
            }
            // Remove whitespace
            s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
            return s;
          };

          std::string installed_normalized = normalize_ver(installed_ver);
          std::string packaged_normalized = normalize_ver(packaged_ver);

          if (installed_normalized < packaged_normalized) {
            BOOST_LOG(info) << "Playnite plugin update available (" << installed_ver
                            << " -> " << packaged_ver << "), auto-updating before launch";
            std::string install_error;
            if (platf::playnite::install_plugin(install_error)) {
              BOOST_LOG(info) << "Playnite plugin auto-update succeeded";
            } else {
              BOOST_LOG(warning) << "Playnite plugin auto-update failed: " << install_error
                                 << " (continuing with game launch)";
            }
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Exception during Playnite plugin auto-update check: " << e.what()
                           << " (continuing with game launch)";
      } catch (...) {
        BOOST_LOG(warning) << "Unknown exception during Playnite plugin auto-update check (continuing with game launch)";
      }

      BOOST_LOG(info) << "Launching Playnite game via helper, id=" << _app.playnite_id;
      bool launched = false;
      // Resolve launcher alongside sunshine.exe: tools\\playnite-launcher.exe
      try {
        WCHAR exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, ARRAYSIZE(exePathW));
        std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
        std::filesystem::path launcher = exeDir / L"tools" / L"playnite-launcher.exe";
        std::string lpath = launcher.string();
        std::string cmd = std::string("\"") + lpath + "\" --game-id " + _app.playnite_id;
        // Pass graceful-exit timeout to launcher for cleanup behavior
        try {
          int exit_to = (int) std::max<std::int64_t>(0, _app.exit_timeout.count());
          if (exit_to > 0) {
            cmd += std::string(" --exit-timeout ") + std::to_string(exit_to);
          }
        } catch (...) {}
        // Pass focus attempts from config so the helper can try to bring Playnite/game to foreground
        try {
          if (config::playnite.focus_attempts > 0) {
            cmd += std::string(" --focus-attempts ") + std::to_string(config::playnite.focus_attempts);
          }
          if (config::playnite.focus_timeout_secs > 0) {
            cmd += std::string(" --focus-timeout ") + std::to_string(config::playnite.focus_timeout_secs);
          }
          if (config::playnite.focus_exit_on_first) {
            cmd += std::string(" --focus-exit-on-first");
          }
        } catch (...) {}
        std::error_code fec;
        boost::filesystem::path wd;  // empty wd
        _process = platf::run_command(false, true, cmd, wd, _env, _pipe.get(), fec, &_process_group);
        if (fec) {
          BOOST_LOG(warning) << "Playnite helper launch failed: "sv << fec.message() << "; attempting URI fallback"sv;
        } else {
          BOOST_LOG(info) << "Playnite helper launched and is being monitored";
          try {
            auto pid = static_cast<uint32_t>(_process.id());
            if (!platf::playnite::announce_launcher(pid, _app.playnite_id)) {
              BOOST_LOG(debug) << "Playnite helper: announce_launcher reported inactive IPC";
            }
          } catch (...) {
          }
          launched = true;
        }
      } catch (...) {
        launched = false;
      }
      if (!launched) {
        // Best-effort fallback using Playnite URI protocol
        std::string uri = std::string("playnite://playnite/start/") + _app.playnite_id;
        std::error_code fec;
        boost::filesystem::path wd;  // empty working dir as lvalue
        auto child = platf::run_command(false, true, std::string("cmd /c start \"\" \"") + uri + "\"", wd, _env, _pipe.get(), fec, nullptr);
        if (fec) {
          BOOST_LOG(warning) << "Playnite URI launch failed: "sv << fec.message();
        } else {
          BOOST_LOG(info) << "Playnite URI launch started";
          child.detach();
          launched = true;
        }
      }
      if (!launched) {
        BOOST_LOG(error) << "Failed to launch Playnite game."sv;
        return -1;
      }
      // Track the helper process; when it exits, Sunshine will terminate the stream automatically
      placebo = false;
    } else
#endif
#ifdef _WIN32
      if (_app.playnite_fullscreen) {
      BOOST_LOG(info) << "Launching Playnite in fullscreen via helper";
      bool launched = false;
      try {
        WCHAR exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, ARRAYSIZE(exePathW));
        std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();
        std::filesystem::path launcher = exeDir / L"tools" / L"playnite-launcher.exe";
        std::string lpath = launcher.string();
        std::string cmd = std::string("\"") + lpath + "\" --fullscreen";
        try {
          if (config::playnite.focus_attempts > 0) {
            cmd += std::string(" --focus-attempts ") + std::to_string(config::playnite.focus_attempts);
          }
          if (config::playnite.focus_timeout_secs > 0) {
            cmd += std::string(" --focus-timeout ") + std::to_string(config::playnite.focus_timeout_secs);
          }
          if (config::playnite.focus_exit_on_first) {
            cmd += std::string(" --focus-exit-on-first");
          }
        } catch (...) {}
        std::error_code fec;
        boost::filesystem::path wd;  // empty wd
        _process = platf::run_command(false, true, cmd, wd, _env, _pipe.get(), fec, &_process_group);
        if (fec) {
          BOOST_LOG(warning) << "Playnite fullscreen helper launch failed: "sv << fec.message();
        } else {
          BOOST_LOG(info) << "Playnite fullscreen helper launched";
          try {
            auto pid = static_cast<uint32_t>(_process.id());
            if (!platf::playnite::announce_launcher(pid, std::string())) {
              BOOST_LOG(debug) << "Playnite helper (fullscreen): announce_launcher reported inactive IPC";
            }
          } catch (...) {
          }
          launched = true;
        }
      } catch (...) {
        launched = false;
      }
      if (!launched) {
        BOOST_LOG(error) << "Failed to launch Playnite fullscreen."sv;
        return -1;
      }
      placebo = false;
    } else
#endif
      if (_app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      BOOST_LOG(info) << "Playnite launch path complete; treating app as placebo (status-driven).";
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

#ifdef _WIN32
    auto resetHDRThread = std::thread([this, enable_hdr = launch_session->enable_hdr] {
      // Windows doesn't seem to be able to set HDR correctly when a display is just connected / changed resolution,
      // so we have tooggle HDR for the virtual display manually after a delay.
      auto retryInterval = 200ms;
      while (is_changing_settings_going_to_fail()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Restoring HDR settings failed due to retry timeout!";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      retryInterval = 200ms;
      while (this->display_name.empty()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Not getting current display in time! HDR will not be toggled.";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      // We should have got the actual streaming display by now
      std::string currentDisplay = this->display_name;
      auto currentDisplayW = platf::from_utf8(currentDisplay);

      initial_hdr = VDISPLAY::getDisplayHDRByName(currentDisplayW.c_str());

      if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
        mode_changed_display = currentDisplay;

        // Try turn off HDR whatever
        // As we always have to apply the workaround by turining off HDR first
        VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false);

        if (enable_hdr) {
          if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
            BOOST_LOG(info) << "HDR enabled for display " << currentDisplay;
          } else {
            BOOST_LOG(info) << "HDR enable failed for display " << currentDisplay;
          }
        }
      } else if (initial_hdr) {
        if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false) && VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
          BOOST_LOG(info) << "HDR toggled successfully for display " << currentDisplay;
        } else {
          BOOST_LOG(info) << "HDR toggle failed for display " << currentDisplay;
        }
      }
    });

    resetHDRThread.detach();
#endif

    fg.disable();

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_playing(_app.name);
#endif

    return 0;
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << "] within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      BOOST_LOG(info) << "Playnite launch path complete; treating app as placebo (status-driven).";
      placebo = true;

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      if (_process.native_exit_code() != 0) {
        system_tray::update_tray_launch_error(proc::proc.get_last_run_app_name(), _process.native_exit_code());
      }
#endif

      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      terminate();
    }

    return 0;
  }

  void proc_t::resume() {
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["APOLLO_APP_STATUS"] = "RESUMING";

        std::error_code ec;
        auto _state_resume_it = std::begin(cmd_list);

        for (; _state_resume_it != std::end(cmd_list); ++_state_resume_it) {
          auto &cmd = *_state_resume_it;

          // Skip empty commands
          if (cmd.do_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.do_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Resume Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }
  }

  void proc_t::pause() {
    if (!running()) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      BOOST_LOG(info) << "Terminating app [" << _app_name << "] when all clients are disconnected. Pause commands are skipped.";
      terminate();
      return;
    }

    BOOST_LOG(info) << "Session pausing for app [" << _app_name << "].";

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["APOLLO_APP_STATUS"] = "PAUSING";

        std::error_code ec;
        auto _state_pause_it = std::begin(cmd_list);

        for (; _state_pause_it != std::end(cmd_list); ++_state_pause_it) {
          auto &cmd = *_state_pause_it;

          // Skip empty commands
          if (cmd.undo_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.undo_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Pause Cmd: ["sv << cmd.undo_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.undo_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.undo_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
  }

  void proc_t::terminate(bool immediate, bool needs_refresh) {
    std::error_code ec;
    placebo = false;
    // For Playnite-managed apps, request a graceful stop via Playnite first
#ifdef _WIN32
    std::chrono::seconds remaining_timeout = _app.exit_timeout;
    if (!_app.playnite_id.empty()) {
      try {
        // Ask Playnite to stop the game; then wait up to exit-timeout to let it close
        platf::playnite::stop_game(_app.playnite_id);
        while (remaining_timeout.count() > 0 && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
          std::this_thread::sleep_for(1s);
          remaining_timeout -= 1s;
        }
      } catch (...) {}
    }
#endif
    // Regardless, ensure process group is terminated (graceful then forceful with remaining timeout)
    terminate_process_group(_process, _process_group, remaining_timeout);
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    _env["APOLLO_APP_STATUS"] = "TERMINATING";

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

#ifdef _WIN32
    // Revert HDR state
    if (has_run && !mode_changed_display.empty()) {
      auto displayNameW = platf::from_utf8(mode_changed_display);
      if (VDISPLAY::setDisplayHDRByName(displayNameW.c_str(), initial_hdr)) {
        BOOST_LOG(info) << "HDR reverted for display " << mode_changed_display;
      } else {
        BOOST_LOG(info) << "HDR revert failed for display " << mode_changed_display;
      }
    }

    bool used_virtual_display = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK && _launch_session && _launch_session->virtual_display;
    if (used_virtual_display) {
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
        BOOST_LOG(info) << "Virtual Display removed successfully";
      } else if (this->virtual_display) {
        BOOST_LOG(warning) << "Virtual Display remove failed";
      } else {
        BOOST_LOG(warning) << "Virtual Display remove failed, but it seems it was not created correctly either.";
      }
    }

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      if (used_virtual_display) {
        display_helper_integration::reset_persistence();
      } else {
        display_helper_integration::revert();
      }
#else
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif
    }

    // Load the configured output_name first
    // to prevent the value being write to empty when the initial terminate happens
    if (!has_run && initial_display.empty()) {
      initial_display = config::video.output_name;
    } else {
      // Restore output name to its original value
      config::video.output_name = initial_display;

      if (config::video.dd.config_revert_on_disconnect) {
        display_helper_integration::revert();
      }
    }

    _app_id = -1;
    _app_name.clear();
    _app = {};
    display_name.clear();
    initial_display.clear();
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    if (needs_refresh) {
      refresh(config::stream.file_apps, false);
    }
  }

  std::vector<ctx_t> proc_t::get_apps() const {
    std::scoped_lock lk(_apps_mutex);
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    std::scoped_lock lk(_apps_mutex);
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    return _app.uuid;
  }

  boost::process::environment proc_t::get_env() {
    return _env;
  }

  bool proc_t::last_run_app_frame_gen_limiter_fix() const {
    return _app.frame_gen_limiter_fix;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        BOOST_LOG(info) << "Playnite URI launch started";
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path;
    if (std::filesystem::exists(full_image_path)) {
      return full_image_path.string();
    } else if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        BOOST_LOG(info) << "Playnite URI launch started";
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Migrate the applications stored in the file tree by merging in a new app.
   *
   * This function updates the application entries in *fileTree_p* using the data in *inputTree_p*.
   * If an app in the file tree does not have a UUID, one is generated and inserted.
   * If an app with the same UUID as the new app is found, it is replaced.
   * Additionally, empty keys (such as "prep-cmd" or "detached") and keys no longer needed ("launching", "index")
   * are removed from the input.
   *
   * Legacy versions of Sunshine/Apollo stored boolean and integer values as strings.
   * The following keys are converted:
   *   - Boolean keys: "exclude-global-prep-cmd", "elevated", "auto-detach", "wait-all",
   *                     "use-app-identity", "per-client-app-identity", "virtual-display"
   *   - Integer keys: "exit-timeout"
   *
   * A migration version is stored in the file tree (under "version") so that future changes can be applied.
   *
   * @param fileTree_p Pointer to the JSON object representing the file tree.
   * @param inputTree_p Pointer to the JSON object representing the new app.
   */
  void migrate_apps(nlohmann::json *fileTree_p, nlohmann::json *inputTree_p) {
    std::string new_app_uuid;

    if (inputTree_p) {
      // If the input contains a non-empty "uuid", use it; otherwise generate one.
      if (inputTree_p->contains("uuid") && !(*inputTree_p)["uuid"].get<std::string>().empty()) {
        new_app_uuid = (*inputTree_p)["uuid"].get<std::string>();
      } else {
        new_app_uuid = uuid_util::uuid_t::generate().string();
        (*inputTree_p)["uuid"] = new_app_uuid;
      }

      // Remove "prep-cmd" if empty.
      if (inputTree_p->contains("prep-cmd") && (*inputTree_p)["prep-cmd"].empty()) {
        inputTree_p->erase("prep-cmd");
      }

      // Remove "detached" if empty.
      if (inputTree_p->contains("detached") && (*inputTree_p)["detached"].empty()) {
        inputTree_p->erase("detached");
      }

      // Remove keys that are no longer needed.
      inputTree_p->erase("launching");
      inputTree_p->erase("index");
    }

    // Get the current apps array; if it doesn't exist, create one.
    nlohmann::json newApps = nlohmann::json::array();
    if (fileTree_p->contains("apps") && (*fileTree_p)["apps"].is_array()) {
      for (auto &app : (*fileTree_p)["apps"]) {
        // For apps without a UUID, generate one and remove "launching".
        if (!app.contains("uuid") || app["uuid"].get<std::string>().empty()) {
          app["uuid"] = uuid_util::uuid_t::generate().string();
          app.erase("launching");
          newApps.push_back(std::move(app));
        } else {
          // If an app with the same UUID as the new app is found, replace it.
          if (!new_app_uuid.empty() && app["uuid"].get<std::string>() == new_app_uuid) {
            newApps.push_back(*inputTree_p);
            new_app_uuid.clear();
          } else {
            newApps.push_back(std::move(app));
          }
        }
      }
    }
    // If the new app's UUID has not been merged yet, add it.
    if (!new_app_uuid.empty() && inputTree_p) {
      newApps.push_back(*inputTree_p);
    }
    (*fileTree_p)["apps"] = newApps;
  }

  void migration_v2(nlohmann::json &fileTree) {
    static const int this_version = 2;
    // Determine the current migration version (default to 1 if not present).
    int file_version = 1;
    if (fileTree.contains("version")) {
      try {
        file_version = fileTree["version"].get<int>();
      } catch (const std::exception &e) {
        BOOST_LOG(info) << "Cannot parse apps.json version, treating as v1: " << e.what();
      }
    }

    // If the version is less than this_version, perform legacy conversion.
    if (file_version < this_version) {
      BOOST_LOG(info) << "Migrating app list from v1 to v2...";
      migrate_apps(&fileTree, nullptr);

      // List of keys to convert to booleans.
      std::vector<std::string> boolean_keys = {
        "allow-client-commands",
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all",
        "use-app-identity",
        "per-client-app-identity",
        "virtual-display"
      };

      // List of keys to convert to integers.
      std::vector<std::string> integer_keys = {
        "exit-timeout",
        "scale-factor"
      };

      // Walk through each app and convert legacy string values.
      for (auto &app : fileTree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key)) {
            auto &_key = app[key];
            if (_key.is_string()) {
              std::string s = _key.get<std::string>();
              std::transform(s.begin(), s.end(), s.begin(), ::tolower);  // Normalize to lowercase for comparison
              _key = (s == "true" || s == "on" || s == "yes");
            } else if (_key.is_array()) {
              // Check if the array contains at least one item and interpret the first element
              if (!_key.empty() && _key[0].is_string()) {
                std::string first = _key[0].get<std::string>();
                std::transform(first.begin(), first.end(), first.begin(), ::tolower);  // Normalize
                if (first == "on" || first == "true" || first == "yes") {
                  _key = true;
                } else if (first == "off" || first == "false" || first == "no") {
                  _key = false;
                } else {
                  _key = false;  // Default for unknown values
                }
              } else {
                _key = false;  // Treat empty arrays or non-string first elements as false
              }
            } else {
              // Fallback: Treat truthy/falsey cases
              if (_key.is_boolean()) {
                // Leave booleans as they are
              } else if (_key.is_number()) {
                _key = (_key.get<double>() != 0);  // Non-zero numbers are truthy
              } else if (_key.is_null()) {
                _key = false;  // Null is false
              } else {
                _key = !_key.empty();  // Non-empty objects/arrays are truthy, empty ones are falsey
              }
            }
          }
        }

        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            std::string s = app[key].get<std::string>();
            app[key] = std::stoi(s);
          }
        }

        // For each entry in the "prep-cmd" array, convert "elevated" if necessary.
        if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              std::string s = prep["elevated"].get<std::string>();
              prep["elevated"] = (s == "true");
            }
          }
        }
      }

      // Update migration version to this_version.
      fileTree["version"] = this_version;

      BOOST_LOG(info) << "Migrated app list from v1 to v2.";
    }
  }

  void migrate(nlohmann::json &fileTree, const std::string &fileName) {
    int last_version = 2;

    int file_version = 0;
    if (fileTree.contains("version")) {
      file_version = fileTree["version"].get<int>();
    }

    if (file_version < last_version) {
      migration_v2(fileTree);
      file_handler::write_file(fileName.c_str(), fileTree.dump(4));
    }
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {
    // Prepare environment variables.
    auto this_env = boost::this_process::environment();

    std::set<std::string> ids;
    std::vector<proc::ctx_t> apps;
    int i = 0;

    size_t fail_count = 0;
    do {
      // Read the JSON file into a tree.
      nlohmann::json tree;
      try {
        std::string content = file_handler::read_file(file_name.c_str());
        tree = nlohmann::json::parse(content);
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Couldn't read apps.json properly! Apps will not be loaded."sv;
        break;
      }

      try {
        migrate(tree, file_name);

        if (tree.contains("env") && tree["env"].is_object()) {
          for (auto &item : tree["env"].items()) {
            this_env[item.key()] = parse_env_val(this_env, item.value().get<std::string>());
          }
        }

        // Ensure the "apps" array exists.
        if (!tree.contains("apps") || !tree["apps"].is_array()) {
          BOOST_LOG(warning) << "No apps were defined in apps.json!!!"sv;
          break;
        }

        // Iterate over each application in the "apps" array.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t ctx {};
          ctx.idx = std::to_string(i);
          ctx.uuid = app_node.at("uuid");

          // Build the list of preparation commands.
          std::vector<proc::cmd_t> prep_cmds;
          bool exclude_global_prep = app_node.value("exclude-global-prep-cmd", false);
          if (!exclude_global_prep) {
            prep_cmds.reserve(config::sunshine.prep_cmds.size());
            for (auto &prep_cmd : config::sunshine.prep_cmds) {
              auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(prep_cmd.elevated)
              );
            }
          }
          if (app_node.contains("prep-cmd") && app_node["prep-cmd"].is_array()) {
            for (auto &prep_node : app_node["prep-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of pause/resume commands.
          std::vector<proc::cmd_t> state_cmds;
          bool exclude_global_state_cmds = app_node.value("exclude-global-state-cmd", false);
          if (!exclude_global_state_cmds) {
            state_cmds.reserve(config::sunshine.state_cmds.size());
            for (auto &state_cmd : config::sunshine.state_cmds) {
              auto do_cmd = parse_env_val(this_env, state_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, state_cmd.undo_cmd);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(state_cmd.elevated)
              );
            }
          }
          if (app_node.contains("state-cmd") && app_node["state-cmd"].is_array()) {
            for (auto &prep_node : app_node["state-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of detached commands.
          std::vector<std::string> detached;
          if (app_node.contains("detached") && app_node["detached"].is_array()) {
            for (auto &detached_val : app_node["detached"]) {
              detached.emplace_back(parse_env_val(this_env, detached_val.get<std::string>()));
            }
          }

          // Process other fields.
          if (app_node.contains("output")) {
            ctx.output = parse_env_val(this_env, app_node.value("output", ""));
          }
          std::string name = parse_env_val(this_env, app_node.value("name", ""));
          if (app_node.contains("cmd")) {
            ctx.cmd = parse_env_val(this_env, app_node.value("cmd", ""));
          }
          if (app_node.contains("working-dir")) {
            ctx.working_dir = parse_env_val(this_env, app_node.value("working-dir", ""));
#ifdef _WIN32
            // The working directory, unlike the command itself, should not be quoted.
            boost::erase_all(ctx.working_dir, "\"");
            ctx.working_dir += '\\';
#endif
          }
          if (app_node.contains("image-path")) {
            ctx.image_path = parse_env_val(this_env, app_node.value("image-path", ""));
          }

          ctx.elevated = app_node.value("elevated", false);
          ctx.auto_detach = app_node.value("auto-detach", true);
          ctx.wait_all = app_node.value("wait-all", true);
          ctx.exit_timeout = std::chrono::seconds {app_node.value("exit-timeout", 5)};
          ctx.virtual_display = app_node.value("virtual-display", false);
          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.gamepad = app_node.value("gamepad", "");

          ctx.playnite_id.clear();
          if (app_node.contains("playnite-id") && app_node["playnite-id"].is_string()) {
            try {
              ctx.playnite_id = parse_env_val(this_env, app_node["playnite-id"].get<std::string>());
            } catch (...) {
              ctx.playnite_id.clear();
            }
          }
          ctx.playnite_fullscreen = false;
          if (app_node.contains("playnite-fullscreen")) {
            try {
              const auto &flag = app_node["playnite-fullscreen"];
              if (flag.is_boolean()) {
                ctx.playnite_fullscreen = flag.get<bool>();
              } else if (flag.is_number_integer()) {
                ctx.playnite_fullscreen = flag.get<int>() != 0;
              } else if (flag.is_string()) {
                auto text = flag.get<std::string>();
                boost::algorithm::trim(text);
                boost::algorithm::to_lower(text);
                ctx.playnite_fullscreen = (text == "true" || text == "1" || text == "yes");
              }
            } catch (...) {
              ctx.playnite_fullscreen = false;
            }
          }

          // Calculate a unique application id.
          auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
          if (ids.count(std::get<0>(possible_ids)) == 0) {
            ctx.id = std::get<0>(possible_ids);
          } else {
            ctx.id = std::get<1>(possible_ids);
          }
          ids.insert(ctx.id);

          ctx.name = std::move(name);
          ctx.prep_cmds = std::move(prep_cmds);
          ctx.state_cmds = std::move(state_cmds);
          ctx.detached = std::move(detached);

          apps.emplace_back(std::move(ctx));
        }

        fail_count = 0;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Error happened during app loading: "sv << e.what();

        fail_count += 1;

        if (fail_count >= 3) {
          // No hope for recovering
          BOOST_LOG(warning) << "Couldn't parse/migrate apps.json properly! Apps will not be loaded."sv;
          break;
        }

        BOOST_LOG(warning) << "App format is still invalid! Trying to re-migrate the app list..."sv;

        // Always try migrating from scratch when error happened
        tree["version"] = 0;

        try {
          migrate(tree, file_name);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Error happened during migration: "sv << e.what();
          break;
        }

        this_env = boost::this_process::environment();
        ids.clear();
        apps.clear();
        i = 0;

        continue;
      }

      break;
    } while (fail_count < 3);

    if (fail_count > 0) {
      BOOST_LOG(warning) << "No applications configured, adding fallback Desktop entry.";
      proc::ctx_t ctx {};
      ctx.idx = std::to_string(i);
      ctx.uuid = FALLBACK_DESKTOP_UUID;  // Placeholder UUID
      ctx.name = "Desktop (fallback)";
      ctx.image_path = parse_env_val(this_env, "desktop-alt.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;  // Desktop doesn't have a specific command to wait for
      ctx.exit_timeout = 5s;

      // Calculate unique ID
      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    // Virtual Display entry
#ifdef _WIN32
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      proc::ctx_t ctx {};
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
#endif

    if (config::input.enable_input_only_mode) {
      // Input Only entry
      {
        proc::ctx_t ctx {};
        ctx.idx = std::to_string(i);
        ctx.uuid = REMOTE_INPUT_UUID;
        ctx.name = "Remote Input";
        ctx.image_path = parse_env_val(this_env, "input_only.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = true;  // There's no need to keep an active input only session ongoing

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        input_only_app_id_str = ctx.id;
        input_only_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }
    }

    // Terminate entry
    {
      proc::ctx_t ctx {};
      ctx.idx = std::to_string(i);
      ctx.uuid = TERMINATE_APP_UUID;
      ctx.name = "Terminate";
      ctx.image_path = parse_env_val(this_env, "terminate.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = true;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      // ids.insert(ctx.id);

      terminate_app_id_str = ctx.id;
      terminate_app_id = util::from_view(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    return proc::proc_t {
      std::move(this_env),
      std::move(apps)
    };
  }

  void refresh(const std::string &file_name, bool needs_terminate) {
    if (needs_terminate) {
      proc.terminate(false, false);
    }

#ifdef _WIN32
    size_t fail_count = 0;
    while (fail_count < 5 && vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        break;
      }

      fail_count += 1;
      std::this_thread::sleep_for(1s);
    }
#endif

    auto proc_opt = proc::parse(file_name);

    if (!proc_opt) {
      return;
    }

    // If an app is currently running, do not replace the entire proc_t instance.
    // Replacing it would drop tracking state and cause the active stream loop
    // to think no app is running, prematurely terminating the session.
    // Instead, update only the applications list to reflect the latest config.
    if (proc.running() > 0) {
      // Move the parsed apps list and environment into the existing proc instance
      // Use proc.update_apps(...) which safely replaces the app list and env
      proc.update_apps(proc_opt->release_apps(), proc_opt->release_env());

    } else {
      // No app running: safe to refresh full state (env + apps)
      proc = std::move(*proc_opt);
    }
  }

  void proc_t::update_apps(std::vector<ctx_t> &&apps, boost::process::v1::environment &&env) {
    // Replace app list and environment while keeping current running app intact
    {
      std::scoped_lock lk(_apps_mutex);
      _apps = std::move(apps);
      _env = std::move(env);
    }
  }

  std::vector<ctx_t> proc_t::release_apps() {
    return std::move(_apps);
  }

  boost::process::v1::environment proc_t::release_env() {
    return std::move(_env);
  }
}  // namespace proc
