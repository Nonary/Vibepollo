// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace dualsense_controls {
  // Session feature flag in x-ml-general.featureFlags (SDP).
  constexpr std::uint32_t session_feature = 0x10;

  // Host -> Client control stream packet carrying DualSense extended outputs.
  constexpr std::uint16_t output_packet_type = 0x5506;
  constexpr std::uint8_t output_version = 1;
  constexpr std::size_t output_payload_size = 12;

  // Bitmask of valid fields in output_packet_type.
  constexpr std::uint16_t out_player_leds = 0x0001;
  constexpr std::uint16_t out_led_brightness = 0x0002;
  constexpr std::uint16_t out_mic_led = 0x0004;
  constexpr std::uint16_t out_speaker_volume = 0x0008;
  constexpr std::uint16_t out_headphone_volume = 0x0010;
  constexpr std::uint16_t out_all_mask = 0x001F;

  // Client -> Host reliable input packet carrying 3.5 mm jack & mic mute state.
  constexpr std::uint32_t audio_status_magic = 0x5500000A;
  constexpr std::size_t audio_status_size = 12;

  // Bitmask of flags in audio_status_magic (matches DS5 USB input byte 54).
  constexpr std::uint8_t audio_headphones_connected = 0x01;
  constexpr std::uint8_t audio_headset_mic_connected = 0x02;
  constexpr std::uint8_t audio_mic_muted = 0x04;
  constexpr std::uint8_t audio_status_mask = 0x07;

  struct output_payload {
    std::uint16_t valid_mask = 0;
    std::uint8_t player_leds = 0;
    std::uint8_t led_brightness = 0;
    std::uint8_t mic_led = 0;
    std::uint8_t speaker_volume = 0;
    std::uint8_t headphone_volume = 0;
  };

  // Includes the 4-byte control_header_v2 (LE type + LE payloadLength).
  inline std::optional<std::array<std::uint8_t, output_payload_size + 4>> encode_output(
    std::uint32_t session_flags,
    std::uint16_t controller,
    const output_payload &out
  ) {
    if (!(session_flags & session_feature) || controller >= 16 ||
        out.valid_mask == 0 || (out.valid_mask & ~out_all_mask) != 0) {
      return std::nullopt;
    }
    if ((out.valid_mask & out_led_brightness) && out.led_brightness > 2) {
      return std::nullopt;
    }
    if ((out.valid_mask & out_mic_led) && out.mic_led > 2) {
      return std::nullopt;
    }
    if ((out.valid_mask & out_headphone_volume) && out.headphone_volume > 0x7F) {
      return std::nullopt;
    }

    std::array<std::uint8_t, output_payload_size + 4> result {};
    result[0] = static_cast<std::uint8_t>(output_packet_type & 0xFF);
    result[1] = static_cast<std::uint8_t>(output_packet_type >> 8);
    result[2] = static_cast<std::uint8_t>(output_payload_size & 0xFF);
    result[3] = static_cast<std::uint8_t>(output_payload_size >> 8);
    result[4] = output_version;
    result[5] = 0;
    result[6] = static_cast<std::uint8_t>(controller & 0xFF);
    result[7] = static_cast<std::uint8_t>(controller >> 8);
    result[8] = static_cast<std::uint8_t>(out.valid_mask & 0xFF);
    result[9] = static_cast<std::uint8_t>(out.valid_mask >> 8);
    result[10] = (out.valid_mask & out_player_leds) ? static_cast<std::uint8_t>(out.player_leds & 0x1F) : 0;
    result[11] = (out.valid_mask & out_led_brightness) ? out.led_brightness : 0;
    result[12] = (out.valid_mask & out_mic_led) ? out.mic_led : 0;
    result[13] = (out.valid_mask & out_speaker_volume) ? out.speaker_volume : 0;
    result[14] = (out.valid_mask & out_headphone_volume) ? out.headphone_volume : 0;
    result[15] = 0;
    return result;
  }

  // Validates and parses the 12-byte Client -> Host audio status input packet.
  inline std::optional<std::pair<std::uint8_t, std::uint8_t>> parse_audio_status(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != audio_status_size || bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 0 || bytes[3] != 8) {
      return std::nullopt;
    }
    const std::uint32_t magic =
      static_cast<std::uint32_t>(bytes[4]) |
      (static_cast<std::uint32_t>(bytes[5]) << 8) |
      (static_cast<std::uint32_t>(bytes[6]) << 16) |
      (static_cast<std::uint32_t>(bytes[7]) << 24);
    if (magic != audio_status_magic || bytes[8] >= 16 || (bytes[9] & ~audio_status_mask) != 0 ||
        bytes[10] != 0 || bytes[11] != 0) {
      return std::nullopt;
    }
    return std::pair {bytes[8], bytes[9]};
  }
}  // namespace dualsense_controls
