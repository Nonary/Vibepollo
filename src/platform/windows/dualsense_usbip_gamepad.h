// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/platform/common.h"

#include <memory>

namespace platf::dualsense_usbip_gamepad {
  // An application owns this preference through its full deferred-launch
  // lifecycle. Opening a scope does not create or enumerate USB hardware.
  class session_scope {
  public:
    ~session_scope();
    session_scope(const session_scope &) = delete;
    session_scope &operator=(const session_scope &) = delete;

  private:
    session_scope();
    friend std::shared_ptr<session_scope> start_session();
  };

  // Returns empty when the optional signed transport is not installed.
  std::shared_ptr<session_scope> start_session();
  bool enabled();
  // End the lifetime of idle endpoints retained at the client's request.
  void set_application_active(bool active);
  // A USB/IP import alone is insufficient: both this controller's HID child
  // and its active four-channel Windows audio endpoint must be ready.
  bool has_ready_controller();
}  // namespace platf::dualsense_usbip_gamepad

namespace platf {
  class usbip_gamepad_t {
  public:
    usbip_gamepad_t();
    ~usbip_gamepad_t();
    usbip_gamepad_t(const usbip_gamepad_t &) = delete;
    usbip_gamepad_t &operator=(const usbip_gamepad_t &) = delete;

    bool probe();
    bool available() const;
    int alloc(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t &feedback_queue);
    void free(int nr, bool retain_for_resume = true);
    void update(int nr, const gamepad_state_t &state);
    void touch(int nr, const gamepad_touch_t &event);
    void motion(int nr, const gamepad_motion_t &event);
    void battery(int nr, const gamepad_battery_t &event);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf
