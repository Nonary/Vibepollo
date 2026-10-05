// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "third-party/libvirtualgamepad/driver/src/dualsense.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>

namespace platf::dualsense_usbip_gamepad {
  struct output_update {
    lvg::playstation_output_feedback state {};
    bool accepted = false;
    bool rumble = false;
    bool rgb = false;
    // Only explicitly enabled trigger programs are forwarded. Omitted
    // programs retain their state, including when the other one is turned off.
    std::uint8_t triggers = 0;
  };

  class output_state {
  public:
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
      result.rumble = rumble_valid &&
                      (!have_rumble_ || previous.low_frequency != state_.low_frequency ||
                       previous.high_frequency != state_.high_frequency);
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
      result.state = state_;
      return result;
    }

  private:
    lvg::playstation_output_feedback state_ {};
    bool have_rumble_ = false;
    bool have_left_ = false;
    bool have_right_ = false;
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
