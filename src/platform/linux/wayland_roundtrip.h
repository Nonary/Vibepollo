/** @file Bounded Wayland discovery for optional capture backends. */
#pragma once

#include <cerrno>
#include <chrono>
#include <poll.h>
#include <wayland-client.h>

namespace platf::wayland {
  inline int roundtrip(wl_display *display, std::chrono::milliseconds timeout) {
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
    wl_callback_add_listener(callback, &listener, &done);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int result = -1;
    while (true) {
      if (wl_display_dispatch_pending(display) < 0) {
        break;
      }
      if (done) {
        result = 0;
        break;
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        errno = ETIMEDOUT;
        break;
      }
      if (wl_display_prepare_read(display) < 0) {
        continue;
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
      const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
      if (ready <= 0 || !(descriptor.revents & POLLIN)) {
        wl_display_cancel_read(display);
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready == 0) {
          errno = ETIMEDOUT;
        }
        if (ready <= 0) {
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
}  // namespace platf::wayland
