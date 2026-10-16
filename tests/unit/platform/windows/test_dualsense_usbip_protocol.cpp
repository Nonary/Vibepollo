// SPDX-License-Identifier: GPL-3.0-or-later
#include "libvirtualgamepad/ds5_usb.h"
#include "src/platform/windows/dualsense_usbip_protocol.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace {
  using bytes = std::vector<std::uint8_t>;
  using platf::dualsense_usbip::session;
  int failures = 0;
#define CHECK(condition) \
  do { \
    if (!(condition)) { \
      std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); \
      ++failures; \
    } \
  } while (false)

  void put32(bytes &data, std::size_t offset, std::uint32_t value) {
    data[offset] = std::uint8_t(value >> 24);
    data[offset + 1] = std::uint8_t(value >> 16);
    data[offset + 2] = std::uint8_t(value >> 8);
    data[offset + 3] = std::uint8_t(value);
  }

  std::uint32_t get32(std::span<const std::uint8_t> data, std::size_t offset) {
    return (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16) | (std::uint32_t(data[offset + 2]) << 8) | data[offset + 3];
  }

  bytes import_request(unsigned slot = 0) {
    bytes result(40, 0);
    result[0] = 1;
    result[1] = 0x11;
    result[2] = 0x80;
    result[3] = 3;
    const auto bus = session::bus_id(std::uint8_t(slot));
    std::copy(bus.begin(), bus.end(), result.begin() + 8);
    return result;
  }

  void attach(session &peer, unsigned slot = 0) {
    CHECK(peer.feed(import_request(slot)));
    CHECK(peer.imported());
    const auto result = peer.take_output();
    CHECK(result.size() == 320);
    CHECK(get32(result, 4) == 0);
    const auto exported_path = "/vibepollo/dualsense/" + std::to_string(slot);
    CHECK(std::memcmp(result.data() + 8, exported_path.c_str(), exported_path.size() + 1) == 0);
    CHECK(get32(result, 296) == 1);
    CHECK(get32(result, 300) == slot + 1);
    CHECK(get32(result, 304) == 3);
    CHECK(result[317] == 1 && result[319] == 4);
  }

  bytes submit(std::uint32_t sequence, bool input, unsigned endpoint, unsigned length, const bytes &payload = {}, const std::array<std::uint8_t, 8> &setup = {}, const std::vector<std::array<std::uint32_t, 2>> &packets = {}) {
    bytes result(48 + (input ? 0 : length) + packets.size() * 16, 0);
    put32(result, 0, 1);
    put32(result, 4, sequence);
    put32(result, 8, 0x10001);
    put32(result, 12, input ? 1 : 0);
    put32(result, 16, endpoint);
    put32(result, 24, length);
    put32(result, 28, 0xffffffff);
    put32(result, 32, packets.empty() ? 0xffffffff : std::uint32_t(packets.size()));
    std::copy(setup.begin(), setup.end(), result.begin() + 40);
    if (!input) {
      std::copy(payload.begin(), payload.end(), result.begin() + 48);
    }
    auto offset = 48 + (input ? 0 : length);
    for (const auto &packet : packets) {
      put32(result, offset, packet[0]);
      put32(result, offset + 4, packet[1]);
      offset += 16;
    }
    return result;
  }

  std::array<std::uint8_t, 8> control_setup(std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
    return {type, request, std::uint8_t(value), std::uint8_t(value >> 8), std::uint8_t(index), std::uint8_t(index >> 8), std::uint8_t(length), std::uint8_t(length >> 8)};
  }

  bytes control(session &peer, std::uint8_t type, std::uint8_t code, std::uint16_t value, std::uint16_t index, std::uint16_t length, const bytes &payload = {}) {
    CHECK(peer.feed(submit(1, (type & 0x80) != 0, 0, length, payload, control_setup(type, code, value, index, length))));
    return peer.take_output();
  }

  bytes unlink(std::uint32_t sequence, std::uint32_t victim) {
    bytes result(48, 0);
    put32(result, 0, 2);
    put32(result, 4, sequence);
    put32(result, 20, victim);
    return result;
  }

  void handshake() {
    session peer(0);
    const auto request = import_request();
    for (unsigned i = 0; i < request.size(); ++i) {
      CHECK(peer.feed(std::span(request).subspan(i, 1)));
      CHECK(peer.imported() == (i == request.size() - 1));
    }
    CHECK(peer.take_output().size() == 320);
    session inventory(2);
    CHECK(inventory.feed(bytes {1, 0x11, 0x80, 5, 0, 0, 0, 0}));
    const auto devices = inventory.take_output();
    CHECK(devices.size() == 340 && inventory.finished());
    CHECK(get32(devices, 8) == 1);
    CHECK(std::memcmp(devices.data() + 268, "1-3", 4) == 0);
    CHECK(devices[324] == 1 && devices[325] == 1 && devices[336] == 3);
    session missing(0);
    CHECK(missing.feed(import_request(1)));
    CHECK(missing.finished() && !missing.imported());
    CHECK(get32(missing.take_output(), 4) == 4);
    session bad_version(0);
    CHECK(!bad_version.feed(bytes {2, 0x11, 0x80, 5, 0, 0, 0, 0}));
  }

  void descriptors_and_features() {
    session peer(0);
    attach(peer);
    auto result = control(peer, 0x80, 6, 0x0100, 0, 18);
    CHECK(result.size() == 66 && result[48] == 18 && result[56] == 0x4c && result[58] == 0xe6);
    CHECK(result[64] == 3);  // serial enables usbip-win2 per-session override
    CHECK(get32(result, 8) == 0 && get32(result, 12) == 0 && get32(result, 16) == 0);
    CHECK(get32(result, 32) == 0xffffffff);
    result = control(peer, 0x80, 6, 0x0200, 0, 9);
    CHECK(result.size() == 57 && result[50] == 227 && result[52] == 4);
    result = control(peer, 0x80, 6, 0x0200, 0, 512);
    CHECK(result.size() == 275 && get32(result, 24) == 227);
    std::size_t offset = 48;
    unsigned hid = 0, playback = 0, microphone = 0;
    while (offset < result.size()) {
      CHECK(result[offset] >= 2);
      if (result[offset + 1] == 5) {
        if (result[offset + 2] == 1) {
          CHECK(result[offset + 4] == 0x88 && result[offset + 5] == 1 && result[offset + 6] == 4);
          ++playback;
        }
        if (result[offset + 2] == 0x82) {
          CHECK(result[offset + 4] == 196 && result[offset + 6] == 4);
          ++microphone;
        }
      }
      if (result[offset + 1] == 0x21) {
        CHECK((unsigned(result[offset + 7]) | (unsigned(result[offset + 8]) << 8)) == lvg::ds5_usb::report_descriptor_size);
        ++hid;
      }
      offset += result[offset];
    }
    CHECK(hid == 1 && playback == 1 && microphone == 1);
    result = control(peer, 0x81, 6, 0x2100, 3, 9);
    CHECK(result.size() == 57 && result[48] == 9 && result[49] == 0x21);
    result = control(peer, 0x81, 6, 0x2200, 3, 1024);
    CHECK(result.size() == 48 + lvg::ds5_usb::report_descriptor_size);
    CHECK(std::equal(result.begin() + 48, result.end(), lvg::ds5_usb::report_descriptor));
    result = control(peer, 0xa1, 1, 0x0305, 3, 64);
    CHECK(result.size() == 89 && std::equal(result.begin() + 48, result.end(), lvg::ds5_usb::calibration.begin()));
    result = control(peer, 0xa1, 1, 0x0320, 3, 64);
    CHECK(result.size() == 112 && std::equal(result.begin() + 48, result.end(), lvg::ds5_usb::firmware.begin()));
    CHECK(result.size() >= 94 &&
          (unsigned(result[92]) | (unsigned(result[93]) << 8)) >= 0x0630);
    result = control(peer, 0xa1, 1, 0x0309, 3, 20);
    CHECK(result[48] == 9 && result[55] == 8 && result[56] == 0x25);
    result = control(peer, 0xa1, 1, 0x0385, 3, 3);
    CHECK(result.size() == 51 && result[48] == 0x85);
    result = control(peer, 0x81, 6, 0x2200, 1, 64);
    CHECK(result.size() == 48 && get32(result, 20) == std::uint32_t(-32));
    result = control(peer, 0x80, 6, 0x0303, 0x0409, 255);
    const std::array<std::uint8_t, 14> expected_serial {14, 3, 'V', 0, 'P', 0, 'D', 0, 'S', 0, '5', 0, '0', 0};
    CHECK(result.size() == 48 + expected_serial.size());
    CHECK(std::equal(expected_serial.begin(), expected_serial.end(), result.begin() + 48));
  }

  void hid_and_unlink() {
    bytes output;
    session peer(0, {.hid_output = [&](auto data) {
                       output.assign(data.begin(), data.end());
                     },
                     .haptics_pcm = {}});
    attach(peer);
    bytes raw(64, 0);
    raw[0] = 1;
    raw[1] = 23;
    CHECK(peer.set_input_report(raw));
    CHECK(!peer.set_input_report(bytes(63, 1)));
    CHECK(!peer.set_input_report(bytes(64, 2)));
    CHECK(peer.feed(submit(9, true, 4, 64)));
    CHECK(peer.take_output().empty());
    const auto start = std::chrono::steady_clock::now();
    peer.poll(start);
    auto result = peer.take_output();
    CHECK(result.size() == 112 && result[48] == 1 && result[49] == 23);
    CHECK(peer.feed(submit(10, true, 4, 64)));
    peer.poll(start + std::chrono::milliseconds(3));
    CHECK(peer.take_output().empty());
    peer.poll(start + std::chrono::milliseconds(4));
    CHECK(peer.take_output().size() == 112);
    bytes effects(48, 0);
    effects[0] = 2;
    effects[11] = 0x21;
    CHECK(peer.feed(submit(11, false, 3, 48, effects)));
    result = peer.take_output();
    CHECK(result.size() == 48 && get32(result, 24) == 48 && output == effects);
    effects.erase(effects.begin());
    result = control(peer, 0x21, 9, 0x0202, 3, 47, effects);
    CHECK(result.size() == 48 && output.size() == 48 && output[0] == 2 && output[11] == 0x21);
    CHECK(peer.feed(submit(12, true, 4, 64)));
    CHECK(peer.feed(unlink(13, 12)));
    result = peer.take_output();
    CHECK(result.size() == 48 && get32(result, 0) == 4 && get32(result, 20) == std::uint32_t(-104));
    peer.poll(start + std::chrono::hours(1));
    CHECK(peer.take_output().empty());
    CHECK(peer.feed(unlink(14, 12)));
    CHECK(get32(peer.take_output(), 20) == 0);
  }

  void audio_capture_and_pacing() {
    bytes haptics;
    unsigned callbacks = 0;
    session peer(0, {.hid_output = {}, .haptics_pcm = [&](auto data) {
                       haptics.insert(haptics.end(), data.begin(), data.end());
                       ++callbacks;
                     }});
    attach(peer);
    // Two 48-frame USB packets with a deliberate offset gap. The speaker
    // channels and transfer-buffer padding must never reach the actuators.
    bytes pcm(800, 0xee);
    for (unsigned packet = 0; packet < 2; ++packet) {
      for (unsigned frame = 0; frame < 48; ++frame) {
        const auto offset = packet * 416 + frame * 8;
        pcm[offset] = 0x55;
        pcm[offset + 1] = 0x66;
        pcm[offset + 2] = 0x77;
        pcm[offset + 3] = 0x88;
        pcm[offset + 4] = std::uint8_t(frame);
        pcm[offset + 5] = 0xff;
        pcm[offset + 6] = std::uint8_t(255 - frame);
        pcm[offset + 7] = 0x7f;
      }
    }
    const auto before = std::chrono::steady_clock::now();
    CHECK(peer.feed(submit(21, false, 1, 800, pcm, {}, {{0, 384}, {416, 384}}), before));
    CHECK(peer.feed(submit(22, false, 1, 800, pcm, {}, {{0, 384}, {416, 384}}), before));
    peer.poll(before);
    CHECK(peer.take_output().empty() && callbacks == 0);
    const auto after = before;
    peer.poll(after + std::chrono::milliseconds(2));
    auto result = peer.take_output();
    CHECK(result.size() == 80 && callbacks == 1 && haptics.size() == 384);
    CHECK(get32(result, 24) == 800 && get32(result, 32) == 2);
    CHECK(get32(result, 48) == 0 && get32(result, 52) == 384 && get32(result, 56) == 384);
    CHECK(get32(result, 64) == 416 && get32(result, 72) == 384);
    for (unsigned frame = 0; frame < 96; ++frame) {
      CHECK(haptics[frame * 4] == frame % 48 && haptics[frame * 4 + 1] == 0xff);
      CHECK(haptics[frame * 4 + 2] == 255 - frame % 48 && haptics[frame * 4 + 3] == 0x7f);
    }
    peer.poll(after + std::chrono::milliseconds(4));
    CHECK(peer.take_output().size() == 80 && callbacks == 2);
    CHECK(peer.feed(submit(23, true, 2, 416, {}, {}, {{0, 196}, {220, 196}}), before));
    peer.poll(before + std::chrono::milliseconds(2));
    result = peer.take_output();
    CHECK(result.size() == 48 + 384 + 32 && get32(result, 24) == 384);
    CHECK(std::all_of(result.begin() + 48, result.begin() + 432, [](auto value) {
      return value == 0;
    }));
    CHECK(get32(result, 432) == 0 && get32(result, 440) == 192);
    CHECK(get32(result, 448) == 220 && get32(result, 456) == 192);
    // Explicit alt 0 closes capture, even if an old filter had previously
    // hidden SET_INTERFACE and traffic opened it by inference.
    result = control(peer, 1, 11, 0, 1, 0);
    CHECK(get32(result, 20) == 0);
    CHECK(peer.feed(submit(24, false, 1, 800, pcm, {}, {{0, 384}, {416, 384}})));
    CHECK(get32(peer.take_output(), 20) == std::uint32_t(-32));
  }

  void audio_controls() {
    session peer(0);
    attach(peer);
    auto result = control(peer, 0xa1, 0x82, 0x0200, 0x0200, 2);
    CHECK(result.size() == 50 && result[48] == 0 && result[49] == 0x9c);
    result = control(peer, 0x21, 1, 0x0200, 0x0200, 2, {0x00, 0xfa});
    CHECK(get32(result, 20) == 0);
    result = control(peer, 0xa1, 0x81, 0x0200, 0x0200, 2);
    CHECK(result[48] == 0 && result[49] == 0xfa);
    result = control(peer, 0xa2, 0x81, 0x0100, 1, 3);
    CHECK(result.size() == 51 && result[48] == 0x80 && result[49] == 0xbb && result[50] == 0);
    result = control(peer, 0x22, 1, 0x0100, 1, 3, {0x80, 0xbb, 0});
    CHECK(get32(result, 20) == 0);
    result = control(peer, 0x22, 1, 0x0100, 1, 3, {0x44, 0xac, 0});
    CHECK(get32(result, 20) == std::uint32_t(-32));
  }

  void reset_and_shared_audio_clock() {
    unsigned captures = 0;
    session peer(0, {.hid_output = {}, .haptics_pcm = [&](auto) {
                       ++captures;
                     }});
    attach(peer);
    const bytes pcm(8 * 384);
    std::vector<std::array<std::uint32_t, 2>> out_packets, in_packets;
    for (unsigned i = 0; i < 8; ++i) {
      out_packets.push_back({i * 384, 384});
      in_packets.push_back({i * 192, 192});
    }
    const auto start = std::chrono::steady_clock::now();
    CHECK(peer.feed(submit(51, false, 1, unsigned(pcm.size()), pcm, {}, out_packets), start));
    CHECK(peer.feed(submit(52, true, 2, 8 * 192, {}, {}, in_packets), start));
    peer.poll(start + std::chrono::milliseconds(8));
    auto result = peer.take_output();
    CHECK(result.size() == 176 + 1712 && captures == 1);
    const auto speaker_frame = get32(result, 28);
    const auto mic_frame = get32(result, 176 + 28);
    CHECK(mic_frame == speaker_frame);
    CHECK(peer.feed(submit(53, false, 1, unsigned(pcm.size()), pcm, {}, out_packets)));
    CHECK(peer.feed(submit(54, true, 4, 64)));
    result = control(peer, 0x23, 3, 4, 0, 0);
    CHECK(result.size() == 176 + 48 + 48);
    CHECK(get32(result, 20) == std::uint32_t(-104));
    CHECK(get32(result, 176 + 20) == std::uint32_t(-104));
    CHECK(get32(result, 176 + 48 + 20) == 0);
    peer.poll(std::chrono::steady_clock::now() + std::chrono::hours(1));
    CHECK(peer.take_output().empty() && captures == 1);
    CHECK(peer.feed(submit(55, true, 4, 64)));
    peer.poll();
    CHECK(peer.take_output().size() == 112);
    // Configuration zero cancels the real endpoints until the host selects 1.
    result = control(peer, 0, 9, 0, 0, 0);
    CHECK(get32(result, 20) == 0);
    CHECK(peer.feed(submit(56, true, 4, 64)));
    CHECK(get32(peer.take_output(), 20) == std::uint32_t(-32));
  }

  void malformed_and_fragmented() {
    session peer(0);
    attach(peer);
    const auto descriptor = submit(41, true, 0, 18, {}, control_setup(0x80, 6, 0x0100, 0, 18));
    for (unsigned i = 0; i < descriptor.size(); ++i) {
      CHECK(peer.feed(std::span(descriptor).subspan(i, 1)));
      if (i + 1 < descriptor.size()) {
        CHECK(peer.take_output().empty());
      }
    }
    CHECK(peer.take_output().size() == 66);
    auto oversized = submit(42, true, 4, 0);
    put32(oversized, 24, 0x80000000);
    CHECK(!peer.feed(oversized) && peer.failed());
    session bad_iso(0);
    attach(bad_iso);
    CHECK(!bad_iso.feed(submit(43, false, 1, 8, bytes(8), {}, {{4, 8}})));
    session bad_packets(0);
    attach(bad_packets);
    auto packets = submit(44, true, 2, 64);
    put32(packets, 32, 0xfffffffe);
    CHECK(!bad_packets.feed(packets));
    session wrong_device(0);
    attach(wrong_device);
    auto foreign = submit(45, true, 4, 64);
    put32(foreign, 8, 0x10002);
    CHECK(!wrong_device.feed(foreign));
    session overlap(0);
    attach(overlap);
    CHECK(!overlap.feed(submit(46, false, 1, 16, bytes(16), {}, {{0, 8}, {4, 8}})));
    session partial_frame(0);
    attach(partial_frame);
    CHECK(partial_frame.feed(submit(47, false, 1, 7, bytes(7), {}, {{0, 7}})));
    CHECK(get32(partial_frame.take_output(), 20) == std::uint32_t(-32));
    session partial_mic_frame(0);
    attach(partial_mic_frame);
    CHECK(partial_mic_frame.feed(submit(48, true, 2, 191, {}, {}, {{0, 191}})));
    CHECK(get32(partial_mic_frame.take_output(), 20) == std::uint32_t(-32));
  }
}  // namespace

int main() {
  handshake();
  descriptors_and_features();
  hid_and_unlink();
  audio_capture_and_pacing();
  audio_controls();
  reset_and_shared_audio_clock();
  malformed_and_fragmented();
  if (failures) {
    return 1;
  }
  std::puts("DualSense USB/IP: descriptors, Sony features, fragmented URBs, unlink, audio pacing and actuator PCM passed");
}
