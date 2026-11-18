/**
 * @file src/posture_state.h
 * @brief Process-wide posture bookkeeping: the display size hand-off and per-client runtime state.
 *
 * The launch path records which physical size a virtual display should get, the display
 * backend reads it at create time and reports what it applied, and the `/posture` endpoint
 * updates a client's hints while it streams. Nothing here touches Windows or Boost, and
 * everything is thread-safe.
 */
#pragma once

#include "posture_size.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace posture {
  /// Virtual display GUID bytes, as the display backends receive them.
  using display_key_t = std::array<std::uint8_t, 16>;

  /// Minimum spacing of accepted `/posture` calls per client.
  inline constexpr std::chrono::milliseconds update_min_interval {1000};

  /**
   * @brief Record the size the next create of this display should request.
   *
   * @param key Display GUID bytes.
   * @param size Policy output; nullopt keeps the driver default size.
   * @param client_uuid Paired client that owns the display, used for status.
   */
  void set_desired_size(const display_key_t &key, std::optional<physical_size_t> size, const std::string &client_uuid = {});

  /// Size to pass to the display driver on create, or nullopt for the driver default.
  std::optional<physical_size_t> desired_size(const display_key_t &key);

  /// Record the size a display was actually created with. Called only after a successful create.
  void note_applied_size(const display_key_t &key, std::optional<physical_size_t> size);

  /// Size a display was created with: outer nullopt when unknown, inner nullopt for the driver default.
  std::optional<std::optional<physical_size_t>> applied_size(const display_key_t &key);

  /// A display was removed: forget the size it was created with. The desired size stays for the next create.
  void forget_display(const display_key_t &key);

  /// Every display was removed: forget all created-with sizes.
  void forget_all_displays();

  /**
   * @brief True when a display created with `applied` no longer matches the wanted size.
   *
   * Only "none versus some" counts: a size change between two touch classes keeps the display.
   * An unknown applied state never reports a mismatch.
   */
  bool size_crosses_boundary(const std::optional<std::optional<physical_size_t>> &applied, const std::optional<physical_size_t> &wanted);

  /// Same check against the size policy output for `hints`.
  bool display_crosses_boundary(const std::optional<std::optional<physical_size_t>> &applied, const hints_t &hints);

  /// Record the hints, stream size and host touch diagonal (mm) a client launched or resumed with.
  void record_launch(const std::string &client_uuid, const hints_t &hints, int width_px, int height_px, int diagonal_mm = default_touch_diagonal_mm);

  struct update_result_t {
    enum class status_e {
      no_session,  ///< No launch was recorded for this client.
      rate_limited,  ///< Closer than update_min_interval to the previous accepted call.
      unchanged,  ///< Hints stored; the derived size did not change.
      changed,  ///< Hints stored; the derived size changed and is now pending.
    };

    status_e status = status_e::no_session;
    /// True when the change flips between "no size" and "a size".
    bool boundary_crossed = false;
    /// True when the display created at launch or resume no longer fits the latest hints ("no size" versus "a size").
    /// Stays true until the next launch or resume, and goes back to false when the client returns to its launch state.
    bool display_change = false;
    std::optional<physical_size_t> previous;
    std::optional<physical_size_t> current;
  };

  /**
   * @brief Replace the stored hints of a streaming client and report what changed.
   *
   * The display itself is never touched here; a change stays pending until the next
   * launch or resume reaches the display code.
   */
  update_result_t apply_runtime_update(const std::string &client_uuid, const hints_t &hints, std::chrono::steady_clock::time_point now);

  struct status_t {
    std::optional<client_class_e> posture;  ///< Class the client declared.
    std::optional<client_class_e> client_class;  ///< Class the size policy applies.
    bool paired_touch = false;
    std::optional<physical_size_t> wanted_size;  ///< Policy output for the latest hints.
    bool applied_known = false;
    std::optional<physical_size_t> applied_size;  ///< Size the display was created with; empty is the driver default.
    bool update_pending = false;  ///< A runtime update changed the wanted size after the display was created.
  };

  /// Posture status of one client, or nullopt when it never launched.
  std::optional<status_t> status_for_client(const std::string &client_uuid);
}  // namespace posture
