#include "../../../tests_common.h"

#include <src/platform/linux/wayland_roundtrip.h>
#include <sys/socket.h>
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
