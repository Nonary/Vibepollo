/** @file Private Wayland connections with bounded observation and cancellation. */
#pragma once

#include "wayland_roundtrip.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::wayland_observation {
  inline wl_display *connect_until(std::chrono::steady_clock::time_point deadline, const std::function<bool()> &allowed) {
    if (!wayland::observation_allowed(deadline, allowed)) return nullptr;
    const char *name = std::getenv("WAYLAND_DISPLAY");
    if (!name || !*name) {
      errno = ENOENT;
      return nullptr;
    }
    std::string path {name};
    if (path.front() != '/') {
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      if (!runtime_dir || runtime_dir[0] != '/') {
        errno = EINVAL;
        return nullptr;
      }
      path = std::string {runtime_dir} + "/" + path;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
      errno = ENAMETOOLONG;
      return nullptr;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto address_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    const int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (descriptor < 0) return nullptr;
    const auto fail = [&]() -> wl_display * {
      const int saved_errno = errno;
      close(descriptor);
      errno = saved_errno;
      return nullptr;
    };
    bool connected = false;
    while (!connected) {
      if (!wayland::observation_allowed(deadline, allowed)) return fail();
      if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), address_length) == 0 || errno == EISCONN) {
        connected = true;
        break;
      }
      if (errno == EAGAIN) {
        // A full AF_UNIX listen backlog returns EAGAIN without starting a
        // connection. Waiting on this unconnected fd would spin on POLLHUP.
        if (poll(nullptr, 0, wayland::observation_poll_timeout(deadline)) < 0 && errno != EINTR) return fail();
        continue;
      }
      if (errno == EINTR) continue;
      if (errno != EINPROGRESS && errno != EALREADY) return fail();
      while (true) {
        if (!wayland::observation_allowed(deadline, allowed)) return fail();
        pollfd pending {descriptor, POLLOUT, 0};
        const int ready = poll(&pending, 1, wayland::observation_poll_timeout(deadline));
        if (ready < 0) {
          if (errno == EINTR) continue;
          return fail();
        }
        if (!ready) continue;
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0) return fail();
        if (socket_error) {
          errno = socket_error;
          return fail();
        }
        if (pending.revents & (POLLERR | POLLHUP | POLLNVAL)) {
          errno = ECONNREFUSED;
          return fail();
        }
        connected = true;
        break;
      }
    }
    if (!wayland::observation_allowed(deadline, allowed)) return fail();
    // libwayland takes ownership of descriptor on both success and failure.
    return wl_display_connect_to_fd(descriptor);
  }

  inline bool roundtrip_until(wl_display *display, std::chrono::steady_clock::time_point deadline, const std::function<bool()> &allowed) {
    return wayland::roundtrip(display, deadline, allowed) == 0;
  }
}  // namespace platf::wayland_observation
