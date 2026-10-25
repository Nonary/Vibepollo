/**
 * @file src/platform/windows/vhf_gamepad_policy.cpp
 * @brief Definitions for translating normalized gamepad state into the VHF driver protocol.
 */
// local includes
#include "src/steam_controller_protocol.h"
#include "vhf_gamepad_policy.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

namespace platf::vhf_gamepad {

  std::optional<std::uint8_t> map_steam_touch(
    std::map<std::uint64_t, std::uint8_t> &pointers,
    std::uint8_t event, std::uint64_t pointer, float x) {
    const auto kind = static_cast<lvg::touch_event>(event);
    if (kind == lvg::touch_event::cancel_all) {
      pointers.clear();
      return 0;
    }
    auto existing = pointers.find(pointer);
    if (existing == pointers.end()) {
      if (kind != lvg::touch_event::down || !std::isfinite(x)) {
        return std::nullopt;
      }
      const std::uint8_t contact = x >= 0.5f ? 1 : 0;
      for (const auto &[id, occupied] : pointers) {
        if (occupied == contact) return std::nullopt;
      }
      existing = pointers.emplace(pointer, contact).first;
    }
    const auto contact = existing->second;
    if (kind == lvg::touch_event::up || kind == lvg::touch_event::cancel) {
      pointers.erase(existing);
    }
    return contact;
  }

  std::optional<lvg::profile> select_automatic_profile(
    const lvg::profile_mask_t available_profiles
  ) noexcept {
    // Xbox Series stays first because it is the only automatic candidate on
    // the XInput path. Generic profiles are intentionally excluded until they
    // have an accepted public USB PID.
    for (const auto candidate : {
           lvg::profile::xbox_series,
           lvg::profile::dualsense,
           lvg::profile::dualshock_4}) {
      if ((available_profiles & lvg::profile_bit(candidate)) != 0) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  bool is_steam_controller_client(const std::uint8_t client_type, const std::uint16_t client_capabilities) noexcept {
    return client_type == LI_CTYPE_STEAM && (client_capabilities & STEAM_CONTROLLER_CLIENT_CAPS) != 0;
  }

  vhf_profile_e select_desired_profile(
    const std::string_view configured_gamepad,
    const std::uint8_t client_type,
    const std::uint16_t client_capabilities,
    const bool motion_as_ds4,
    const bool touchpad_as_ds4
  ) noexcept {
    if (configured_gamepad == "vhf_ds5") {
      return vhf_profile_e::dualsense;
    }
    if (configured_gamepad == "vhf_ds4") {
      return vhf_profile_e::dualshock4;
    }
    if (configured_gamepad == "vhf_xbox") {
      return vhf_profile_e::xbox_series;
    }
    if (configured_gamepad == "vhf_xbox_one") {
      return vhf_profile_e::xbox_one;
    }
    if (configured_gamepad == "vhf_switch") {
      return vhf_profile_e::switch_pro;
    }
    if (configured_gamepad == "vhf_steam" && is_steam_controller_client(client_type, client_capabilities)) {
      return vhf_profile_e::steam_controller;
    }
    // vhf_steam for anything else falls through to what the controller's own metadata earns:
    // a virtual Steam Controller fed by a client without the pads, the haptic relay and the
    // click flags would be a second, half-dead Steam Controller next to the real one.

    if (is_steam_controller_client(client_type, client_capabilities)) {
      // A client that identifies a Steam Controller gets the real thing: two pads, grips,
      // motion and Steam's own glyphs, instead of a DualSense approximation.
      return vhf_profile_e::steam_controller;
    }
    if (client_type == LI_CTYPE_PS) {
      return vhf_profile_e::dualsense;
    }
    if (client_type == LI_CTYPE_XBOX) {
      return vhf_profile_e::automatic;
    }
    if (client_type == LI_CTYPE_NINTENDO) {
      return vhf_profile_e::switch_pro;
    }
    if (motion_as_ds4 && (client_capabilities & (LI_CCAP_ACCEL | LI_CCAP_GYRO))) {
      return vhf_profile_e::dualsense;
    }
    if (touchpad_as_ds4 && (client_capabilities & LI_CCAP_TOUCHPAD)) {
      return vhf_profile_e::dualsense;
    }
    return vhf_profile_e::automatic;
  }

  bool supports_controller_touch(
    const std::string_view configured_gamepad,
    const bool composite_dualsense
  ) noexcept {
    return composite_dualsense ||
           (configured_gamepad != "x360" &&
            configured_gamepad != "vhf_xbox" &&
            configured_gamepad != "vhf_xbox_one" &&
            configured_gamepad != "vhf_switch");
  }

  lvg::input_state_request make_input_state(
    const std::uint32_t controller_id,
    const normalized_state_t &state,
    const lvg::profile profile,
    const std::uint16_t client_capabilities
  ) noexcept {
    lvg::input_state_request request {};
    request.header.size = sizeof(request);
    request.header.version = lvg::k_protocol_version;
    request.controller_id = controller_id;

    // The protocol's button bits are defined to match Vibepollo's normalized flags, so the
    // supported bits are a straight copy and the driver owns the HID button/hat encoding.
    request.buttons = state.button_flags & supported_button_mask;
    // The client cannot inject this driver metadata bit. Derive it from the
    // arrival capability so an all-released first report is authoritative too.
    if (profile == lvg::profile::steam_controller && (client_capabilities & LI_CCAP_SC26_BUTTONS)) {
      request.buttons |= lvg::button_mask::sc26_buttons_explicit;
    }

    // Axes go over the wire as Vibepollo's normalized state, positive-up. Each
    // driver profile converts to its own device's convention, and they disagree:
    // HID sticks are positive-down while a DualShock 4's are unsigned. Flipping
    // here as well double-inverted both sticks on every profile that does its own
    // conversion, which is all of them.
    request.left_x = state.left_x;
    request.left_y = state.left_y;
    request.right_x = state.right_x;
    request.right_y = state.right_y;
    request.left_trigger = state.left_trigger;
    request.right_trigger = state.right_trigger;
    return request;
  }

  std::uint8_t to_protocol_touch_event(const std::uint8_t event_type) noexcept {
    switch (event_type) {
      case LI_TOUCH_EVENT_HOVER:
        return static_cast<std::uint8_t>(lvg::touch_event::hover);
      case LI_TOUCH_EVENT_DOWN:
        return static_cast<std::uint8_t>(lvg::touch_event::down);
      case LI_TOUCH_EVENT_UP:
        return static_cast<std::uint8_t>(lvg::touch_event::up);
      case LI_TOUCH_EVENT_MOVE:
        return static_cast<std::uint8_t>(lvg::touch_event::move);
      case LI_TOUCH_EVENT_CANCEL:
        return static_cast<std::uint8_t>(lvg::touch_event::cancel);
      case LI_TOUCH_EVENT_CANCEL_ALL:
        return static_cast<std::uint8_t>(lvg::touch_event::cancel_all);
      default:
        // Mapping anything else to cancel_all let one unsupported event lift every
        // contact on the pad; the ViGEm path ignores such events, and so does this one.
        return PROTOCOL_TOUCH_EVENT_UNSUPPORTED;
    }
  }

  std::uint8_t to_protocol_motion_kind(const std::uint8_t motion_type) noexcept {
    switch (motion_type) {
      case LI_MOTION_TYPE_ACCEL:
        return static_cast<std::uint8_t>(lvg::motion_kind::accelerometer);
      case LI_MOTION_TYPE_GYRO:
        return static_cast<std::uint8_t>(lvg::motion_kind::gyroscope);
      default:
        return 0;
    }
  }

  std::uint8_t to_protocol_battery_state(const std::uint8_t state) noexcept {
    switch (state) {
      case LI_BATTERY_STATE_NOT_PRESENT:
        return static_cast<std::uint8_t>(lvg::battery_state::not_present);
      case LI_BATTERY_STATE_DISCHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::discharging);
      case LI_BATTERY_STATE_CHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::charging);
      case LI_BATTERY_STATE_FULL:
        return static_cast<std::uint8_t>(lvg::battery_state::full);
      case LI_BATTERY_STATE_NOT_CHARGING:
        return static_cast<std::uint8_t>(lvg::battery_state::not_charging);
      default:
        return static_cast<std::uint8_t>(lvg::battery_state::unknown);
    }
  }

  std::uint16_t to_normalized_touch(const float value) noexcept {
    if (!(value > 0.0f)) {  // Also catches NaN.
      return 0;
    }
    if (value >= 1.0f) {
      return 65535;
    }
    return static_cast<std::uint16_t>(value * 65535.0f);
  }

  std::int32_t to_milli_units(const float value) noexcept {
    if (std::isnan(value)) {
      return 0;
    }
    const float scaled = value * 1000.0f;
    if (scaled >= 2147483000.0f) {
      return 2147483000;
    }
    if (scaled <= -2147483000.0f) {
      return -2147483000;
    }
    return static_cast<std::int32_t>(scaled);
  }

  rumble_poll_observation_t observe_rumble_poll(
    const lvg::feedback_event &event,
    const std::uint32_t driver_slot,
    const std::uint16_t client_relative_index,
    const rumble_rgb_t &feedback,
    const bool queue_present,
    const bool have_previous,
    const rumble_rgb_t &previous
  ) noexcept {
    const auto outcome = !queue_present ? rumble_poll_outcome_e::unavailable :
                         (!have_previous ||
                          previous.low_frequency != feedback.low_frequency ||
                          previous.high_frequency != feedback.high_frequency) ?
                           rumble_poll_outcome_e::raise_attempted :
                           rumble_poll_outcome_e::deduplicated;

    return {
      driver_slot,
      static_cast<std::uint32_t>(event.type),
      static_cast<std::uint32_t>(event.payload_size),
      client_relative_index,
      feedback.low_frequency,
      feedback.high_frequency,
      outcome
    };
  }

  namespace {
    bool rumble_poll_summary_due(
      rumble_poll_summary_t &summary,
      const std::chrono::steady_clock::time_point now
    ) noexcept {
      if (!summary.window_started) {
        summary.window_started_at = now;
        summary.window_started = true;
        return true;
      }

      if (now - summary.window_started_at < rumble_poll_summary_interval) {
        return false;
      }

      summary.window_started_at = now;
      return true;
    }
  }  // namespace

  bool record_rumble_poll_summary(
    rumble_poll_summary_t &summary,
    const rumble_poll_observation_t &observation,
    const std::chrono::steady_clock::time_point now
  ) noexcept {
    summary.driver_slot = observation.driver_slot;
    ++summary.decoded_events;
    switch (observation.outcome) {
      case rumble_poll_outcome_e::raise_attempted:
        ++summary.raise_attempts;
        break;
      case rumble_poll_outcome_e::deduplicated:
        ++summary.deduplicated_events;
        break;
      case rumble_poll_outcome_e::unavailable:
        ++summary.unavailable_events;
        break;
    }
    summary.latest = observation;
    summary.has_latest_observation = true;

    return rumble_poll_summary_due(summary, now);
  }

  bool record_rumble_poll_error(
    rumble_poll_summary_t &summary,
    const std::uint32_t driver_slot,
    const std::uint32_t status,
    const std::chrono::steady_clock::time_point now
  ) noexcept {
    summary.driver_slot = driver_slot;
    ++summary.poll_errors;
    summary.latest_poll_error_status = status;
    return rumble_poll_summary_due(summary, now);
  }

  void clear_rumble_poll_summary(rumble_poll_summary_t &summary) noexcept {
    summary.decoded_events = 0;
    summary.raise_attempts = 0;
    summary.deduplicated_events = 0;
    summary.unavailable_events = 0;
    summary.poll_errors = 0;
    summary.latest_poll_error_status = 0;
    summary.latest = {};
    summary.has_latest_observation = false;
  }

  bool decode_rumble_rgb(const lvg::feedback_event &event, rumble_rgb_t &feedback) noexcept {
    if (event.type == lvg::feedback_type::playstation_output) {
      if (event.payload_size != sizeof(lvg::playstation_output_feedback)) {
        return false;
      }

      lvg::playstation_output_feedback payload {};
      std::memcpy(&payload, event.payload, sizeof(payload));

      feedback = {};
      feedback.low_frequency = payload.low_frequency;
      feedback.high_frequency = payload.high_frequency;

      if (payload.valid & lvg::ps_output_lightbar_valid) {
        feedback.red = payload.red;
        feedback.green = payload.green;
        feedback.blue = payload.blue;
        feedback.has_rgb = true;
      }

      if (payload.valid & lvg::ps_output_triggers_valid) {
        feedback.left_effect.mode = payload.left_trigger.mode;
        feedback.right_effect.mode = payload.right_trigger.mode;
        std::memcpy(feedback.left_effect.parameters.data(), payload.left_trigger.parameters,
                    feedback.left_effect.parameters.size());
        std::memcpy(feedback.right_effect.parameters.data(), payload.right_trigger.parameters,
                    feedback.right_effect.parameters.size());
        // Both triggers travel in one report, so both are always current.
        feedback.trigger_event_flags = DS_EFFECT_LEFT_TRIGGER | DS_EFFECT_RIGHT_TRIGGER;
        feedback.has_trigger_effects = true;
      }
      return true;
    }

    if (event.type == lvg::feedback_type::xbox_rumble) {
      if (event.payload_size != sizeof(lvg::xbox_rumble_feedback)) {
        return false;
      }

      lvg::xbox_rumble_feedback payload {};
      std::memcpy(&payload, event.payload, sizeof(payload));

      feedback.low_frequency = payload.low_frequency;
      feedback.high_frequency = payload.high_frequency;
      feedback.left_trigger = payload.left_trigger;
      feedback.right_trigger = payload.right_trigger;
      feedback.has_triggers = true;
      feedback.has_rgb = false;
      feedback.red = 0;
      feedback.green = 0;
      feedback.blue = 0;
      return true;
    }

    const bool has_rgb = event.type == lvg::feedback_type::generic_rumble_rgb;
    const bool rumble_only = event.type == lvg::feedback_type::generic_rumble;
    if ((!has_rgb && !rumble_only) ||
        event.payload_size != sizeof(lvg::generic_rumble_rgb_feedback)) {
      return false;
    }

    lvg::generic_rumble_rgb_feedback payload {};
    std::memcpy(&payload, event.payload, sizeof(payload));

    feedback.low_frequency = payload.low_frequency;
    feedback.high_frequency = payload.high_frequency;
    feedback.red = has_rgb ? payload.red : 0;
    feedback.green = has_rgb ? payload.green : 0;
    feedback.blue = has_rgb ? payload.blue : 0;
    feedback.has_rgb = has_rgb;
    feedback.left_trigger = 0;
    feedback.right_trigger = 0;
    feedback.has_triggers = false;
    return true;
  }

  bool decode_steam_haptic(const lvg::feedback_event &event, steam_haptic_t &haptic) noexcept {
    if (event.type != lvg::feedback_type::steam_haptic ||
        event.payload_size != sizeof(lvg::steam_haptic_feedback)) {
      return false;
    }
    lvg::steam_haptic_feedback payload {};
    std::memcpy(&payload, event.payload, sizeof(payload));
    if (payload.length < 2 || payload.length > sizeof(payload.report)) {
      return false;
    }
    haptic.length = payload.length;
    std::memcpy(haptic.report.data(), payload.report, haptic.report.size());
    return true;
  }

  bool decode_steam_settings(const lvg::feedback_event &event, steam_settings_t &settings) noexcept {
    if (event.type != lvg::feedback_type::steam_settings ||
        event.payload_size != sizeof(lvg::steam_settings_feedback)) {
      return false;
    }
    static_assert(sizeof(lvg::steam_settings_feedback::triples) == sizeof(steam_settings_t::triples));
    lvg::steam_settings_feedback payload {};
    std::memcpy(&payload, event.payload, sizeof(payload));
    const bool set_values = payload.command == STEAM_SETTINGS_SET_VALUES &&
                            payload.count >= 1 && payload.count <= STEAM_SETTINGS_TRIPLES_MAX;
    const bool load_defaults = payload.command == STEAM_SETTINGS_LOAD_DEFAULTS && payload.count == 0;
    if (!set_values && !load_defaults) {
      return false;
    }
    settings.command = payload.command;
    settings.count = payload.count;
    std::memcpy(settings.triples.data(), payload.triples, settings.triples.size());
    return true;
  }

  namespace {
    std::uint16_t read_le16(const std::uint8_t *bytes) noexcept {
      return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    }

    // A gain in dB relative to full scale as a 16-bit magnitude; positive gains clip.
    std::uint16_t magnitude_from_db(const std::int8_t gain_db) noexcept {
      if (gain_db >= 0) {
        return 65535;
      }
      return static_cast<std::uint16_t>(std::lround(65535.0 * std::pow(10.0, gain_db / 20.0)));
    }

    // The pads a report's side byte names depend on the report, and the two conventions are
    // opposite (captured from Steam driving the real unit over USB and BLE; the client's
    // SteamControllerBle.isLeftPad applies the same table to the reports it replays):
    //   0x81 pulse:   side 1 = left, 0 = right
    //   0x82 command: side 0 = left, 1 = right
    //   side 2:       both pads, for either report (the stop key in common.h reads it as mask 3)
    // Steam's tick (82 <side> 01 FD) and click (82 <side> 02 F2) therefore land on the motor
    // the pad is on only when 0x82 is read with its own numbering. Returns 0 for a side byte
    // the firmware has no pad for.
    std::uint8_t sides_of(const std::uint8_t report_id, const std::uint8_t side) noexcept {
      if (side == 2) {
        return STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT;
      }
      if (side > 2) {
        return 0;
      }
      const bool left = report_id == 0x82 ? side == 0 : side == 1;
      return left ? STEAM_HAPTIC_LEFT : STEAM_HAPTIC_RIGHT;
    }

    // Applies a magnitude and hold to every side in the mask. Returns the sides touched.
    std::uint8_t set_sides(synthesized_rumble_t &rumble, const std::uint8_t sides, const std::uint16_t magnitude, const std::uint32_t hold_ms) noexcept {
      if (sides & STEAM_HAPTIC_LEFT) {
        rumble.left = magnitude;
        rumble.left_hold_ms = hold_ms;
      }
      if (sides & STEAM_HAPTIC_RIGHT) {
        rumble.right = magnitude;
        rumble.right_hold_ms = hold_ms;
      }
      return sides;
    }
  }  // namespace

  std::uint8_t synthesize_steam_rumble(const steam_haptic_t &haptic, synthesized_rumble_t &rumble) noexcept {
    const auto &report = haptic.report;
    switch (report[0]) {
      case 0x80: {
        // Rumble: type u8, intensity u16, left {speed u16, gain s8}, right {speed u16, gain s8}.
        // The speeds are already the client's motor range. Steam keeps re-sending while it
        // rumbles (every <= 50 ms) and sends zeros to stop; the hold is the safety timeout
        // the real unit applies when those re-sends stop coming.
        if (haptic.length < 10) {
          return 0;
        }
        rumble.left = read_le16(&report[4]);
        rumble.right = read_le16(&report[7]);
        rumble.left_hold_ms = STEAM_HAPTIC_RUMBLE_TIMEOUT_MS;
        rumble.right_hold_ms = STEAM_HAPTIC_RUMBLE_TIMEOUT_MS;
        return STEAM_HAPTIC_LEFT | STEAM_HAPTIC_RIGHT;
      }
      case 0x81: {
        // Pulse: side u8, on_us u16, off_us u16, repeat u16. The duty cycle is the magnitude
        // and the train's length the hold (never shorter than a motor can show), and a
        // zero-repeat pulse (Steam's stop) silences the side.
        if (haptic.length < 8) {
          return 0;
        }
        const std::uint64_t on_us = read_le16(&report[2]);
        const std::uint64_t off_us = read_le16(&report[4]);
        const std::uint64_t repeat = read_le16(&report[6]);
        std::uint16_t magnitude = 0;
        std::uint32_t hold_ms = 0;
        if (repeat != 0 && on_us != 0) {
          const std::uint64_t period_us = on_us + off_us;
          magnitude = static_cast<std::uint16_t>(on_us * 65535u / period_us);
          const std::uint64_t train_ms = (repeat * period_us + 999) / 1000;
          hold_ms = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(train_ms, STEAM_HAPTIC_MIN_HOLD_MS, 10'000));
        }
        return set_sides(rumble, sides_of(0x81, report[1]), magnitude, hold_ms);
      }
      case 0x82: {
        // Command: side u8, command u8 (0 off, 1 tick, 2 click, 3 tone, 4 rumble, 5 noise,
        // 6 script, 7 sweep), gain_db s8. Steam's test screen re-sends a click every 100 ms
        // for as long as it wants the pad to buzz and never sends an off, so a tick or click
        // holds a little longer than that interval; the continuous effects run until their off.
        if (haptic.length < 4) {
          return 0;
        }
        const std::uint8_t command = report[2];
        const std::uint16_t magnitude = command == 0 ? 0 : magnitude_from_db(static_cast<std::int8_t>(report[3]));
        const std::uint32_t hold_ms = (command == 1 || command == 2) ? STEAM_HAPTIC_CLICK_HOLD_MS : 0;
        return set_sides(rumble, sides_of(0x82, report[1]), magnitude, hold_ms);
      }
      default:
        // LFO tone, log sweep, script: nothing a rumble motor can stand in for.
        return 0;
    }
  }

  bool accepts_steam_attributes(const lvg::profile profile, const std::uint16_t client_capabilities) noexcept {
    return profile == lvg::profile::steam_controller && (client_capabilities & LI_CCAP_STEAM_ATTRIBUTES) != 0;
  }

  namespace {
    void copy_printable(std::array<char, 16> &dst, const std::array<char, 16> &src) noexcept {
      dst.fill('\0');
      for (std::size_t i = 0; i < dst.size() - 1 && src[i] != '\0'; ++i) {
        const auto c = static_cast<unsigned char>(src[i]);
        dst[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
      }
    }

    std::string attribute_string(const std::array<char, 16> &value) {
      const auto end = std::find(value.begin(), value.end(), '\0');
      return std::string {value.begin(), end};
    }
  }  // namespace

  steam_attributes_request_t make_steam_attributes_request(const steam_attributes_t &attributes) noexcept {
    steam_attributes_request_t request {};
    request.flags = attributes.valid_mask & steam_attributes_driver_mask;
    if (request.flags & LI_SCATTR_FIRMWARE_BUILD_TIME) {
      request.firmware_build_time = attributes.firmware_build_time;
    }
    if (request.flags & LI_SCATTR_BOOTLOADER_BUILD_TIME) {
      request.bootloader_build_time = attributes.bootloader_build_time;
    }
    if (request.flags & LI_SCATTR_BOARD_REVISION) {
      request.board_revision = attributes.board_revision;
    }
    if (request.flags & LI_SCATTR_CAPABILITIES) {
      request.capabilities = attributes.capabilities;
    }
    if (request.flags & LI_SCATTR_BUILD_ID) {
      copy_printable(request.build_id, attributes.build_id);
    }
    if (request.flags & LI_SCATTR_SERIALS) {
      copy_printable(request.unit_serial, attributes.unit_serial);
      copy_printable(request.board_serial, attributes.board_serial);
    }
    return request;
  }

  std::string format_unix_time_utc(const std::uint32_t seconds) {
    // Howard Hinnant's civil_from_days: no locale, no time zone, no <ctime> re-entrancy question.
    const std::uint32_t days = seconds / 86400;
    const std::uint32_t second_of_day = seconds % 86400;
    const std::uint32_t z = days + 719468;
    const std::uint32_t era = z / 146097;
    const std::uint32_t doe = z - era * 146097;
    const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint32_t mp = (5 * doy + 2) / 153;
    const std::uint32_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::uint32_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::uint32_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u UTC", year, month, day,
                  second_of_day / 3600, (second_of_day / 60) % 60, second_of_day % 60);
    return text;
  }

  std::string describe_steam_attributes(const steam_attributes_t &attributes) {
    const auto build_time = [&](const std::uint32_t flag, const std::uint32_t value) {
      if (!(attributes.valid_mask & flag)) {
        return std::string {"unknown"};
      }
      char hex[16];
      std::snprintf(hex, sizeof(hex), " (0x%08x)", value);
      return format_unix_time_utc(value) + hex;
    };
    const auto text = [&](const std::uint32_t flag, const std::array<char, 16> &value) {
      if (!(attributes.valid_mask & flag) || value[0] == '\0') {
        return std::string {"unknown"};
      }
      std::array<char, 16> printable {};
      copy_printable(printable, value);
      return attribute_string(printable);
    };
    std::string transport;
    switch (attributes.transport) {
      case LI_SCATTR_TRANSPORT_USB:
        transport = "usb";
        break;
      case LI_SCATTR_TRANSPORT_BLE:
        transport = "ble";
        break;
      case LI_SCATTR_TRANSPORT_PUCK:
        transport = "puck";
        break;
      default:
        transport = "transport " + std::to_string(attributes.transport);
        break;
    }

    std::string description = "real controller firmware build " + build_time(LI_SCATTR_FIRMWARE_BUILD_TIME, attributes.firmware_build_time) +
                              ", bootloader " + build_time(LI_SCATTR_BOOTLOADER_BUILD_TIME, attributes.bootloader_build_time) +
                              ", board rev " + ((attributes.valid_mask & LI_SCATTR_BOARD_REVISION) ? std::to_string(attributes.board_revision) : std::string {"unknown"}) +
                              ", build id " + text(LI_SCATTR_BUILD_ID, attributes.build_id) +
                              ", serial " + text(LI_SCATTR_SERIALS, attributes.unit_serial) +
                              ", via " + transport;
    if (attributes.valid_mask & LI_SCATTR_RADIO_FW_BUILD_TIME) {
      description += ", radio firmware build " + build_time(LI_SCATTR_RADIO_FW_BUILD_TIME, attributes.radio_firmware_build_time);
    }
    if (attributes.valid_mask & LI_SCATTR_DONGLE_FW_BUILD_TIME) {
      description += ", dongle firmware build " + build_time(LI_SCATTR_DONGLE_FW_BUILD_TIME, attributes.dongle_firmware_build_time);
    }
    return description;
  }

  bool feedback_counters_notable(const lvg::feedback_counters &counters) noexcept {
    return counters.haptic_evicted != 0 || counters.settings_evicted != 0 ||
           counters.transitions_dropped != 0 || counters.unknown_commands != 0;
  }

  std::string describe_feedback_counters(const lvg::feedback_counters &counters) {
    const auto count = [](const std::uint8_t value) {
      // The driver keeps the full count; the wire saturates at 255.
      return value == 0xFF ? std::string {"255+"} : std::to_string(value);
    };
    return "haptic_evicted=" + count(counters.haptic_evicted) +
           " settings_evicted=" + count(counters.settings_evicted) +
           " transitions_dropped=" + count(counters.transitions_dropped) +
           " unknown_commands=" + count(counters.unknown_commands);
  }

}  // namespace platf::vhf_gamepad
