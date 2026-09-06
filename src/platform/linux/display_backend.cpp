/**
 * @file src/platform/linux/display_backend.cpp
 * @brief Linux display backend selection and managed desktop implementation.
 */
#include "display_backend.h"
#include "private_display.h"

#include <display_device/json.h>
#include <utility>

#ifdef SUNSHINE_BUILD_GAMESCOPE
  #include "gamescopegrab.h"
#endif

namespace platf::linux_display {
  namespace {
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
