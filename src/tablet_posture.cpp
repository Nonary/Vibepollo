/**
 * @file src/tablet_posture.cpp
 * @brief Client-driven laptop/slate state (see src/tablet_posture.h); platform backends do the actual switching.
 */
// standard includes
#include <condition_variable>
#include <mutex>
#include <thread>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/tablet_posture.h"

using namespace std::literals;

namespace tablet_posture {
  namespace backend {
    /// Switch the host to `state`. Returns false when the platform could not do it. Implemented per platform.
    bool apply(state_e state);

    /// Enter (`true`) or leave (`false`) the gaming posture, the Xbox full screen experience. Only called while the host is a slate. Implemented per platform.
    bool apply_gaming(bool enter);

    /// Undo a previous host process' leftovers; with `resync` also put a freshly booted indicator back to laptop. Implemented per platform.
    void recover(bool resync);

    /// Undo what a previous host process left behind for the gaming posture. Implemented per platform.
    void recover_gaming();
  }  // namespace backend

#ifndef _WIN32
  namespace backend {
    bool apply(state_e) {
      return true;
    }

    bool apply_gaming(bool) {
      return true;
    }

    void recover(bool) {}

    void recover_gaming() {}
  }  // namespace backend
#endif

  namespace {
    constexpr auto retry_delay = 5s;

    std::mutex mutex;
    std::condition_variable wake;
    registry_t registry;
    state_e applied = state_e::laptop;
    bool gaming_applied = false;
    std::thread worker;
    bool stopping = false;

    const char *name(state_e state) {
      return state == state_e::slate ? "slate (tablet)" : "laptop (desktop)";
    }

    const char *mode_name(client_mode_e mode) {
      switch (mode) {
        case client_mode_e::tablet:
          return "tablet";
        case client_mode_e::handheld:
          return "handheld";
        case client_mode_e::desktop:
          break;
      }
      return "desktop";
    }

    void worker_main() {
      std::unique_lock lock {mutex};
      std::optional<std::chrono::steady_clock::time_point> laptop_due;
      std::optional<std::chrono::steady_clock::time_point> retry_due;
      std::optional<std::chrono::steady_clock::time_point> gaming_leave_due;
      // The gaming state a failed enter was last tried for: no retry until the wanted state changes again.
      std::optional<bool> gaming_failed_for;
      while (!stopping) {
        const auto wanted = registry.state();
        const bool wanted_gaming = registry.gaming();
        const auto now = std::chrono::steady_clock::now();

        if (gaming_failed_for && *gaming_failed_for != wanted_gaming) {
          gaming_failed_for.reset();
        }

        // Leave the gaming posture first, after the same grace as laptop, and before any switch to laptop.
        if (gaming_applied && !wanted_gaming) {
          if (!gaming_leave_due) {
            gaming_leave_due = now + laptop_grace;
          }
          if (now < *gaming_leave_due) {
            wake.wait_until(lock, *gaming_leave_due);
            continue;
          }
          gaming_leave_due.reset();
          lock.unlock();
          const bool left = backend::apply_gaming(false);
          lock.lock();
          if (!left) {
            BOOST_LOG(warning) << "Tablet posture: could not leave the Xbox full screen experience"sv;
          }
          gaming_applied = false;
          if (registry.state() == state_e::laptop) {
            laptop_due = std::chrono::steady_clock::now();  // the grace already passed
          }
          continue;
        }
        gaming_leave_due.reset();

        if (wanted != applied) {
          if (retry_due && now < *retry_due) {
            wake.wait_until(lock, *retry_due);
            continue;
          }
          if (wanted == state_e::laptop) {
            if (!laptop_due) {
              laptop_due = now + laptop_grace;
            }
            if (now < *laptop_due) {
              wake.wait_until(lock, *laptop_due);
              continue;
            }
          }
          laptop_due.reset();
          lock.unlock();
          const bool ok = backend::apply(wanted);
          lock.lock();
          if (ok) {
            BOOST_LOG(info) << "Tablet posture: host is now "sv << name(wanted);
            applied = wanted;
            retry_due.reset();
          } else {
            BOOST_LOG(warning) << "Tablet posture: could not switch the host to "sv << name(wanted) << "; retrying"sv;
            retry_due = std::chrono::steady_clock::now() + retry_delay;
          }
          continue;
        }
        laptop_due.reset();
        retry_due.reset();

        if (wanted_gaming && applied == state_e::slate && !gaming_applied && !gaming_failed_for) {
          lock.unlock();
          const bool entered = backend::apply_gaming(true);
          lock.lock();
          if (entered) {
            BOOST_LOG(info) << "Tablet posture: host is now in the Xbox full screen experience"sv;
            gaming_applied = true;
          } else {
            BOOST_LOG(warning) << "Tablet posture: could not enter the Xbox full screen experience; staying in tablet mode"sv;
            gaming_failed_for = true;
          }
          continue;
        }

        wake.wait(lock);
      }
    }

    /// Caller holds the mutex.
    void ensure_worker() {
      if (!worker.joinable() && !stopping) {
        worker = std::thread(worker_main);
      }
    }

    void changed() {
      ensure_worker();
      wake.notify_all();
    }
  }  // namespace

  posture::host_mode_e host_mode_for(const posture::hints_t &hints) {
    const auto policy = posture::parse_handheld_policy(config::video.handheld_mode).value_or(posture::handheld_policy_e::xbox_fullscreen);
    return posture::host_mode(hints, policy);
  }

  void client_started(const std::string &client, client_mode_e mode) {
    if (!config::video.touch_tablet_mode) {
      return;
    }
    std::lock_guard lock {mutex};
    registry.start(client, mode);
    changed();
  }

  void client_updated(const std::string &client, client_mode_e mode) {
    if (!config::video.touch_tablet_mode) {
      return;
    }
    std::lock_guard lock {mutex};
    registry.update(client, mode);
    changed();
  }

  void client_stopped(const std::string &client) {
    std::lock_guard lock {mutex};
    registry.stop(client);
    changed();
  }

  void set_override(std::optional<client_mode_e> forced) {
    std::lock_guard lock {mutex};
    registry.set_override(forced);
    BOOST_LOG(info) << "Tablet posture: override "sv << (forced ? mode_name(*forced) : "cleared");
    changed();
  }

  status_t status() {
    std::lock_guard lock {mutex};
    std::optional<state_e> override_state;
    if (const auto forced = registry.override_mode()) {
      override_state = *forced == client_mode_e::desktop ? state_e::laptop : state_e::slate;
    }
    return {applied, registry.state(), registry.mode(), gaming_applied, override_state, registry.override_mode(), config::video.touch_tablet_mode, registry.clients()};
  }

  void recover() {
    std::lock_guard lock {mutex};
    backend::recover(config::video.touch_tablet_mode);
    backend::recover_gaming();
  }

  void shutdown() {
    {
      std::lock_guard lock {mutex};
      stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    std::lock_guard lock {mutex};
    if (gaming_applied) {
      backend::apply_gaming(false);
      gaming_applied = false;
    }
    if (applied != state_e::laptop || registry.state() != state_e::laptop) {
      backend::apply(state_e::laptop);
      applied = state_e::laptop;
    }
  }
}  // namespace tablet_posture
