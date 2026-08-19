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
#include <mutex>
#include <shared_mutex>
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
      feedback_queue_t feedback_queue;

      // Written while only the shared lock is held, so concurrent input threads cannot race.
      std::atomic<bool> submit_failed {false};

      // Only the feedback thread touches these.
      bool have_feedback {};
      vhf_gamepad::rumble_rgb_t last_feedback {};

      void reset() {
        active = false;
        client_relative_index = 0;
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
     * @details Xbox Series reaches XInput; the PID profile is a generic game pad plus the
     *          DirectInput force-feedback report set; the plain generic profile is what an older
     *          driver, or one whose HID stack rejected a larger descriptor, still offers.
     * @param client The connected driver client.
     * @param selected Receives the chosen profile.
     * @return `true` when the driver offers a usable profile.
     */
    bool select_profile(const lvg::client &client, lvg::profile &selected) {
      // Xbox Series first: it is the only profile Windows puts on the XInput
      // path, so it is the only one an XInput-only game can see at all.
      if ((client.available_profiles() & lvg::profile_bit(lvg::profile::xbox_series)) != 0) {
        selected = lvg::profile::xbox_series;
        return true;
      }
      if ((client.available_profiles() & lvg::profile_bit(lvg::profile::generic_pid)) != 0) {
        selected = lvg::profile::generic_pid;
        return true;
      }
      if ((client.available_profiles() & lvg::profile_bit(lvg::profile::generic_hid)) != 0) {
        selected = lvg::profile::generic_hid;
        return true;
      }
      return false;
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
        case lvg::profile::generic_pid:
          return "a generic HID game pad with DirectInput force feedback"sv;
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
    if (!select_profile(probe_client, profile)) {
      BOOST_LOG(warning) << "Vibeshine virtual gamepad driver does not expose a usable gamepad profile"sv;
      return false;
    }

    BOOST_LOG(info) << "Vibeshine virtual gamepad driver is available (up to "sv
                    << probe_client.maximum_controllers() << " controllers, "sv
                    << describe_profile(profile) << ')';
    impl->driver_available = true;
    return true;
  }

  bool vhf_gamepad_t::available() const {
    std::shared_lock lock {impl->lifetime};
    return impl->driver_available;
  }

  int vhf_gamepad_t::alloc(const gamepad_id_t &id, feedback_queue_t &feedback_queue) {
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
    if (!select_profile(impl->client, profile)) {
      BOOST_LOG(error) << "Vibeshine virtual gamepad driver offers no usable gamepad profile"sv;
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
    slot.client_relative_index = id.clientRelativeIndex;
    slot.feedback_queue = std::move(feedback_queue);
    impl->active_count.fetch_add(1, std::memory_order_acq_rel);

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
