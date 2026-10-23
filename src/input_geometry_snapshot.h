#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace platf {
  struct input_geometry_target_identity_t {
    std::int32_t adapter_high_part = 0;
    std::uint32_t adapter_low_part = 0;
    std::wstring output_name;
    std::uintptr_t monitor = 0;
    bool attached_to_desktop = false;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 0;
    int rect_bottom = 0;
    std::uint32_t rotation = 0;
    bool hdr_valid = false;
    bool hdr = false;

    bool operator==(const input_geometry_target_identity_t &) const = default;
  };

  // Snapshot values consumed by session input ports. A generation identifies
  // one capture lifetime; sequence orders geometry/invalidation updates within it.
  struct input_geometry_snapshot_t {
    std::uint64_t capture_generation = 0;
    std::uint64_t geometry_sequence = 0;
    bool valid = true;
    input_geometry_target_identity_t target;
    int offset_x = 0;
    int offset_y = 0;
    int desktop_width = 0;
    int desktop_height = 0;

    bool operator==(const input_geometry_snapshot_t &) const = default;
  };

  // Serializes geometry publication against capture-lifetime termination.
  // A terminal generation can never become valid again; a replacement capture
  // owns a new store with a new capture_generation.
  class input_geometry_snapshot_store_t {
  public:
    input_geometry_snapshot_store_t() = default;
    explicit input_geometry_snapshot_store_t(input_geometry_snapshot_t initial):
        snapshot_ {std::move(initial)} {}

    void initialize(input_geometry_snapshot_t initial) {
      std::lock_guard lock {mutex_};
      if (!terminal_ && !snapshot_) snapshot_ = std::move(initial);
    }

    std::optional<input_geometry_snapshot_t> snapshot() const {
      std::lock_guard lock {mutex_};
      return snapshot_;
    }

    bool publish_mapping(const int offset_x, const int offset_y, const int desktop_width, const int desktop_height) {
      std::lock_guard lock {mutex_};
      if (terminal_ || !snapshot_) return false;
      ++snapshot_->geometry_sequence;
      snapshot_->valid = true;
      snapshot_->offset_x = offset_x;
      snapshot_->offset_y = offset_y;
      snapshot_->desktop_width = desktop_width;
      snapshot_->desktop_height = desktop_height;
      return true;
    }

    bool invalidate() {
      std::lock_guard lock {mutex_};
      if (terminal_ || !snapshot_ || !snapshot_->valid) return false;
      invalidate_locked();
      return true;
    }

    bool terminate_generation() {
      std::lock_guard lock {mutex_};
      if (terminal_) return false;
      terminal_ = true;
      invalidate_locked();
      return true;
    }

    bool terminal() const {
      std::lock_guard lock {mutex_};
      return terminal_;
    }

  private:
    void invalidate_locked() {
      if (!snapshot_ || !snapshot_->valid) return;
      ++snapshot_->geometry_sequence;
      snapshot_->valid = false;
    }

    mutable std::mutex mutex_;
    std::optional<input_geometry_snapshot_t> snapshot_;
    bool terminal_ = false;
  };
}  // namespace platf
