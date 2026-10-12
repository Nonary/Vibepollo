#include "../../../tests_common.h"

#include <src/platform/linux/wayland_roundtrip.h>
#include <array>
#include <atomic>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

TEST(WaylandRoundtrip, UnresponsiveCompositorTimesOut) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  auto *display = wl_display_connect_to_fd(sockets[0]);
  ASSERT_NE(display, nullptr);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(platf::wayland::roundtrip(display, std::chrono::milliseconds {30}), -1);
  EXPECT_EQ(errno, ETIMEDOUT);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds {1});
  wl_display_disconnect(display);
  close(sockets[1]);
}

TEST(WaylandRoundtrip, DisconnectedCompositorFailsImmediately) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  auto *display = wl_display_connect_to_fd(sockets[0]);
  ASSERT_NE(display, nullptr);
  close(sockets[1]);
  EXPECT_EQ(platf::wayland::roundtrip(display, std::chrono::milliseconds {30}), -1);
  wl_display_disconnect(display);
}

TEST(WaylandRoundtrip, CancellationInterruptsSilentCompositor) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  auto *display = wl_display_connect_to_fd(sockets[0]);
  ASSERT_NE(display, nullptr);
  std::atomic<bool> allowed {true};
  std::jthread cancel {[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
    allowed = false;
  }};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(platf::wayland::roundtrip(display, start + std::chrono::seconds {5}, [&] { return allowed.load(); }), -1);
  EXPECT_EQ(errno, ECANCELED);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds {500});
  wl_display_disconnect(display);
  close(sockets[1]);
}

TEST(WaylandRoundtrip, ExpiredDeadlineDoesNotSendSync) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  auto *display = wl_display_connect_to_fd(sockets[0]);
  ASSERT_NE(display, nullptr);
  EXPECT_EQ(platf::wayland::roundtrip(display, std::chrono::steady_clock::now(), [] { return true; }), -1);
  EXPECT_EQ(errno, ETIMEDOUT);
  char value;
  EXPECT_EQ(recv(sockets[1], &value, 1, MSG_DONTWAIT), -1);
  EXPECT_EQ(errno, EAGAIN);
  wl_display_disconnect(display);
  close(sockets[1]);
}

TEST(WaylandRoundtrip, SyncResponseCompletesOnPrivateProtocolPeer) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
  auto *display = wl_display_connect_to_fd(sockets[0]);
  ASSERT_NE(display, nullptr);
  std::atomic<bool> replied {false};
  std::jthread server {[&] {
    std::array<std::uint32_t, 3> request {};
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {1};
    while (received < sizeof(request)) {
      if (std::chrono::steady_clock::now() >= deadline) return;
      pollfd descriptor {sockets[1], POLLIN, 0};
      if (poll(&descriptor, 1, 50) <= 0) continue;
      const auto amount = recv(sockets[1], reinterpret_cast<char *>(request.data()) + received, sizeof(request) - received, MSG_DONTWAIT);
      if (amount <= 0) return;
      received += amount;
    }
    // wl_display.sync contains the new callback id; wl_callback.done echoes
    // that object id with opcode 0 and a single uint32_t callback value.
    const std::array<std::uint32_t, 3> response {request[2], 12U << 16, 0};
    replied = send(sockets[1], response.data(), sizeof(response), MSG_NOSIGNAL) == sizeof(response);
  }};
  EXPECT_EQ(platf::wayland::roundtrip(display, std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; }), 0);
  server.join();
  EXPECT_TRUE(replied.load());
  wl_display_disconnect(display);
  close(sockets[1]);
}
