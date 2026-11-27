/**
 * @file src/posture_state.cpp
 * @brief Process-wide posture bookkeeping: the display size hand-off and per-client runtime state.
 */
#include "posture_state.h"

#include <map>
#include <mutex>

namespace posture {
  namespace {
    // Unknown state is always safe (it never recreates a display), so a full table is simply reset.
    constexpr std::size_t max_entries = 512;

    struct display_entry_t {
      std::optional<physical_size_t> desired;
      bool applied_known = false;
      std::optional<physical_size_t> applied;
      std::string owner_uuid;
      std::uint64_t sequence = 0;
    };

    struct client_entry_t {
      hints_t hints;
      int width_px = 0;
      int height_px = 0;
      int diagonal_mm = default_touch_diagonal_mm;
      bool update_pending = false;
      bool launch_sized = false;  ///< The display created at launch or resume got a touch size.
      std::optional<std::chrono::steady_clock::time_point> last_update;
    };

    std::mutex state_mutex;
    std::map<display_key_t, display_entry_t> displays;
    std::map<std::string, client_entry_t> clients;
    std::uint64_t display_sequence = 0;

    display_entry_t &display_entry_locked(const display_key_t &key) {
      if (displays.size() >= max_entries && !displays.contains(key)) {
        displays.clear();
      }
      return displays[key];
    }
  }  // namespace

  void set_desired_size(const display_key_t &key, std::optional<physical_size_t> size, const std::string &client_uuid) {
    std::lock_guard lock {state_mutex};
    auto &entry = display_entry_locked(key);
    entry.desired = size;
    entry.sequence = ++display_sequence;
    if (!client_uuid.empty()) {
      entry.owner_uuid = client_uuid;
    }
  }

  std::optional<physical_size_t> desired_size(const display_key_t &key) {
    std::lock_guard lock {state_mutex};
    const auto it = displays.find(key);
    return it == displays.end() ? std::nullopt : it->second.desired;
  }

  void note_applied_size(const display_key_t &key, std::optional<physical_size_t> size) {
    std::lock_guard lock {state_mutex};
    auto &entry = display_entry_locked(key);
    entry.applied_known = true;
    entry.applied = size;
    entry.sequence = ++display_sequence;
  }

  std::optional<std::optional<physical_size_t>> applied_size(const display_key_t &key) {
    std::lock_guard lock {state_mutex};
    const auto it = displays.find(key);
    if (it == displays.end() || !it->second.applied_known) {
      return std::nullopt;
    }
    return it->second.applied;
  }

  void forget_display(const display_key_t &key) {
    std::lock_guard lock {state_mutex};
    if (const auto it = displays.find(key); it != displays.end()) {
      it->second.applied_known = false;
      it->second.applied.reset();
    }
  }

  void forget_all_displays() {
    std::lock_guard lock {state_mutex};
    for (auto &[key, display] : displays) {
      display.applied_known = false;
      display.applied.reset();
    }
  }

  bool size_crosses_boundary(const std::optional<std::optional<physical_size_t>> &applied, const std::optional<physical_size_t> &wanted) {
    if (!applied) {
      return false;
    }
    return applied->has_value() != wanted.has_value();
  }

  bool display_crosses_boundary(const std::optional<std::optional<physical_size_t>> &applied, const hints_t &hints) {
    // Only whether a size applies matters here, so the stream size and diagonal are irrelevant.
    return size_crosses_boundary(applied, derive_physical_size(hints, 0, 0));
  }

  void record_launch(const std::string &client_uuid, const hints_t &hints, int width_px, int height_px, int diagonal_mm) {
    if (client_uuid.empty()) {
      return;
    }
    std::lock_guard lock {state_mutex};
    if (clients.size() >= max_entries && !clients.contains(client_uuid)) {
      clients.clear();
    }
    auto &entry = clients[client_uuid];
    entry.hints = hints;
    entry.width_px = width_px;
    entry.height_px = height_px;
    entry.diagonal_mm = sanitize_touch_diagonal_mm(diagonal_mm);
    entry.update_pending = false;
    entry.launch_sized = derive_physical_size(hints, width_px, height_px, entry.diagonal_mm).has_value();
  }

  update_result_t apply_runtime_update(const std::string &client_uuid, const hints_t &hints, std::chrono::steady_clock::time_point now) {
    update_result_t result;
    std::lock_guard lock {state_mutex};
    const auto it = clients.find(client_uuid);
    if (it == clients.end()) {
      return result;
    }
    auto &entry = it->second;
    if (entry.last_update && now - *entry.last_update < update_min_interval) {
      result.status = update_result_t::status_e::rate_limited;
      return result;
    }
    entry.last_update = now;

    result.previous = derive_physical_size(entry.hints, entry.width_px, entry.height_px, entry.diagonal_mm);
    result.current = derive_physical_size(hints, entry.width_px, entry.height_px, entry.diagonal_mm);
    result.boundary_crossed = result.previous.has_value() != result.current.has_value();
    result.display_change = entry.launch_sized != result.current.has_value();
    result.status = result.previous == result.current ? update_result_t::status_e::unchanged : update_result_t::status_e::changed;

    entry.hints = hints;
    if (result.status == update_result_t::status_e::changed) {
      entry.update_pending = true;
    }
    return result;
  }

  std::optional<status_t> status_for_client(const std::string &client_uuid) {
    std::lock_guard lock {state_mutex};
    const auto it = clients.find(client_uuid);
    if (it == clients.end()) {
      return std::nullopt;
    }
    const auto &entry = it->second;

    status_t status;
    status.posture = entry.hints.posture;
    status.client_class = effective_class(entry.hints);
    status.paired_touch = entry.hints.paired_touch;
    status.wanted_size = derive_physical_size(entry.hints, entry.width_px, entry.height_px, entry.diagonal_mm);
    status.update_pending = entry.update_pending;

    const display_entry_t *latest = nullptr;
    for (const auto &[key, display] : displays) {
      if (display.owner_uuid == client_uuid && display.applied_known && (!latest || display.sequence > latest->sequence)) {
        latest = &display;
      }
    }
    if (latest) {
      status.applied_known = true;
      status.applied_size = latest->applied;
    }
    return status;
  }
}  // namespace posture
