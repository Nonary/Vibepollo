/**
 * @file src/platform/windows/vhf_gamepad.cpp
 * @brief Definitions for the Vibeshine VHF virtual gamepad input backend.
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
#include "vhf_gamepad_policy.h"

namespace platf {
  using namespace std::literals;

  namespace {
    // The driver coalesces output reports into one pending slot per controller, so the poll rate
    // only bounds how quickly rumble reaches the client. 8ms keeps that under a frame at 120 FPS.
    constexpr auto k_feedback_poll_interval = 8ms;

    // The wire protocol reuses Vibeshine's normalized button values verbatim. Pin that here so a
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

      void reset() {
        active = false;
        client_relative_index = 0;
        profile = {};
        contact_of_pointer.clear();
        free_contacts = 0x3;
        feedback_queue.reset();
        submit_failed.store(false, std::memory_order_relaxed);
        have_feedback = false;
        last_feedback = {};
      }
    };
  }  // namespace

  namespace {
    /**
     * @brief Picks the richest profile the connected driver offers.
     * @details Xbox Series reaches XInput; the PlayStation profiles add a touchpad, motion,
     *          battery, and a lightbar; the PID profile is a generic game pad plus the
     *          DirectInput force-feedback report set; the plain generic profile is what an older
     *          driver, or one whose HID stack rejected a larger descriptor, still offers.
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

      // Xbox Series first for the automatic case: it is the only profile
      // Windows puts on the XInput path, so it is the only one an XInput-only
      // game can see at all.
      for (const lvg::profile candidate : {
             lvg::profile::xbox_series,
             lvg::profile::dualsense,
             lvg::profile::dualshock_4,
             lvg::profile::generic_pid,
             lvg::profile::generic_hid}) {
        if (offers(client, candidate)) {
          selected = candidate;
          return true;
        }
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
        case lvg::profile::generic_pid:
          return "a generic HID game pad that also publishes force feedback reports"sv;
        default:
          return "a generic HID game pad"sv;
      }
    }
  }  // namespace

  struct vhf_gamepad_t::impl_t {
    // Guards the driver connection and slot ownership. Submitting input and polling feedback take
    // it shared because those IOCTLs are independent per controller; only connection and slot
    // lifetime changes need exclusive access.
    std::shared_mutex lifetime;
    lvg::client client;
    std::array<slot_t, MAX_GAMEPADS> slots;

    std::atomic<unsigned> active_count {0};
    std::atomic<bool> stopping {false};
    bool probed {false};
    bool driver_available {false};

    std::mutex wake_mutex;
    std::condition_variable wake;
    std::thread feedback_thread;

    void feedback_loop();
    void raise_feedback(int nr, const vhf_gamepad::rumble_rgb_t &feedback);
  };

  /**
   * @brief Publishes a decoded feedback report to the owning client.
   * @details Runs on the feedback thread while the shared lock is held.
   * @param nr The gamepad index.
   * @param feedback The decoded rumble/RGB values.
   */
  void vhf_gamepad_t::impl_t::raise_feedback(const int nr, const vhf_gamepad::rumble_rgb_t &feedback) {
    auto &slot = slots[nr];
    if (!slot.active || !slot.feedback_queue) {
      return;
    }

    if (slot.have_feedback && slot.last_feedback == feedback) {
      return;
    }

    const bool rumble_changed = !slot.have_feedback ||
                                slot.last_feedback.low_frequency != feedback.low_frequency ||
                                slot.last_feedback.high_frequency != feedback.high_frequency;
    // A force-feedback effect carries no colour, so raising an LED update for it would switch
    // off the light on the client's real controller.
    const bool rgb_changed = feedback.has_rgb &&
                             (!slot.have_feedback ||
                              slot.last_feedback.red != feedback.red ||
                              slot.last_feedback.green != feedback.green ||
                              slot.last_feedback.blue != feedback.blue);

    // We have to use the client-relative index when communicating back to the client
    if (rumble_changed) {
      slot.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(
        slot.client_relative_index,
        feedback.low_frequency,
        feedback.high_frequency
      ));
    }
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

      std::shared_lock lock {lifetime};
      if (!client.connected()) {
        continue;
      }

      for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
        if (!slots[nr].active) {
          continue;
        }

        lvg::feedback_event event {};
        const DWORD status = client.poll_feedback(static_cast<std::uint32_t>(nr), &event);
        if (status != ERROR_SUCCESS) {
          if (status != ERROR_NO_MORE_ITEMS) {
            BOOST_LOG(debug) << "VHF gamepad "sv << nr << " feedback poll failed ["sv
                             << util::hex(status).to_string_view() << ']';
          }
          continue;
        }

        vhf_gamepad::rumble_rgb_t feedback {};
        if (!vhf_gamepad::decode_rumble_rgb(event, feedback)) {
          continue;
        }

        raise_feedback(nr, feedback);
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
          std::ignore = impl->client.destroy_controller(static_cast<std::uint32_t>(nr));
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
      BOOST_LOG(info) << "Vibeshine virtual gamepad driver is not available ["sv
                      << util::hex(status).to_string_view() << ']';
      return false;
    }

    lvg::profile profile {};
    if (!select_profile(probe_client, vhf_profile_e::automatic, profile)) {
      BOOST_LOG(warning) << "Vibeshine virtual gamepad driver does not expose a usable gamepad profile"sv;
      return false;
    }

    // List what the driver offers rather than what an automatic selection would
    // pick. This line is read to confirm a configured controller type is
    // reachable, and naming one profile made every other setting look ignored.
    std::string offered;
    for (const auto &[candidate, name] : {
           std::pair {lvg::profile::xbox_series, "Xbox Series"sv},
           std::pair {lvg::profile::xbox_one, "Xbox One"sv},
           std::pair {lvg::profile::dualsense, "DualSense"sv},
           std::pair {lvg::profile::dualshock_4, "DualShock 4"sv},
           std::pair {lvg::profile::switch_pro, "Switch Pro"sv},
           std::pair {lvg::profile::generic_pid, "generic HID with force feedback reports"sv},
           std::pair {lvg::profile::generic_hid, "generic HID"sv}}) {
      if (offers(probe_client, candidate)) {
        if (!offered.empty()) {
          offered += ", ";
        }
        offered.append(name);
      }
    }

    BOOST_LOG(info) << "Vibeshine virtual gamepad driver is available (up to "sv
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
    auto &slot = impl->slots[id.globalIndex];
    if (slot.active) {
      BOOST_LOG(error) << "VHF gamepad "sv << id.globalIndex << " is already allocated"sv;
      return -1;
    }

    if (!impl->client.connected()) {
      const DWORD connect_status = impl->client.connect();
      if (connect_status != ERROR_SUCCESS) {
        BOOST_LOG(error) << "Couldn't connect to the Vibeshine virtual gamepad driver ["sv
                         << util::hex(connect_status).to_string_view() << ']';
        return -1;
      }
      BOOST_LOG(debug) << "Connected to the Vibeshine virtual gamepad driver"sv;
    }

    lvg::profile profile {};
    if (!select_profile(impl->client, desired, profile)) {
      BOOST_LOG(error) << "Vibeshine virtual gamepad driver does not offer the requested gamepad profile"sv;
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

    const DWORD status = impl->client.destroy_controller(static_cast<std::uint32_t>(nr));
    if (status != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "Couldn't destroy VHF gamepad "sv << nr << " ["sv
                         << util::hex(status).to_string_view() << ']';
    }

    slot.reset();
    if (impl->active_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      BOOST_LOG(debug) << "Disconnecting from the Vibeshine virtual gamepad driver"sv;
      impl->client.close();
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
    if (!slot.active || !is_playstation(slot.profile)) {
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
    if (!slot.active || !has_motion(slot.profile)) {
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

    std::ignore = impl->client.submit_motion_state(request);
  }

  void vhf_gamepad_t::battery(const int nr, const gamepad_battery_t &battery_event) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::shared_lock lock {impl->lifetime};
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

    std::ignore = impl->client.submit_battery_state(request);
  }

  void vhf_gamepad_t::update(const int nr, const gamepad_state_t &gamepad_state) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }

    std::shared_lock lock {impl->lifetime};
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
    const DWORD status = impl->client.submit_input_state(request);
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
