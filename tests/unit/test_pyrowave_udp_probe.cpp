#include "src/pyrowave_udp_probe.h"
#include <gtest/gtest.h>
#include <future>
#include <cstring>

static void handshakeProbe(const char* address) {
  using namespace boost::asio;
  using namespace std::chrono;
  io_context io;
  const auto local = ip::make_address(address);
  ip::udp::socket receiver(io, {local, 0});
  receiver.non_blocking(true);
  receiver.set_option(socket_base::receive_buffer_size(1024 * 1024));
  const auto peer = receiver.local_endpoint();
  const std::string token(32, 'b');
  std::promise<unsigned short> announced;
  auto port = announced.get_future();
  auto sender = std::async(std::launch::async, [&] {
    return pyrowave::probe::send(local, peer, 5000, 1392, token,
      [](auto due) { std::this_thread::sleep_until(due); },
      [&](unsigned short value) { announced.set_value(value); });
  });
  const auto sender_port = port.get();
  const ip::udp::endpoint target(local, sender_port);
  // A wrong token and a token from the wrong IP must not authorize sending.
  const std::string wrong(32, 'c');
  receiver.send_to(buffer(wrong), target);
  receiver.send_to(buffer(token.data(), token.size() - 1), target);
  if (local.is_v4()) {
    ip::udp::socket stranger(io, {ip::make_address("127.0.0.2"), 0});
    stranger.send_to(buffer(token), target);
  }
  const auto warmup_until = steady_clock::now() + milliseconds(250);
  unsigned warmups = 0;
  while (steady_clock::now() < warmup_until) {
    unsigned char data[2048];
    ip::udp::endpoint source;
    boost::system::error_code error;
    const auto bytes = receiver.receive_from(buffer(data), source, 0, error);
    if (error == error::would_block || error == error::try_again) {
      std::this_thread::sleep_for(microseconds(100));
      continue;
    }
    ASSERT_FALSE(error);
    ASSERT_EQ(bytes, token.size()) << "Measured transfer began before a valid UDP token";
    ASSERT_EQ(std::memcmp(data, token.data(), token.size()), 0);
    ASSERT_EQ(source.port(), sender_port);
    ++warmups;
  }
  EXPECT_GE(warmups, 2u);
  receiver.send_to(buffer(token), target);
  constexpr unsigned expected = std::uint64_t(5000) * 2000 / (8 * (1392 + 134));
  std::vector<bool> seen(expected, false);
  unsigned received = 0;
  const auto deadline = steady_clock::now() + milliseconds(2300);
  while (steady_clock::now() < deadline) {
    unsigned char data[2048];
    ip::udp::endpoint source;
    boost::system::error_code error;
    const auto bytes = receiver.receive_from(buffer(data), source, 0, error);
    if (error == error::would_block || error == error::try_again) {
      std::this_thread::sleep_for(microseconds(100));
      continue;
    }
    ASSERT_FALSE(error);
    if (bytes == token.size()) continue;
    ASSERT_EQ(bytes, 1392u + 48);
    ASSERT_EQ(source.port(), sender_port);
    ASSERT_EQ(std::memcmp(data, token.data(), token.size()), 0);
    const auto seq = (std::uint32_t(data[32]) << 24) | (std::uint32_t(data[33]) << 16) |
                     (std::uint32_t(data[34]) << 8) | data[35];
    ASSERT_LT(seq, expected);
    ASSERT_FALSE(seen[seq]);
    seen[seq] = true;
    ++received;
  }
  const auto result = sender.get();
  EXPECT_EQ(result.expected, expected);
  EXPECT_EQ(result.sent, expected);
  EXPECT_EQ(received, expected);
  EXPECT_GE(result.elapsed_ms, 2000);
  EXPECT_LT(result.elapsed_ms, 2200); // Handshake wait is excluded from send timing.
}

TEST(PyroWaveUdpProbe, HandshakeIpv4) { handshakeProbe("127.0.0.1"); }
TEST(PyroWaveUdpProbe, HandshakeIpv6) { handshakeProbe("::1"); }

TEST(PyroWaveUdpProbe, MissingHandshakeIsBoundedAndSendsNoMeasuredPackets) {
  using namespace boost::asio;
  using namespace std::chrono;
  io_context io;
  const auto local = ip::make_address("127.0.0.1");
  ip::udp::socket receiver(io, {local, 0});
  receiver.non_blocking(true);
  const auto start = steady_clock::now();
  unsigned announcements = 0;
  EXPECT_THROW(pyrowave::probe::send(local, receiver.local_endpoint(), 5000, 1392, std::string(32, 'a'),
    [](auto due) { std::this_thread::sleep_until(due); },
    [&](unsigned short) { ++announcements; }), std::runtime_error);
  EXPECT_EQ(announcements, 1u);
  const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
  EXPECT_GE(elapsed, 1500);
  EXPECT_LT(elapsed, 2200);
  for (;;) {
    unsigned char data[2048];
    boost::system::error_code error;
    const auto bytes = receiver.receive(buffer(data), 0, error);
    if (error == error::would_block || error == error::try_again) break;
    ASSERT_FALSE(error);
    EXPECT_EQ(bytes, 32u);
  }
}

TEST(PyroWaveUdpProbe, BoundedPacedPacketsHaveUniqueSequenceAndToken) {
  using namespace boost::asio;
  io_context io;
  const auto loopback = ip::make_address("127.0.0.1");
  ip::udp::socket receiver(io, {loopback, 0});
  receiver.non_blocking(true);
  receiver.set_option(socket_base::receive_buffer_size(1024 * 1024));
  const auto peer = receiver.local_endpoint();
  const std::string token(32, 'a');
  constexpr int rate = 5000, packet_size = 1392;
  constexpr unsigned expected = std::uint64_t(rate) * 2000 / (8 * (packet_size + 134));
  std::vector<bool> seen(expected, false);
  unsigned received = 0;
  const auto start = std::chrono::steady_clock::now();
  auto sender = std::async(std::launch::async, [&] {
    return pyrowave::probe::send(loopback, peer, rate, packet_size, token);
  });
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2300)) {
    unsigned char data[2048];
    boost::system::error_code error;
    const auto bytes = receiver.receive(buffer(data), 0, error);
    if (error == error::would_block || error == error::try_again) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    ASSERT_FALSE(error);
    ASSERT_EQ(bytes, packet_size + 48u);
    ASSERT_EQ(std::memcmp(data, token.data(), 32), 0);
    const auto seq = (std::uint32_t(data[32]) << 24) | (std::uint32_t(data[33]) << 16) |
                     (std::uint32_t(data[34]) << 8) | data[35];
    ASSERT_LT(seq, expected);
    ASSERT_FALSE(seen[seq]);
    seen[seq] = true;
    ++received;
  }
  const auto result = sender.get();
  EXPECT_EQ(result.expected, expected);
  EXPECT_EQ(result.sent, expected);
  EXPECT_EQ(received, expected);
  EXPECT_GE(result.elapsed_ms, 2000);
  // An unloaded loopback never refuses a send; refusals would be reported.
  EXPECT_EQ(result.send_retries, 0u);
  EXPECT_EQ(result.last_send_error, 0);
}

TEST(PyroWaveUdpProbe, BurstGroupChargesWireOverhead) {
  // 1392-byte probes are 1440-byte datagrams; IPv4 adds 66 bytes on the wire.
  EXPECT_EQ(pyrowave::probe::burst_packets_per_ms(100'000'000, 1440, false), 8u);
  EXPECT_EQ(pyrowave::probe::burst_packets_per_ms(2'500'000'000, 1440, false), 207u);
  EXPECT_EQ(pyrowave::probe::burst_packets_per_ms(2'500'000'000, 1440, true), 204u);
  EXPECT_EQ(pyrowave::probe::burst_packets_per_ms(1, 1440, false), 1u);
}

TEST(PyroWaveUdpProbe, BurstScheduleSendsFramesInPacedMillisecondGroups) {
  using namespace boost::asio;
  using namespace std::chrono;
  io_context io;
  const auto loopback = ip::make_address("127.0.0.1");
  ip::udp::socket receiver(io, {loopback, 0});
  receiver.non_blocking(true);
  receiver.set_option(socket_base::receive_buffer_size(4 * 1024 * 1024));
  const auto peer = receiver.local_endpoint();
  const std::string token(32, 'd');
  // 20 frames of ~164 packets, paced 8 packets per 1 ms group: 21 groups each.
  constexpr int rate = 20000, packet_size = 1392, fps = 10;
  constexpr unsigned expected = std::uint64_t(rate) * 2000 / (8 * (packet_size + 134));
  std::vector<steady_clock::time_point> dues;
  auto sender = std::async(std::launch::async, [&] {
    return pyrowave::probe::send(loopback, peer, rate, packet_size, token,
      [&](auto due) { dues.push_back(due); std::this_thread::sleep_until(due); }, {},
      pyrowave::probe::burst_t {fps, 100'000'000});
  });
  std::vector<bool> seen(expected, false);
  unsigned received = 0;
  const auto deadline = steady_clock::now() + milliseconds(2300);
  while (steady_clock::now() < deadline) {
    unsigned char data[2048];
    boost::system::error_code error;
    const auto bytes = receiver.receive(buffer(data), 0, error);
    if (error == error::would_block || error == error::try_again) {
      std::this_thread::sleep_for(microseconds(100));
      continue;
    }
    ASSERT_FALSE(error);
    ASSERT_EQ(bytes, packet_size + 48u);
    const auto seq = (std::uint32_t(data[32]) << 24) | (std::uint32_t(data[33]) << 16) |
                     (std::uint32_t(data[34]) << 8) | data[35];
    ASSERT_LT(seq, expected);
    ASSERT_FALSE(seen[seq]);
    seen[seq] = true;
    ++received;
  }
  const auto result = sender.get();
  EXPECT_EQ(result.sent, expected);
  EXPECT_EQ(received, expected);
  ASSERT_FALSE(dues.empty());
  std::vector<unsigned> groups_per_frame(fps * 2, 0);
  for (const auto due : dues) {
    const auto offset = duration_cast<microseconds>(due - dues.front()).count();
    if (offset >= 2'000'000) continue; // Final wait for the sending window to close.
    EXPECT_EQ(offset % 1000, 0) << "Groups start on whole milliseconds from their frame";
    const auto ms = offset / 1000;
    EXPECT_LT(ms % 100, 21) << "Frame packets leave as one paced burst at the frame start";
    ++groups_per_frame[ms / 100];
  }
  for (const auto groups : groups_per_frame) EXPECT_EQ(groups, 21u);
}
