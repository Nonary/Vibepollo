/**
 * @file src/platform/linux/display_backend.h
 * @brief Session display ownership and capture targeting for Linux compositors.
 */
#pragma once

#include <chrono>
#include <display_device/types.h>
#include <optional>
#include <string>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace platf::linux_display {
  struct prepared_display_t {
    // Ownership controls connector cleanup; existing scenes return false.
    bool owns_output {false};
    std::string output_name;
    std::string error;
  };

  struct capabilities_t {
    const char *backend_name {"unknown"};
    // Independent monitor capabilities, separate from capture HDR support.
    bool independent_outputs {false};
    bool independent_outputs_ready {false};
    bool independent_outputs_hdr {false};
  };

  class backend_t {
  public:
    virtual ~backend_t() = default;
    virtual prepared_display_t prepare_session(rtsp_stream::launch_session_t &session, bool no_active_sessions, bool allow_display_changes) const = 0;
    virtual capabilities_t capabilities() const = 0;
    virtual std::optional<display_device::EnumeratedDeviceList> enumerate_devices(display_device::DeviceEnumerationDetail detail) const = 0;
    virtual std::string capture_target(const std::string &requested_output) const = 0;

    // Backends capturing an existing scene own no display topology to mutate.
    virtual bool initialize() const { return true; }
    virtual bool apply_session(rtsp_stream::launch_session_t &) const { return true; }
    virtual bool revert() const { return true; }
    virtual bool reset_persistence() const { return true; }
    virtual void schedule_revert(std::chrono::milliseconds, std::string) const {}
    virtual void cancel_scheduled_revert() const {}

    std::string enumerate_devices_json(display_device::DeviceEnumerationDetail detail) const;
  };

  /** Selected from the verified capture backend; fixed for the host session. */
  const backend_t &backend();
}  // namespace platf::linux_display
