// SPDX-License-Identifier: GPL-3.0-or-later
#include "dualsense_usbip_protocol.h"

#include "libvirtualgamepad/ds5_usb.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace platf::dualsense_usbip {
  namespace {
    constexpr std::size_t header_size = 48;
    constexpr std::size_t max_transfer = 256 * 1024;
    constexpr std::size_t max_buffer = 2 * 1024 * 1024;
    constexpr std::uint32_t non_iso = 0xffffffff;
    constexpr std::int32_t stall = -32;
    constexpr std::int32_t canceled = -104;

    constexpr std::array<std::uint8_t, 18> device_descriptor {
      18,
      1,
      0x00,
      0x02,
      0,
      0,
      0,
      64,
      0x4c,
      0x05,
      0xe6,
      0x0c,
      0x00,
      0x01,
      1,
      2,
      3,
      1,
    };
    // Captured Sony composite configuration: the same interface/endpoint
    // contract used by the Linux gadget. Hardware descriptor bytes below
    // follow HIDMaestro's MIT dualsense-composite profile; no GPL-2.0-only
    // Linux gadget implementation is incorporated into this source.
    // Copyright (c) 2026 HIDMaestro Contributors
    //
    // Permission is hereby granted, free of charge, to any person obtaining
    // a copy of this software and associated documentation files (the
    // "Software"), to deal in the Software without restriction, including
    // without limitation the rights to use, copy, modify, merge, publish,
    // distribute, sublicense, and/or sell copies of the Software, and to
    // permit persons to whom the Software is furnished to do so, subject to
    // the following conditions:
    // The above copyright notice and this permission notice shall be
    // included in all copies or substantial portions of the Software.
    // THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    // EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    // MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    // IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    // CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    // TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    // SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
    constexpr std::uint8_t configuration_descriptor[] {
      0x09,
      0x02,
      0xe3,
      0x00,
      0x04,
      0x01,
      0x00,
      0xc0,
      0xfa,
      0x09,
      0x04,
      0x00,
      0x00,
      0x00,
      0x01,
      0x01,
      0x00,
      0x00,
      0x0a,
      0x24,
      0x01,
      0x00,
      0x01,
      0x49,
      0x00,
      0x02,
      0x01,
      0x02,
      0x0c,
      0x24,
      0x02,
      0x01,
      0x01,
      0x01,
      0x06,
      0x04,
      0x33,
      0x00,
      0x00,
      0x00,
      0x0c,
      0x24,
      0x06,
      0x02,
      0x01,
      0x01,
      0x03,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x09,
      0x24,
      0x03,
      0x03,
      0x01,
      0x03,
      0x04,
      0x02,
      0x00,
      0x0c,
      0x24,
      0x02,
      0x04,
      0x02,
      0x04,
      0x03,
      0x02,
      0x03,
      0x00,
      0x00,
      0x00,
      0x09,
      0x24,
      0x06,
      0x05,
      0x04,
      0x01,
      0x03,
      0x00,
      0x00,
      0x09,
      0x24,
      0x03,
      0x06,
      0x01,
      0x01,
      0x01,
      0x05,
      0x00,
      0x09,
      0x04,
      0x01,
      0x00,
      0x00,
      0x01,
      0x02,
      0x00,
      0x00,
      0x09,
      0x04,
      0x01,
      0x01,
      0x01,
      0x01,
      0x02,
      0x00,
      0x00,
      0x07,
      0x24,
      0x01,
      0x01,
      0x01,
      0x01,
      0x00,
      0x0b,
      0x24,
      0x02,
      0x01,
      0x04,
      0x02,
      0x10,
      0x01,
      0x80,
      0xbb,
      0x00,
      0x09,
      0x05,
      0x01,
      0x09,
      0x88,
      0x01,
      0x04,
      0x00,
      0x00,
      0x07,
      0x25,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x09,
      0x04,
      0x02,
      0x00,
      0x00,
      0x01,
      0x02,
      0x00,
      0x00,
      0x09,
      0x04,
      0x02,
      0x01,
      0x01,
      0x01,
      0x02,
      0x00,
      0x00,
      0x07,
      0x24,
      0x01,
      0x06,
      0x01,
      0x01,
      0x00,
      0x0b,
      0x24,
      0x02,
      0x01,
      0x02,
      0x02,
      0x10,
      0x01,
      0x80,
      0xbb,
      0x00,
      0x09,
      0x05,
      0x82,
      0x05,
      0xc4,
      0x00,
      0x04,
      0x00,
      0x00,
      0x07,
      0x25,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x09,
      0x04,
      0x03,
      0x00,
      0x02,
      0x03,
      0x00,
      0x00,
      0x00,
      0x09,
      0x21,
      0x11,
      0x01,
      0x00,
      0x01,
      0x22,
      sizeof(lvg::ds5_usb::report_descriptor) & 0xff,
      sizeof(lvg::ds5_usb::report_descriptor) >> 8,
      0x07,
      0x05,
      0x84,
      0x03,
      0x40,
      0x00,
      0x06,
      0x07,
      0x05,
      0x03,
      0x03,
      0x40,
      0x00,
      0x06,
    };
    static_assert(sizeof(configuration_descriptor) == 227);
    constexpr std::size_t hid_descriptor_offset = 204;

    std::uint32_t read32(const std::uint8_t *bytes) {
      return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
    }

    std::uint16_t read16(const std::uint8_t *bytes) {
      return (std::uint16_t(bytes[0]) << 8) | bytes[1];
    }

    std::uint16_t usb16(const std::uint8_t *bytes) {
      return bytes[0] | (std::uint16_t(bytes[1]) << 8);
    }

    void write32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
      bytes.push_back(std::uint8_t(value >> 24));
      bytes.push_back(std::uint8_t(value >> 16));
      bytes.push_back(std::uint8_t(value >> 8));
      bytes.push_back(std::uint8_t(value));
    }

    void write16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
      bytes.push_back(std::uint8_t(value >> 8));
      bytes.push_back(std::uint8_t(value));
    }

    void fixed_string(std::vector<std::uint8_t> &bytes, const std::string &value, std::size_t capacity) {
      const auto start = bytes.size();
      bytes.resize(start + capacity, 0);
      std::memcpy(bytes.data() + start, value.data(), std::min(value.size(), capacity - 1));
    }

    std::vector<std::uint8_t> usb_string(const std::string &value) {
      std::vector<std::uint8_t> result {std::uint8_t(value.size() * 2 + 2), 3};
      for (unsigned char character : value) {
        result.push_back(character);
        result.push_back(0);
      }
      return result;
    }
  }  // namespace

  session::session(std::uint8_t slot, callbacks handlers):
      slot_(slot),
      callbacks_(std::move(handlers)) {
    if (slot_ >= 16) {
      fail();
    }
    input_[0] = 1;
    std::fill(input_.begin() + 1, input_.begin() + 5, 0x80);
    input_[8] = 8;
    input_[25] = 0x20;  // stationary +1g on Y, 8192 counts/g
    input_[33] = input_[37] = 0x80;  // inactive touch contacts
    input_[53] = 0x2a;
  }

  std::string session::bus_id(std::uint8_t slot) {
    return "1-" + std::to_string(unsigned(slot) + 1);
  }

  bool session::set_input_report(std::span<const std::uint8_t> report) {
    if (report.size() != input_.size() || report[0] != 1) {
      return false;
    }
    std::lock_guard guard(input_mutex_);
    std::copy(report.begin(), report.end(), input_.begin());
    return true;
  }

  bool session::imported() const noexcept {
    return imported_;
  }

  bool session::failed() const noexcept {
    return failed_;
  }

  bool session::finished() const noexcept {
    return finished_;
  }

  void session::fail() {
    failed_ = finished_ = true;
    receive_.clear();
    receive_incarnation_ = 0;
    pending_.clear();
  }

  std::vector<std::uint8_t> session::take_output() {
    return std::exchange(transmit_, {});
  }

  void session::header(std::uint16_t operation, std::uint32_t status) {
    write16(transmit_, 0x0111);
    write16(transmit_, operation);
    write32(transmit_, status);
  }

  void session::device_record() {
    fixed_string(transmit_, "/vibepollo/dualsense/" + std::to_string(slot_), 256);
    fixed_string(transmit_, bus_id(slot_), 32);
    write32(transmit_, 1);
    write32(transmit_, unsigned(slot_) + 1);
    write32(transmit_, 3);  // high speed
    write16(transmit_, 0x054c);
    write16(transmit_, 0x0ce6);
    write16(transmit_, 0x0100);
    transmit_.insert(transmit_.end(), {0, 0, 0, 1, 1, 4});
  }

  std::uint64_t session::capture_receive_incarnation() const {
    return callbacks_.capture_speaker_incarnation ? callbacks_.capture_speaker_incarnation() : 0;
  }

  void session::consume_received(std::size_t count) {
    receive_.erase(receive_.begin(), receive_.begin() + count);
    receive_incarnation_ = receive_.empty() ? 0 : feed_incarnation_;
  }

  bool session::feed(std::span<const std::uint8_t> bytes, std::chrono::steady_clock::time_point now) {
    return feed(bytes, capture_receive_incarnation(), now);
  }

  bool session::feed(std::span<const std::uint8_t> bytes, std::uint64_t incarnation,
                     std::chrono::steady_clock::time_point now) {
    if (failed_ || finished_ || bytes.size() > max_buffer - receive_.size()) {
      fail();
      return false;
    }
    feed_incarnation_ = incarnation;
    if (receive_.empty()) receive_incarnation_ = incarnation;
    receive_time_ = now;
    receive_.insert(receive_.end(), bytes.begin(), bytes.end());
    while (!failed_ && !finished_ && parse_one()) {
      if (transmit_.size() > max_buffer) {
        fail();
      }
    }
    return !failed_;
  }

  bool session::parse_one() {
    if (!imported_) {
      if (receive_.size() < 8) {
        return false;
      }
      if (read16(receive_.data()) != 0x0111 || read32(receive_.data() + 4) != 0) {
        fail();
        return false;
      }
      const auto operation = read16(receive_.data() + 2);
      if (operation == 0x8005) {
        header(5);
        write32(transmit_, 1);
        device_record();
        transmit_.insert(transmit_.end(), {1, 1, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 3, 0, 0, 0});
        receive_.clear();
        receive_incarnation_ = 0;
        finished_ = true;
        return false;
      }
      if (operation != 0x8003) {
        fail();
        return false;
      }
      if (receive_.size() < 40) {
        return false;
      }
      const auto end = std::find(receive_.begin() + 8, receive_.begin() + 40, 0);
      const std::string bus(receive_.begin() + 8, end);
      if (end == receive_.begin() + 40 || bus != bus_id(slot_)) {
        header(3, 4);  // ST_NODEV
        receive_.clear();
        receive_incarnation_ = 0;
        finished_ = true;
        return false;
      }
      header(3);
      device_record();
      imported_ = true;
      // usbip-win2 can consume configuration/interface selection locally.
      // Its first endpoint URB is authoritative until it forwards an explicit
      // SET_CONFIGURATION/SET_INTERFACE request.
      configuration_ = 1;
      consume_received(40);
      return true;
    }

    if (receive_.size() < header_size) {
      return false;
    }
    const auto *wire = receive_.data();
    const auto command = read32(wire);
    const auto sequence = read32(wire + 4);
    if (command == 2) {
      const auto victim = read32(wire + 20);
      const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const urb &request) {
        return request.sequence == victim;
      });
      const bool removed = found != pending_.end();
      if (removed) {
        pending_.erase(found);
      }
      write32(transmit_, 4);
      write32(transmit_, sequence);
      transmit_.resize(transmit_.size() + 12, 0);
      write32(transmit_, removed ? std::uint32_t(canceled) : 0);
      transmit_.resize(transmit_.size() + 24, 0);
      consume_received(header_size);
      return true;
    }
    if (command != 1 || read32(wire + 8) != ((1u << 16) | (unsigned(slot_) + 1))) {
      fail();
      return false;
    }
    urb request {};
    request.sequence = sequence;
    request.direction = read32(wire + 12);
    request.endpoint = read32(wire + 16);
    request.length = read32(wire + 24);
    request.start_frame = read32(wire + 28);
    request.packet_count = read32(wire + 32);
    if (request.direction > 1 || request.endpoint > 15 || request.length > max_transfer || (request.packet_count != non_iso && request.packet_count > 1024)) {
      fail();
      return false;
    }
    const auto packet_count = request.packet_count == non_iso ? 0 : request.packet_count;
    const auto data_size = request.direction == 0 ? request.length : 0;
    const auto message_size = header_size + data_size + std::size_t(packet_count) * 16;
    if (receive_.size() < message_size) {
      return false;
    }
    std::copy_n(wire + 40, 8, request.setup.begin());
    request.data.assign(wire + header_size, wire + header_size + data_size);
    const auto *iso = wire + header_size + data_size;
    std::uint64_t end = 0;
    for (unsigned i = 0; i < packet_count; ++i, iso += 16) {
      const iso_packet packet {read32(iso), read32(iso + 4)};
      if (packet.offset < end || packet.offset > request.length || packet.length > request.length - packet.offset) {
        fail();
        return false;
      }
      end = std::uint64_t(packet.offset) + packet.length;
      request.packets.push_back(packet);
    }
    if (std::any_of(pending_.begin(), pending_.end(), [&](const urb &pending) {
          return pending.sequence == sequence;
        })) {
      fail();
      return false;
    }
    request.speaker_incarnation = receive_incarnation_;
    consume_received(message_size);
    submit(std::move(request));
    return true;
  }

  void session::reply(const urb &request, std::int32_t status, std::span<const std::uint8_t> data, std::uint32_t out_length) {
    write32(transmit_, 3);
    write32(transmit_, request.sequence);
    transmit_.resize(transmit_.size() + 12, 0);  // response devid/direction/ep
    write32(transmit_, std::uint32_t(status));
    write32(transmit_, status ? 0 : request.direction ? std::uint32_t(data.size()) :
                                                        out_length);
    write32(transmit_, request.packets.empty() ? 0 : request.start_frame);
    write32(transmit_, request.packets.empty() ? non_iso : request.packet_count);
    write32(transmit_, status ? std::uint32_t(request.packets.size()) : 0);
    transmit_.resize(transmit_.size() + 8, 0);
    if (request.direction && !status) {
      transmit_.insert(transmit_.end(), data.begin(), data.end());
    }
    for (const auto &packet : request.packets) {
      write32(transmit_, packet.offset);
      write32(transmit_, packet.length);
      write32(transmit_, status ? 0 : request.direction ? std::min(packet.length, 192u) :
                                                          packet.length);
      write32(transmit_, std::uint32_t(status));
    }
  }

  void session::submit(urb request) {
    const bool iso = request.packet_count != non_iso && request.packet_count != 0;
    if (!request.endpoint && !iso) {
      control(request);
      return;
    }
    if (!configuration_) {
      reply(request, stall);
      return;
    }
    if (request.endpoint == 3 && request.direction == 0 && !iso && request.length <= 64) {
      if (callbacks_.hid_output && !request.data.empty()) {
        callbacks_.hid_output(request.data);
      }
      reply(request, 0, {}, request.length);
      return;
    }
    if (request.endpoint == 4 && request.direction == 1 && !iso) {
      if (!request.length) {
        reply(request, 0);
      } else if (pending_.size() >= 64) {
        reply(request, -12);
      } else {
        pending_.push_back(std::move(request));
      }
      return;
    }
    const bool audio_out = request.endpoint == 1 && request.direction == 0;
    const bool microphone_in = request.endpoint == 2 && request.direction == 1;
    const unsigned interface = audio_out ? 1 : 2;
    if ((!audio_out && !microphone_in) || !iso || (alternate_explicit_[interface] && alternate_[interface] == 0)) {
      reply(request, stall);
      return;
    }
    for (const auto &packet : request.packets) {
      if (packet.length > (audio_out ? 392u : 196u) || packet.length % (audio_out ? 8 : 4)) {
        reply(request, stall);
        return;
      }
    }
    if (pending_.size() >= 64) {
      reply(request, -12);
      return;
    }
    alternate_[interface] = 1;
    const auto now = receive_time_;
    auto &cursor = audio_out ? next_audio_ : next_mic_;
    // Preserve the audio clock through normal wake/network jitter. Re-anchor
    // after a real stream pause, rather than accumulating a late wake in every
    // 1 ms packet interval and slowing the nominal 48 kHz sample clock.
    if (cursor == std::chrono::steady_clock::time_point {} || cursor < now - std::chrono::milliseconds(50)) {
      cursor = now;
    }
    cursor += std::chrono::milliseconds(request.packet_count);
    request.due = cursor;
    // Concurrent speaker and microphone URBs occupy the same USB frames.
    request.start_frame = std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(cursor - epoch_).count());
    pending_.push_back(std::move(request));
  }

  void session::poll(std::chrono::steady_clock::time_point now) {
    if (failed_ || finished_) {
      return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      const bool hid = it->endpoint == 4;
      if (hid ? now < next_hid_ : now < it->due) {
        ++it;
        continue;
      }
      urb request = std::move(*it);
      it = pending_.erase(it);
      if (hid) {
        std::array<std::uint8_t, 64> report;
        {
          std::lock_guard guard(input_mutex_);
          report = input_;
        }
        reply(request, 0, std::span(report).first(std::min<std::size_t>(request.length, report.size())));
        next_hid_ = now + std::chrono::milliseconds(4);
      } else if (request.direction == 1) {
        std::size_t size = 0;
        for (const auto &packet : request.packets) {
          size += std::min(packet.length, 192u);
        }
        const std::vector<std::uint8_t> silence(size, 0);
        reply(request, 0, silence);
      } else {
        std::vector<std::uint8_t> haptics;
        std::vector<std::uint8_t> speaker;
        haptics.reserve(request.length / 2);
        if (callbacks_.speaker_pcm) {
          speaker.reserve(request.length / 2);
        }
        for (const auto &packet : request.packets) {
          for (std::size_t frame = packet.offset; frame < packet.offset + packet.length; frame += 8) {
            haptics.insert(haptics.end(), request.data.begin() + frame + 4, request.data.begin() + frame + 8);
            if (callbacks_.speaker_pcm) {
              speaker.insert(speaker.end(), request.data.begin() + frame, request.data.begin() + frame + 4);
            }
          }
        }
        if (callbacks_.haptics_pcm && !haptics.empty()) {
          callbacks_.haptics_pcm(haptics);
        }
        if (callbacks_.speaker_pcm && !speaker.empty()) {
          callbacks_.speaker_pcm(speaker, request.speaker_incarnation);
        }
        reply(request, 0, {}, request.length);
      }
      if (transmit_.size() > max_buffer) {
        fail();
        break;
      }
    }
  }

  void session::control(const urb &request) {
    const auto type = request.setup[0];
    const auto code = request.setup[1];
    const auto value = usb16(request.setup.data() + 2);
    const auto index = usb16(request.setup.data() + 4);
    const auto length = usb16(request.setup.data() + 6);
    const bool input = (type & 0x80) != 0;
    if (input != bool(request.direction) || request.length < length) {
      reply(request, stall);
      return;
    }
    const auto send = [&](std::span<const std::uint8_t> data) {
      reply(request, 0, data.first(std::min<std::size_t>(data.size(), length)));
    };
    const auto accept = [&]() {
      reply(request, 0, {}, request.length);
    };
    const auto reject = [&]() {
      reply(request, stall);
    };
    if (type == 0x23 && code == 3 && value == 4 && !input) {
      // usbip-win2 forwards hub PORT_RESET as a synthetic setup request.
      // The imported composite remains selected after the virtual bus reset.
      configuration_ = 1;
      alternate_.fill(0);
      alternate_explicit_.fill(false);
      next_hid_ = next_audio_ = next_mic_ = {};
      for (const auto &pending : pending_) {
        reply(pending, canceled);
      }
      pending_.clear();
      accept();
      return;
    }
    if ((type & 0x60) == 0) {
      if (code == 6 && input) {  // GET_DESCRIPTOR
        switch (value >> 8) {
          case 1:
            send(device_descriptor);
            return;
          case 2:
            send(configuration_descriptor);
            return;
          case 3:
            {
              const auto number = value & 0xff;
              if (number == 0) {
                constexpr std::uint8_t languages[] {4, 3, 9, 4};
                send(languages);
              } else if (number == 1 || number == 2 || number == 3) {
                send(usb_string(number == 1 ? "Sony Interactive Entertainment" : number == 2 ? "DualSense Wireless Controller" :
                                                                                               "VPDS5" + std::to_string(slot_)));
              } else {
                reject();
              }
              return;
            }
          case 6:
            {  // DEVICE_QUALIFIER
              constexpr std::uint8_t qualifier[] {10, 6, 0, 2, 0, 0, 0, 64, 1, 0};
              send(qualifier);
              return;
            }
          case 7:
            {  // OTHER_SPEED_CONFIGURATION: full-speed intervals
              std::vector<std::uint8_t> other(std::begin(configuration_descriptor), std::end(configuration_descriptor));
              other[1] = 7;
              for (std::size_t offset = 0; offset < other.size(); offset += other[offset]) {
                if (other[offset + 1] == 5) {
                  other[offset + 6] = (other[offset + 3] & 3) == 1 ? 1 : 4;
                }
              }
              send(other);
              return;
            }
          case 0x21:
            if ((index & 0xff) == 3) {
              send(std::span(configuration_descriptor).subspan(hid_descriptor_offset, 9));
            } else {
              reject();
            }
            return;
          case 0x22:
            if ((index & 0xff) == 3) {
              send(lvg::ds5_usb::report_descriptor);
            } else {
              reject();
            }
            return;
          default:
            reject();
            return;
        }
      }
      if (code == 0 && input) {  // GET_STATUS
        const std::uint8_t status[] {std::uint8_t((type & 0x1f) == 0 ? 1 : 0), 0};
        send(status);
        return;
      }
      if ((code == 1 || code == 3 || code == 5) && !input) {
        accept();
        return;
      }
      if (code == 9 && type == 0 && value <= 1) {
        configuration_ = std::uint8_t(value);
        alternate_.fill(0);
        alternate_explicit_.fill(false);
        next_audio_ = next_mic_ = {};
        for (const auto &pending : pending_) {
          reply(pending, canceled);
        }
        pending_.clear();
        accept();
        return;
      }
      if (code == 8 && type == 0x80) {
        send(std::span(&configuration_, 1));
        return;
      }
      if (code == 11 && type == 1 && index < 4 && value <= ((index == 1 || index == 2) ? 1 : 0)) {
        alternate_[index] = std::uint8_t(value);
        alternate_explicit_[index] = true;
        if (value == 0 && (index == 1 || index == 2)) {
          (index == 1 ? next_audio_ : next_mic_) = {};
          for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->endpoint == index) {
              reply(*it, canceled);
              it = pending_.erase(it);
            } else {
              ++it;
            }
          }
        }
        accept();
        return;
      }
      if (code == 10 && type == 0x81 && index < 4) {
        send(std::span(&alternate_[index], 1));
        return;
      }
      reject();
      return;
    }

    if ((type & 0x60) != 0x20) {
      reject();
      return;
    }
    if ((type & 0x1f) == 1 && index == 3) {  // HID class
      const auto id = std::uint8_t(value);
      const auto report_type = value >> 8;
      if (code == 1 && input) {
        if (report_type == 1 && id == 1) {
          std::array<std::uint8_t, 64> report;
          {
            std::lock_guard guard(input_mutex_);
            report = input_;
          }
          send(report);
          return;
        }
        if (report_type == 3) {
          std::array<std::uint8_t, 64> feature {};
          lvg::ds5_usb::feature_state state;
          state.address[0] = slot_;
          const auto size = lvg::ds5_usb::get_feature(id, feature.data(), feature.size(), state);
          if (size) {
            send(std::span(feature).first(size));
          } else if (id == 0x85) {
            feature[0] = id;
            send(std::span(feature).first(3));
          } else {
            reject();
          }
          return;
        }
      }
      if (code == 9 && !input && (report_type == 2 || report_type == 3)) {
        if (report_type == 2 && id == 2 && request.data.size() == 47) {
          // Some HID SET_REPORT callers put the report id only in wValue.
          std::array<std::uint8_t, 48> report;
          report[0] = id;
          std::copy(request.data.begin(), request.data.end(), report.begin() + 1);
          if (callbacks_.hid_output) {
            callbacks_.hid_output(report);
          }
          accept();
          return;
        }
        if (request.data.empty() || request.data[0] != id || request.length > 64) {
          reject();
          return;
        }
        if (report_type == 2 && callbacks_.hid_output) {
          callbacks_.hid_output(request.data);
        }
        accept();
        return;
      }
      if (code == 10 && !input) {
        idle_[3] = std::uint8_t(value >> 8);
        accept();
        return;
      }
      if (code == 2 && input) {
        send(std::span(&idle_[3], 1));
        return;
      }
      if (code == 11 && !input && value <= 1) {
        protocol_[3] = std::uint8_t(value);
        accept();
        return;
      }
      if (code == 3 && input) {
        send(std::span(&protocol_[3], 1));
        return;
      }
      reject();
      return;
    }

    // UAC1 endpoint sampling-frequency controls. Both directions are fixed
    // at 48 kHz and cannot silently switch the captured PCM interpretation.
    if ((type & 0x1f) == 2 && (index == 1 || index == 0x82) && value == 0x0100) {
      constexpr std::uint8_t rate[] {0x80, 0xbb, 0x00};
      if (input && code >= 0x81 && code <= 0x84) {
        send(rate);
        return;
      }
      if (!input && code == 1 && length == 3 && std::equal(std::begin(rate), std::end(rate), request.data.begin())) {
        accept();
        return;
      }
      reject();
      return;
    }
    // UAC1 mute/volume controls, feature units 2 (speaker/haptics) and 5 (mic).
    const auto unit = index >> 8;
    const auto channel = value & 0xff;
    const auto selector = value >> 8;
    if ((type & 0x1f) != 1 || (index & 0xff) != 0 || (unit != 2 && unit != 5) || channel > (unit == 2 ? 4 : 2)) {
      reject();
      return;
    }
    const auto control_index = unit == 2 ? 0 : 1;
    if (selector == 1 && channel == 0) {
      if (input && code == 0x81) {
        send(std::span(&mute_[control_index], 1));
        return;
      }
      if (!input && code == 1 && length == 1) {
        mute_[control_index] = request.data[0] != 0;
        accept();
        return;
      }
    }
    if (selector == 2) {
      if (input && code >= 0x81 && code <= 0x84) {
        const std::int16_t amount = code == 0x81 ? volume_[control_index][channel] :
                                    code == 0x82 ? (unit == 2 ? -25600 : 0) :
                                    code == 0x83 ? (unit == 2 ? 0 : 12288) :
                                                   (unit == 2 ? 256 : 122);
        const std::uint8_t data[] {std::uint8_t(amount), std::uint8_t(std::uint16_t(amount) >> 8)};
        send(data);
        return;
      }
      if (!input && code == 1 && length == 2) {
        volume_[control_index][channel] = std::int16_t(usb16(request.data.data()));
        accept();
        return;
      }
    }
    reject();
  }
}  // namespace platf::dualsense_usbip
