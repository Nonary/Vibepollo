// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dualsense_usbip_protocol.h"

#include <chrono>
#include <memory>

namespace platf::dualsense_usbip {
  // The optional, already-installed usbip-win2 virtual host controller must
  // be accessible and accept the pinned release ABI through a read-only
  // query. This never installs a driver or changes its policy.
  bool available();

  class controller {
  public:
    ~controller();
    controller(const controller &) = delete;
    controller &operator=(const controller &) = delete;

    bool set_input_report(std::span<const std::uint8_t> report);
    bool connected() const noexcept;
    // Wait for this device's HID and USB Audio functions to start, rather
    // than assuming a successful USB/IP import proves enumeration succeeded.
    bool wait_until_ready(std::chrono::milliseconds timeout);

  private:
    struct impl;
    explicit controller(std::shared_ptr<impl> state);
    std::shared_ptr<impl> state_;
    friend std::unique_ptr<controller> create(std::uint8_t slot, callbacks handlers);
  };

  // A loopback server and one imported composite device belong to each
  // controller. Destruction detaches only that controller's owned port.
  // Callbacks run on its transport thread; they must not destroy controller.
  std::unique_ptr<controller> create(std::uint8_t slot, callbacks handlers = {});
}  // namespace platf::dualsense_usbip
