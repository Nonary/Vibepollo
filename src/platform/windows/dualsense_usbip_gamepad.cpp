// SPDX-License-Identifier: GPL-3.0-or-later
#include "dualsense_usbip_gamepad.h"

#include "dualsense_usbip_gamepad_policy.h"
#include "dualsense_usbip_transport.h"
#include "src/logging.h"
#include "vhf_gamepad_policy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <moonlight-common-c/src/ControllerHaptics.h>
#include <mutex>
#include <span>
#include <tuple>
#include <vector>

namespace platf {
  namespace {
    using namespace std::chrono_literals;

    struct usb_slot {
      // Setup/removal may wait for PnP or a transport thread. Other slots and
      // this slot's callbacks never take this lifecycle mutex.
      std::mutex lifecycle;
      std::mutex mutex;
      bool active = true;
      bool ready = false;
      bool waveform_capable = false;
      bool persist_after_disconnect = false;
      bool nonzero_pcm_received = false;
      std::uint16_t client_index = 0;
      std::uint8_t global_index = 0;
      feedback_queue_t feedback_queue;
      std::unique_ptr<dualsense_usbip::controller> controller;
      lvg::driver::ds5_state state {};
      lvg::input_state_request input {};
      std::map<std::uint32_t, std::uint8_t> contacts;
      std::uint8_t free_contacts = 3;
      dualsense_usbip_gamepad::output_state output;
      dualsense_usbip_gamepad::pcm_packetizer pcm;
      std::chrono::steady_clock::time_point last_pcm {};
      vhf_gamepad::rumble_rgb_t last_output {};
      bool pending_rumble = false;
      bool pending_rgb = false;
      std::uint8_t pending_triggers = 0;
      bool pending_accel_request = false;
      bool pending_gyro_request = false;

      bool publish_latest(gamepad_feedback_msg_t message) {
        const auto type = message.type;
        return feedback_queue->raise_latest(std::move(message), [index = client_index, type](const gamepad_feedback_msg_t &pending) {
          return pending.id == index && pending.type == type;
        });
      }

      void flush_output() {
        if (!active || !feedback_queue) {
          return;
        }
        const auto request_motion = [this](std::uint8_t type) {
          return feedback_queue->raise_latest(gamepad_feedback_msg_t::make_motion_event_state(client_index, type, 100), [index = client_index, type](const gamepad_feedback_msg_t &pending) {
            return pending.id == index && pending.type == gamepad_feedback_e::set_motion_event_state &&
                   pending.data.motion_event_state.motion_type == type;
          });
        };
        if (pending_accel_request && request_motion(LI_MOTION_TYPE_ACCEL)) {
          pending_accel_request = false;
        }
        if (pending_gyro_request && request_motion(LI_MOTION_TYPE_GYRO)) {
          pending_gyro_request = false;
        }
        if (pending_rumble && publish_latest(gamepad_feedback_msg_t::make_rumble(client_index, last_output.low_frequency, last_output.high_frequency))) {
          pending_rumble = false;
        }
        if (pending_rgb && publish_latest(gamepad_feedback_msg_t::make_rgb_led(client_index, last_output.red, last_output.green, last_output.blue))) {
          pending_rgb = false;
        }
        // A later right-only command must not replace an unsent left-only
        // command in the shared queue. Preserve these enables and order;
        // only commands rejected by a full queue merge in pending_triggers.
        if (pending_triggers && feedback_queue->try_raise(gamepad_feedback_msg_t::make_adaptive_triggers(client_index, pending_triggers, last_output.left_effect.mode, last_output.right_effect.mode, last_output.left_effect.parameters, last_output.right_effect.parameters))) {
          pending_triggers = 0;
        }
      }

      void submit() {
        flush_output();
        if (!controller || !active) {
          return;
        }
        const auto report = lvg::driver::encode_ds5_input(input, &state);
        std::ignore = controller->set_input_report({reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
      }

      void hid_output(std::span<const std::uint8_t> bytes) {
        std::lock_guard lock {mutex};
        const auto change = output.apply(bytes);
        if (!change.accepted) {
          return;
        }
        lvg::feedback_event event {};
        event.type = lvg::feedback_type::playstation_output;
        event.payload_size = sizeof(change.state);
        std::memcpy(event.payload, &change.state, sizeof(change.state));
        vhf_gamepad::rumble_rgb_t decoded;
        if (!vhf_gamepad::decode_rumble_rgb(event, decoded)) {
          return;
        }
        last_output = decoded;
        pending_rumble = pending_rumble || change.rumble;
        pending_rgb = pending_rgb || change.rgb;
        pending_triggers |= change.triggers;
        flush_output();
      }

      void haptics_pcm(std::span<const std::uint8_t> bytes);
      void clear_pcm();
    };

    std::atomic<unsigned> session_count {0};
    std::atomic<bool> application_active {false};
    // A reused client-relative slot must not restart sequence zero while a
    // previous stream still has in-flight network packets for that slot.
    std::array<std::atomic<std::uint32_t>, MAX_GAMEPADS> pcm_sequence {};
    std::mutex registry_mutex;
    std::vector<std::weak_ptr<usb_slot>> registry;

    std::vector<std::shared_ptr<usb_slot>> live_slots() {
      std::lock_guard lock {registry_mutex};
      std::vector<std::shared_ptr<usb_slot>> result;
      for (auto it = registry.begin(); it != registry.end();) {
        if (auto slot = it->lock()) {
          result.push_back(std::move(slot));
          ++it;
        } else {
          it = registry.erase(it);
        }
      }
      return result;
    }

    void usb_slot::clear_pcm() {
      pcm.reset();
      last_pcm = {};
      if (feedback_queue) {
        feedback_queue->discard_if([index = client_index](const gamepad_feedback_msg_t &msg) {
          return msg.type == gamepad_feedback_e::haptics_pcm && msg.id == index;
        });
      }
    }

    void usb_slot::haptics_pcm(std::span<const std::uint8_t> bytes) {
      std::lock_guard lock {mutex};
      flush_output();
      if (!nonzero_pcm_received && std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0; })) {
        nonzero_pcm_received = true;
        BOOST_LOG(debug) << "Composite DualSense " << global_index << " received nonzero USB actuator PCM";
      }
      if (!active || !ready || !waveform_capable || !feedback_queue) {
        pcm.reset();
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (last_pcm != std::chrono::steady_clock::time_point {} && now - last_pcm > 50ms) {
        clear_pcm();
      }
      last_pcm = now;
      pcm.push(bytes, now, [this](const auto &samples) {
        gamepad_feedback_msg_t message {};
        message.type = gamepad_feedback_e::haptics_pcm;
        message.id = client_index;
        message.data.haptics.sequence = pcm_sequence[global_index].fetch_add(1, std::memory_order_relaxed);
        message.data.haptics.samples = samples;
        // A stalled control connection must not make the USB transport thread
        // block, grow a queue, or clear feedback from another controller.
        if (!feedback_queue->try_raise(message)) {
          feedback_queue->discard_if([index = client_index](const gamepad_feedback_msg_t &pending) {
            return pending.type == gamepad_feedback_e::haptics_pcm && pending.id == index;
          });
          feedback_queue->try_raise(std::move(message));
        }
      });
    }
  }  // namespace

  namespace dualsense_usbip_gamepad {
    void set_application_active(bool active) {
      application_active.store(active, std::memory_order_release);
      if (active) {
        return;
      }
      for (const auto &slot : live_slots()) {
        std::lock_guard lifecycle_lock {slot->lifecycle};
        std::unique_ptr<dualsense_usbip::controller> controller;
        {
          std::lock_guard lock {slot->mutex};
          if (!slot->active) {
            slot->ready = false;
            controller = std::move(slot->controller);
          }
        }
        // Joining transport callbacks must happen outside their mutex.
        controller.reset();
      }
    }

    session_scope::session_scope() {
      session_count.fetch_add(1, std::memory_order_acq_rel);
    }

    session_scope::~session_scope() {
      if (session_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Registry ownership is independent of callback locks. Never destroy
        // a USB transport while holding a lock its callback may acquire.
        for (const auto &slot : live_slots()) {
          std::lock_guard lock {slot->mutex};
          slot->clear_pcm();
        }
      }
    }

    std::shared_ptr<session_scope> start_session() {
      if (!dualsense_usbip::available()) {
        BOOST_LOG(error) << "DualSense USB/audio requires the optional usbip-win2 driver; install DualSense USB audio and haptics support in the Windows installer";
        return {};
      }
      return std::shared_ptr<session_scope> {new session_scope};
    }

    bool enabled() {
      return session_count.load(std::memory_order_acquire) != 0;
    }

    bool has_ready_controller() {
      for (const auto &slot : live_slots()) {
        std::lock_guard lock {slot->mutex};
        if (slot->active && slot->ready && slot->waveform_capable && slot->controller && slot->controller->connected()) {
          return true;
        }
      }
      return false;
    }
  }  // namespace dualsense_usbip_gamepad

  struct usbip_gamepad_t::impl_t {
    std::mutex mutex;
    bool driver_available = false;
    std::array<std::shared_ptr<usb_slot>, MAX_GAMEPADS> slots;

    std::shared_ptr<usb_slot> find(int nr) {
      if (nr < 0 || nr >= MAX_GAMEPADS) {
        return {};
      }
      std::lock_guard lock {mutex};
      return slots[nr];
    }
  };

  usbip_gamepad_t::usbip_gamepad_t():
      impl {std::make_unique<impl_t>()} {
  }

  usbip_gamepad_t::~usbip_gamepad_t() {
    for (int nr = 0; nr < MAX_GAMEPADS; ++nr) {
      free(nr, false);
    }
  }

  bool usbip_gamepad_t::probe() {
    std::lock_guard lock {impl->mutex};
    impl->driver_available = dualsense_usbip::available();
    BOOST_LOG(info) << "DualSense USB/audio transport is " << (impl->driver_available ? "available" : "not installed");
    return impl->driver_available;
  }

  bool usbip_gamepad_t::available() const {
    std::lock_guard lock {impl->mutex};
    return impl->driver_available;
  }

  int usbip_gamepad_t::alloc(const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t &feedback_queue) {
    if (id.globalIndex < 0 || id.globalIndex >= MAX_GAMEPADS) {
      return -1;
    }
    if (const auto retained = impl->find(id.globalIndex)) {
      {
        std::lock_guard lifecycle_lock {retained->lifecycle};
        std::lock_guard lock {retained->mutex};
        if (retained->active) {
          return -1;
        }
        if (retained->ready && retained->controller && retained->controller->connected()) {
          retained->client_index = id.clientRelativeIndex;
          retained->waveform_capable = (metadata.capabilities & LI_CCAP_HAPTICS_PCM) != 0;
          retained->persist_after_disconnect = metadata.persist_after_disconnect;
          retained->feedback_queue = feedback_queue;
          retained->clear_pcm();
          retained->active = true;
          retained->pending_accel_request = (metadata.capabilities & LI_CCAP_ACCEL) != 0;
          retained->pending_gyro_request = (metadata.capabilities & LI_CCAP_GYRO) != 0;
          retained->pending_rumble = true;
          retained->pending_rgb = retained->last_output.has_rgb;
          retained->pending_triggers = retained->last_output.has_trigger_effects ?
                                         DS_EFFECT_LEFT_TRIGGER | DS_EFFECT_RIGHT_TRIGGER : 0;
          retained->submit();
          BOOST_LOG(info) << "Composite DualSense " << id.globalIndex << " rebound to resumed client";
          return 0;
        }
      }
      free(id.globalIndex, false);
    }
    auto slot = std::make_shared<usb_slot>();
    std::unique_lock lifecycle_lock {slot->lifecycle};
    slot->client_index = id.clientRelativeIndex;
    slot->global_index = id.globalIndex;
    slot->waveform_capable = (metadata.capabilities & LI_CCAP_HAPTICS_PCM) != 0;
    slot->persist_after_disconnect = metadata.persist_after_disconnect;
    slot->feedback_queue = feedback_queue;
    slot->state.reset();
    slot->input.header.size = sizeof(slot->input);
    slot->input.header.version = lvg::k_protocol_version;
    slot->input.controller_id = id.globalIndex;
    {
      std::lock_guard lock {impl->mutex};
      if (impl->slots[id.globalIndex]) {
        return -1;
      }
      impl->slots[id.globalIndex] = slot;
    }
    const auto failed = [&] {
      std::unique_ptr<dualsense_usbip::controller> controller;
      {
        std::lock_guard slot_lock {slot->mutex};
        slot->active = false;
        slot->clear_pcm();
        controller = std::move(slot->controller);
      }
      // Complete detach before another controller can reserve this index.
      controller.reset();
      std::lock_guard lock {impl->mutex};
      impl->slots[id.globalIndex].reset();
    };
    const std::weak_ptr<usb_slot> weak = slot;
    auto controller = dualsense_usbip::create(id.globalIndex, {[weak](std::span<const std::uint8_t> output) {
                                                                 if (const auto alive = weak.lock()) {
                                                                   alive->hid_output(output);
                                                                 }
                                                               },
                                                               [weak](std::span<const std::uint8_t> samples) {
                                                                 if (const auto alive = weak.lock()) {
                                                                   alive->haptics_pcm(samples);
                                                                 }
                                                               }});
    if (!controller) {
      BOOST_LOG(error) << "Couldn't attach composite DualSense USB gamepad " << id.globalIndex;
      failed();
      return -1;
    }
    {
      std::lock_guard slot_lock {slot->mutex};
      slot->controller = std::move(controller);
      slot->submit();
    }
    // This runs after RTSP has admitted the client, on its controller input
    // worker. start_session() and the launch request never wait for PnP here.
    if (!slot->controller->wait_until_ready(5s)) {
      BOOST_LOG(error) << "Composite DualSense " << id.globalIndex << " did not enumerate both HID and a four-channel audio endpoint";
      failed();
      return -1;
    }
    {
      std::lock_guard slot_lock {slot->mutex};
      slot->ready = true;
      slot->clear_pcm();
      slot->pending_accel_request = (metadata.capabilities & LI_CCAP_ACCEL) != 0;
      slot->pending_gyro_request = (metadata.capabilities & LI_CCAP_GYRO) != 0;
      slot->flush_output();
    }
    {
      std::lock_guard registry_lock {registry_mutex};
      registry.erase(std::remove_if(registry.begin(), registry.end(), [](const auto &entry) {
                       return entry.expired();
                     }),
                     registry.end());
      registry.emplace_back(slot);
    }
    if (!slot->waveform_capable && dualsense_usbip_gamepad::enabled()) {
      BOOST_LOG(warning) << "Gamepad " << id.globalIndex << " does not support waveform haptics; waiting for a capable client controller before launching this app";
    }
    BOOST_LOG(debug) << "Gamepad " << id.globalIndex << " created as a composite DualSense with USB audio; client waveform haptics "
                    << (slot->waveform_capable ? "supported" : "unsupported");
    return 0;
  }

  void usbip_gamepad_t::free(int nr, bool retain_for_resume) {
    if (nr < 0 || nr >= MAX_GAMEPADS) {
      return;
    }
    const auto slot = impl->find(nr);
    if (!slot) {
      return;
    }
    std::unique_lock lifecycle_lock {slot->lifecycle};
    {
      std::lock_guard lock {impl->mutex};
      // Another removal may have completed while this one waited for setup.
      if (impl->slots[nr] != slot) {
        return;
      }
    }
    std::unique_ptr<dualsense_usbip::controller> controller;
    {
      std::lock_guard lock {slot->mutex};
      slot->active = false;
      slot->clear_pcm();
      slot->feedback_queue.reset();
      slot->state.reset();
      slot->contacts.clear();
      slot->free_contacts = 3;
      slot->input = vhf_gamepad::make_input_state(nr, {});
      if (slot->controller) {
        const auto report = lvg::driver::encode_ds5_input(slot->input, &slot->state);
        std::ignore = slot->controller->set_input_report({reinterpret_cast<const std::uint8_t *>(&report), sizeof(report)});
      }
      if (retain_for_resume && slot->persist_after_disconnect && application_active.load(std::memory_order_acquire) && slot->ready && slot->controller && slot->controller->connected()) {
        BOOST_LOG(info) << "Composite DualSense " << nr << " retained while application is paused";
        return;
      }
      slot->ready = false;
      controller = std::move(slot->controller);
    }
    // Detach and join callbacks after releasing their mutex. A pending
    // callback can finish but cannot publish into this released client slot.
    controller.reset();
    std::lock_guard lock {impl->mutex};
    if (impl->slots[nr] == slot) {
      impl->slots[nr].reset();
    }
  }

  void usbip_gamepad_t::update(int nr, const gamepad_state_t &state) {
    const auto slot = impl->find(nr);
    if (!slot) {
      return;
    }
    std::lock_guard lock {slot->mutex};
    slot->input = vhf_gamepad::make_input_state(nr, {state.buttonFlags, state.lt, state.rt, state.lsX, state.lsY, state.rsX, state.rsY});
    slot->submit();
  }

  void usbip_gamepad_t::touch(int nr, const gamepad_touch_t &event) {
    const auto slot = impl->find(nr);
    if (!slot) {
      return;
    }
    std::lock_guard lock {slot->mutex};
    lvg::touch_state_request request {};
    request.event_type = vhf_gamepad::to_protocol_touch_event(event.eventType);
    if (request.event_type == static_cast<std::uint8_t>(lvg::touch_event::cancel_all)) {
      slot->contacts.clear();
      slot->free_contacts = 3;
    } else if (request.event_type == static_cast<std::uint8_t>(lvg::touch_event::down)) {
      if (const auto existing = slot->contacts.find(event.pointerId); existing != slot->contacts.end()) {
        request.contact_index = existing->second;
      } else if (slot->free_contacts) {
        request.contact_index = (slot->free_contacts & 1) ? 0 : 1;
        slot->free_contacts &= static_cast<std::uint8_t>(~(1u << request.contact_index));
        slot->contacts[event.pointerId] = request.contact_index;
      } else {
        return;
      }
    } else {
      const auto existing = slot->contacts.find(event.pointerId);
      if (existing == slot->contacts.end()) {
        return;
      }
      request.contact_index = existing->second;
      if (request.event_type == static_cast<std::uint8_t>(lvg::touch_event::up) || request.event_type == static_cast<std::uint8_t>(lvg::touch_event::cancel)) {
        slot->free_contacts |= static_cast<std::uint8_t>(1u << request.contact_index);
        slot->contacts.erase(existing);
      }
    }
    request.x = vhf_gamepad::to_normalized_touch(event.x);
    request.y = vhf_gamepad::to_normalized_touch(event.y);
    request.pressure = vhf_gamepad::to_normalized_touch(event.pressure);
    if (lvg::driver::apply_ds5_touch(request, &slot->state)) {
      slot->submit();
    }
  }

  void usbip_gamepad_t::motion(int nr, const gamepad_motion_t &event) {
    const auto slot = impl->find(nr);
    if (!slot) {
      return;
    }
    std::lock_guard lock {slot->mutex};
    lvg::motion_state_request request {};
    request.motion_type = vhf_gamepad::to_protocol_motion_kind(event.motionType);
    // Match the existing PlayStation input limits before the shared Sony
    // encoder multiplies milli-units by sensor counts in signed 32-bit math.
    const auto maximum = event.motionType == LI_MOTION_TYPE_ACCEL ? 200.0f : 4000.0f;
    const auto scaled = [maximum](float value) {
      return vhf_gamepad::to_milli_units(std::isfinite(value) ? std::clamp(value, -maximum, maximum) : 0.0f);
    };
    request.x_milli = scaled(event.x);
    request.y_milli = scaled(event.y);
    request.z_milli = scaled(event.z);
    if (lvg::driver::apply_ds5_motion(request, &slot->state)) {
      slot->submit();
    }
  }

  void usbip_gamepad_t::battery(int nr, const gamepad_battery_t &event) {
    const auto slot = impl->find(nr);
    if (!slot) {
      return;
    }
    std::lock_guard lock {slot->mutex};
    lvg::battery_state_request request {};
    request.percent = event.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN ? 0xFF : event.percentage;
    request.flags = vhf_gamepad::to_protocol_battery_state(event.state);
    if (lvg::driver::apply_ds5_battery(request, &slot->state)) {
      slot->submit();
    }
  }
}  // namespace platf
