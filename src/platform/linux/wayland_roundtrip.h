/** @file Bounded Wayland discovery for optional capture backends. */
#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <wayland-client.h>

namespace platf::wayland {
  /**
   * These observations use a private display connection, with no other reader.
   * libwayland's read_events may wait for other prepared readers on a shared
   * connection, so a shared connection cannot provide this deadline guarantee.
   * Socket waits are bounded; dispatched callbacks must also return promptly.
   */
  inline bool observation_allowed(std::chrono::steady_clock::time_point deadline, const std::function<bool()> &allowed) {
    if (allowed && !allowed()) {
      errno = ECANCELED;
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      errno = ETIMEDOUT;
      return false;
    }
    return true;
  }

  inline int observation_poll_timeout(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, 50));
  }

  inline int roundtrip(wl_display *display, std::chrono::steady_clock::time_point deadline, const std::function<bool()> &allowed) {
    if (!display) {
      errno = EINVAL;
      return -1;
    }
    if (!observation_allowed(deadline, allowed)) return -1;
    bool done = false;
    static constexpr wl_callback_listener listener {
      .done = [](void *data, wl_callback *, uint32_t) {
        *static_cast<bool *>(data) = true;
      },
    };
    auto *callback = wl_display_sync(display);
    if (!callback) {
      return -1;
    }
    if (wl_callback_add_listener(callback, &listener, &done) < 0) {
      const int saved_errno = errno;
      wl_callback_destroy(callback);
      errno = saved_errno;
      return -1;
    }
    int result = -1;
    while (true) {
      if (!observation_allowed(deadline, allowed)) break;
      if (wl_display_dispatch_pending(display) < 0) {
        break;
      }
      if (!observation_allowed(deadline, allowed)) break;
      if (done) {
        result = 0;
        break;
      }
      if (wl_display_prepare_read(display) < 0) {
        if (const int error = wl_display_get_error(display)) {
          errno = error;
          break;
        }
        continue;
      }
      if (!observation_allowed(deadline, allowed)) {
        wl_display_cancel_read(display);
        break;
      }
      short events = POLLIN;
      if (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
          wl_display_cancel_read(display);
          break;
        }
        events |= POLLOUT;
      }
      pollfd descriptor {wl_display_get_fd(display), events, 0};
      const int ready = poll(&descriptor, 1, observation_poll_timeout(deadline));
      if (!observation_allowed(deadline, allowed)) {
        wl_display_cancel_read(display);
        break;
      }
      if (ready <= 0 || !(descriptor.revents & POLLIN)) {
        wl_display_cancel_read(display);
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready < 0) {
          break;
        }
        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
          errno = EPIPE;
          break;
        }
        continue;
      }
      if (wl_display_read_events(display) < 0) {
        break;
      }
    }
    const int saved_errno = errno;
    wl_callback_destroy(callback);
    errno = saved_errno;
    return result;
  }

  inline int roundtrip(wl_display *display, std::chrono::milliseconds timeout) {
    return roundtrip(display, std::chrono::steady_clock::now() + timeout, [] { return true; });
  }
}  // namespace platf::wayland
