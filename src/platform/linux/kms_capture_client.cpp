/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include "kms_capture_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/capability.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <drm_fourcc.h>

namespace platf::kms_capture {
  namespace {
    using namespace std::chrono_literals;

    bool helper_is_trusted() {
      std::error_code error;
      const auto resolved = std::filesystem::canonical(VIBESHINE_KMS_CAPTURE_HELPER, error);
      if (error || !resolved.is_absolute()) return false;
      for (auto path = resolved; !path.empty(); path = path.parent_path()) {
        struct stat attributes {};
        if (lstat(path.c_str(), &attributes) || attributes.st_uid || (attributes.st_mode & 0022)) return false;
        if (path == resolved ? !S_ISREG(attributes.st_mode) : !S_ISDIR(attributes.st_mode)) return false;
        if (path == path.parent_path()) break;
      }
      cap_t actual = cap_get_file(resolved.c_str());
      cap_t expected = cap_init();
      cap_value_t admin = CAP_SYS_ADMIN;
      const bool valid = actual && expected && !cap_set_flag(expected, CAP_PERMITTED, 1, &admin, CAP_SET) &&
                         cap_compare(actual, expected) == 0;
      if (actual) cap_free(actual);
      if (expected) cap_free(expected);
      return valid;
    }

    vibeshine_kms_capture_request make_request(uint16_t operation) {
      vibeshine_kms_capture_request request {};
      request.magic = VIBESHINE_KMS_CAPTURE_MAGIC;
      request.version = VIBESHINE_KMS_CAPTURE_VERSION;
      request.operation = operation;
      return request;
    }

    bool zero_bytes(const void *object, size_t length) {
      const auto *bytes = static_cast<const unsigned char *>(object);
      return std::all_of(bytes, bytes + length, [](unsigned char value) { return value == 0; });
    }

    bool validate_frame(const vibeshine_drm_frame &frame, uint32_t crtc, bool framebuffer) {
      if (frame.abi_version != VIBESHINE_DRM_FRAME_ABI_VERSION || frame.crtc_id != crtc ||
          frame.reserved_u32 || !zero_bytes(frame.reserved, sizeof(frame.reserved))) return false;
      if (frame.flags == VIBESHINE_DRM_FRAME_EMPTY && !framebuffer) {
        if (frame.width || frame.height || frame.fourcc || frame.modifier || frame.plane_count) return false;
      } else if (frame.flags != VIBESHINE_DRM_FRAME_READY || !frame.width || !frame.height ||
                 frame.width > 32768 || frame.height > 32768 || !frame.fourcc ||
                 !frame.plane_count || frame.plane_count > 4 || (!framebuffer && (!frame.sequence || !frame.timestamp_ns))) return false;
      if (framebuffer && (frame.sequence || frame.timestamp_ns)) return false;
      for (unsigned i = 0; i < 4; ++i) {
        if (i < frame.plane_count) {
          if (frame.dma_buf_fds[i] < 0 || frame.sync_file_fds[i] < -1 || !frame.pitches[i] ||
              (framebuffer && frame.sync_file_fds[i] != -1)) return false;
        } else if (frame.dma_buf_fds[i] != -1 || frame.sync_file_fds[i] != -1 || frame.pitches[i] || frame.offsets[i]) return false;
      }
      return true;
    }
  }  // namespace

  std::unique_ptr<client_t> client_t::open(int ordinary_card_fd) {
    struct stat attributes {};
    if (fstat(ordinary_card_fd, &attributes) || !S_ISCHR(attributes.st_mode)) {
      errno = EINVAL;
      return nullptr;
    }
    if (!helper_is_trusted()) {
      errno = EACCES;
      return nullptr;
    }
    int pair[2] {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return nullptr;
    // Keep the source above fd 3 so file actions cannot overwrite their source.
    int source = fcntl(pair[1], F_DUPFD_CLOEXEC, 4);
    const int duplicate_error = errno;
    close(pair[1]);
    if (source < 0) { close(pair[0]); errno = duplicate_error; return nullptr; }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes_spawn;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { close(pair[0]); close(source); errno = error; return nullptr; }
    error = posix_spawnattr_init(&attributes_spawn);
    if (error) { posix_spawn_file_actions_destroy(&actions); close(pair[0]); close(source); errno = error; return nullptr; }
    auto step = [&](int result) { if (!error && result) error = result; };
    step(posix_spawn_file_actions_addclose(&actions, pair[0]));
    step(posix_spawn_file_actions_adddup2(&actions, source, VIBESHINE_KMS_CAPTURE_FD));
    step(posix_spawn_file_actions_addclosefrom_np(&actions, VIBESHINE_KMS_CAPTURE_FD + 1));
    step(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    step(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0));
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    for (int signal : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD}) sigaddset(&defaults, signal);
    step(posix_spawnattr_setsigmask(&attributes_spawn, &mask));
    step(posix_spawnattr_setsigdefault(&attributes_spawn, &defaults));
    step(posix_spawnattr_setflags(&attributes_spawn, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    char executable[] = VIBESHINE_KMS_CAPTURE_HELPER;
    char path[] = "PATH=/usr/bin:/bin";
    char locale[] = "LC_ALL=C";
    char *argv[] {executable, nullptr};
    char *environment[] {path, locale, nullptr};
    pid_t child = -1;
    if (!error) error = posix_spawn(&child, executable, &actions, &attributes_spawn, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes_spawn);
    close(source);
    if (error) { close(pair[0]); errno = error; return nullptr; }
    auto result = std::unique_ptr<client_t> {new client_t {pair[0], child}};
    auto request = make_request(VIBESHINE_KMS_CAPTURE_OPEN);
    request.card_major = major(attributes.st_rdev);
    request.card_minor = minor(attributes.st_rdev);
    vibeshine_kms_capture_response response {};
    if (result->transact(request, response)) return nullptr;
    return result;
  }

  client_t::~client_t() {
    const int saved_errno = errno;
    disconnect();
    if (child_ > 0) {
      const pid_t child = child_;
      child_ = -1;
      int status = 0;
      auto reap_until = [&](std::chrono::steady_clock::time_point deadline) {
        do {
          const pid_t waited = waitpid(child, &status, WNOHANG);
          if (waited == child || (waited < 0 && errno == ECHILD)) return true;
          if (waited < 0 && errno != EINTR) return true;
          std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
      };
      if (!reap_until(std::chrono::steady_clock::now() + 100ms)) {
        kill(child, SIGTERM);
        if (!reap_until(std::chrono::steady_clock::now() + 100ms)) {
          kill(child, SIGKILL);
          if (!reap_until(std::chrono::steady_clock::now() + 500ms)) {
            // A wedged driver must not block the capture worker's destructor.
            // Keep one waiter responsible for eventually reaping this child.
            try {
              std::thread {[child] {
                int status;
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
              }}.detach();
            } catch (...) {}
          }
        }
      }
    }
    errno = saved_errno;
  }

  void client_t::disconnect() noexcept {
    if (socket_ >= 0) {
      shutdown(socket_, SHUT_RDWR);
      close(socket_);
      socket_ = -1;
    }
  }

  int client_t::transact(const vibeshine_kms_capture_request &request,
                         vibeshine_kms_capture_response &response) {
    std::lock_guard lock {mutex_};
    std::array<int, VIBESHINE_KMS_CAPTURE_MAX_FDS> received_fds;
    received_fds.fill(-1);
    size_t descriptor_count = 0;
    auto fail = [&](int error, bool close_connection = true) {
      for (int fd : received_fds) if (fd >= 0) close(fd);
      if (close_connection) disconnect();
      errno = error;
      return -1;
    };
    if (socket_ < 0) return fail(ENOTCONN);
    if (!vibeshine_kms_capture_request_valid(&request, request.operation != VIBESHINE_KMS_CAPTURE_OPEN)) return fail(EINVAL, false);
    ssize_t sent;
    do { sent = send(socket_, &request, sizeof(request), MSG_NOSIGNAL | MSG_DONTWAIT); } while (sent < 0 && errno == EINTR);
    if (sent != sizeof(request)) return fail(sent < 0 ? errno : EIO);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(request.timeout_ms) + 1500ms;
    for (;;) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= 0ms) return fail(ETIMEDOUT);
      struct pollfd ready {.fd = socket_, .events = POLLIN, .revents = 0};
      const int result = poll(&ready, 1, static_cast<int>(remaining.count()));
      if (result < 0 && errno == EINTR) continue;
      if (result < 0) return fail(errno);
      if (!result) return fail(ETIMEDOUT);
      if (!(ready.revents & POLLIN)) return fail(ECONNRESET);
      break;
    }
    std::array<unsigned char, CMSG_SPACE(sizeof(int) * VIBESHINE_KMS_CAPTURE_MAX_FDS)> controls {};
    struct iovec io {.iov_base = &response, .iov_len = sizeof(response)};
    struct msghdr message {};
    message.msg_iov = &io; message.msg_iovlen = 1;
    message.msg_control = controls.data(); message.msg_controllen = controls.size();
    ssize_t received;
    do { received = recvmsg(socket_, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); } while (received < 0 && errno == EINTR);
    bool invalid_ancillary = false;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0) ||
          (c->cmsg_len - CMSG_LEN(0)) % sizeof(int)) { invalid_ancillary = true; continue; }
      const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const auto *fds = reinterpret_cast<const int *>(CMSG_DATA(c));
      for (size_t i = 0; i < count; ++i) {
        if (descriptor_count < received_fds.size()) received_fds[descriptor_count++] = fds[i];
        else { close(fds[i]); invalid_ancillary = true; }
      }
    }
    if (received < 0) return fail(errno);
    if (received != sizeof(response) || invalid_ancillary || message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
        response.magic != VIBESHINE_KMS_CAPTURE_MAGIC || response.version != VIBESHINE_KMS_CAPTURE_VERSION ||
        response.operation != request.operation || response.error < 0 || response.error > 4095 ||
        response.fd_count != descriptor_count) return fail(EPROTO);
    if (response.error) {
      if (descriptor_count || !zero_bytes(&response.frame, sizeof(response.frame)) ||
          !zero_bytes(&response.presentation, sizeof(response.presentation))) return fail(EPROTO);
      return fail(response.error, false);
    }
    if (request.operation == VIBESHINE_KMS_CAPTURE_OPEN) {
      if (descriptor_count || !zero_bytes(&response.frame, sizeof(response.frame)) ||
          !zero_bytes(&response.presentation, sizeof(response.presentation))) return fail(EPROTO);
    } else if (request.operation == VIBESHINE_KMS_CAPTURE_WAIT) {
      const auto &value = response.presentation;
      if (descriptor_count || !zero_bytes(&response.frame, sizeof(response.frame)) ||
          value.abi_version != VIBESHINE_DRM_PRESENT_ABI_VERSION || value.crtc_id != request.crtc_id ||
          value.timeout_ms != request.timeout_ms || value.reserved[0] || value.reserved[1] ||
          (value.flags & ~(VIBESHINE_DRM_PRESENT_CHANGED | VIBESHINE_DRM_PRESENT_TIMEOUT | VIBESHINE_DRM_PRESENT_PENDING))) return fail(EPROTO);
    } else {
      if (!zero_bytes(&response.presentation, sizeof(response.presentation)) ||
          !validate_frame(response.frame, request.crtc_id, request.operation == VIBESHINE_KMS_CAPTURE_FB)) return fail(EPROTO);
      std::array<bool, VIBESHINE_KMS_CAPTURE_MAX_FDS> used {};
      for (unsigned i = 0; i < 4; ++i) {
        for (int32_t *index : {&response.frame.dma_buf_fds[i], &response.frame.sync_file_fds[i]}) {
          if (*index == -1) continue;
          if (*index < 0 || static_cast<size_t>(*index) >= descriptor_count || used[*index]) return fail(EPROTO);
          used[*index] = true;
        }
      }
      if (static_cast<size_t>(std::count(used.begin(), used.end(), true)) != descriptor_count) return fail(EPROTO);
      for (unsigned i = 0; i < 4; ++i) {
        for (int32_t *index : {&response.frame.dma_buf_fds[i], &response.frame.sync_file_fds[i]}) {
          if (*index >= 0) {
            const int fd = received_fds[*index];
            received_fds[*index] = -1;
            *index = fd;
          }
        }
      }
    }
    return 0;
  }

  int client_t::framebuffer(uint32_t plane_id, uint32_t crtc_id, uint32_t fb_id,
                            drmModeFB2 &metadata, std::array<int, 4> &dma_buf_fds) {
    dma_buf_fds.fill(-1);
    auto request = make_request(VIBESHINE_KMS_CAPTURE_FB);
    request.plane_id = plane_id; request.crtc_id = crtc_id; request.fb_id = fb_id;
    vibeshine_kms_capture_response response {};
    if (transact(request, response)) return -1;
    metadata = {};
    metadata.fb_id = fb_id; metadata.width = response.frame.width; metadata.height = response.frame.height;
    metadata.pixel_format = response.frame.fourcc; metadata.modifier = response.frame.modifier;
    if (metadata.modifier != DRM_FORMAT_MOD_INVALID) metadata.flags = DRM_MODE_FB_MODIFIERS;
    std::copy_n(response.frame.pitches, 4, metadata.pitches);
    std::copy_n(response.frame.offsets, 4, metadata.offsets);
    std::copy_n(response.frame.dma_buf_fds, 4, dma_buf_fds.begin());
    return 0;
  }

  int client_t::wait(vibeshine_drm_wait_present &value) {
    if (value.abi_version != VIBESHINE_DRM_PRESENT_ABI_VERSION || value.flags || value.timestamp_ns ||
        value.reserved[0] || value.reserved[1]) { errno = EINVAL; return -1; }
    auto request = make_request(VIBESHINE_KMS_CAPTURE_WAIT);
    request.crtc_id = value.crtc_id; request.sequence = value.sequence; request.timeout_ms = value.timeout_ms;
    vibeshine_kms_capture_response response {};
    if (transact(request, response)) return -1;
    value = response.presentation;
    return 0;
  }

  int client_t::frame(vibeshine_drm_frame &value) {
    vibeshine_drm_frame expected {};
    expected.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION; expected.crtc_id = value.crtc_id;
    if (std::memcmp(&value, &expected, sizeof(value))) { errno = EINVAL; return -1; }
    auto request = make_request(VIBESHINE_KMS_CAPTURE_FRAME);
    request.crtc_id = value.crtc_id;
    vibeshine_kms_capture_response response {};
    if (transact(request, response)) return -1;
    value = response.frame;
    return 0;
  }
}  // namespace platf::kms_capture
