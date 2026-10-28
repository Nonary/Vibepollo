#pragma once

#include "input_geometry_snapshot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>

namespace input::geometry {
  inline bool request_serialized_worker_wake(std::atomic_bool &scheduled) noexcept {
    return !scheduled.exchange(true, std::memory_order_acq_rel);
  }

  enum class contact_event_e : std::uint8_t {
    hover,
    down,
    move,
    up,
    cancel,
    cancel_all,
    other,
  };

  enum class contact_result_e : std::uint8_t {
    accept,
    suppress,
  };

  enum class snapshot_result_e : std::uint8_t {
    initial,
    updated,
    unchanged,
    stale,
  };

  template<class Port>
  void apply_snapshot_to_port(Port &port, const platf::input_geometry_snapshot_t &snapshot) {
    if (snapshot.valid) {
      port.offset_x = snapshot.offset_x;
      port.offset_y = snapshot.offset_y;
      port.env_width = snapshot.desktop_width;
      port.env_height = snapshot.desktop_height;
    }
    port.capture_generation = snapshot.capture_generation;
    port.geometry_sequence = snapshot.geometry_sequence;
    port.geometry_valid = snapshot.valid;
    if constexpr (requires { port.geometry_target; }) {
      port.geometry_target = snapshot.target;
    }
  }

  // Touch-port updates use a last-write-wins event, so compare the version
  // again under the event lock at publication time. The preceding view is
  // only an optimization and may be stale if two encoder producers overlap.
  template<class Event, class Port>
  bool publish_monotonic_port(const Event &event, Port port) {
    return event->raise_monotonic(std::move(port), [](const auto &candidate, const auto &published) {
      return candidate.capture_generation < published.capture_generation ||
             (candidate.capture_generation == published.capture_generation &&
              candidate.geometry_sequence < published.geometry_sequence);
    });
  }

  // Close the shared capture's old-display admission window before draining
  // contexts that were queued while capture() was unwinding. Only an actual
  // reinitialization raises the reinit event; ordinary capture termination
  // still needs terminal geometry fanout without changing encoder teardown.
  template<class ReinitEvent, class Queue, class Contexts, class IsLive, class PublishTerminal>
  void terminalize_capture_contexts(
    ReinitEvent &reinit_event,
    Queue &pending_contexts,
    Contexts &active_contexts,
    bool reinitializing,
    IsLive &&is_live,
    PublishTerminal &&publish_terminal
  ) {
    if (reinitializing) {
      reinit_event.raise(true);
    }
    for (auto &pending : pending_contexts.drain()) {
      if (std::invoke(is_live, pending)) {
        active_contexts.emplace_back(std::move(pending));
      }
    }
    for (auto &context : active_contexts) {
      if (std::invoke(is_live, context)) {
        std::invoke(publish_terminal, context);
      }
    }
  }

  template<class Event, class Port>
  void publish_port_if_geometry_changed(const Event &event, Port port) {
    const auto latest = event->view(std::chrono::milliseconds::zero());
    if (!latest || latest->capture_generation != port.capture_generation ||
        latest->geometry_sequence != port.geometry_sequence || latest->geometry_valid != port.geometry_valid) {
      publish_monotonic_port(event, std::move(port));
    }
  }

  // All calls are made by the serialized input worker. The cancel callback is
  // invoked while the port still contains its old map, before the new version
  // becomes visible to input translation.
  class transport_t {
  public:
    template<class Port, class Cancel>
    snapshot_result_e refresh(const platf::input_geometry_snapshot_t &snapshot, Port &port, Cancel &&cancel_old_contacts) {
      if (_has_snapshot) {
        if (snapshot.capture_generation < _current.capture_generation ||
            (snapshot.capture_generation == _current.capture_generation &&
             snapshot.geometry_sequence < _current.geometry_sequence)) {
          return snapshot_result_e::stale;
        }
        if (snapshot.capture_generation == _current.capture_generation &&
            snapshot.geometry_sequence == _current.geometry_sequence) {
          return snapshot_result_e::unchanged;
        }

        cancel_old_contacts();
        _quarantined_touches.insert(_active_touches.begin(), _active_touches.end());
        _active_touches.clear();
        _pen_quarantined = _pen_quarantined || _pen_active;
        _pen_active = false;
      }

      apply_snapshot_to_port(port, snapshot);
      _current = snapshot;
      const bool was_initialized = _has_snapshot;
      _has_snapshot = true;
      return was_initialized ? snapshot_result_e::updated : snapshot_result_e::initial;
    }

    // The event carries the complete stream-to-target transform. Install it
    // together with the policy-owned desktop mapping; otherwise fields such as
    // stream dimensions and client scaling stay at the input context defaults.
    template<class Port, class EventPort, class Cancel>
    snapshot_result_e refresh_from_event(
      const platf::input_geometry_snapshot_t &snapshot,
      Port &port,
      const EventPort &event_port,
      Cancel &&cancel_old_contacts
    ) {
      const auto result = refresh(snapshot, port, cancel_old_contacts);
      if (result == snapshot_result_e::stale) return result;

      // refresh() intentionally preserves the old absolute mapping when an
      // observation becomes invalid. Retain that behavior across the complete
      // event copy so invalidation cannot replace it with an unverified map.
      const auto offset_x = port.offset_x;
      const auto offset_y = port.offset_y;
      const auto env_width = port.env_width;
      const auto env_height = port.env_height;
      const auto &accepted_snapshot =
        result == snapshot_result_e::unchanged ? _current : snapshot;

      port = event_port;
      port.offset_x = offset_x;
      port.offset_y = offset_y;
      port.env_width = env_width;
      port.env_height = env_height;
      apply_snapshot_to_port(port, accepted_snapshot);
      return result;
    }

    contact_result_e touch(contact_event_e event, std::uint32_t pointer_id, bool mapping_valid) {
      if (event == contact_event_e::cancel_all) {
        _active_touches.clear();
        _quarantined_touches.clear();
        return contact_result_e::accept;
      }
      if (event == contact_event_e::down) {
        if (!mapping_valid) return contact_result_e::suppress;
        _quarantined_touches.erase(pointer_id);
        _active_touches.insert(pointer_id);
        return contact_result_e::accept;
      }
      if (event == contact_event_e::move) {
        return mapping_valid && _active_touches.contains(pointer_id) && !_quarantined_touches.contains(pointer_id) ?
                 contact_result_e::accept : contact_result_e::suppress;
      }
      if (event == contact_event_e::up || event == contact_event_e::cancel) {
        if (_quarantined_touches.erase(pointer_id)) return contact_result_e::suppress;
        _active_touches.erase(pointer_id);
        return mapping_valid ? contact_result_e::accept : contact_result_e::suppress;
      }
      return mapping_valid && !_quarantined_touches.contains(pointer_id) ?
               contact_result_e::accept : contact_result_e::suppress;
    }

    contact_result_e pen(contact_event_e event, bool mapping_valid) {
      if (event == contact_event_e::cancel_all) {
        _pen_active = false;
        _pen_quarantined = false;
        return contact_result_e::accept;
      }
      if (event == contact_event_e::down) {
        if (!mapping_valid) return contact_result_e::suppress;
        _pen_quarantined = false;
        _pen_active = true;
        return contact_result_e::accept;
      }
      if (event == contact_event_e::move) {
        return mapping_valid && _pen_active && !_pen_quarantined ?
                 contact_result_e::accept : contact_result_e::suppress;
      }
      if (event == contact_event_e::up || event == contact_event_e::cancel) {
        if (_pen_quarantined) {
          _pen_quarantined = false;
          _pen_active = false;
          return contact_result_e::suppress;
        }
        _pen_active = false;
        return mapping_valid ? contact_result_e::accept : contact_result_e::suppress;
      }
      return mapping_valid && !_pen_quarantined ?
               contact_result_e::accept : contact_result_e::suppress;
    }

    bool has_snapshot() const noexcept { return _has_snapshot; }
    const platf::input_geometry_snapshot_t &current() const noexcept { return _current; }

  private:
    bool _has_snapshot = false;
    platf::input_geometry_snapshot_t _current;
    std::unordered_set<std::uint32_t> _active_touches;
    std::unordered_set<std::uint32_t> _quarantined_touches;
    bool _pen_active = false;
    bool _pen_quarantined = false;
  };
}  // namespace input::geometry
