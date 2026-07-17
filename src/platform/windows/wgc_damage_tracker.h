/**
 * @file src/platform/windows/wgc_damage_tracker.h
 * @brief Per-texture damage tracking for Windows Graphics Capture shared rings.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// local includes
#include "ipc/pipes.h"

namespace platf::dxgi {

  struct wgc_damage_rect_t {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
  };

  enum class wgc_damage_copy_kind_e {
    skip,
    partial,
    full,
  };

  struct wgc_damage_copy_plan_t {
    wgc_damage_copy_kind_e kind = wgc_damage_copy_kind_e::full;
    wgc_damage_rect_t rect {};
  };

  /**
   * Tracks the damage each helper-owned ring texture still needs to receive.
   *
   * A shared texture is not necessarily updated on every WGC frame: it may be
   * leased by an encoder, or a newer ready texture may be reclaimed. Every
   * valid frame's damage is therefore accumulated independently for every
   * texture. When a texture becomes available, one bounding-box copy brings it
   * current without risking stale pixels from frames it skipped.
   */
  class wgc_texture_ring_damage_tracker_t {
  public:
    wgc_texture_ring_damage_tracker_t(uint32_t width, uint32_t height):
        _width(width),
        _height(height) {}

    /**
     * Add one frame's ReportOnly dirty regions to every initialized ring slot.
     * Empty regions are valid and represent a no-change frame. Invalid regions
     * make every slot require a conservative full copy.
     *
     * @return `true` when all regions were valid.
     */
    bool accumulate(std::span<const wgc_damage_rect_t> regions) {
      for (const auto &region : regions) {
        if (!is_valid(region)) {
          invalidate();
          return false;
        }
      }

      for (auto &slot : _slots) {
        if (!slot.initialized || slot.requires_full_copy) {
          continue;
        }

        for (const auto &region : regions) {
          slot.damage = union_rect(slot.damage, region);
        }
      }

      return true;
    }

    /** Mark all slots stale after an unsupported or failed dirty-region query. */
    void invalidate() {
      for (auto &slot : _slots) {
        slot.requires_full_copy = true;
        slot.damage.reset();
      }
    }

    [[nodiscard]] wgc_damage_copy_plan_t plan_for_slot(size_t slot_index) const {
      if (slot_index >= _slots.size()) {
        return {};
      }

      const auto &slot = _slots[slot_index];
      if (!slot.initialized || slot.requires_full_copy) {
        return {.kind = wgc_damage_copy_kind_e::full};
      }

      if (!slot.damage) {
        return {.kind = wgc_damage_copy_kind_e::skip};
      }

      if (should_copy_full(*slot.damage)) {
        return {.kind = wgc_damage_copy_kind_e::full};
      }

      return {
        .kind = wgc_damage_copy_kind_e::partial,
        .rect = *slot.damage
      };
    }

    /** Record that a slot now contains the latest captured desktop contents. */
    void mark_copied(size_t slot_index) {
      if (slot_index >= _slots.size()) {
        return;
      }

      auto &slot = _slots[slot_index];
      slot.initialized = true;
      slot.requires_full_copy = false;
      slot.damage.reset();
    }

  private:
    struct slot_damage_t {
      bool initialized = false;
      bool requires_full_copy = false;
      std::optional<wgc_damage_rect_t> damage;
    };

    [[nodiscard]] bool is_valid(const wgc_damage_rect_t &rect) const {
      if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0) {
        return false;
      }

      const auto right = static_cast<int64_t>(rect.x) + rect.width;
      const auto bottom = static_cast<int64_t>(rect.y) + rect.height;
      return right <= _width && bottom <= _height;
    }

    [[nodiscard]] static std::optional<wgc_damage_rect_t> union_rect(const std::optional<wgc_damage_rect_t> &current, const wgc_damage_rect_t &next) {
      if (!current) {
        return next;
      }

      const auto left = (std::min) (current->x, next.x);
      const auto top = (std::min) (current->y, next.y);
      const auto right = (std::max) (static_cast<int64_t>(current->x) + current->width,
                                     static_cast<int64_t>(next.x) + next.width);
      const auto bottom = (std::max) (static_cast<int64_t>(current->y) + current->height,
                                      static_cast<int64_t>(next.y) + next.height);

      return wgc_damage_rect_t {
        .x = left,
        .y = top,
        .width = static_cast<int32_t>(right - left),
        .height = static_cast<int32_t>(bottom - top)
      };
    }

    [[nodiscard]] bool should_copy_full(const wgc_damage_rect_t &rect) const {
      const auto damage_pixels = static_cast<uint64_t>(rect.width) * static_cast<uint64_t>(rect.height);
      const auto frame_pixels = static_cast<uint64_t>(_width) * static_cast<uint64_t>(_height);
      return damage_pixels * 4 >= frame_pixels * 3;
    }

    uint32_t _width;
    uint32_t _height;
    std::array<slot_damage_t, WGC_IPC_TEXTURE_SLOT_COUNT> _slots {};
  };

}  // namespace platf::dxgi
