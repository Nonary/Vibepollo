// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include "src/controller_speaker_pcm.h"
#include "third-party/moonlight-common-c/src/ControllerHaptics.h"

namespace platf::gamepad {
  // Preserve signed waveform samples. Never mix controller speaker channels
  // into the actuators. Packets are 5 ms and fit below the control MTU.
  class ds5_haptics_packetizer {
  public:
    template<class Emit>
    void push(const unsigned char *pcm, std::size_t bytes, Emit emit) {
      if (!pcm || bytes % 8) return;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_input > std::chrono::milliseconds(20)) {
        if (frames) ++sequence;
        frames = 0;
      }
      last_input = now;
      for (std::size_t offset = 0; offset < bytes; offset += 8) {
        std::memcpy(samples.data() + frames * 4, pcm + offset + 4, 4);
        if (++frames == ML_HAPTICS_MAX_FRAMES) {
          emit(sequence++, samples);
          frames = 0;
        }
      }
    }
  private:
    std::array<std::uint8_t, ML_HAPTICS_MAX_FRAMES * ML_HAPTICS_FRAME_BYTES> samples {};
    std::size_t frames = 0;
    std::uint32_t sequence = 0;
    std::chrono::steady_clock::time_point last_input {};
  };

  // The gadget reports four-channel interleaved USB audio. Keep the speaker
  // stream independent of actuator feedback and invalidate queued blocks when
  // the producer is destroyed or its input stream has a discontinuity.
  class ds5_speaker_packetizer {
  public:
    using clock = std::chrono::steady_clock;

    explicit ds5_speaker_packetizer(std::uint64_t owner): owner_(owner), epoch_(std::make_shared<controller_speaker_pcm::producer_epoch>(next_generation())) {}

    ~ds5_speaker_packetizer() { epoch_->reset(0); }

    ds5_speaker_packetizer(const ds5_speaker_packetizer &) = delete;
    ds5_speaker_packetizer &operator=(const ds5_speaker_packetizer &) = delete;

    bool take_first_nonzero(const controller_speaker_pcm::block &block) {
      if (diagnostic_logged_ || std::none_of(block.samples.begin(), block.samples.end(), [](auto sample) { return sample != 0; })) return false;
      diagnostic_logged_ = true;
      return true;
    }

    template<class Emit>
    void push(const unsigned char *pcm, std::size_t bytes, clock::time_point now, Emit emit) {
      if (!owner_ || !bytes || !epoch_->load()) return;
      if (!pcm || bytes % 8 != 0) {
        reset();
        return;
      }
      if (last_input_ != clock::time_point {} && now - last_input_ > std::chrono::milliseconds(50)) reset();
      last_input_ = now;
      for (std::size_t offset = 0; offset < bytes; offset += 8) {
        std::memcpy(samples_.data() + frames_ * 4, pcm + offset, 4);
        if (++frames_ == controller_speaker_pcm::frames_per_block) {
          controller_speaker_pcm::block block {};
          block.owner = owner_;
          block.generation = epoch_->load();
          if (!block.generation) return;
          block.sequence = sequence_++;
          block.sample_position = position_;
          block.flags = position_ == 0 ? 1 : 0;
          block.samples = samples_;
          emit(block, std::weak_ptr<controller_speaker_pcm::producer_epoch>(epoch_));
          position_ += controller_speaker_pcm::frames_per_block;
          frames_ = 0;
          if (sequence_ == 0 || position_ > UINT64_MAX - controller_speaker_pcm::frames_per_block) reset();
        }
      }
    }

  private:
    void reset() {
      frames_ = 0;
      sequence_ = 0;
      position_ = 0;
      last_input_ = {};
      epoch_->reset(next_generation());
    }

    static std::uint32_t next_generation() {
      auto previous = last_generation_.load(std::memory_order_relaxed);
      for (;;) {
        // The client orders generations with signed 32-bit differences.
        if (previous == INT32_MAX) return 0;
        if (last_generation_.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) return previous + 1;
      }
    }

    inline static std::atomic<std::uint32_t> last_generation_ {0};
    const std::uint64_t owner_;
    std::shared_ptr<controller_speaker_pcm::producer_epoch> epoch_;
    std::array<std::uint8_t, controller_speaker_pcm::frames_per_block * 4> samples_ {};
    std::size_t frames_ = 0;
    std::uint32_t sequence_ = 0;
    std::uint64_t position_ = 0;
    clock::time_point last_input_ {};
    bool diagnostic_logged_ = false;
  };
}
