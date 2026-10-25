/**
 * @file src/platform/windows/vhf_gamepad.cpp
 * @brief Definitions for the Vibepollo VHF virtual gamepad input backend.
 */
#define WINVER 0x0A00

// platform includes
#include <WinSock2.h>
#include <Windows.h>

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

// lib includes
#include <libvirtualgamepad/client.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/steam_controller_protocol.h"
#include "src/utility.h"
#include "vhf_gamepad.h"
#include "dualsense_haptics.h"
#include "vhf_gamepad_policy.h"
#include "vhf_gamepad_recovery.h"

namespace platf {
  using namespace std::literals;

  namespace {
    // The driver coalesces output reports into one pending slot per controller, so the poll rate
    // only bounds how quickly rumble reaches the client. 8ms keeps that under a frame at 120 FPS.
    constexpr auto k_feedback_poll_interval = 8ms;
    constexpr int k_feedback_drain_per_poll = 4;

    // The wire protocol reuses Vibepollo's normalized button values verbatim. Pin that here so a
    // change on either side breaks the build instead of silently remapping every controller.
    static_assert(DPAD_UP == lvg::button_mask::dpad_up);
    static_assert(DPAD_DOWN == lvg::button_mask::dpad_down);
    static_assert(DPAD_LEFT == lvg::button_mask::dpad_left);
    static_assert(DPAD_RIGHT == lvg::button_mask::dpad_right);
    static_assert(START == lvg::button_mask::start);
    static_assert(BACK == lvg::button_mask::back);
    static_assert(LEFT_STICK == lvg::button_mask::left_stick);
    static_assert(RIGHT_STICK == lvg::button_mask::right_stick);
    static_assert(LEFT_BUTTON == lvg::button_mask::left_shoulder);
    static_assert(RIGHT_BUTTON == lvg::button_mask::right_shoulder);
    static_assert(HOME == lvg::button_mask::home);
    static_assert(A == lvg::button_mask::south);
    static_assert(B == lvg::button_mask::east);
    static_assert(X == lvg::button_mask::west);
    static_assert(Y == lvg::button_mask::north);
    static_assert(PADDLE1 == lvg::button_mask::paddle_1);
    static_assert(PADDLE2 == lvg::button_mask::paddle_2);
    static_assert(PADDLE3 == lvg::button_mask::paddle_3);
    static_assert(PADDLE4 == lvg::button_mask::paddle_4);
    static_assert(TOUCHPAD_BUTTON == lvg::button_mask::touchpad);
    static_assert(MISC_BUTTON == lvg::button_mask::misc);
    // Grip sense (LI_CCAP_GRIP_SENSE) rides along in the same button word; the driver
    // treats it as the Steam Controller's capacitive grips, not as buttons.
    static_assert(LEFT_GRIP_TOUCH == lvg::button_mask::left_grip_touch);
    static_assert(RIGHT_GRIP_TOUCH == lvg::button_mask::right_grip_touch);
    static_assert(LEFT_STICK_TOUCH == lvg::button_mask::left_stick_touch);
    static_assert(RIGHT_STICK_TOUCH == lvg::button_mask::right_stick_touch);
    static_assert(LEFT_PAD_CLICK == lvg::button_mask::left_pad_click);
    static_assert(RIGHT_PAD_CLICK == lvg::button_mask::right_pad_click);
    static_assert(LEFT_TRIGGER_CLICK == lvg::button_mask::left_trigger_click);
    static_assert(RIGHT_TRIGGER_CLICK == lvg::button_mask::right_trigger_click);

    // Global indices address driver slots directly, so the two limits have to agree.
    static_assert(MAX_GAMEPADS <= lvg::k_max_controllers);

    // The client's LI_SCATTR_* bits are handed to the driver as its sc_attr_* flags unchanged.
    static_assert(lvg::sc_attr_firmware_build_time == LI_SCATTR_FIRMWARE_BUILD_TIME);
    static_assert(lvg::sc_attr_bootloader_build_time == LI_SCATTR_BOOTLOADER_BUILD_TIME);
    static_assert(lvg::sc_attr_board_revision == LI_SCATTR_BOARD_REVISION);
    static_assert(lvg::sc_attr_capabilities == LI_SCATTR_CAPABILITIES);
    static_assert(lvg::sc_attr_build_id == LI_SCATTR_BUILD_ID);
    static_assert(lvg::sc_attr_serials == LI_SCATTR_SERIALS);

    struct slot_t {
      bool active {};
      std::uint16_t client_relative_index {};
      lvg::profile profile {};
      // Durable client state, guarded by lifetime exclusively even while recovery is pending.
      // Touch contacts and time-dependent motion samples belong to the old device epoch.
      std::optional<lvg::input_state_request> last_input;
      std::optional<lvg::battery_state_request> last_battery;
      // Unknown percentage updates preserve the producer level. Replay its original
      // percentage-bearing request first, with its original cable/charging flags.
      std::optional<lvg::battery_state_request> battery_level_seed;

      // A client pointer id is an arbitrary handle; the touchpad has two
      // numbered contacts. Reserving a contact per pointer keeps a moved touch
      // on the slot it went down on.
      std::map<std::uint64_t, std::uint8_t> contact_of_pointer;  // keyed by touch_pointer_key
      std::uint8_t free_contacts {0x3};
      // The client's LI_CCAP_* flags for this controller (0 when unknown).
      std::uint16_t client_capabilities {};
      feedback_queue_t feedback_queue;

      // Written while only the shared lock is held, so concurrent input threads cannot race.
      std::atomic<bool> submit_failed {false};

      // Only the feedback thread touches these.
      bool have_feedback {};
      vhf_gamepad::rumble_rgb_t last_feedback {};
      // Rumble stood in for Steam Controller haptics on a client without the pads, with the
      // moment each side falls silent on its own (no deadline while the hold is 0).
      vhf_gamepad::synthesized_rumble_t synth_rumble {};
      std::optional<std::chrono::steady_clock::time_point> synth_left_until;
      std::optional<std::chrono::steady_clock::time_point> synth_right_until;
      vhf_gamepad::rumble_poll_summary_t rumble_poll_summary {};
      // The real Steam Controller's firmware attributes as last handed to the driver, replayed
      // after device-removal recovery because create_controller resets them.
      std::optional<lvg::steam_controller_attributes> steam_attributes;

      void reset() {
        active = false;
        client_relative_index = 0;
        profile = {};
        last_input.reset();
        last_battery.reset();
        battery_level_seed.reset();
        contact_of_pointer.clear();
        free_contacts = 0x3;
        client_capabilities = 0;
        feedback_queue.reset();
        submit_failed.store(false, std::memory_order_relaxed);
        synth_rumble = {};
        synth_left_until.reset();
        synth_right_until.reset();
        steam_attributes.reset();
        have_feedback = false;
        last_feedback = {};
        // Keep the per-driver-slot rate-limit window across controller reuse.
        vhf_gamepad::clear_rumble_poll_summary(rumble_poll_summary);
      }
    };
  }  // namespace

  namespace {
    /**
     * @brief Picks a requested public console profile the connected driver offers.
     * @details Xbox Series reaches XInput; the PlayStation profiles add a touchpad, motion,
     *          battery, and a lightbar. Generic profile enum values remain reserved and are not
     *          eligible for automatic selection until they have an accepted public USB PID.
     * @param client The connected driver client.
     * @param selected Receives the chosen profile.
     * @return `true` when the driver offers a usable profile.
     */
    bool offers(const lvg::client &client, const lvg::profile profile) {
      return (client.available_profiles() & lvg::profile_bit(profile)) != 0;
    }

    bool select_profile(
      const lvg::client &client,
      const vhf_profile_e desired,
      lvg::profile &selected) {
      // An explicit choice is honoured or refused; silently substituting a
      // different controller would be worse than telling the caller no.
      switch (desired) {
        case vhf_profile_e::dualsense:
          if (offers(client, lvg::profile::dualsense)) {
            selected = lvg::profile::dualsense;
            return true;
          }
          return false;
        case vhf_profile_e::dualshock4:
          if (offers(client, lvg::profile::dualshock_4)) {
            selected = lvg::profile::dualshock_4;
            return true;
          }
          return false;
        case vhf_profile_e::xbox_series:
          if (offers(client, lvg::profile::xbox_series)) {
            selected = lvg::profile::xbox_series;
            return true;
          }
          return false;
        case vhf_profile_e::xbox_one:
          if (offers(client, lvg::profile::xbox_one)) {
            selected = lvg::profile::xbox_one;
            return true;
          }
          return false;
        case vhf_profile_e::switch_pro:
          if (offers(client, lvg::profile::switch_pro)) {
            selected = lvg::profile::switch_pro;
            return true;
          }
          return false;
        case vhf_profile_e::steam_controller:
          if (offers(client, lvg::profile::steam_controller)) {
            selected = lvg::profile::steam_controller;
            return true;
          }
          return false;
        case vhf_profile_e::automatic:
          break;
      }

      const auto automatic = vhf_gamepad::select_automatic_profile(client.available_profiles());
      if (automatic) {
        selected = *automatic;
        return true;
      }
      return false;
    }

    bool is_playstation(const lvg::profile profile) {
      return profile == lvg::profile::dualshock_4 || profile == lvg::profile::dualsense;
    }

    bool is_steam_controller(const lvg::profile profile) {
      return profile == lvg::profile::steam_controller;
    }

    // Profiles whose controller has at least one touchpad. The PlayStation pad
    // is one two-finger surface; the Steam Controller has two single-touch pads
    // addressed by the client's touchpad index.
    bool has_touch(const lvg::profile profile) {
      return is_playstation(profile) || is_steam_controller(profile);
    }

    // Profiles whose controller carries motion sensors and a battery. The
    // Switch Pro has both but no touchpad, so the two are not the same set.
    bool has_motion(const lvg::profile profile) {
      return is_playstation(profile) || profile == lvg::profile::switch_pro ||
             is_steam_controller(profile);
    }

    /**
     * @brief Names a profile for the log.
     * @param profile The profile.
     * @return A short human readable description.
     */
    std::string_view describe_profile(const lvg::profile profile) {
      switch (profile) {
        case lvg::profile::xbox_series:
          return "an Xbox Series controller on the XInput path"sv;
        case lvg::profile::xbox_one:
          return "an Xbox One controller on the XInput path"sv;
        case lvg::profile::dualsense:
          return "a DualSense controller"sv;
        case lvg::profile::dualshock_4:
          return "a DualShock 4 controller"sv;
        case lvg::profile::switch_pro:
          return "a Switch Pro Controller"sv;
        case lvg::profile::steam_controller:
          return "a Steam Controller (2026)"sv;
        default:
          return "an unsupported gamepad profile"sv;
      }
    }

    // Logged when a slot is torn down (controller detached or the session ended): the
    // driver counts what it had to discard for the slot, and nothing else distinguishes a
    // quiet controller from one whose haptics, settings or input transitions were thrown
    // away. The counters reset with the slot, so each line covers exactly one session.
    void log_feedback_counters(lvg::client &client, const int nr, const lvg::profile profile) {
      lvg::feedback_counters counters {};
      const DWORD status = client.query_feedback_counters(static_cast<std::uint32_t>(nr), &counters);
      if (status == ERROR_REVISION_MISMATCH) {
        BOOST_LOG(debug) << "VHF gamepad "sv << nr << " feedback-drop counters unavailable: the driver predates them"sv;
        return;
      }
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "VHF gamepad "sv << nr << " feedback-drop counters query failed ["sv
                         << util::hex(status).to_string_view() << ']';
        return;
      }
      if (vhf_gamepad::feedback_counters_notable(counters)) {
        BOOST_LOG(warning) << "VHF gamepad "sv << nr << " ("sv << describe_profile(profile)
                           << ") feedback the driver discarded this session: "sv
                           << vhf_gamepad::describe_feedback_counters(counters);
        return;
      }
      BOOST_LOG(info) << "VHF gamepad "sv << nr << " ("sv << describe_profile(profile)
                      << ") feedback discarded by the driver this session: none"sv;
    }

    std::string_view describe_rumble_poll_outcome(const vhf_gamepad::rumble_poll_outcome_e outcome) {
      switch (outcome) {
        case vhf_gamepad::rumble_poll_outcome_e::raise_attempted:
          return "raise_attempted"sv;
        case vhf_gamepad::rumble_poll_outcome_e::deduplicated:
          return "deduplicated"sv;
        case vhf_gamepad::rumble_poll_outcome_e::unavailable:
          return "unavailable"sv;
      }
      return "unknown"sv;
    }

    void emit_rumble_poll_summary(vhf_gamepad::rumble_poll_summary_t &summary) {
      // Prerelease builds default to debug logging, so aggregate decoded reports and driver poll
      // failures to one line per slot immediately, then at most once every five seconds.
      BOOST_LOG(debug) << "VHF feedback poll summary: driver_slot="sv << summary.driver_slot
                       << " has_rumble_observation="sv << summary.has_latest_observation
                       << " event_type="sv << summary.latest.event_type
                       << " payload_bytes="sv << summary.latest.payload_size
                       << " client_index="sv << summary.latest.client_relative_index
                       << " low="sv << summary.latest.low_frequency
                       << " high="sv << summary.latest.high_frequency
                       << " outcome="sv
                       << (summary.has_latest_observation ? describe_rumble_poll_outcome(summary.latest.outcome) : "none"sv)
                       << " decoded="sv << summary.decoded_events
                       << " raise_attempts="sv << summary.raise_attempts
                       << " deduplicated="sv << summary.deduplicated_events
                       << " unavailable="sv << summary.unavailable_events
                       << " poll_errors="sv << summary.poll_errors
                       << " last_poll_error_status=["sv << util::hex(summary.latest_poll_error_status).to_string_view() << ']';
      vhf_gamepad::clear_rumble_poll_summary(summary);
    }

    void log_rumble_poll(
      vhf_gamepad::rumble_poll_summary_t &summary,
      const vhf_gamepad::rumble_poll_observation_t &observation
    ) {
      if (vhf_gamepad::record_rumble_poll_summary(summary, observation, std::chrono::steady_clock::now())) {
        emit_rumble_poll_summary(summary);
      }
    }

    void log_rumble_poll_error(
      vhf_gamepad::rumble_poll_summary_t &summary,
      const std::uint32_t driver_slot,
      const std::uint32_t status
    ) {
      if (vhf_gamepad::record_rumble_poll_error(summary, driver_slot, status, std::chrono::steady_clock::now())) {
        emit_rumble_poll_summary(summary);
      }
    }
  }  // namespace

  struct vhf_gamepad_t::impl_t {
    // Guards the driver connection and slot ownership. Submitting input and polling feedback take
    // it shared because those IOCTLs are independent per controller; only connection and slot
    // lifetime changes need exclusive access.
    std::shared_mutex lifetime;
    lvg::client client;
    vhf_gamepad::connection_recovery_t recovery;
    bool recovery_failure_logged {false};
    std::array<slot_t, MAX_GAMEPADS> slots;

    std::atomic<unsigned> active_count {0};
    std::atomic<bool> stopping {false};
    bool probed {false};
    bool driver_available {false};

    std::mutex wake_mutex;
    std::condition_variable wake;
    std::thread feedback_thread;

    // Called under the lifetime lock by every transport path. No lock upgrade
    // or driver call here: the feedback worker owns exclusive reconstruction.
    void observe_status(const DWORD status) {
      if (status == ERROR_DEVICE_REMOVED) {
        recovery.request();
      }
    }

    void feedback_loop();
    void raise_feedback(
      int nr,
      const lvg::feedback_event &event,
      const vhf_gamepad::rumble_rgb_t &feedback
    );
    // Rumble this host synthesized for a Steam Controller haptic (no driver event behind it).
    void raise_feedback(int nr, const vhf_gamepad::rumble_rgb_t &feedback);
    void raise_steam_haptic(int nr, const vhf_gamepad::steam_haptic_t &haptic, std::chrono::steady_clock::time_point now);
    void raise_steam_settings(int nr, const vhf_gamepad::steam_settings_t &settings);
    void expire_synthesized_rumble(int nr, std::chrono::steady_clock::time_point now);
  };

  /**
   * @brief Publishes a decoded feedback report to the owning client.
   * @details Runs on the feedback thread while the shared lock is held.
   * @param nr The gamepad index.
   * @param feedback The decoded rumble/RGB values.
   */
  void vhf_gamepad_t::impl_t::raise_feedback(
    const int nr,
    const lvg::feedback_event &event,
    const vhf_gamepad::rumble_rgb_t &feedback
  ) {
    auto &slot = slots[nr];
    const auto observation = vhf_gamepad::observe_rumble_poll(
      event,
      static_cast<std::uint32_t>(nr),
      slot.client_relative_index,
      feedback,
      slot.active && static_cast<bool>(slot.feedback_queue),
      slot.have_feedback,
      slot.last_feedback
    );
    if (!slot.active || !slot.feedback_queue) {
      log_rumble_poll(slot.rumble_poll_summary, observation);
      return;
    }

    if (slot.have_feedback && slot.last_feedback == feedback) {
      log_rumble_poll(slot.rumble_poll_summary, observation);
      return;
    }

    // A force-feedback effect carries no colour, so raising an LED update for it would switch
    // off the light on the client's real controller.
    const bool rgb_changed = feedback.has_rgb &&
                             (!slot.have_feedback ||
                              slot.last_feedback.red != feedback.red ||
                              slot.last_feedback.green != feedback.green ||
                              slot.last_feedback.blue != feedback.blue);

    // We have to use the client-relative index when communicating back to the client.
    // Every message takes the protected state-key admission (raise_gamepad_feedback), so a
    // stop is never dropped by a mailbox full of ordinary events. A rejected message leaves
    // the recorded feedback stale and the next poll retries it.
    bool admitted = true;
    if (observation.outcome == vhf_gamepad::rumble_poll_outcome_e::raise_attempted) {
      admitted = vhf_gamepad::dispatch_rumble_poll(
        observation,
        slot.feedback_queue,
        [](const auto client_relative_index, const auto low_frequency, const auto high_frequency) {
          return gamepad_feedback_msg_t::make_rumble(
            client_relative_index,
            low_frequency,
            high_frequency
          );
        },
        [](auto &queue, gamepad_feedback_msg_t msg) {
          return raise_gamepad_feedback(queue, std::move(msg));
        }
      );
    }
    if (rgb_changed) {
      admitted = raise_gamepad_feedback(*slot.feedback_queue, gamepad_feedback_msg_t::make_rgb_led(
        slot.client_relative_index,
        feedback.red,
        feedback.green,
        feedback.blue
      )) && admitted;
    }

    if (feedback.has_trigger_effects) {
      const bool effects_changed =
        !slot.have_feedback ||
        !slot.last_feedback.has_trigger_effects ||
        slot.last_feedback.trigger_event_flags != feedback.trigger_event_flags ||
        slot.last_feedback.left_effect != feedback.left_effect ||
        slot.last_feedback.right_effect != feedback.right_effect;
      if (effects_changed) {
        admitted = raise_gamepad_feedback(*slot.feedback_queue, gamepad_feedback_msg_t::make_adaptive_triggers(
          slot.client_relative_index,
          feedback.trigger_event_flags,
          feedback.left_effect.mode,
          feedback.right_effect.mode,
          feedback.left_effect.parameters,
          feedback.right_effect.parameters
        )) && admitted;
      }
    }

    const bool triggers_changed = feedback.has_triggers &&
                                  (!slot.have_feedback ||
                                   slot.last_feedback.left_trigger != feedback.left_trigger ||
                                   slot.last_feedback.right_trigger != feedback.right_trigger);
    if (triggers_changed) {
      admitted = raise_gamepad_feedback(*slot.feedback_queue, gamepad_feedback_msg_t::make_rumble_triggers(
        slot.client_relative_index,
        feedback.left_trigger,
        feedback.right_trigger
      )) && admitted;
    }

    if (admitted) {
      slot.last_feedback = feedback;
      slot.have_feedback = true;
    }
    log_rumble_poll(slot.rumble_poll_summary, observation);
  }

  /**
   * @brief Publishes rumble this host synthesized, with no driver event behind it.
   * @details Stands rumble in for a Steam Controller haptic on a client without the pads. The
   *          poll summary records it against an empty event. Runs on the feedback thread.
   */
  void vhf_gamepad_t::impl_t::raise_feedback(const int nr, const vhf_gamepad::rumble_rgb_t &feedback) {
    raise_feedback(nr, lvg::feedback_event {}, feedback);
  }

  /**
   * @brief Forwards a Steam Controller haptic report, or stands rumble in for it.
   * @details A client that advertised `LI_CCAP_STEAM_HAPTIC` gets every report as it came
   *          (Steam re-sends the same click to keep a buzz going, so nothing is deduplicated);
   *          any other client gets the rumble `synthesize_steam_rumble` derives, with a timer
   *          that silences a side once its hold runs out. Runs on the feedback thread.
   */
  void vhf_gamepad_t::impl_t::raise_steam_haptic(const int nr, const vhf_gamepad::steam_haptic_t &haptic, const std::chrono::steady_clock::time_point now) {
    auto &slot = slots[nr];
    if (!slot.active || !slot.feedback_queue) {
      return;
    }
    if (!config::input.forward_rumble) {
      return;  // haptics are rumble to the client as well
    }
    if (slot.client_capabilities & LI_CCAP_STEAM_HAPTIC) {
      auto msg = gamepad_feedback_msg_t::make_steam_haptic(
        slot.client_relative_index,
        haptic.length,
        haptic.report
      );
      if (!raise_gamepad_feedback(*slot.feedback_queue, std::move(msg))) {
        BOOST_LOG(debug) << "VHF gamepad "sv << nr << ": Steam haptic queue full, dropping report 0x"sv
                         << std::hex << int(haptic.report[0]) << std::dec;
      }
      return;
    }

    const std::uint8_t sides = vhf_gamepad::synthesize_steam_rumble(haptic, slot.synth_rumble);
    if (sides == 0) {
      return;
    }
    // Only the sides this report addressed restart their holds: a right-pad click must
    // not keep a finished left pulse going, and a repeated click restarts its own hold
    // even though the magnitude did not change.
    auto &rumble = slot.synth_rumble;
    if (sides & vhf_gamepad::STEAM_HAPTIC_LEFT) {
      slot.synth_left_until = rumble.left_hold_ms == 0 || rumble.left == 0 ?
                                std::nullopt :
                                std::optional {now + std::chrono::milliseconds(rumble.left_hold_ms)};
    }
    if (sides & vhf_gamepad::STEAM_HAPTIC_RIGHT) {
      slot.synth_right_until = rumble.right_hold_ms == 0 || rumble.right == 0 ?
                                 std::nullopt :
                                 std::optional {now + std::chrono::milliseconds(rumble.right_hold_ms)};
    }
    vhf_gamepad::rumble_rgb_t feedback {};
    feedback.low_frequency = rumble.left;
    feedback.high_frequency = rumble.right;
    raise_feedback(nr, feedback);
  }

  /**
   * @brief Forwards a Steam Controller settings write to a client that drives the real controller.
   * @details Only a client that advertised `LI_CCAP_STEAM_SETTINGS` gets them; nothing stands in
   *          for them elsewhere (the virtual device keeps its own table either way). Settings are
   *          events, never coalesced, so a chunk of a longer write cannot replace its siblings.
   *          Independent of `forward_rumble`: the writes also carry pad modes and click pressure.
   *          Runs on the feedback thread.
   */
  void vhf_gamepad_t::impl_t::raise_steam_settings(const int nr, const vhf_gamepad::steam_settings_t &settings) {
    auto &slot = slots[nr];
    if (!slot.active || !slot.feedback_queue || !(slot.client_capabilities & LI_CCAP_STEAM_SETTINGS)) {
      return;
    }
    auto msg = gamepad_feedback_msg_t::make_steam_settings(
      slot.client_relative_index,
      settings.command,
      settings.count,
      settings.triples
    );
    if (!raise_gamepad_feedback(*slot.feedback_queue, std::move(msg))) {
      BOOST_LOG(warning) << "VHF gamepad "sv << nr << ": feedback queue full, dropping Steam settings write 0x"sv
                         << std::hex << int(settings.command) << std::dec << " ("sv << int(settings.count) << " triples)"sv;
    }
  }

  /**
   * @brief Silences a synthesized rumble side whose hold has run out.
   * @details Runs on the feedback thread on every poll, so a side goes quiet within one poll
   *          interval of its deadline whether or not the driver has anything new.
   */
  void vhf_gamepad_t::impl_t::expire_synthesized_rumble(const int nr, const std::chrono::steady_clock::time_point now) {
    auto &slot = slots[nr];
    bool changed = false;
    if (slot.synth_left_until && now >= *slot.synth_left_until) {
      slot.synth_left_until.reset();
      slot.synth_rumble.left = 0;
      slot.synth_rumble.left_hold_ms = 0;
      changed = true;
    }
    if (slot.synth_right_until && now >= *slot.synth_right_until) {
      slot.synth_right_until.reset();
      slot.synth_rumble.right = 0;
      slot.synth_rumble.right_hold_ms = 0;
      changed = true;
    }
    if (changed) {
      vhf_gamepad::rumble_rgb_t feedback {};
      feedback.low_frequency = slot.synth_rumble.left;
      feedback.high_frequency = slot.synth_rumble.right;
      raise_feedback(nr, feedback);
    }
  }

  /**
   * @brief Drains driver output reports for every owned controller.
   * @details The driver has no completion notification, so feedback is polled. The loop sleeps
   *          until a controller exists so an idle host does not wake on a timer.
   */
  void vhf_gamepad_t::impl_t::feedback_loop() {
    while (!stopping.load(std::memory_order_acquire)) {
      {
        std::unique_lock wake_lock {wake_mutex};
        if (active_count.load(std::memory_order_acquire) == 0) {
          wake.wait(wake_lock, [this] {
            return stopping.load(std::memory_order_acquire) ||
                   active_count.load(std::memory_order_acquire) > 0;
          });
        } else {
          wake.wait_for(wake_lock, k_feedback_poll_interval, [this] {
            return stopping.load(std::memory_order_acquire);
          });
        }
      }

      if (stopping.load(std::memory_order_acquire)) {
        break;
      }

      if (recovery.pending()) {
        std::unique_lock lock {lifetime};
        if (active_count.load(std::memory_order_acquire) == 0) {
          recovery.reset();
          continue;
        }
        const auto result = recovery.attempt(client, slots, vhf_gamepad::connection_recovery_t::clock::now(), [](const auto nr, auto &slot) {
          // Retain logical ownership, profile and feedback routing, but not
          // touch contacts or feedback cached for the previous device.
          slot.contact_of_pointer.clear();
          slot.free_contacts = 0x3;
          slot.submit_failed.store(false, std::memory_order_relaxed);
          slot.have_feedback = false;
          slot.last_feedback = {};
          dualsense_audio::set_slot(nr, false);
        });
        if (result && *result == ERROR_SUCCESS) {
          for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
            if (slots[nr].active) {
              dualsense_audio::set_slot(nr, slots[nr].profile == lvg::profile::dualsense);
              if (slots[nr].steam_attributes) {
                // Best effort, as at first creation: an older driver keeps its captured defaults.
                const DWORD status = client.set_steam_controller_attributes(static_cast<std::uint32_t>(nr), *slots[nr].steam_attributes);
                if (status != ERROR_SUCCESS && status != ERROR_REVISION_MISMATCH) {
                  BOOST_LOG(warning) << "VHF gamepad "sv << nr
                                     << " (Steam Controller): could not re-apply the real controller's firmware attributes after recovery ["sv
                                     << util::hex(status).to_string_view() << ']';
                }
              }
            }
          }
          recovery_failure_logged = false;
          BOOST_LOG(info) << "Recreated owned VHF gamepads after device removal"sv;
        } else if (result && !recovery_failure_logged) {
          recovery_failure_logged = true;
          BOOST_LOG(warning) << "VHF device removal recovery is waiting for the driver ["sv
                             << util::hex(*result).to_string_view() << ']';
        }
        continue;
      }

      std::shared_lock lock {lifetime};
      if (!client.connected() || recovery.pending()) {
        continue;
      }

      const auto now = std::chrono::steady_clock::now();
      for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (recovery.pending()) {
          break;
        }
        if (!slots[nr].active) {
          continue;
        }

        if (slots[nr].profile == lvg::profile::dualsense && slots[nr].feedback_queue) {
          for (const auto &packet : dualsense_audio::drain(nr)) {
            gamepad_feedback_msg_t msg {};
            msg.type = gamepad_feedback_e::haptics_pcm;
            msg.id = slots[nr].client_relative_index;
            msg.data.haptics.sequence = packet.sequence;
            msg.data.haptics.samples = packet.samples;
            slots[nr].feedback_queue->try_raise(std::move(msg));
          }
        }
        // The driver queues a Steam Controller's haptic reports (Steam's per-side stops
        // arrive 0.7 ms apart); take a few per poll so they are not spread over 8 ms each.
        for (int drained = 0; drained < k_feedback_drain_per_poll; ++drained) {
          lvg::feedback_event event {};
          const DWORD status = client.poll_feedback(static_cast<std::uint32_t>(nr), &event);
          observe_status(status);
          if (status != ERROR_SUCCESS) {
            if (status != ERROR_NO_MORE_ITEMS) {
              log_rumble_poll_error(slots[nr].rumble_poll_summary, static_cast<std::uint32_t>(nr), status);
            }
            break;
          }

          vhf_gamepad::steam_haptic_t haptic {};
          if (vhf_gamepad::decode_steam_haptic(event, haptic)) {
            raise_steam_haptic(nr, haptic, now);
            continue;
          }

          vhf_gamepad::steam_settings_t settings {};
          if (vhf_gamepad::decode_steam_settings(event, settings)) {
            raise_steam_settings(nr, settings);
            continue;
          }

          vhf_gamepad::rumble_rgb_t feedback {};
          if (vhf_gamepad::decode_rumble_rgb(event, feedback)) {
            raise_feedback(nr, event, feedback);
          }
        }
        // After the poll: a click that was just re-sent has restarted its hold, so it is
        // not switched off and straight back on in the same pass.
        expire_synthesized_rumble(nr, now);
      }
    }
  }

  vhf_gamepad_t::vhf_gamepad_t():
      impl {std::make_unique<impl_t>()} {
  }

  vhf_gamepad_t::~vhf_gamepad_t() {
    impl->stopping.store(true, std::memory_order_release);
    {
      // Under the wait's mutex, or a notify between the predicate and the wait is lost
      // and join() hangs.
      std::lock_guard wake_guard {impl->wake_mutex};
    }
    impl->wake.notify_all();
    if (impl->feedback_thread.joinable()) {
      impl->feedback_thread.join();
    }

    std::unique_lock lock {impl->lifetime};
    if (impl->client.connected()) {
      // Closing the control handle releases every controller this process owns, but destroying
      // them explicitly keeps removal ordered while the driver is still responsive.
      for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (impl->slots[nr].active) {
          if (!impl->recovery.pending()) {
            log_feedback_counters(impl->client, nr, impl->slots[nr].profile);
            std::ignore = impl->client.destroy_controller(static_cast<std::uint32_t>(nr));
          }
          dualsense_audio::set_slot(nr, false);
          impl->slots[nr].reset();
        }
      }
      impl->client.close();
    }
    impl->active_count.store(0, std::memory_order_release);
  }

  bool vhf_gamepad_t::probe() {
    std::unique_lock lock {impl->lifetime};
    if (impl->probed) {
      return impl->driver_available;
    }
    impl->probed = true;

    lvg::client probe_client;
    const DWORD status = probe_client.connect();
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(info) << "Vibepollo virtual gamepad driver is not available ["sv
                      << util::hex(status).to_string_view() << ']';
      return false;
    }

    lvg::profile profile {};
    if (!select_profile(probe_client, vhf_profile_e::automatic, profile)) {
      BOOST_LOG(warning) << "Vibepollo virtual gamepad driver does not expose a usable gamepad profile"sv;
      return false;
    }

    // List the public console profiles rather than what automatic selection
    // would pick. Reserved generic enum values are intentionally not surfaced.
    std::string offered;
    for (const auto &[candidate, name] : {
           std::pair {lvg::profile::xbox_series, "Xbox Series"sv},
           std::pair {lvg::profile::xbox_one, "Xbox One"sv},
           std::pair {lvg::profile::dualsense, "DualSense"sv},
           std::pair {lvg::profile::dualshock_4, "DualShock 4"sv},
           std::pair {lvg::profile::switch_pro, "Switch Pro"sv},
           std::pair {lvg::profile::steam_controller, "Steam Controller"sv}}) {
      if (offers(probe_client, candidate)) {
        if (!offered.empty()) {
          offered += ", ";
        }
        offered.append(name);
      }
    }

    BOOST_LOG(info) << "Vibepollo virtual gamepad driver is available (up to "sv
                    << probe_client.maximum_controllers() << " controllers; offers "sv
                    << offered << ')';
    impl->driver_available = true;
    return true;
  }

  bool vhf_gamepad_t::available() const {
    std::shared_lock lock {impl->lifetime};
    return impl->driver_available;
  }

  int vhf_gamepad_t::alloc(
    const gamepad_id_t &id,
    feedback_queue_t &feedback_queue,
    const vhf_profile_e desired,
    const std::uint16_t client_capabilities) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
      BOOST_LOG(error) << "VHF gamepad index out of range: "sv << id.globalIndex;
      return -1;
    }

    std::unique_lock lock {impl->lifetime};
    if (impl->recovery.pending()) {
      BOOST_LOG(warning) << "VHF controller allocation deferred while recovering a removed device"sv;
      return -1;
    }
    auto &slot = impl->slots[id.globalIndex];
    if (slot.active) {
      BOOST_LOG(error) << "VHF gamepad "sv << id.globalIndex << " is already allocated"sv;
      return -1;
    }

    if (!impl->client.connected()) {
      const DWORD connect_status = impl->client.connect();
      if (connect_status != ERROR_SUCCESS) {
        BOOST_LOG(error) << "Couldn't connect to the Vibepollo virtual gamepad driver ["sv
                         << util::hex(connect_status).to_string_view() << ']';
        return -1;
      }
      BOOST_LOG(debug) << "Connected to the Vibepollo virtual gamepad driver"sv;
    }

    lvg::profile profile {};
    if (!select_profile(impl->client, desired, profile)) {
      BOOST_LOG(error) << "Vibepollo virtual gamepad driver does not offer the requested gamepad profile"sv;
      if (impl->active_count.load(std::memory_order_acquire) == 0) {
        impl->client.close();
      }
      return -1;
    }

    const DWORD status = impl->client.create_controller(
      static_cast<std::uint32_t>(id.globalIndex),
      profile
    );
    if (status != ERROR_SUCCESS) {
      if (impl->active_count.load(std::memory_order_acquire) > 0) {
        impl->observe_status(status);
      }
      BOOST_LOG(error) << "Couldn't create VHF gamepad "sv << id.globalIndex << " ["sv
                       << util::hex(status).to_string_view() << ']';
      if (impl->active_count.load(std::memory_order_acquire) == 0) {
        impl->client.close();
      }
      return -1;
    }

    slot.reset();
    slot.active = true;
    slot.profile = profile;
    slot.client_relative_index = id.clientRelativeIndex;
    slot.client_capabilities = client_capabilities;
    slot.feedback_queue = std::move(feedback_queue);
    dualsense_audio::set_slot(id.globalIndex, profile == lvg::profile::dualsense);
    impl->active_count.fetch_add(1, std::memory_order_acq_rel);

    if (has_motion(profile) && slot.feedback_queue) {
      // The client only streams motion when asked. Without this a PlayStation
      // pad enumerates with sensors that never report. The Steam Controller's
      // state report carries its IMU at 250 Hz, so ask for the native rate there;
      // the client decimates to whatever is requested.
      const std::uint16_t motion_rate_hz = is_steam_controller(profile) ? 250 : 100;
      raise_gamepad_feedback(*slot.feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(
        slot.client_relative_index, LI_MOTION_TYPE_ACCEL, motion_rate_hz));
      raise_gamepad_feedback(*slot.feedback_queue, gamepad_feedback_msg_t::make_motion_event_state(
        slot.client_relative_index, LI_MOTION_TYPE_GYRO, motion_rate_hz));
    }

    if (!impl->feedback_thread.joinable()) {
      impl->feedback_thread = std::thread {[this] {
        impl->feedback_loop();
      }};
    }
    {
      // Under the wait's mutex, or a notify between the predicate and the wait is lost
      // and the thread sleeps through the whole session.
      std::lock_guard wake_guard {impl->wake_mutex};
    }
    impl->wake.notify_all();

    BOOST_LOG(info) << "VHF gamepad "sv << id.globalIndex << " created as "sv
                    << describe_profile(profile);
    if (is_steam_controller(profile)) {
      // Steam offers firmware updates to this device as if it were the real unit. The update
      // cannot reach the real controller through the stream and leaves Steam confused.
      BOOST_LOG(warning) << "VHF gamepad "sv << id.globalIndex
                         << " (Steam Controller): Do not start a Steam Controller firmware update from Steam while streaming: "sv
                         << "the update would target this virtual controller, not the real one. "sv
                         << "Update with the controller connected directly to a PC."sv;
    }
    return 0;
  }

  void vhf_gamepad_t::free(const int nr) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::unique_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active) {
      return;
    }

    if (!impl->recovery.pending()) {
      log_feedback_counters(impl->client, nr, slot.profile);
    }
    const DWORD status = impl->recovery.pending() ? ERROR_SUCCESS :
                                                    impl->client.destroy_controller(static_cast<std::uint32_t>(nr));
    impl->observe_status(status);
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Couldn't destroy VHF gamepad "sv << nr << " ["sv
                         << util::hex(status).to_string_view() << ']';
    }

    dualsense_audio::set_slot(nr, false);
    // The mailbox belongs to the stream and outlives this controller. The
    // exclusive lifetime lock has stopped its feedback producer; remove its
    // queued output before the client-relative index can be reused.
    if (slot.feedback_queue) {
      slot.feedback_queue->discard_if([index = slot.client_relative_index](const gamepad_feedback_msg_t &msg) {
        return msg.id == index;
      });
    }
    slot.reset();
    if (impl->active_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      BOOST_LOG(debug) << "Disconnecting from the Vibepollo virtual gamepad driver"sv;
      impl->client.close();
      impl->recovery.reset();
      impl->recovery_failure_logged = false;
    }
  }

  bool vhf_gamepad_t::slot_has_sensors(const int nr) const {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return false;
    }
    std::shared_lock lock {impl->lifetime};
    const auto &slot = impl->slots[nr];
    return slot.active && has_motion(slot.profile);
  }

  void vhf_gamepad_t::touch(const int nr, const gamepad_touch_t &touch_event) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    // Exclusive: the pointer-to-contact mapping is mutated here.
    std::unique_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active || impl->recovery.pending() || !has_touch(slot.profile)) {
      return;
    }

    const std::uint8_t event = vhf_gamepad::to_protocol_touch_event(touch_event.eventType);
    if (event == vhf_gamepad::PROTOCOL_TOUCH_EVENT_UNSUPPORTED) {
      BOOST_LOG(debug) << "Unsupported touch event for VHF gamepad: "sv << static_cast<unsigned>(touch_event.eventType);
      return;
    }
    const std::uint64_t pointer_key =
      vhf_gamepad::touch_pointer_key(touch_event.touchpadIndex, touch_event.pointerId);
    std::uint8_t contact = 0;
    float x = touch_event.x;

    if (is_steam_controller(slot.profile) && (slot.client_capabilities & LI_CCAP_DUAL_TOUCHPAD)) {
      // Each pad is single-touch, so the pad index is the contact: 0 left, 1 right.
      // Pointer ids are irrelevant and a third finger cannot exist.
      contact = touch_event.touchpadIndex != 0 ? 1 : 0;
    } else if (is_steam_controller(slot.profile)) {
      // A client without LI_CCAP_DUAL_TOUCHPAD sends both pads as one DualShock-style
      // touchpad: the left half is the left pad and the right half the right pad (how
      // Steam Input splits a DualShock 4 pad). Choose the pad where the finger went
      // down and keep it for the pointer's moves and release, since a release carries
      // no position. Each half stretches back to its pad's full width.
      const auto mapped = vhf_gamepad::map_steam_touch(slot.contact_of_pointer, event,
                                                      pointer_key, x);
      if (!mapped) return;
      contact = *mapped;
      x = std::clamp(contact ? (x - 0.5f) * 2.0f : x * 2.0f, 0.0f, 1.0f);
    } else if (event == static_cast<std::uint8_t>(lvg::touch_event::cancel_all)) {
      slot.contact_of_pointer.clear();
      slot.free_contacts = 0x3;
    } else if (event == static_cast<std::uint8_t>(lvg::touch_event::down)) {
      const auto existing = slot.contact_of_pointer.find(pointer_key);
      if (existing != slot.contact_of_pointer.end()) {
        contact = existing->second;
      } else if (slot.free_contacts & 0x1) {
        contact = 0;
        slot.free_contacts &= ~0x1;
        slot.contact_of_pointer[pointer_key] = contact;
      } else if (slot.free_contacts & 0x2) {
        contact = 1;
        slot.free_contacts &= ~0x2;
        slot.contact_of_pointer[pointer_key] = contact;
      } else {
        // The pad has two contacts; a third would have to evict one, and
        // evicting produces a phantom jump on whichever finger loses.
        BOOST_LOG(debug) << "VHF gamepad "sv << nr << " has no free touch contact"sv;
        return;
      }
    } else {
      const auto existing = slot.contact_of_pointer.find(pointer_key);
      if (existing == slot.contact_of_pointer.end()) {
        return;  // A move or release for a pointer that never went down.
      }
      contact = existing->second;
      if (event == static_cast<std::uint8_t>(lvg::touch_event::up) ||
          event == static_cast<std::uint8_t>(lvg::touch_event::cancel)) {
        slot.free_contacts |= static_cast<std::uint8_t>(1u << contact);
        slot.contact_of_pointer.erase(existing);
      }
    }

    lvg::touch_state_request request {};
    request.header.size = sizeof(request);
    request.header.version = lvg::k_protocol_version;
    request.controller_id = static_cast<std::uint32_t>(nr);
    request.contact_index = contact;
    request.event_type = event;
    request.x = vhf_gamepad::to_normalized_touch(x);
    request.y = vhf_gamepad::to_normalized_touch(touch_event.y);
    request.pressure = vhf_gamepad::to_normalized_touch(touch_event.pressure);

    const DWORD status = impl->client.submit_touch_state(request);
    impl->observe_status(status);
    if (status != ERROR_SUCCESS && !slot.submit_failed.exchange(true, std::memory_order_relaxed)) {
      BOOST_LOG(warning) << "Couldn't send gamepad touch to the VHF driver ["sv
                         << util::hex(status).to_string_view() << ']';
    }
  }

  void vhf_gamepad_t::motion(const int nr, const gamepad_motion_t &motion_event) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::shared_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active || impl->recovery.pending() || !has_motion(slot.profile)) {
      return;
    }

    lvg::motion_state_request request {};
    request.header.size = sizeof(request);
    request.header.version = lvg::k_protocol_version;
    request.controller_id = static_cast<std::uint32_t>(nr);
    request.motion_type = vhf_gamepad::to_protocol_motion_kind(motion_event.motionType);
    if (request.motion_type == 0) {
      return;
    }
    request.x_milli = vhf_gamepad::to_milli_units(motion_event.x);
    request.y_milli = vhf_gamepad::to_milli_units(motion_event.y);
    request.z_milli = vhf_gamepad::to_milli_units(motion_event.z);

    impl->observe_status(impl->client.submit_motion_state(request));
  }

  void vhf_gamepad_t::battery(const int nr, const gamepad_battery_t &battery_event) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::unique_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active || !has_motion(slot.profile)) {
      return;
    }

    lvg::battery_state_request request {};
    request.header.size = sizeof(request);
    request.header.version = lvg::k_protocol_version;
    request.controller_id = static_cast<std::uint32_t>(nr);
    request.percent = battery_event.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN
                        ? 0xFFu
                        : battery_event.percentage;
    request.flags = vhf_gamepad::to_protocol_battery_state(battery_event.state);

    // Battery events are not periodic: retain even a pending or failed submission.
    if (request.percent <= 100) {
      slot.battery_level_seed.reset();
    } else if (slot.last_battery && slot.last_battery->percent <= 100) {
      slot.battery_level_seed = slot.last_battery;
    }
    slot.last_battery = request;
    if (impl->recovery.pending()) {
      return;
    }
    impl->observe_status(impl->client.submit_battery_state(request));
  }

  void vhf_gamepad_t::attributes(const int nr, const steam_controller_attributes_t &attributes_event) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::shared_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active) {
      return;
    }
    if (!vhf_gamepad::accepts_steam_attributes(slot.profile, slot.client_capabilities)) {
      BOOST_LOG(debug) << "VHF gamepad "sv << nr
                       << " ignores Steam Controller attributes: not a Steam Controller slot with LI_CCAP_STEAM_ATTRIBUTES"sv;
      return;
    }

    const vhf_gamepad::steam_attributes_t attributes {
      attributes_event.transport,
      attributes_event.valid_mask,
      attributes_event.firmware_build_time,
      attributes_event.bootloader_build_time,
      attributes_event.board_revision,
      attributes_event.capabilities,
      attributes_event.radio_firmware_build_time,
      attributes_event.dongle_firmware_build_time,
      attributes_event.build_id,
      attributes_event.unit_serial,
      attributes_event.board_serial,
    };
    const auto request = vhf_gamepad::make_steam_attributes_request(attributes);

    lvg::steam_controller_attributes driver_attributes {};
    driver_attributes.flags = request.flags;
    driver_attributes.firmware_build_time = request.firmware_build_time;
    driver_attributes.bootloader_build_time = request.bootloader_build_time;
    driver_attributes.board_revision = request.board_revision;
    driver_attributes.capabilities = request.capabilities;
    static_assert(sizeof(driver_attributes.build_id) == sizeof(request.build_id));
    static_assert(sizeof(driver_attributes.unit_serial) == sizeof(request.unit_serial));
    static_assert(sizeof(driver_attributes.board_serial) == sizeof(request.board_serial));
    std::copy(request.build_id.begin(), request.build_id.end(), std::begin(driver_attributes.build_id));
    std::copy(request.unit_serial.begin(), request.unit_serial.end(), std::begin(driver_attributes.unit_serial));
    std::copy(request.board_serial.begin(), request.board_serial.end(), std::begin(driver_attributes.board_serial));

    slot.steam_attributes = driver_attributes;
    if (impl->recovery.pending()) {
      return;
    }
    const DWORD status = impl->client.set_steam_controller_attributes(static_cast<std::uint32_t>(nr), driver_attributes);
    impl->observe_status(status);
    if (status == ERROR_REVISION_MISMATCH) {
      BOOST_LOG(info) << "VHF gamepad "sv << nr
                      << " (Steam Controller): the driver predates firmware attributes; Steam sees the virtual controller's defaults, not the "sv
                      << vhf_gamepad::describe_steam_attributes(attributes);
      return;
    }
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "VHF gamepad "sv << nr << " (Steam Controller): couldn't apply the real controller's firmware attributes ["sv
                         << util::hex(status).to_string_view() << "]; Steam sees the virtual controller's defaults"sv;
      return;
    }
    BOOST_LOG(info) << "VHF gamepad "sv << nr << " (Steam Controller): "sv << vhf_gamepad::describe_steam_attributes(attributes);
  }

  void vhf_gamepad_t::update(const int nr, const gamepad_state_t &gamepad_state) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    // Serialize the cached snapshot and submission with reconstruction and slot reuse.
    std::unique_lock lock {impl->lifetime};
    auto &slot = impl->slots[nr];
    if (!slot.active) {
      return;
    }

    const vhf_gamepad::normalized_state_t normalized {
      gamepad_state.buttonFlags,
      gamepad_state.lt,
      gamepad_state.rt,
      gamepad_state.lsX,
      gamepad_state.lsY,
      gamepad_state.rsX,
      gamepad_state.rsY
    };

    const auto request = vhf_gamepad::make_input_state(
      static_cast<std::uint32_t>(nr), normalized, slot.profile, slot.client_capabilities);
    slot.last_input = request;
    if (impl->recovery.pending()) {
      return;
    }
    const DWORD status = impl->client.submit_input_state(request);
    impl->observe_status(status);
    if (status != ERROR_SUCCESS) {
      // A wedged or removed device would otherwise log on every input packet.
      if (!slot.submit_failed.exchange(true, std::memory_order_relaxed)) {
        BOOST_LOG(warning) << "Couldn't send gamepad input to the VHF driver ["sv
                           << util::hex(status).to_string_view() << ']';
      }
    } else {
      slot.submit_failed.store(false, std::memory_order_relaxed);
    }
  }

}  // namespace platf
