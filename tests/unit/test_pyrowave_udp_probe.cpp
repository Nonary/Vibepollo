#include "src/pyrowave_udp_probe.h"
#include <gtest/gtest.h>
#include <future>
#include <cstring>

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
}
