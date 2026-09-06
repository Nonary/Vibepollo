/**
 * @file src/platform/linux/gamescope_display_backend.cpp
 * @brief Existing-scene display backend for Gaming Mode and headless Gamescope.
 */
#include "display_backend.h"
#include "src/logging.h"
#include "src/rtsp.h"

#include <utility>

namespace platf::linux_display {
  namespace {
    class gamescope_backend_t final : public backend_t {
    public:
      prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool, bool) const override {
        // Desktop monitor preferences are persistent configuration. Only this
        // session targets the compositor scene; there is no connector lease.
        session.virtual_display = false;
        session.virtual_display_failed = false;
        session.virtual_display_device_id.clear();
        session.virtual_display_ready_since.reset();
        session.virtual_display_hdr_enabled.reset();
        session.virtual_display_recreated_on_demand = false;
        session.virtual_display_needs_resume_apply = false;
        session.virtual_display_mode_override = config::video_t::virtual_display_mode_e::disabled;
        session.output_name_override = capture_target({});
        // Preserve enable_hdr/force_sdr for the capture backend's verified
        // protocol and pixel-format negotiation, including patched Gamescope.
        BOOST_LOG(info) << "Gaming Mode: capturing the existing Gamescope compositor output.";
        return {false, *session.output_name_override, {}};
      }

      capabilities_t capabilities() const override { return {"gamescope"}; }

      std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail) const override {
        display_device::EnumeratedDevice device;
        device.m_device_id = capture_target({});
        device.m_display_name = device.m_device_id;
        device.m_friendly_name = "Gamescope compositor output";
        return display_device::EnumeratedDeviceList {std::move(device)};
      }

      std::string capture_target(const std::string &) const override {
        return "gamescope";
      }
    };
  }  // namespace

  const backend_t &gamescope_backend() {
    static const gamescope_backend_t gamescope;
    return gamescope;
  }
}  // namespace platf::linux_display
