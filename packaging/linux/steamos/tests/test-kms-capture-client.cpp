/* SPDX-License-Identifier: GPL-3.0-only
 * Malformed privileged replies must never leak descriptors into the host.
 */
#include "src/platform/linux/kms_capture_client.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <drm_fourcc.h>

namespace platf::kms_capture {
  struct client_test_access {
    static std::unique_ptr<client_t> connect(int socket) {
      return std::unique_ptr<client_t> {new client_t {socket, -1}};
    }
  };
}

namespace {
  using platf::kms_capture::client_test_access;

  int descriptor_count() {
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    int count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
  }

  vibeshine_drm_frame frame_request() {
    vibeshine_drm_frame result {};
    result.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION;
    result.crtc_id = 4;
    return result;
  }

  vibeshine_kms_capture_response frame_response() {
    vibeshine_kms_capture_response result {};
    result.magic = VIBESHINE_KMS_CAPTURE_MAGIC;
    result.version = VIBESHINE_KMS_CAPTURE_VERSION;
    result.operation = VIBESHINE_KMS_CAPTURE_FRAME;
    result.fd_count = 2;
    auto &frame = result.frame;
    frame.abi_version = VIBESHINE_DRM_FRAME_ABI_VERSION;
    frame.crtc_id = 4;
    frame.flags = VIBESHINE_DRM_FRAME_READY;
    frame.width = 3840; frame.height = 2160;
    frame.fourcc = DRM_FORMAT_XRGB2101010;
    frame.sequence = 7; frame.timestamp_ns = 123456;
    frame.plane_count = 1;
    for (unsigned i = 0; i < 4; ++i) frame.dma_buf_fds[i] = frame.sync_file_fds[i] = -1;
    frame.dma_buf_fds[0] = 0; frame.sync_file_fds[0] = 1;
    frame.pitches[0] = frame.width * 4;
    return result;
  }

  void receive_frame_request(int socket) {
    vibeshine_kms_capture_request request {};
    assert(recv(socket, &request, sizeof(request), 0) == sizeof(request));
    assert(vibeshine_kms_capture_request_valid(&request, true));
    assert(request.operation == VIBESHINE_KMS_CAPTURE_FRAME && request.crtc_id == 4);
  }

  void send_response(int socket, const vibeshine_kms_capture_response &response,
                     size_t fd_count, int payload_delta = 0) {
    assert(fd_count <= 16);
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    std::array<int, 16> descriptors;
    descriptors.fill(fd);
    std::array<unsigned char, CMSG_SPACE(sizeof(int) * 16)> controls {};
    std::array<unsigned char, sizeof(response) + 1> payload {};
    std::memcpy(payload.data(), &response, sizeof(response));
    iovec io {.iov_base = payload.data(), .iov_len = sizeof(response) + payload_delta};
    msghdr message {};
    message.msg_iov = &io; message.msg_iovlen = 1;
    if (fd_count) {
      message.msg_control = controls.data();
      message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
      cmsghdr *c = CMSG_FIRSTHDR(&message);
      c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
      std::memcpy(CMSG_DATA(c), descriptors.data(), sizeof(int) * fd_count);
    }
    assert(sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(io.iov_len));
    close(fd);
  }

  void malformed(const std::function<void(vibeshine_kms_capture_response &)> &mutate,
                  size_t descriptors = 2, int payload_delta = 0) {
    const int before = descriptor_count();
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    auto client = client_test_access::connect(pair[0]);
    std::thread peer {[&] {
      receive_frame_request(pair[1]);
      auto response = frame_response();
      mutate(response);
      send_response(pair[1], response, descriptors, payload_delta);
      close(pair[1]);
    }};
    auto frame = frame_request();
    const int result = client->frame(frame);
    const int error = errno;
    if (result != -1 || error != EPROTO) {
      std::cerr << "Malformed reply returned " << result << " with errno " << error << '\n';
    }
    assert(result == -1 && error == EPROTO);
    frame = frame_request();
    assert(client->frame(frame) == -1 && errno == ENOTCONN);
    peer.join();
    client.reset();
    assert(descriptor_count() == before);
  }

  void valid_and_recoverable_error() {
    const int before = descriptor_count();
    int pair[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    auto client = client_test_access::connect(pair[0]);
    std::thread peer {[&] {
      receive_frame_request(pair[1]);
      auto response = frame_response();
      response.error = EAGAIN; response.fd_count = 0; response.frame = {};
      send_response(pair[1], response, 0);
      receive_frame_request(pair[1]);
      send_response(pair[1], frame_response(), 2);
      close(pair[1]);
    }};
    auto frame = frame_request();
    assert(client->frame(frame) == -1 && errno == EAGAIN);
    frame = frame_request();
    assert(!client->frame(frame));
    assert(frame.width == 3840 && frame.height == 2160 && frame.fourcc == DRM_FORMAT_XRGB2101010);
    assert(frame.dma_buf_fds[0] >= 0 && frame.sync_file_fds[0] >= 0);
    assert(frame.dma_buf_fds[0] != frame.sync_file_fds[0]);
    assert(fcntl(frame.dma_buf_fds[0], F_GETFD) & FD_CLOEXEC);
    assert(fcntl(frame.sync_file_fds[0], F_GETFD) & FD_CLOEXEC);
    close(frame.dma_buf_fds[0]); close(frame.sync_file_fds[0]);
    peer.join();
    client.reset();
    assert(descriptor_count() == before);
  }
}

int main() {
  malformed([](auto &r) { r.magic ^= 1; });
  malformed([](auto &r) { ++r.version; });
  malformed([](auto &r) { r.operation = VIBESHINE_KMS_CAPTURE_WAIT; });
  malformed([](auto &r) { ++r.frame.crtc_id; });
  malformed([](auto &r) { r.frame.width = 32769; });
  malformed([](auto &r) { r.frame.reserved[0] = 1; });
  malformed([](auto &r) { r.frame.sync_file_fds[0] = 0; });
  malformed([](auto &r) { r.frame.sync_file_fds[0] = 2; });
  malformed([](auto &r) { r.frame.sync_file_fds[0] = -2; });
  malformed([](auto &r) { r.frame.sync_file_fds[0] = -1; });
  malformed([](auto &r) { r.fd_count = 3; });
  malformed([](auto &) {}, 1);
  malformed([](auto &r) { r.fd_count = 3; }, 3);
  malformed([](auto &r) { r.fd_count = 16; }, 16);
  malformed([](auto &r) { r.error = EIO; });
  malformed([](auto &) {}, 2, -1);
  malformed([](auto &) {}, 2, 1);
  valid_and_recoverable_error();
  std::cout << "Capture client rejected malformed replies, closed all received fds, and preserved valid HDR descriptors.\n";
}
