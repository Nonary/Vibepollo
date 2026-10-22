// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace controller_speaker_pcm {
  // The 16-bit controller capability space is exhausted. This alias is
  // explicitly type-scoped: 0x4000 means speaker PCM only for PlayStation
  // controllers; the pending Steam extension uses it for Steam attributes.
  constexpr std::uint16_t playstation_capability = 0x4000;
  constexpr std::uint32_t session_feature = 0x08;
  constexpr std::uint16_t packet_type = 0x5602;
  constexpr std::uint16_t frames_per_block = 240;
  constexpr std::size_t payload_size = 32 + frames_per_block * 4;

  inline bool capable(std::uint8_t type, std::uint16_t capabilities) {
    return type == 0x02 && (capabilities & playstation_capability) != 0;
  }

  // Only final ENet queue admission and reset share this narrow gate. Never
  // acquire the producer slot mutex or flush the network inside enqueue().
  class producer_epoch {
  public:
    explicit producer_epoch(std::uint32_t value) : generation_(value) {}

    std::uint32_t load(std::memory_order order = std::memory_order_acquire) const {
      return generation_.load(order);
    }

    void reset(std::uint32_t value) {
      std::lock_guard lock {enqueue_gate_};
      generation_.store(value, std::memory_order_release);
    }

    template<class Enqueue>
    int enqueue(std::uint32_t expected, Enqueue enqueue_packet) {
      std::lock_guard lock {enqueue_gate_};
      if (!expected || generation_.load(std::memory_order_acquire) != expected) {
        return 0;  // Obsolete feedback is intentionally dropped.
      }
      return enqueue_packet();
    }

  private:
    std::atomic<std::uint32_t> generation_;
    std::mutex enqueue_gate_;
  };

  template<class Enqueue>
  int enqueue_if_current(const std::weak_ptr<producer_epoch> &epoch,
                         std::uint32_t generation, Enqueue enqueue_packet) {
    const auto live = epoch.lock();
    return live ? live->enqueue(generation, enqueue_packet) : 0;
  }

  constexpr std::uint32_t binding_magic = 0x55000009;
  constexpr std::size_t binding_size = 20;

  // Validated independently before any allocation state is modified.
  inline std::optional<std::pair<std::uint16_t, std::uint64_t>> parse_binding(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != binding_size || bytes[0] || bytes[1] || bytes[2] || bytes[3] != 16) return std::nullopt;
    const auto read = [&](unsigned offset, unsigned count) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(bytes[offset + i]) << (i * 8);
      return value;
    };
    if (read(4, 4) != binding_magic || read(8, 2) >= 16 || read(10, 2) != 1 || !read(12, 8)) return std::nullopt;
    return std::pair {std::uint16_t(read(8, 2)), read(12, 8)};
  }

  class owner_bindings {
  public:
    template<class Available>
    bool bind(std::span<const std::uint8_t> bytes, Available available) {
      const auto binding = parse_binding(bytes);
      if (!binding || !available(binding->first)) return false;
      pending_[binding->first] = binding->second;
      return true;
    }
    std::uint64_t take(std::uint16_t slot) {
      return slot < pending_.size() ? std::exchange(pending_[slot], 0) : 0;
    }
  private:
    std::array<std::uint64_t, 16> pending_ {};
  };

  struct block {
    std::uint64_t owner;
    std::uint32_t generation;
    std::uint32_t sequence;
    std::uint64_t sample_position;
    std::uint16_t flags;
    std::array<std::uint8_t, frames_per_block * 4> samples;
  };

  // Includes the four-byte control header. Encryption remains in the existing
  // session-owned encoder, exactly as for actuator PCM.
  inline std::optional<std::array<std::uint8_t, payload_size + 4>> encode(
    std::uint32_t session_flags,
    unsigned control_protocol,
    std::uint16_t controller,
    const block &pcm,
    const std::weak_ptr<producer_epoch> &epoch
  ) {
    const auto live = epoch.lock();
    if (control_protocol != 13 || !(session_flags & session_feature) || controller >= 16 || !pcm.generation || !pcm.owner || pcm.sample_position > UINT64_MAX - frames_per_block ||
        (pcm.flags & ~1u) || !live || live->load(std::memory_order_acquire) != pcm.generation) {
      return std::nullopt;
    }
    std::array<std::uint8_t, payload_size + 4> result {};
    const auto write = [&](std::size_t offset, std::uint64_t value, unsigned bytes) {
      for (unsigned i = 0; i < bytes; ++i) {
        result[offset + i] = std::uint8_t(value >> (8 * i));
      }
    };
    write(0, packet_type, 2);
    write(2, payload_size, 2);
    result[4] = 2;
    result[5] = 1;
    write(6, controller, 2);
    write(8, pcm.generation, 4);
    write(12, pcm.sequence, 4);
    write(16, pcm.sample_position, 8);
    write(24, frames_per_block, 2);
    write(26, pcm.flags, 2);
    write(28, pcm.owner, 8);
    std::copy(pcm.samples.begin(), pcm.samples.end(), result.begin() + 36);
    return result;
  }
}  // namespace controller_speaker_pcm
