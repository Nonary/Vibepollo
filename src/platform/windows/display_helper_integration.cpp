/**
 * @file src/platform/windows/display_helper_integration.cpp
 */
#ifdef _WIN32

// standard
#include <filesystem>
#include <string>

// libdisplaydevice json
#include <display_device/json.h>

// sunshine
#include "display_helper_integration.h"
#include "src/display_device.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/ipc/display_settings_client.h"
#include "src/platform/windows/ipc/process_handler.h"

namespace {
  // Persistent process handler to keep helper alive while Sunshine runs
  ProcessHandler &helper_proc() {
    static ProcessHandler h(/*use_job=*/false);
    return h;
  }

  bool ensure_helper_started() {
    // Already started?
    if (helper_proc().get_process_handle() != nullptr) {
      return true;
    }
    // Compute path to display-settings-helper.exe inside the tools subdirectory next to Sunshine.exe
    wchar_t module_path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
      BOOST_LOG(error) << "Failed to resolve Sunshine module path; cannot launch display helper.";
      return false;
    }
    std::filesystem::path exe_path(module_path);
    std::filesystem::path dir = exe_path.parent_path();
    std::filesystem::path helper = dir / L"tools" / L"display-settings-helper.exe";

    if (!std::filesystem::exists(helper)) {
      BOOST_LOG(warning) << "Display helper not found at: " << platf::to_utf8(helper.wstring())
                         << ". Ensure the tools subdirectory is present and contains display-settings-helper.exe.";
      return false;
    }

    const bool started = helper_proc().start(helper.wstring(), L"");
    if (!started) {
      BOOST_LOG(error) << "Failed to start display helper: " << platf::to_utf8(helper.wstring());
    }
    return started;
  }
}

namespace display_helper_integration {
  bool apply_from_session(const config::video_t &video_config, const rtsp_stream::launch_session_t &session) {
    if (!ensure_helper_started()) {
      return false;
    }

    const auto parsed = display_device::parse_configuration(video_config, session);
    if (const auto *cfg = std::get_if<display_device::SingleDisplayConfiguration>(&parsed)) {
      const std::string json = display_device::toJson(*cfg);
      return platf::display_helper_client::send_apply_json(json);
    }
    if (std::holds_alternative<display_device::configuration_disabled_tag_t>(parsed)) {
      // If disabled, request revert so helper can restore
      return platf::display_helper_client::send_revert();
    }
    // failed_to_parse -> let caller fallback
    return false;
  }

  bool revert() {
    if (!ensure_helper_started()) {
      return false;
    }
    return platf::display_helper_client::send_revert();
  }
}

#endif
