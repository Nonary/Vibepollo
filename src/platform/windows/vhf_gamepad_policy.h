/**
 * @file src/platform/windows/vhf_gamepad_policy.h
 * @brief Declarations for translating normalized gamepad state into the VHF driver protocol.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// lib includes
#include <libvirtualgamepad/protocol.h>

// local includes
#include "src/steam_controller_protocol.h"

namespace platf {

  /**
   * @brief The controller the VHF driver should present.
   */
  enum class vhf_profile_e {
    automatic,  ///< Best available console profile, preferring Xbox Series on the XInput path.
    xbox_series,
    xbox_one,
    dualshock4,
    dualsense,
    switch_pro,
    steam_controller  ///< Valve Steam Controller (2026): two pads, motion, battery, grips.
  };

}  // namespace platf

namespace platf::vhf_gamepad {

  // Contacts are tracked per (touchpad, pointer): a dual-touchpad client reuses
  // pointer id 0 on both pads, and a backend with one pad must still keep the
  // two fingers apart.
  [[nodiscard]] constexpr std::uint64_t touch_pointer_key(std::uint8_t touchpad, std::uint32_t pointer) noexcept {
    return (static_cast<std::uint64_t>(touchpad) << 32) | pointer;
  }

  // Single-touch Steam pads transported as the two halves of a DS4 touchpad.
  // A pointer owns its original half until release. Unknown releases/moves and
  // additional fingers on an occupied pad must not affect the existing owner.
  [[nodiscard]] std::optional<std::uint8_t> map_steam_touch(
    std::map<std::uint64_t, std::uint8_t> &pointers,
    std::uint8_t event, std::uint64_t pointer, float x);

  enum class backend_e {
    unavailable,
    vigem,
    vhf
  };

  /**
   * @brief Selects the backend used by the Automatic gamepad setting.
   * @details ViGEm remains preferred when usable. Vibepollo's VHF driver is the fallback when
   *          ViGEmBus is absent or cannot be opened.
   * @param vigem_available Whether a connection to ViGEmBus succeeded.
   * @param vhf_available Whether Vibepollo's VHF driver exposes a usable controller profile.
   * @return The selected backend, or `unavailable` when neither backend can create controllers.
   */
  [[nodiscard]] constexpr backend_e select_automatic_backend(
    const bool vigem_available,
    const bool vhf_available
  ) noexcept {
    if (vigem_available) {
      return backend_e::vigem;
    }
    if (vhf_available) {
      return backend_e::vhf;
    }
    return backend_e::unavailable;
  }

  /**
   * @brief Chooses the VHF controller profile from the setting and client metadata.
   * @details Explicit profile selections take precedence. Client-reported PlayStation, Xbox,
   *          and Nintendo types win over motion/touchpad preferences, which apply to other pads.
   * @param configured_gamepad The selected gamepad setting.
   * @param client_type The client-reported controller type.
   * @param client_capabilities The client-reported capability flags.
   * @param motion_as_ds4 Whether motion sensors should select a PlayStation profile.
   * @param touchpad_as_ds4 Whether a touchpad should select a PlayStation profile.
   * @return The requested profile; `automatic` prefers the driver's Xbox Series XInput profile.
   */
  [[nodiscard]] vhf_profile_e select_desired_profile(
    std::string_view configured_gamepad,
    std::uint8_t client_type,
    std::uint16_t client_capabilities,
    bool motion_as_ds4,
    bool touchpad_as_ds4
  ) noexcept;

  /// The capability bits only a Steam Controller (2026) client driver reports.
  inline constexpr std::uint16_t STEAM_CONTROLLER_CLIENT_CAPS = LI_CCAP_GRIP_SENSE | LI_CCAP_STEAM_HAPTIC | LI_CCAP_SC26_BUTTONS;

  /**
   * @brief Whether a client controller is a Steam Controller (2026) behind a client that speaks its extensions.
   * @details `LI_CTYPE_STEAM` alone is not enough: a Steam Deck's built-in controls arrive through
   *          SDL with that type and none of the Steam capabilities, and a client forwards other
   *          gamepads (even a keyboard's joystick interface) alongside the real controller. Only
   *          the type together with grip sense, the pad haptic relay or the explicit pad/trigger
   *          clicks earns the Steam Controller profile.
   * @param client_type The client-reported controller type.
   * @param client_capabilities The client-reported capability flags.
   * @return `true` for a Steam Controller (2026) client controller.
   */
  [[nodiscard]] bool is_steam_controller_client(std::uint8_t client_type, std::uint16_t client_capabilities) noexcept;

  /**
   * @brief Reports whether controller selection can create a touchpad-capable profile.
   * @details Capabilities are advertised before controller arrival. Automatic modes can still
   *          select DualSense for a PlayStation client when both selection heuristics are off.
   * @param configured_gamepad The selected gamepad setting.
   * @param composite_dualsense Whether the application forces a composite DualSense.
   * @return Whether the client should be allowed to send controller touch events.
   */
  [[nodiscard]] bool supports_controller_touch(
    std::string_view configured_gamepad,
    bool composite_dualsense
  ) noexcept;

  /**
   * @brief Vibepollo's normalized controller state, copied field-for-field out of `gamepad_state_t`.
   * @details Keeping this struct free of platform headers lets the translation be tested on its own.
   */
  struct normalized_state_t {
    std::uint32_t button_flags {};
    std::uint8_t left_trigger {};
    std::uint8_t right_trigger {};
    std::int16_t left_x {};
    std::int16_t left_y {};
    std::int16_t right_x {};
    std::int16_t right_y {};
  };

  /**
   * @brief One adaptive trigger's effect program.
   */
  struct trigger_effect_t {
    std::uint8_t mode {};
    std::array<std::uint8_t, 10> parameters {};

    bool operator==(const trigger_effect_t &other) const noexcept {
      return mode == other.mode && parameters == other.parameters;
    }
  };

  /**
   * @brief A decoded rumble feedback report from the driver.
   * @details `has_rgb` is false for force-feedback events. A DirectInput effect says nothing
   *          about a light, so forwarding its zeroed colour channels would switch off the LED on
   *          the client's real controller.
   */
  struct rumble_rgb_t {
    std::uint16_t low_frequency {};
    std::uint16_t high_frequency {};
    std::uint8_t red {};
    std::uint8_t green {};
    std::uint8_t blue {};
    bool has_rgb {};

    // Xbox controllers have impulse triggers. The driver sends all four
    // actuators in one event because it keeps a single pending feedback slot,
    // so splitting them would let coalescing drop one.
    std::uint16_t left_trigger {};
    std::uint16_t right_trigger {};
    bool has_triggers {};

    // DualSense adaptive trigger programs, forwarded to the client verbatim.
    trigger_effect_t left_effect {};
    trigger_effect_t right_effect {};
    std::uint8_t trigger_event_flags {};
    bool has_trigger_effects {};

    bool operator==(const rumble_rgb_t &other) const noexcept {
      return low_frequency == other.low_frequency &&
             high_frequency == other.high_frequency &&
             red == other.red && green == other.green && blue == other.blue &&
             has_rgb == other.has_rgb &&
             left_trigger == other.left_trigger &&
             right_trigger == other.right_trigger &&
             has_triggers == other.has_triggers &&
             left_effect == other.left_effect &&
             right_effect == other.right_effect &&
             trigger_event_flags == other.trigger_event_flags &&
             has_trigger_effects == other.has_trigger_effects;
    }
  };

  /**
   * @brief Outcome for the ordinary rumble part of one decoded VHF feedback event.
   */
  enum class rumble_poll_outcome_e {
    raise_attempted,
    deduplicated,
    unavailable
  };

  /**
   * @brief Observation at the VHF poll/decode/ordinary-rumble queue boundary.
   * @details The record contains no raw driver payload or persistent device/client identifier.
   *          Driver slots and client-relative indexes can be reused; the exact motor values expose
   *          feedback activity. `raise_attempted` covers the first decoded sample (including zero
   *          motors) or changed ordinary rumble. A stopped queue may silently reject that attempt,
   *          so it does not mean the message was inserted, serialized, sent, received, or actuated.
   */
  struct rumble_poll_observation_t {
    std::uint32_t driver_slot {};
    std::uint32_t event_type {};
    std::uint32_t payload_size {};
    std::uint16_t client_relative_index {};
    std::uint16_t low_frequency {};
    std::uint16_t high_frequency {};
    rumble_poll_outcome_e outcome {rumble_poll_outcome_e::unavailable};
  };

  /**
   * @brief Per-slot counts folded into a rate-limited debug summary.
   * @details There is one immediate summary, then no more than one summary every five seconds
   *          while decoded reports continue. Counters describe decoded poll observations, not
   *          messages delivered to the client.
   */
  struct rumble_poll_summary_t {
    std::uint32_t driver_slot {};
    std::uint32_t decoded_events {};
    std::uint32_t raise_attempts {};
    std::uint32_t deduplicated_events {};
    std::uint32_t unavailable_events {};
    std::uint32_t poll_errors {};
    std::uint32_t latest_poll_error_status {};
    rumble_poll_observation_t latest {};
    std::chrono::steady_clock::time_point window_started_at {};
    bool has_latest_observation {};
    bool window_started {};
  };

  inline constexpr auto rumble_poll_summary_interval = std::chrono::seconds {5};

  /**
   * @brief Adds one decoded event and reports when a per-slot summary should be emitted.
   * @details This only updates counters and a steady-clock window. The caller logs at debug
   *          severity when this returns true, avoiding per-event string formatting and sink work.
   */
  [[nodiscard]] bool record_rumble_poll_summary(
    rumble_poll_summary_t &summary,
    const rumble_poll_observation_t &observation,
    std::chrono::steady_clock::time_point now
  ) noexcept;

  /** @brief Adds one failed driver poll and reports when the per-slot summary should be emitted. */
  [[nodiscard]] bool record_rumble_poll_error(
    rumble_poll_summary_t &summary,
    std::uint32_t driver_slot,
    std::uint32_t status,
    std::chrono::steady_clock::time_point now
  ) noexcept;

  /** @brief Clears counters after the current summary has been considered for logging. */
  void clear_rumble_poll_summary(rumble_poll_summary_t &summary) noexcept;

  /**
   * @brief Builds the ordinary-rumble observation and determines whether the feedback changes.
   * @details This is shared with the VHF feedback loop so the portable policy test exercises the
   *          same ordinary-rumble queue decision. The first decoded sample attempts a queue call
   *          even when both motors are zero, preserving the backend's existing initial state
   *          behavior. A non-null queue may already be stopped, so the helper reports an attempt,
   *          not successful insertion.
   */
  [[nodiscard]] rumble_poll_observation_t observe_rumble_poll(
    const lvg::feedback_event &event,
    std::uint32_t driver_slot,
    std::uint16_t client_relative_index,
    const rumble_rgb_t &feedback,
    bool queue_present,
    bool have_previous,
    const rumble_rgb_t &previous
  ) noexcept;

  /**
   * @brief Submits the ordinary-rumble message recorded by an observation.
   * @details This shared dispatch helper keeps the VHF feedback path and portable policy test on
   *          the same outcome gate. The message is handed to `admit`, which performs the queue
   *          call under the caller's admission policy and reports whether the queue accepted it.
   *          Production passes `raise_gamepad_feedback`, so a stop (both motors zero) competes as
   *          a protected state key and is not dropped by a mailbox full of ordinary events, as the
   *          plain `raise` would do. A stopped queue rejects the message.
   * @param observation The ordinary-rumble decision and decoded values.
   * @param queue The client feedback queue; a null queue prevents the call.
   * @param make_rumble Creates the platform's gamepad rumble message.
   * @param admit Submits a message to `*queue` and returns whether it was accepted.
   * @return Whether the queue accepted the rumble message.
   */
  template<class Queue, class MakeRumble, class Admit>
  [[nodiscard]] bool dispatch_rumble_poll(
    const rumble_poll_observation_t &observation,
    Queue &queue,
    MakeRumble &&make_rumble,
    Admit &&admit
  ) {
    if (observation.outcome != rumble_poll_outcome_e::raise_attempted || !queue) {
      return false;
    }

    return std::forward<Admit>(admit)(*queue, std::forward<MakeRumble>(make_rumble)(
      observation.client_relative_index,
      observation.low_frequency,
      observation.high_frequency
    ));
  }

  /**
   * @brief Every button the protocol can carry.
   * @details Bits outside this mask are dropped instead of being sent as unspecified wire state.
   */
  inline constexpr std::uint32_t supported_button_mask =
    lvg::button_mask::dpad_up | lvg::button_mask::dpad_down |
    lvg::button_mask::dpad_left | lvg::button_mask::dpad_right |
    lvg::button_mask::start | lvg::button_mask::back |
    lvg::button_mask::left_stick | lvg::button_mask::right_stick |
    lvg::button_mask::left_shoulder | lvg::button_mask::right_shoulder |
    lvg::button_mask::home |
    lvg::button_mask::south | lvg::button_mask::east |
    lvg::button_mask::west | lvg::button_mask::north |
    lvg::button_mask::paddle_1 | lvg::button_mask::paddle_2 |
    lvg::button_mask::paddle_3 | lvg::button_mask::paddle_4 |
    lvg::button_mask::touchpad | lvg::button_mask::misc |
    // Grip sense (LI_CCAP_GRIP_SENSE): carried in the same word; the Steam Controller
    // profile reports them as the capacitive grips and follows the client for good
    // once it has seen one, so dropping them here left the grips dark on every stream.
    lvg::button_mask::left_grip_touch | lvg::button_mask::right_grip_touch |
    // Stick touch (LI_CCAP_STICK_TOUCH): the driver stops guessing it from deflection
    // once a client has sent one of these.
    lvg::button_mask::left_stick_touch | lvg::button_mask::right_stick_touch |
    lvg::button_mask::left_pad_click | lvg::button_mask::right_pad_click |
    lvg::button_mask::left_trigger_click | lvg::button_mask::right_trigger_click;

  /**
   * @brief Selects the preferred public console profile from a driver mask.
   * @details The generic protocol enum values remain reserved, but they have no accepted public
   *          USB PID and must never become an automatic fallback.
   * @param available_profiles The mask returned by the VHF driver.
   * @return The selected console profile, or no value when none is available.
   */
  [[nodiscard]] std::optional<lvg::profile> select_automatic_profile(
    lvg::profile_mask_t available_profiles
  ) noexcept;

  /**
   * @brief Builds a protocol input report for a controller.
   * @param controller_id The driver-side controller slot.
   * @param state The normalized controller state.
   * @return A fully populated request the client can submit as-is.
   */
  [[nodiscard]] lvg::input_state_request make_input_state(
    std::uint32_t controller_id,
    const normalized_state_t &state,
    lvg::profile profile = lvg::profile::xbox_series,
    std::uint16_t client_capabilities = 0
  ) noexcept;

  /**
   * @brief Decodes a driver feedback event into rumble and RGB values.
   * @details Accepts both the rumble/RGB report a vendor output report produces and the
   *          rumble-only report a DirectInput force-feedback effect produces.
   * @param event The event returned by the driver.
   * @param feedback Receives the decoded values when the event carries rumble.
   * @return `true` when `feedback` was populated.
   */
  [[nodiscard]] bool decode_rumble_rgb(const lvg::feedback_event &event, rumble_rgb_t &feedback) noexcept;

  /**
   * @brief A Steam Controller (2026) haptic output report the driver forwarded verbatim.
   * @details `report[0]` is the report id (0x80 rumble, 0x81 pulse, 0x82 command, 0x83 LFO,
   *          0x84 sweep, 0x85 script) and `length` counts it. A client with the controller's
   *          pads replays the report; for the others see `synthesize_steam_rumble`.
   */
  struct steam_haptic_t {
    std::uint8_t length {};
    std::array<std::uint8_t, STEAM_HAPTIC_REPORT_MAX> report {};
  };

  /**
   * @brief Decodes a driver feedback event that carries a Steam Controller haptic report.
   * @return `true` when `haptic` was populated.
   */
  [[nodiscard]] bool decode_steam_haptic(const lvg::feedback_event &event, steam_haptic_t &haptic) noexcept;

  /**
   * @brief A Steam Controller (2026) settings write the driver forwarded verbatim.
   * @details `command` is 0x87 (SET_SETTINGS_VALUES) with `count` (id, lo, hi) triples, at most
   *          nine per event (the driver splits a longer write into events in order), or 0x8E
   *          (LOAD_DEFAULT_SETTINGS) with none. A client with the real controller applies them so
   *          its firmware runs with Steam's settings; the virtual device keeps its own table.
   */
  struct steam_settings_t {
    std::uint8_t command {};
    std::uint8_t count {};
    std::array<std::uint8_t, STEAM_SETTINGS_PAYLOAD_SIZE> triples {};
  };

  // STEAM_SETTINGS_SET_VALUES, STEAM_SETTINGS_LOAD_DEFAULTS and STEAM_SETTINGS_TRIPLES_MAX
  // come from src/steam_controller_protocol.h (the client fork's names).

  /**
   * @brief Decodes a driver feedback event that carries a Steam Controller settings write.
   * @return `true` when `settings` was populated: the payload size matches, the command is one of
   *         the two, and the count is what that command allows.
   */
  [[nodiscard]] bool decode_steam_settings(const lvg::feedback_event &event, steam_settings_t &settings) noexcept;

  /**
   * @brief Rumble rendered from haptic reports for a client without the controller's pads.
   * @details Each side keeps its own hold: 0 means "until the next report for that side",
   *          otherwise the side should fall silent after that many milliseconds. Steam's UI
   *          sends single clicks and re-sends them while it wants a buzz, never an "off".
   */
  struct synthesized_rumble_t {
    std::uint16_t left {};
    std::uint16_t right {};
    std::uint32_t left_hold_ms {};
    std::uint32_t right_hold_ms {};
  };

  /// Sides a haptic report addressed (`synthesize_steam_rumble`).
  constexpr std::uint8_t STEAM_HAPTIC_LEFT = 0x1;
  constexpr std::uint8_t STEAM_HAPTIC_RIGHT = 0x2;
  /// A rendered pulse shorter than this is lost between two feedback polls, and a motor
  /// needs about that long to spin up: Steam's single 400 us UI click becomes this.
  constexpr std::uint32_t STEAM_HAPTIC_MIN_HOLD_MS = 50;
  /// Steam re-sends a test-screen click every 100 ms; the hold outlasts one interval by a
  /// few polls so the stream of clicks reads as one buzz, not an off/on stutter.
  constexpr std::uint32_t STEAM_HAPTIC_CLICK_HOLD_MS = 150;
  /// A 0x80 rumble report with no follow-up: the real unit stops after a safety timeout
  /// (Steam re-sends every <= 50 ms while it rumbles), so the synthesized rumble must stop
  /// too when the sender dies or its zero report is lost.
  constexpr std::uint32_t STEAM_HAPTIC_RUMBLE_TIMEOUT_MS = 200;

  /**
   * @brief Folds a haptic report into the synthesized rumble state.
   * @return The sides the report addressed (`STEAM_HAPTIC_LEFT`/`RIGHT`), 0 when it carried
   *         nothing a rumble motor can render. Only those sides' holds restart.
   */
  [[nodiscard]] std::uint8_t synthesize_steam_rumble(const steam_haptic_t &haptic, synthesized_rumble_t &rumble) noexcept;

  /// Returned for touch events the pads have no equivalent for; the caller drops them.
  constexpr std::uint8_t PROTOCOL_TOUCH_EVENT_UNSUPPORTED = 0xFF;

  /**
   * @brief Converts a client touch event type into the protocol's.
   * @param event_type The client event type.
   * @return The protocol value, or PROTOCOL_TOUCH_EVENT_UNSUPPORTED for anything unmapped.
   */
  [[nodiscard]] std::uint8_t to_protocol_touch_event(std::uint8_t event_type) noexcept;

  /**
   * @brief Converts a client motion type into the protocol's.
   * @param motion_type The client motion type.
   * @return The protocol value, or 0 when the type has no mapping.
   */
  [[nodiscard]] std::uint8_t to_protocol_motion_kind(std::uint8_t motion_type) noexcept;

  /**
   * @brief Converts a client battery state into the protocol's.
   * @param state The client battery state.
   * @return The protocol value.
   */
  [[nodiscard]] std::uint8_t to_protocol_battery_state(std::uint8_t state) noexcept;

  /**
   * @brief Normalizes a 0..1 touch coordinate to the protocol's 0..65535.
   * @param value The client value.
   * @return The normalized value, clamped.
   */
  [[nodiscard]] std::uint16_t to_normalized_touch(float value) noexcept;

  /**
   * @brief Converts a motion sample to the protocol's signed milli-units.
   * @param value Acceleration in m/s^2 or angular velocity in degrees/second.
   * @return The value scaled by 1000 and clamped to the protocol's range.
   */
  [[nodiscard]] std::int32_t to_milli_units(float value) noexcept;

  /**
   * @brief The real Steam Controller's firmware attributes as the client relayed them.
   * @details Mirrors `platf::steam_controller_attributes_t` without the slot id, so the
   *          translation can be tested without the platform headers. `valid_mask` holds the
   *          client's LI_SCATTR_* bits; the strings are NUL padded.
   */
  struct steam_attributes_t {
    std::uint8_t transport {};  ///< LI_SCATTR_TRANSPORT_*
    std::uint8_t valid_mask {};  ///< LI_SCATTR_*
    std::uint32_t firmware_build_time {};
    std::uint32_t bootloader_build_time {};
    std::uint32_t board_revision {};
    std::uint32_t capabilities {};
    std::uint32_t radio_firmware_build_time {};
    std::uint32_t dongle_firmware_build_time {};
    std::array<char, 16> build_id {};
    std::array<char, 16> unit_serial {};
    std::array<char, 16> board_serial {};
  };

  /**
   * @brief What the driver applies to the virtual controller: `lvg::steam_controller_attributes`
   *        field for field.
   * @details A host-side copy so this translation unit and its test build against a driver header
   *          that predates the call; vhf_gamepad.cpp copies it into the driver's struct. `flags`
   *          are LI_SCATTR_* bits, which the driver's `sc_attr_*` values equal by design.
   */
  struct steam_attributes_request_t {
    std::uint32_t flags {};
    std::uint32_t firmware_build_time {};
    std::uint32_t bootloader_build_time {};
    std::uint32_t board_revision {};
    std::uint32_t capabilities {};
    std::array<char, 16> build_id {};
    std::array<char, 16> unit_serial {};
    std::array<char, 16> board_serial {};

    bool operator==(const steam_attributes_request_t &other) const noexcept = default;
  };

  /// The LI_SCATTR_* bits the driver takes. The radio and dongle build times are logged only:
  /// the virtual device is a wired unit and has neither.
  inline constexpr std::uint32_t steam_attributes_driver_mask =
    LI_SCATTR_FIRMWARE_BUILD_TIME | LI_SCATTR_BOOTLOADER_BUILD_TIME | LI_SCATTR_BOARD_REVISION |
    LI_SCATTR_CAPABILITIES | LI_SCATTR_BUILD_ID | LI_SCATTR_SERIALS;

  /**
   * @brief Whether a slot has a use for the real controller's attributes.
   * @param profile The slot's profile.
   * @param client_capabilities The client's LI_CCAP_* flags for the controller.
   * @return `true` only for a Steam Controller slot whose client advertised LI_CCAP_STEAM_ATTRIBUTES.
   */
  [[nodiscard]] bool accepts_steam_attributes(lvg::profile profile, std::uint16_t client_capabilities) noexcept;

  /**
   * @brief Builds the driver request from the client's attributes.
   * @details Flags pass straight through for the fields the driver takes; a field whose bit is
   *          clear is zeroed. Strings keep at most 15 printable ASCII characters (anything else
   *          becomes '?') and are NUL terminated.
   * @param attributes The client's attributes.
   * @return The request to copy into `lvg::steam_controller_attributes`.
   */
  [[nodiscard]] steam_attributes_request_t make_steam_attributes_request(const steam_attributes_t &attributes) noexcept;

  /**
   * @brief Formats unix seconds as `YYYY-MM-DD HH:MM:SS UTC`.
   * @details No locale and no time zone: a firmware build time must read the same in every log.
   * @param seconds Seconds since the epoch.
   * @return The formatted time.
   */
  [[nodiscard]] std::string format_unix_time_utc(std::uint32_t seconds);

  /**
   * @brief Renders the real controller's attributes for the log.
   * @param attributes The client's attributes.
   * @return `real controller firmware build <time> (0x%08x), bootloader <time> (0x%08x), board rev N,
   *         build id X, serial Y, via usb|ble|puck`, with a field whose bit is clear shown as
   *         `unknown`, and the radio/dongle firmware build times appended when present.
   */
  [[nodiscard]] std::string describe_steam_attributes(const steam_attributes_t &attributes);

  /**
   * @brief Whether the driver discarded anything for a slot.
   * @details A quiet controller and one whose output the driver is throwing away look the
   * same from the client; `lvg::feedback_counters` is the only tell, so a slot with any
   * non-zero counter is worth a warning when it is torn down.
   * @param counters The slot's counters from `lvg::client::query_feedback_counters`.
   * @return True when any counter is non-zero.
   */
  [[nodiscard]] bool feedback_counters_notable(const lvg::feedback_counters &counters) noexcept;

  /**
   * @brief Renders a slot's feedback-drop counters for the log.
   * @details Each counter saturates at 255 on the wire, so that value is shown as "255+".
   * @param counters The slot's counters.
   * @return `haptic_evicted=N settings_evicted=N transitions_dropped=N unknown_commands=N`.
   */
  [[nodiscard]] std::string describe_feedback_counters(const lvg::feedback_counters &counters);

}  // namespace platf::vhf_gamepad
