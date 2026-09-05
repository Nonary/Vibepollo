/* SPDX-License-Identifier: GPL-3.0-only */
#include <gtest/gtest.h>
#include <src/platform/linux/private_display_mode_client.h>

#include <chrono>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace display = platf::linux_private_display;
using namespace std::chrono_literals;

namespace {
  constexpr display::mode_policy::requested_mode_t mac_mode {3024, 1890, 120000};

  struct connection_t {
    int sockets[2] {-1, -1};
    connection_t() {
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) throw std::runtime_error("socketpair failed");
    }
    ~connection_t() {
      close(sockets[0]);
      close(sockets[1]);
    }
    display::mode_admission_result_t transact(std::string_view name = "Virtual-1",
                                            display::mode_policy::requested_mode_t mode = mac_mode,
                                            uid_t uid = getuid()) {
      return display::detail::transact_requested_mode(sockets[0], name, mode, uid,
                                                      std::chrono::steady_clock::now() + 250ms);
    }
  };
}

TEST(LinuxPrivateDisplayModeClient, SendsExactRequestedModeAndAcceptsReplyBeforeHangup) {
  connection_t connection;
  std::string received;
  std::jthread broker([&] {
    char data[128];
    const auto count = recv(connection.sockets[1], data, sizeof(data), 0);
    if (count > 0) received.assign(data, static_cast<std::size_t>(count));
    const std::string reply = "OK mode Virtual-1 3024 1890 120000\n";
    send(connection.sockets[1], reply.data(), reply.size(), MSG_NOSIGNAL);
    shutdown(connection.sockets[1], SHUT_WR);
  });
  const auto result = connection.transact();
  broker.join();
  EXPECT_TRUE(result.success) << result.detail;
  EXPECT_EQ(received, "mode Virtual-1 3024 1890 120000\n");
}

TEST(LinuxPrivateDisplayModeClient, AcceptsFragmentedReplyWithExactFractionalMode) {
  connection_t connection;
  std::jthread broker([&] {
    char data[128];
    recv(connection.sockets[1], data, sizeof(data), 0);
    const std::string reply = "OK mode Virtual-3 3025 1891 119880\n";
    send(connection.sockets[1], reply.data(), 8, MSG_NOSIGNAL);
    std::this_thread::sleep_for(5ms);
    send(connection.sockets[1], reply.data() + 8, reply.size() - 8, MSG_NOSIGNAL);
  });
  const auto result = connection.transact("Virtual-3", {3025, 1891, 119880});
  EXPECT_TRUE(result.success) << result.detail;
}

TEST(LinuxPrivateDisplayModeClient, RejectsWrongModeAcknowledgementAndMalformedReplies) {
  for (const std::string reply : {
         "OK mode Virtual-1 1920 1080 60000\n",
         "OK mode Virtual-2 3024 1890 120000\n",
         "OK mode Virtual-1 3024 1890 120000\r\n",
         "OK mode Virtual-1 3024 1890 120000\nextra\n",
         "ERROR connector owned by another uid\n",
       }) {
    connection_t connection;
    send(connection.sockets[1], reply.data(), reply.size(), MSG_NOSIGNAL);
    const auto result = connection.transact();
    EXPECT_FALSE(result.success) << reply;
  }
}

TEST(LinuxPrivateDisplayModeClient, RejectsUntrustedPeerBeforeSendingAnything) {
  connection_t connection;
  const auto result = connection.transact("Virtual-1", mac_mode, getuid() + 1);
  EXPECT_FALSE(result.success);
  char byte;
  EXPECT_EQ(recv(connection.sockets[1], &byte, 1, MSG_DONTWAIT), -1);
}

TEST(LinuxPrivateDisplayModeClient, RejectsInvalidRequestBeforeSendingAnything) {
  connection_t connection;
  EXPECT_FALSE(connection.transact("DP-1").success);
  EXPECT_FALSE(connection.transact("Virtual-1\nconnect Virtual-2").success);
  EXPECT_FALSE(connection.transact("Virtual-1", {3024, 1890, 0}).success);
  char byte;
  EXPECT_EQ(recv(connection.sockets[1], &byte, 1, MSG_DONTWAIT), -1);
}

TEST(LinuxPrivateDisplayModeClient, BoundsIncompleteAndOversizedReplies) {
  connection_t incomplete;
  const std::string prefix = "OK mode Virtual-1 3024";
  send(incomplete.sockets[1], prefix.data(), prefix.size(), MSG_NOSIGNAL);
  shutdown(incomplete.sockets[1], SHUT_WR);
  EXPECT_FALSE(incomplete.transact().success);
  connection_t oversized;
  const std::string body(150, 'x');
  send(oversized.sockets[1], body.data(), body.size(), MSG_NOSIGNAL);
  EXPECT_FALSE(oversized.transact().success);
}

TEST(LinuxPrivateDisplayModeClient, TimesOutWithoutReceivingAcknowledgement) {
  connection_t connection;
  const auto start = std::chrono::steady_clock::now();
  const auto result = display::detail::transact_requested_mode(
    connection.sockets[0], "Virtual-1", mac_mode, getuid(), start + 20ms);
  EXPECT_FALSE(result.success);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
}
