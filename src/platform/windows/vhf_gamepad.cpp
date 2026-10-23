/**
 * @file src/platform/windows/vhf_gamepad.cpp
 * @brief Definitions for the Vibepollo VHF virtual gamepad input backend.
 */
#define WINVER 0x0A00

// platform includes
#include <WinSock2.h>
#include <Windows.h>

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

// lib includes
#include <libvirtualgamepad/client.h>

// local includes
#include "src/logging.h"
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

    // Global indices address driver slots directly, so the two limits have to agree.
    static_assert(MAX_GAMEPADS <= lvg::k_max_controllers);

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
      std::map<std::uint32_t, std::uint8_t> contact_of_pointer;
      std::uint8_t free_contacts {0x3};
      feedback_queue_t feedback_queue;

      // Written while only the shared lock is held, so concurrent input threads cannot race.
      std::atomic<bool> submit_failed {false};

      // Only the feedback thread touches these.
      bool have_feedback {};
      vhf_gamepad::rumble_rgb_t last_feedback {};
      vhf_gamepad::rumble_poll_summary_t rumble_poll_summary {};

      void reset() {
        active = false;
        client_relative_index = 0;
        profile = {};
        last_input.reset();
        last_battery.reset();
        battery_level_seed.reset();
        contact_of_pointer.clear();
        free_contacts = 0x3;
        feedback_queue.reset();
        submit_failed.store(false, std::memory_order_relaxed);
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

    // Profiles whose controller carries motion sensors and a battery. The
    // Switch Pro has both but no touchpad, so the two are not the same set.
    bool has_motion(const lvg::profile profile) {
      return is_playstation(profile) || profile == lvg::profile::switch_pro;
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
        default:
          return "an unsupported gamepad profile"sv;
      }
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

    // We have to use the client-relative index when communicating back to the client
    (void) vhf_gamepad::dispatch_rumble_poll(
      observation,
      slot.feedback_queue,
      [](const auto client_relative_index, const auto low_frequency, const auto high_frequency) {
        return gamepad_feedback_msg_t::make_rumble(
          client_relative_index,
          low_frequency,
          high_frequency
        );
      }
    );
    if (rgb_changed) {
      slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(
        slot.client_relative_index,
        feedback.red,
        feedback.green,
        feedback.blue
      ));
    }

    if (feedback.has_trigger_effects) {
      const bool effects_changed =
        !slot.have_feedback ||
        !slot.last_feedback.has_trigger_effects ||
        slot.last_feedback.trigger_event_flags != feedback.trigger_event_flags ||
        slot.last_feedback.left_effect != feedback.left_effect ||
        slot.last_feedback.right_effect != feedback.right_effect;
      if (effects_changed) {
        slot.feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(
          slot.client_relative_index,
          feedback.trigger_event_flags,
          feedback.left_effect.mode,
          feedback.right_effect.mode,
          feedback.left_effect.parameters,
          feedback.right_effect.parameters
        ));
      }
    }

    const bool triggers_changed = feedback.has_triggers &&
                                  (!slot.have_feedback ||
                                   slot.last_feedback.left_trigger != feedback.left_trigger ||
                                   slot.last_feedback.right_trigger != feedback.right_trigger);
    if (triggers_changed) {
      slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(
        slot.client_relative_index,
        feedback.left_trigger,
        feedback.right_trigger
      ));
    }

    slot.last_feedback = feedback;
    slot.have_feedback = true;
    log_rumble_poll(slot.rumble_poll_summary, observation);
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
        lvg::feedback_event event {};
        const DWORD status = client.poll_feedback(static_cast<std::uint32_t>(nr), &event);
        observe_status(status);
        if (status != ERROR_SUCCESS) {
          if (status != ERROR_NO_MORE_ITEMS) {
            log_rumble_poll_error(slots[nr].rumble_poll_summary, static_cast<std::uint32_t>(nr), status);
          }
          continue;
        }

        vhf_gamepad::rumble_rgb_t feedback {};
        if (!vhf_gamepad::decode_rumble_rgb(event, feedback)) {
          continue;
        }

        raise_feedback(nr, event, feedback);
      }
    }
  }

  vhf_gamepad_t::vhf_gamepad_t():
      impl {std::make_unique<impl_t>()} {
  }

  vhf_gamepad_t::~vhf_gamepad_t() {
    impl->stopping.store(true, std::memory_order_release);
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
           std::pair {lvg::profile::switch_pro, "Switch Pro"sv}}) {
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
    const vhf_profile_e desired) {
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
    slot.feedback_queue = std::move(feedback_queue);
    dualsense_audio::set_slot(id.globalIndex, profile == lvg::profile::dualsense);
    impl->active_count.fetch_add(1, std::memory_order_acq_rel);

    if (has_motion(profile) && slot.feedback_queue) {
      // The client only streams motion when asked. Without this a PlayStation
      // pad enumerates with sensors that never report.
      slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(
        slot.client_relative_index, LI_MOTION_TYPE_ACCEL, 100));
      slot.feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(
        slot.client_relative_index, LI_MOTION_TYPE_GYRO, 100));
    }

    if (!impl->feedback_thread.joinable()) {
      impl->feedback_thread = std::thread {[this] {
        impl->feedback_loop();
      }};
    }
    impl->wake.notify_all();

    BOOST_LOG(info) << "VHF gamepad "sv << id.globalIndex << " created as "sv
                    << describe_profile(profile);
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

    const DWORD status = impl->recovery.pending() ? ERROR_SUCCESS :
                                                    impl->client.destroy_controller(static_cast<std::uint32_t>(nr));
    impl->observe_status(status);
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Couldn't destroy VHF gamepad "sv << nr << " ["sv
                         << util::hex(status).to_string_view() << ']';
    }

    dualsense_audio::set_slot(nr, false);
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
    if (!slot.active || impl->recovery.pending() || !is_playstation(slot.profile)) {
      return;
    }

    const std::uint8_t event = vhf_gamepad::to_protocol_touch_event(touch_event.eventType);
    std::uint8_t contact = 0;

    if (event == static_cast<std::uint8_t>(lvg::touch_event::cancel_all)) {
      slot.contact_of_pointer.clear();
      slot.free_contacts = 0x3;
    } else if (event == static_cast<std::uint8_t>(lvg::touch_event::down)) {
      const auto existing = slot.contact_of_pointer.find(touch_event.pointerId);
      if (existing != slot.contact_of_pointer.end()) {
        contact = existing->second;
      } else if (slot.free_contacts & 0x1) {
        contact = 0;
        slot.free_contacts &= ~0x1;
        slot.contact_of_pointer[touch_event.pointerId] = contact;
      } else if (slot.free_contacts & 0x2) {
        contact = 1;
        slot.free_contacts &= ~0x2;
        slot.contact_of_pointer[touch_event.pointerId] = contact;
      } else {
        // The pad has two contacts; a third would have to evict one, and
        // evicting produces a phantom jump on whichever finger loses.
        BOOST_LOG(debug) << "VHF gamepad "sv << nr << " has no free touch contact"sv;
        return;
      }
    } else {
      const auto existing = slot.contact_of_pointer.find(touch_event.pointerId);
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
    request.x = vhf_gamepad::to_normalized_touch(touch_event.x);
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

    const auto request = vhf_gamepad::make_input_state(static_cast<std::uint32_t>(nr), normalized);
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
