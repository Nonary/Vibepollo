/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <gio/gio.h>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <mutex>
#include <thread>
#include <unordered_set>

// local includes
#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "inputtino_touch_mapping.h"
#include "inputtino_touch_wait.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::touch {
  namespace {
    void inject_touch(client_input_raw_t *raw, const touch_input_t &touch);
  }

  struct worker_t {
    client_input_raw_t *raw;
    std::string mapped_output;
    mapping_retry_t mapping_retry;
    bool stopping {false};
    bool overflow {false};
    std::unordered_set<uint32_t> active_fingers;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::pair<touch_port_t, touch_input_t>> pending;
    GSubprocess *active_process {nullptr};
    std::thread thread;

    explicit worker_t(client_input_raw_t *raw):
        raw(raw),
        thread([this] {
          run();
        }) {}

    ~worker_t() {
      {
        std::lock_guard lock(mutex);
        stopping = true;
        if (active_process) {
          g_subprocess_force_exit(active_process);
        }
      }
      wake.notify_one();
      thread.join();
      release_contacts();
    }

    void release_contacts() {
      for (const auto id : active_fingers) {
        (*raw->touch).release_finger(id);
      }
      active_fingers.clear();
    }

    void submit(const touch_port_t &port, const touch_input_t &touch) {
      {
        std::lock_guard lock(mutex);
        if (pending.size() == 4096) {
          pending.clear();
          overflow = true;
        }
        pending.emplace_back(port, touch);
      }
      wake.notify_one();
    }

    void run() {
      for (;;) {
        std::unique_lock lock(mutex);
        wake.wait(lock, [this] {
          return stopping || !pending.empty();
        });
        if (stopping) {
          return;
        }
        if (overflow) {
          release_contacts();
          overflow = false;
        }
        auto [port, touch] = std::move(pending.front());
        pending.pop_front();
        lock.unlock();
        associate_output(port);
        {
          std::lock_guard stopped_lock(mutex);
          if (stopping) {
            return;
          }
        }
        inject_touch(raw, touch);
        if (touch.eventType == LI_TOUCH_EVENT_DOWN || touch.eventType == LI_TOUCH_EVENT_MOVE || touch.eventType == LI_TOUCH_EVENT_HOVER) {
          active_fingers.insert(touch.pointerId);
        } else {
          active_fingers.erase(touch.pointerId);
        }
      }
    }

    void associate_output(const touch_port_t &port) {
      const bool output_changed = mapping_retry.output != port.output_name;
      if (!mapping_retry.begin(port.output_name, std::getenv("WAYLAND_DISPLAY") != nullptr)) {
        return;
      }
      if (output_changed) {
        release_contacts();
      }
      for (const auto &device : (*raw->touch).get_nodes()) {
        const auto node = std::filesystem::path(device).filename().string();
        int result = 1;
        const bool machine_host = std::getenv("VIBEPOLLO_MACHINE_HOST");
        GSubprocess *process = machine_host ?
                                 g_subprocess_new(
                                   static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                   nullptr,
                                   "/usr/libexec/vibeshine/vibepollo-session-exec",
                                   "touch-map",
                                   node.c_str(),
                                   port.output_name.c_str(),
                                   nullptr
                                 ) :
                                 g_subprocess_new(
                                   static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                   nullptr,
                                   "/usr/libexec/vibeshine/vibepollo-touch-map",
                                   node.c_str(),
                                   port.output_name.c_str(),
                                   nullptr
                                 );
        if (process) {
          {
            std::lock_guard lock(mutex);
            active_process = process;
            if (stopping) {
              g_subprocess_force_exit(process);
            }
          }
          result = wait_for_mapping(process);
          {
            std::lock_guard lock(mutex);
            active_process = nullptr;
          }
          g_object_unref(process);
        }
        if (result == 0) {
          if (mapped_output != port.output_name) {
            BOOST_LOG(info) << "Mapped native touch device " << node << " to " << port.output_name;
          }
          mapped_output = port.output_name;
        } else if (result != 3) {
          BOOST_LOG(warning) << "Unable to associate native touch device " << node << " with " << port.output_name;
        }
      }
    }
  };

  namespace {
    void inject_touch(client_input_raw_t *raw, const touch_input_t &touch) {
      switch (touch.eventType) {
        case LI_TOUCH_EVENT_HOVER:
        case LI_TOUCH_EVENT_DOWN:
        case LI_TOUCH_EVENT_MOVE:
          {
            // Convert our 0..360 range to -90..90 relative to Y axis
            int adjusted_angle = touch.rotation;

            if (adjusted_angle > 90 && adjusted_angle < 270) {
              // Lower hemisphere
              adjusted_angle = 180 - adjusted_angle;
            }

            // Wrap the value if it's out of range
            if (adjusted_angle > 90) {
              adjusted_angle -= 360;
            } else if (adjusted_angle < -90) {
              adjusted_angle += 360;
            }
            (*raw->touch).place_finger(touch.pointerId, touch.x, touch.y, touch.pressureOrDistance, adjusted_angle);
            break;
          }
        case LI_TOUCH_EVENT_CANCEL:
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_HOVER_LEAVE:
          {
            (*raw->touch).release_finger(touch.pointerId);
            break;
          }
      }
    }
  }  // namespace

  void update(client_input_raw_t *raw, const touch_port_t &port, const touch_input_t &touch) {
    if (!raw->touch) {
      return;
    }
    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      raw->touch_worker.reset();
      return;
    }
    // Mapping can wait for KWin to discover a new uinput node. Keep that wait
    // off the shared input task pool, and preserve a lone DOWN/UP in order.
    if (!raw->touch_worker) {
      raw->touch_worker = std::make_shared<worker_t>(raw);
    }
    raw->touch_worker->submit(port, touch);
  }
}  // namespace platf::touch
