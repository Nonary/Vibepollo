/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include "private_display_mode_client.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace platf::linux_private_display {
  namespace {
    constexpr char control_socket[] = "/run/vibeshine/vkms-control.sock";

    bool wait_ready(int fd, short events, std::chrono::steady_clock::time_point deadline) {
      while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;
        pollfd descriptor {.fd = fd, .events = events, .revents = 0};
        const auto status = poll(&descriptor, 1, static_cast<int>(std::min<long long>(remaining.count(), INT_MAX)));
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0 || (descriptor.revents & (POLLERR | POLLNVAL))) return false;
        return (descriptor.revents & events) || (events == POLLIN && (descriptor.revents & POLLHUP));
      }
    }
  }

  mode_admission_result_t detail::transact_requested_mode(
    int fd,
    const std::string_view output_name,
    const mode_policy::requested_mode_t mode,
    const uid_t expected_peer,
    const std::chrono::steady_clock::time_point deadline
  ) {
    if (!mode_policy::managed_connector_name(output_name) || !mode.valid()) {
      return {false, "invalid managed connector or requested mode"};
    }
    ucred peer {};
    socklen_t peer_size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) ||
        peer_size != sizeof(peer) || peer.uid != expected_peer) {
      return {false, "requested-mode broker has an unexpected peer identity"};
    }
    const std::string payload = std::string {output_name} + " " + std::to_string(mode.width) + " " +
                                std::to_string(mode.height) + " " + std::to_string(mode.refresh_millihz);
    const std::string request = "mode " + payload + "\n";
    for (std::size_t offset = 0; offset < request.size();) {
      if (!wait_ready(fd, POLLOUT, deadline)) return {false, "requested-mode broker write timed out or failed"};
      const auto count = send(fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count <= 0) return {false, "requested-mode broker connection closed while writing"};
      offset += static_cast<std::size_t>(count);
    }
    std::string reply;
    while (reply.size() <= 128) {
      if (!wait_ready(fd, POLLIN, deadline)) return {false, "requested-mode broker reply timed out or failed"};
      char buffer[64];
      const auto count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (count <= 0) return {false, "requested-mode broker closed without a complete reply"};
      reply.append(buffer, static_cast<std::size_t>(count));
      if (reply.size() > 128) break;
      const auto newline = reply.find('\n');
      if (newline == std::string::npos) continue;
      if (newline != reply.size() - 1 || reply.find('\r') != std::string::npos) {
        return {false, "malformed requested-mode broker reply"};
      }
      if (reply == "OK mode " + payload + "\n") return {true, {}};
      reply.pop_back();
      return {false, "requested-mode admission rejected: " + reply};
    }
    return {false, "requested-mode broker reply exceeds its size limit"};
  }

  mode_admission_result_t request_managed_mode(const std::string_view output_name, const mode_policy::requested_mode_t mode) {
    if (!mode_policy::managed_connector_name(output_name) || !mode.valid()) {
      return {false, "requested mode is outside the managed display limits"};
    }
    struct stat attributes {};
    if (lstat(control_socket, &attributes) || !S_ISSOCK(attributes.st_mode) ||
        attributes.st_uid != 0 || (attributes.st_mode & 0007)) {
      return {false, "root-owned requested-mode broker socket is unavailable"};
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return {false, "could not create requested-mode broker connection"};
    struct close_t {
      int fd;
      ~close_t() { close(fd); }
    } close_fd {fd};
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, control_socket, sizeof(control_socket));
    // Keep this later than the broker's five-second runtime and one-second
    // cgroup stop limits: no timed-out mutation may outlive the admission fence.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {8};
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
      if (errno != EINPROGRESS || !wait_ready(fd, POLLOUT, deadline)) {
        return {false, "could not connect to requested-mode broker"};
      }
      int error = 0;
      socklen_t size = sizeof(error);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
        return {false, "requested-mode broker connection failed"};
      }
    }
    return detail::transact_requested_mode(fd, output_name, mode, 0, deadline);
  }
}  // namespace platf::linux_private_display
