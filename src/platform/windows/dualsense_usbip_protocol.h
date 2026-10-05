// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace platf::dualsense_usbip {
  struct callbacks {
    std::function<void(std::span<const std::uint8_t>)> hid_output;
    // Stereo signed 16-bit little-endian PCM, 48 kHz. USB speaker channels
    // are discarded; each span contains only the two actuator channels.
    std::function<void(std::span<const std::uint8_t>)> haptics_pcm;
  };

  // Portable USB/IP 1.1.1 device endpoint. Except set_input_report(), methods
  // are called by one transport thread. No sockets, drivers, or OS APIs live
  // here, so the actual descriptors and URB stream are testable on Linux.
  class session {
  public:
    explicit session(std::uint8_t slot, callbacks handlers = {});
    static std::string bus_id(std::uint8_t slot);
    bool set_input_report(std::span<const std::uint8_t> report);
    bool feed(std::span<const std::uint8_t> bytes, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    std::vector<std::uint8_t> take_output();
    bool imported() const noexcept;
    bool failed() const noexcept;
    // Send buffered output before closing a DEVLIST/error connection.
    bool finished() const noexcept;

  private:
    struct iso_packet {
      std::uint32_t offset;
      std::uint32_t length;
    };

    struct urb {
      std::uint32_t sequence;
      std::uint32_t direction;
      std::uint32_t endpoint;
      std::uint32_t length;
      std::uint32_t start_frame;
      std::uint32_t packet_count;
      std::array<std::uint8_t, 8> setup;
      std::vector<iso_packet> packets;
      std::vector<std::uint8_t> data;
      std::chrono::steady_clock::time_point due {};
    };

    bool parse_one();
    void submit(urb request);
    void control(const urb &request);
    void reply(const urb &request, std::int32_t status, std::span<const std::uint8_t> data = {}, std::uint32_t out_length = 0);
    void device_record();
    void header(std::uint16_t operation, std::uint32_t status = 0);
    void fail();
    std::uint8_t slot_;
    callbacks callbacks_;
    bool imported_ = false;
    bool failed_ = false;
    bool finished_ = false;
    std::uint8_t configuration_ = 0;
    std::array<std::uint8_t, 4> alternate_ {};
    std::array<bool, 4> alternate_explicit_ {};
    std::array<std::uint8_t, 4> idle_ {};
    std::array<std::uint8_t, 4> protocol_ {1, 1, 1, 1};
    std::array<std::uint8_t, 2> mute_ {};
    std::array<std::array<std::int16_t, 5>, 2> volume_ {};
    std::vector<std::uint8_t> receive_;
    std::vector<std::uint8_t> transmit_;
    std::deque<urb> pending_;
    std::chrono::steady_clock::time_point next_hid_ {};
    std::chrono::steady_clock::time_point next_mic_ {};
    std::chrono::steady_clock::time_point next_audio_ {};
    std::chrono::steady_clock::time_point epoch_ {std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point receive_time_ {};
    std::mutex input_mutex_;
    std::array<std::uint8_t, 64> input_ {};
  };
}  // namespace platf::dualsense_usbip
