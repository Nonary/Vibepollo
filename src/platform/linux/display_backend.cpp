/**
 * @file src/platform/linux/display_backend.cpp
 * @brief Linux display backend selection and managed desktop implementation.
 */
#include "display_backend.h"
#include "private_display.h"
#include "session_backend.h"
#include "src/platform/common.h"
#include "src/rtsp.h"

#include <display_device/json.h>
#include <utility>
#include <algorithm>

#ifdef SUNSHINE_BUILD_GAMESCOPE
  #include "gamescopegrab.h"
#endif

namespace platf::linux_display {
  namespace {
    // An explicit capability subset: capture existing physical SDR scanout.
    // No KScreen, connector lease, topology restoration or X11 authority is
    // inferred from the presence of a running Hyprland process.
    class hyprland_backend_t final : public backend_t {
      static bool physical_output(const std::string &name) {
        const auto separator = name.find_last_of(':');
        const auto connector = separator == std::string::npos ? name : name.substr(separator + 1);
        return !linux_private_display::is_kernel_output(connector) &&
          std::ranges::find(config::video.dd.virtual_display_outputs, connector) == config::video.dd.virtual_display_outputs.end();
      }

    public:
      prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool, bool) const override {
        const bool virtual_requested = session.virtual_display ||
          session.client_virtual_display_override.value_or(session.client_requests_virtual_display) ||
          session.virtual_display_mode_override.value_or(config::video.virtual_display_mode) != config::video_t::virtual_display_mode_e::disabled;
        if (virtual_requested || rtsp_stream::hdr_capture_required(session, config::video)) {
          session.virtual_display_failed = true;
          return {false, {}, "Hyprland KMS supports existing physical SDR outputs only; disable private/Remote Monitor and native HDR capture requests"};
        }
        const auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Full);
        const auto requested = session.output_name_override.value_or(config::video.output_name);
        if (requested.empty()) {
          return {false, {}, "Hyprland KMS requires an explicit physical SDR connector; set output_name before streaming"};
        }
        const auto selected = devices ? std::ranges::find_if(*devices, [&](const auto &device) {
          return requested == device.m_device_id;
        }) : display_device::EnumeratedDeviceList::const_iterator {};
        if (!devices || selected == devices->end()) {
          return {false, {}, "Hyprland KMS requires an enabled physical output selected by its exact connector name"};
        }
        session.output_name_override = selected->m_device_id;
        session.virtual_display = false;
        session.virtual_display_failed = false;
        session.virtual_display_device_id.clear();
        session.virtual_display_ready_since.reset();
        session.virtual_display_hdr_enabled.reset();
        session.virtual_display_recreated_on_demand = false;
        session.virtual_display_needs_resume_apply = false;
        return {false, *session.output_name_override, {}};
      }

      capabilities_t capabilities() const override { return {"hyprland-kms-physical-sdr"}; }

      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) const override {
        display_device::EnumeratedDeviceList result;
        for (const auto &name : platf::display_names()) {
          if (!physical_output(name)) continue;
          display_device::EnumeratedDevice device;
          device.m_device_id = name;
          device.m_display_name = name;
          device.m_friendly_name = name;
          result.push_back(std::move(device));
        }
        return result;
      }

      std::string capture_target(const std::string &requested) const override {
        const auto devices = enumerate_devices(display_device::DeviceEnumerationDetail::Full);
        if (devices) {
          for (const auto &device : *devices) {
            if (!requested.empty() && device.m_device_id == requested) return device.m_device_id;
          }
        }
        // A nonempty unresolvable name makes KMS fail instead of choosing the
        // first output, which could be a preserved connector from a KDE host.
        return "__unsupported-hyprland-output__";
      }
      bool reset_persistence() const override { return false; }
    };

    class desktop_backend_t final : public backend_t {
    public:
      prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool no_active_sessions, bool allow_display_changes) const override {
        auto prepared = linux_private_display::prepare_session(session, no_active_sessions, allow_display_changes);
        return {prepared.active, std::move(prepared.output_name), std::move(prepared.error)};
      }

      capabilities_t capabilities() const override {
        return {"kscreen-vkms", linux_private_display::capable(), linux_private_display::ready(), linux_private_display::hdr_capable()};
      }

      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail detail) const override {
        return linux_private_display::enumerate_devices(detail);
      }

      std::string capture_target(const std::string &requested_output) const override {
        return requested_output;
      }

      bool initialize() const override { return linux_private_display::initialize(); }
      bool apply_session(rtsp_stream::launch_session_t &session) const override { return linux_private_display::apply_session(session); }
      bool revert() const override { return linux_private_display::revert(); }
      bool reset_persistence() const override { return linux_private_display::reset_persistence(); }
      void schedule_revert(std::chrono::milliseconds delay, std::string reason) const override {
        linux_private_display::schedule_revert(delay, std::move(reason));
      }
      void cancel_scheduled_revert() const override { linux_private_display::cancel_scheduled_revert(); }
    };
  }  // namespace

#ifdef SUNSHINE_BUILD_GAMESCOPE
  const backend_t &gamescope_backend();
#endif

  const backend_t &backend() {
    if (hyprland_kms_session()) {
      static const hyprland_backend_t hyprland;
      return hyprland;
    }
#ifdef SUNSHINE_BUILD_GAMESCOPE
    if (gamescope_capture_selected()) {
      return gamescope_backend();
    }
#endif
    static const desktop_backend_t desktop;
    return desktop;
  }

  std::string backend_t::enumerate_devices_json(display_device::DeviceEnumerationDetail detail) const {
    const auto devices = enumerate_devices(detail);
    return devices ? display_device::toJson(*devices, std::nullopt) : "[]";
  }
}  // namespace platf::linux_display
