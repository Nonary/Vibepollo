// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "src/dualsense_controls_protocol.h"
#include "third-party/libvirtualgamepad/driver/src/dualsense.h"
#include "vhf_gamepad_policy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace platf::dualsense_usbip_gamepad {
  // A DualSense needs its matching USB audio function for native haptics.
  // Application opt-in only forces that profile and delays launch until ready;
  // it is not required when normal controller selection already chose DualSense.
  [[nodiscard]] inline bool select_composite_dualsense(
    std::string_view configured_gamepad,
    vhf_profile_e selected_profile,
    bool application_requested,
    bool transport_available) {
    return configured_gamepad == "usbip_ds5" || application_requested ||
           (transport_available && selected_profile == vhf_profile_e::dualsense &&
            configured_gamepad != "x360" && configured_gamepad != "ds4");
  }

  // A callback may run every millisecond. Record each boundary once per
  // controller binding, not once per packet or on every state transition.
  enum class pcm_event : unsigned {
    inactive,
    not_ready,
    incapable,
    missing_queue,
    malformed,
    queued,
    queue_rejected,
  };

  inline std::optional<pcm_event> pcm_block_reason(bool active, bool ready, bool waveform_capable, bool has_queue) {
    if (!active) return pcm_event::inactive;
    if (!ready) return pcm_event::not_ready;
    if (!waveform_capable) return pcm_event::incapable;
    if (!has_queue) return pcm_event::missing_queue;
    return std::nullopt;
  }

  class pcm_diagnostics {
  public:
    bool first(pcm_event event) {
      const auto bit = 1u << static_cast<unsigned>(event);
      const bool first = !(seen_ & bit);
      seen_ |= bit;
      return first;
    }

  private:
    unsigned seen_ = 0;
  };

  struct output_update {
    lvg::playstation_output_feedback state {};
    bool accepted = false;
    bool rumble = false;
    bool rgb = false;
    // Only explicitly enabled trigger programs are forwarded. Omitted
    // programs retain their state, including when the other one is turned off.
    std::uint8_t triggers = 0;
    // Extended DualSense controls that changed in this report (valid_mask != 0).
    dualsense_controls::output_payload ds_controls {};
  };

  class output_state {
  public:
    static constexpr std::uint8_t k_ds5_flag0_headphone_volume = 0x10;
    static constexpr std::uint8_t k_ds5_flag0_speaker_volume = 0x20;
    static constexpr std::uint8_t k_ds5_flag2_led_brightness = 0x01;

    output_update apply(std::span<const std::uint8_t> bytes) {
      output_update result;
      if (bytes.size() < sizeof(lvg::driver::ds5_output_report)) {
        return result;
      }
      lvg::driver::ds5_output_report report {};
      std::memcpy(&report, bytes.data(), sizeof(report));
      const auto previous = state_;
      if (!lvg::driver::apply_ds5_output(report, &state_)) {
        return result;
      }
      result.accepted = true;
      const bool rumble_valid =
        (report.valid_flag0 & lvg::driver::k_ds5_flag0_compatible_vibration) ||
        (report.valid_flag2 & lvg::driver::k_ds5_flag2_compatible_vibration);
      // SDL disables emulated rumble with an unflagged zero-motor report.
      // Reports carrying other validity flags can be partial updates, so their
      // unused motor bytes cannot safely be interpreted as a stop.
      const bool rumble_disabled = !report.valid_flag0 && !report.valid_flag1 &&
                                   !report.valid_flag2 && !report.motor_left && !report.motor_right &&
                                   (previous.low_frequency || previous.high_frequency);
      if (rumble_disabled) {
        state_.low_frequency = 0;
        state_.high_frequency = 0;
      }
      result.rumble = rumble_valid &&
                      (!have_rumble_ || previous.low_frequency != state_.low_frequency ||
                       previous.high_frequency != state_.high_frequency);
      result.rumble = result.rumble || rumble_disabled;
      have_rumble_ = have_rumble_ || rumble_valid;
      result.rgb = (report.valid_flag1 & lvg::driver::k_ds5_flag1_lightbar) &&
                   (!(previous.valid & lvg::ps_output_lightbar_valid) ||
                    previous.red != state_.red || previous.green != state_.green ||
                    previous.blue != state_.blue);
      if (report.valid_flag0 & lvg::driver::k_ds5_flag0_left_trigger_effect) {
        if (!have_left_ || std::memcmp(&previous.left_trigger, &state_.left_trigger, sizeof(state_.left_trigger))) {
          result.triggers |= lvg::driver::k_ds5_flag0_left_trigger_effect;
        }
        have_left_ = true;
      }
      if (report.valid_flag0 & lvg::driver::k_ds5_flag0_right_trigger_effect) {
        if (!have_right_ || std::memcmp(&previous.right_trigger, &state_.right_trigger, sizeof(state_.right_trigger))) {
          result.triggers |= lvg::driver::k_ds5_flag0_right_trigger_effect;
        }
        have_right_ = true;
      }

      // Extended DualSense controls: player LEDs, LED brightness, mute button LED,
      // speaker volume (byte 6 = reserved0[1]), headphone volume (byte 5 = reserved0[0]).
      std::uint16_t changed_mask = 0;
      if (report.valid_flag1 & lvg::driver::k_ds5_flag1_player_indicator) {
        const auto leds = static_cast<std::uint8_t>(report.player_leds & 0x1F);
        if (!(ds_controls_seen_ & dualsense_controls::out_player_leds) || ds_controls_.player_leds != leds) {
          ds_controls_.player_leds = leds;
          changed_mask |= dualsense_controls::out_player_leds;
        }
        ds_controls_seen_ |= dualsense_controls::out_player_leds;
      }
      if (report.valid_flag2 & k_ds5_flag2_led_brightness) {
        const auto brightness = static_cast<std::uint8_t>(std::min<std::uint8_t>(report.led_brightness, 2));
        if (!(ds_controls_seen_ & dualsense_controls::out_led_brightness) || ds_controls_.led_brightness != brightness) {
          ds_controls_.led_brightness = brightness;
          changed_mask |= dualsense_controls::out_led_brightness;
        }
        ds_controls_seen_ |= dualsense_controls::out_led_brightness;
      }
      if (report.valid_flag1 & lvg::driver::k_ds5_flag1_mic_mute_led) {
        const auto mic_led = static_cast<std::uint8_t>(std::min<std::uint8_t>(report.mute_button_led, 2));
        if (!(ds_controls_seen_ & dualsense_controls::out_mic_led) || ds_controls_.mic_led != mic_led) {
          ds_controls_.mic_led = mic_led;
          changed_mask |= dualsense_controls::out_mic_led;
        }
        ds_controls_seen_ |= dualsense_controls::out_mic_led;
      }
      if (report.valid_flag0 & k_ds5_flag0_speaker_volume) {
        const auto speaker_vol = report.reserved0[1];
        if (!(ds_controls_seen_ & dualsense_controls::out_speaker_volume) || ds_controls_.speaker_volume != speaker_vol) {
          ds_controls_.speaker_volume = speaker_vol;
          changed_mask |= dualsense_controls::out_speaker_volume;
        }
        ds_controls_seen_ |= dualsense_controls::out_speaker_volume;
      }
      if (report.valid_flag0 & k_ds5_flag0_headphone_volume) {
        const auto hp_vol = static_cast<std::uint8_t>(std::min<std::uint8_t>(report.reserved0[0], 0x7F));
        if (!(ds_controls_seen_ & dualsense_controls::out_headphone_volume) || ds_controls_.headphone_volume != hp_vol) {
          ds_controls_.headphone_volume = hp_vol;
          changed_mask |= dualsense_controls::out_headphone_volume;
        }
        ds_controls_seen_ |= dualsense_controls::out_headphone_volume;
      }
      if (changed_mask != 0) {
        result.ds_controls = ds_controls_;
        result.ds_controls.valid_mask = changed_mask;
      }

      result.state = state_;
      return result;
    }

    [[nodiscard]] dualsense_controls::output_payload current_ds_controls() const noexcept {
      auto copy = ds_controls_;
      copy.valid_mask = ds_controls_seen_;
      return copy;
    }

  private:
    lvg::playstation_output_feedback state_ {};
    bool have_rumble_ = false;
    bool have_left_ = false;
    bool have_right_ = false;
    dualsense_controls::output_payload ds_controls_ {};
    std::uint16_t ds_controls_seen_ = 0;
  };

  // The USB endpoint already supplies stereo 48 kHz S16LE. Preserve sample
  // bytes verbatim, collect 240 frames, and drop incomplete stale blocks.
  class pcm_packetizer {
  public:
    void reset() {
      used_ = 0;
      last_ = {};
    }

    template<class Emit>
    void push(std::span<const std::uint8_t> bytes, std::chrono::steady_clock::time_point now, Emit emit) {
      if (bytes.size() % 4 != 0) {
        reset();
        return;
      }
      if (last_ != std::chrono::steady_clock::time_point {} && now - last_ > std::chrono::milliseconds {50}) {
        used_ = 0;
      }
      last_ = now;
      while (!bytes.empty()) {
        const auto copied = std::min(samples_.size() - used_, bytes.size());
        std::copy_n(bytes.begin(), copied, samples_.begin() + used_);
        used_ += copied;
        bytes = bytes.subspan(copied);
        if (used_ == samples_.size()) {
          emit(samples_);
          used_ = 0;
        }
      }
    }

  private:
    std::array<std::uint8_t, 960> samples_ {};
    std::size_t used_ = 0;
    std::chrono::steady_clock::time_point last_ {};
  };
}  // namespace platf::dualsense_usbip_gamepad
