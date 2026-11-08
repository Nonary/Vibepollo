/** Mapping from the GameStream US virtual-key layout to HID keyboard usages. */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace platf::vhf_keyboard_mouse {
  struct mouse_delivery_t {
    std::uint8_t pending_buttons {}, pressed_buttons {};
    bool absolute_pending {};
    long absolute_x {}, absolute_y {};
  };
  inline constexpr std::uint16_t win32_fallback = 0xffff;
  // This key was first pressed while VHF removal was uncertain and was never
  // sent to either backend. Consume its matching release as well.
  inline constexpr std::uint16_t suppressed_fallback = 0xfffe;
  inline constexpr std::uint16_t consumer_route = 0x100;

  struct absolute_point {
    bool inside_primary;
    std::uint16_t x;
    std::uint16_t y;
  };

  // Translate virtual-desktop normalized coordinates to the primary-monitor
  // coordinate domain. Windows' HID mouse mapper uses that domain for the
  // producer's absolute collection. Do not clamp an off-primary target onto
  // the primary display: the caller may use its existing fallback instead.
  constexpr absolute_point map_primary_absolute(std::uint16_t x, std::uint16_t y,
      int virtual_x, int virtual_y, int virtual_width, int virtual_height,
      int primary_width, int primary_height) noexcept {
    if (virtual_width <= 0 || virtual_height <= 0 || primary_width <= 1 || primary_height <= 1) {
      return {false, 0, 0};
    }
    const auto px = static_cast<std::int64_t>(virtual_x) +
      (static_cast<std::int64_t>(x) * (virtual_width - 1) + 32767) / 65535;
    const auto py = static_cast<std::int64_t>(virtual_y) +
      (static_cast<std::int64_t>(y) * (virtual_height - 1) + 32767) / 65535;
    if (px < 0 || py < 0 || px >= primary_width || py >= primary_height) {
      return {false, 0, 0};
    }
    return {true,
      static_cast<std::uint16_t>((px * 65535 + (primary_width - 1) / 2) / (primary_width - 1)),
      static_cast<std::uint16_t>((py * 65535 + (primary_height - 1) / 2) / (primary_height - 1))};
  }

  // usage is evaluated at press; subsequent repeats/releases use that same
  // route, even if the key's layout or client flags change while it is held.
  constexpr std::uint16_t pin_key_route(std::uint16_t &route, bool release, std::uint16_t usage) noexcept {
    if (!release && route == 0) route = usage != 0 ? usage : win32_fallback;
    const auto result = route;
    if (release) route = 0;
    return result;
  }

  struct key_transition {
    std::uint16_t route;
    bool emit;
  };

  // Multiple VKs can resolve to one physical HID usage. Keep that usage down
  // until its last owner releases it, and do not count typematic repeats as
  // new owners. VK identity includes the high byte used by keybindings.
  struct keyboard_routes {
    std::array<std::uint16_t, 65536> keys {};
    std::array<std::uint32_t, 0x200> usages {};

    key_transition update(std::uint16_t key, bool release, std::uint16_t usage) noexcept {
      auto &slot = keys[key];
      const bool was_down = slot != 0;
      const auto route = pin_key_route(slot, release, usage);
      if (route == 0 || route == win32_fallback) return {route, true};
      if (route >= usages.size()) return {win32_fallback, true};
      auto &owners = usages[route];
      if (!release && !was_down) return {route, owners++ == 0};
      if (release && was_down && owners != 0) return {route, --owners == 0};
      return {route, false};
    }
  };

  // After confirmed VHF device removal, release packets for keys owned by the
  // removed child must not become unmatched SendInput releases. Win32 fallback
  // keys still need their normal releases.
  inline bool consume_removed_key(keyboard_routes &keys, std::uint16_t key, bool release) noexcept {
    const auto route = keys.keys[key];
    if (route == 0) return false;
    if (release) keys.update(key, true, 0);
    return route != win32_fallback;
  }

  inline bool consume_removed_button(std::uint8_t &held, std::uint8_t mask, bool release) noexcept {
    if (!mask || !(held & mask)) return false;
    if (release) held &= ~mask;
    return true;
  }

  // Returns zero for keys that need the existing Win32 path (media keys, IME,
  // and VKs with no page-7 equivalent). The high byte marks extended Return.
  constexpr std::uint16_t key_usage(std::uint16_t key) noexcept {
    const auto vk = static_cast<std::uint8_t>(key);
    if (vk >= 0x41 && vk <= 0x5a) return 0x04 + vk - 0x41;
    if (vk >= 0x31 && vk <= 0x39) return 0x1e + vk - 0x31;
    if (vk == 0x30) return 0x27;
    if (vk >= 0x70 && vk <= 0x7b) return 0x3a + vk - 0x70;
    if (vk >= 0x7c && vk <= 0x87) return 0x68 + vk - 0x7c;
    if (vk >= 0x61 && vk <= 0x69) return 0x59 + vk - 0x61;
    switch (vk) {
      case 0x08: return 0x2a; // Backspace
      case 0x09: return 0x2b; // Tab
      case 0x0d: return (key & 0xff00) ? 0x58 : 0x28; // keypad Enter / Enter
      case 0x10: case 0xa0: return 0xe1;
      case 0x11: case 0xa2: return 0xe0;
      case 0x12: case 0xa4: return 0xe2;
      case 0xa1: return 0xe5;
      case 0xa3: return 0xe4;
      case 0xa5: return 0xe6;
      case 0x5b: return 0xe3;
      case 0x5c: return 0xe7;
      case 0x13: return 0x48; // Pause
      case 0x14: return 0x39; // Caps lock
      case 0x1b: return 0x29;
      case 0x20: return 0x2c;
      case 0x21: return 0x4b;
      case 0x22: return 0x4e;
      case 0x23: return 0x4d;
      case 0x24: return 0x4a;
      case 0x25: return 0x50;
      case 0x26: return 0x52;
      case 0x27: return 0x4f;
      case 0x28: return 0x51;
      case 0x2c: return 0x46;
      case 0x2d: return 0x49;
      case 0x2e: return 0x4c;
      case 0x5d: return 0x65;
      case 0x60: return 0x62;
      case 0x6a: return 0x55;
      case 0x6b: return 0x57;
      case 0x6d: return 0x56;
      case 0x6e: return 0x63;
      case 0x6f: return 0x54;
      case 0x90: return 0x53;
      case 0x91: return 0x47;
      case 0xba: return 0x33;
      case 0xbb: return 0x2e;
      case 0xbc: return 0x36;
      case 0xbd: return 0x2d;
      case 0xbe: return 0x37;
      case 0xbf: return 0x38;
      case 0xc0: return 0x35;
      case 0xdb: return 0x2f;
      case 0xdc: return 0x31;
      case 0xdd: return 0x30;
      case 0xde: return 0x34;
      case 0xe2: return 0x64;
      default: return 0;
    }
  }

  // The producer's consumer collection uses the same ordered FIFO as its
  // keyboard and mouse reports. Zero means this VK needs the Win32 fallback.
  constexpr std::uint16_t consumer_usage(std::uint16_t key) noexcept {
    switch (key) {
      case 0xad: return 0xe2; // mute
      case 0xae: return 0xea; // volume down
      case 0xaf: return 0xe9; // volume up
      case 0xb0: return 0xb5; // next track
      case 0xb1: return 0xb6; // previous track
      case 0xb2: return 0xb7; // stop
      case 0xb3: return 0xcd; // play/pause
      default: return 0;
    }
  }

  constexpr std::uint8_t mouse_button(int button) noexcept {
    switch (button) {
      case 1: return 1;
      case 2: return 3;
      case 3: return 2;
      case 4: return 4;
      case 5: return 5;
      default: return 0;
    }
  }

  // Map a Windows set-1 scan code to a physical keyboard-page usage. This is
  // used for client VKs that were not normalized to the US layout.
  constexpr std::uint16_t scan_usage(std::uint16_t scan) noexcept {
    const auto code = static_cast<std::uint8_t>(scan);
    if ((scan & 0xff00) == 0xe000) {
      switch (code) {
        case 0x1c: return 0x58; case 0x1d: return 0xe4;
        case 0x35: return 0x54; case 0x37: return 0x46;
        case 0x38: return 0xe6; case 0x47: return 0x4a;
        case 0x48: return 0x52; case 0x49: return 0x4b;
        case 0x4b: return 0x50; case 0x4d: return 0x4f;
        case 0x4f: return 0x4d; case 0x50: return 0x51;
        case 0x51: return 0x4e; case 0x52: return 0x49;
        case 0x53: return 0x4c; case 0x5b: return 0xe3;
        case 0x5c: return 0xe7; case 0x5d: return 0x65;
        default: return 0;
      }
    }
    if ((scan & 0xff00) != 0) return 0;
    constexpr auto map = [] {
      std::array<std::uint16_t, 128> result {};
      result[0x01] = 0x29;
      for (int i = 0; i < 9; ++i) result[0x02 + i] = 0x1e + i;
      result[0x0b] = 0x27; result[0x0c] = 0x2d; result[0x0d] = 0x2e;
      result[0x0e] = 0x2a; result[0x0f] = 0x2b;
      constexpr std::array<std::uint8_t, 26> letters {
        0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
      };
      for (int i = 0; i < 26; ++i) result[letters[i]] = 0x04 + i;
      result[0x1a] = 0x2f; result[0x1b] = 0x30; result[0x1c] = 0x28;
      result[0x1d] = 0xe0; result[0x27] = 0x33; result[0x28] = 0x34;
      result[0x29] = 0x35; result[0x2a] = 0xe1; result[0x2b] = 0x31;
      result[0x33] = 0x36; result[0x34] = 0x37; result[0x35] = 0x38;
      result[0x36] = 0xe5; result[0x37] = 0x55; result[0x38] = 0xe2;
      result[0x39] = 0x2c; result[0x3a] = 0x39;
      for (int i = 0; i < 10; ++i) result[0x3b + i] = 0x3a + i;
      result[0x45] = 0x53; result[0x46] = 0x47;
      result[0x47] = 0x5f; result[0x48] = 0x60; result[0x49] = 0x61;
      result[0x4a] = 0x56; result[0x4b] = 0x5c; result[0x4c] = 0x5d;
      result[0x4d] = 0x5e; result[0x4e] = 0x57; result[0x4f] = 0x59;
      result[0x50] = 0x5a; result[0x51] = 0x5b; result[0x52] = 0x62;
      result[0x53] = 0x63; result[0x56] = 0x64;
      result[0x57] = 0x44; result[0x58] = 0x45;
      return result;
    }();
    return code < map.size() ? map[code] : 0;
  }

  constexpr bool retain_control_handle(unsigned active_pads, bool keyboard_mouse_owned) noexcept {
    return active_pads != 0 || keyboard_mouse_owned;
  }

  // Only an explicitly validated BUSY is safe to retry. All other errors can
  // represent an accepted event whose response was lost.
  template<class Submit, class Wait>
  std::uint32_t submit_with_backpressure(Submit submit, Wait wait, std::uint32_t busy) {
    std::uint32_t status = busy;
    for (int attempt = 0; attempt < 20 && status == busy; ++attempt) {
      status = submit();
      if (status == busy && attempt != 19) wait();
    }
    return status;
  }

  struct relative_fragment {
    std::int16_t x;
    std::int16_t y;
    std::int8_t wheel;
    std::int8_t pan;
  };

  // Wheel arguments are whole detents accumulated separately by each client.
  // Never merge admitted fragments: relative values describe events.
  template<class Submit>
  bool send_relative(int x, int y, int wheel, int pan,
                     Submit submit) {
    auto wheel_detents = static_cast<std::int64_t>(wheel);
    auto pan_detents = static_cast<std::int64_t>(pan);
    while (x != 0 || y != 0 || wheel_detents != 0 || pan_detents != 0) {
      const relative_fragment part {
        static_cast<std::int16_t>(std::clamp(x, -32768, 32767)),
        static_cast<std::int16_t>(std::clamp(y, -32768, 32767)),
        static_cast<std::int8_t>(std::clamp<std::int64_t>(wheel_detents, -127, 127)),
        static_cast<std::int8_t>(std::clamp<std::int64_t>(pan_detents, -127, 127)),
      };
      if (!submit(part)) return false;
      x -= part.x;
      y -= part.y;
      wheel_detents -= part.wheel;
      pan_detents -= part.pan;
    }
    return true;
  }
}
