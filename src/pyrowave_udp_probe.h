#pragma once

#include <boost/asio.hpp>
#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <stdexcept>

namespace pyrowave::probe {
constexpr int duration_ms = 2000;
struct result_t {
  std::uint32_t expected = 0;
  std::uint32_t sent = 0;
  double elapsed_ms = 0;
  // Sends refused for a full buffer and retried, and the last refusal's error
  // value (0 = none). Unsent packets are expected - sent.
  std::uint32_t send_retries = 0;
  int last_send_error = 0;
};

// A momentarily full send buffer (a NIC stall or link pause) is retried for up
// to this long, as the stream sender does, rather than silently counted lost.
constexpr auto send_retry_budget = std::chrono::microseconds(1000);

// Frame-shaped schedule. The default spreads every millisecond's share evenly,
// which never exposes receivers that cannot absorb back-to-back traffic at link
// rate. With fps set, the same packets leave as video frames on the stream
// pacer's schedule: each frame starts on its cadence and sends pace_bps worth of
// packets back-to-back per 1 ms group.
struct burst_t {
  int fps = 0;
  std::uint64_t pace_bps = 0;
};

// Packets per 1 ms group for a probe datagram of payload_bytes, charging the
// UDP, IP and Ethernet preamble/gap/FCS bytes that occupy the link.
inline std::uint32_t burst_packets_per_ms(std::uint64_t pace_bps, std::size_t payload_bytes, bool ipv6) {
  const auto wire_bytes = payload_bytes + 8 + (ipv6 ? 40 : 20) + 38;
  return (std::max)(std::uint32_t(1), std::uint32_t(pace_bps / 8 / 1000 / wire_bytes));
}

// The caller authenticates the HTTPS peer and supplies only that peer's IP.
// No persistent socket, detached thread, or streaming session is created.
inline result_t send(const boost::asio::ip::address &local,
                     const boost::asio::ip::udp::endpoint &peer,
                     int kbps, int packetsize, const std::string &token,
                     const std::function<void(std::chrono::steady_clock::time_point)> &wait_until =
                       [](auto due) { std::this_thread::sleep_until(due); },
                     const std::function<void(unsigned short)> &announce_port = {},
                     const burst_t &burst = {}) {
  using namespace std::chrono;
  boost::asio::io_context io;
  boost::asio::ip::udp::socket socket(io);
  socket.open(peer.protocol());
  socket.bind({local, 0});
  socket.non_blocking(true);
  boost::system::error_code ec;
  socket.set_option(boost::asio::socket_base::send_buffer_size(4 * 1024 * 1024), ec);
  auto destination = peer;
  if (announce_port) {
    // Establish outbound state on the host too. This small warmup is never a
    // measured packet, and may be dropped until the client sends its token.
    socket.send_to(boost::asio::buffer(token), peer, 0, ec);
    announce_port(socket.local_endpoint().port());
    const auto deadline = steady_clock::now() + milliseconds(1500);
    auto next_warmup = steady_clock::now() + milliseconds(100);
    bool confirmed = false;
    while (steady_clock::now() < deadline) {
      std::array<char, 64> hello {};
      boost::asio::ip::udp::endpoint source;
      ec.clear();
      const auto bytes = socket.receive_from(boost::asio::buffer(hello), source, 0, ec);
      if (!ec && bytes == token.size() && source.address() == peer.address() &&
          source.port() >= 1024 && std::equal(token.begin(), token.end(), hello.begin())) {
        destination = source; // Retain the client's observed UDP return port.
        confirmed = true;
        break;
      }
      if (ec && ec != boost::asio::error::would_block && ec != boost::asio::error::try_again &&
          ec != boost::asio::error::message_size && ec != boost::asio::error::connection_refused &&
          ec != boost::asio::error::connection_reset) {
        throw boost::system::system_error(ec);
      }
      const auto now = steady_clock::now();
      if (now >= next_warmup) {
        socket.send_to(boost::asio::buffer(token), peer, 0, ec);
        next_warmup = now + milliseconds(100);
      }
      wait_until((std::min)(deadline, now + milliseconds(1)));
    }
    if (!confirmed) throw std::runtime_error("The client did not complete the UDP handshake");
  }
  // Same worst-case wire charge as the streaming budget: packet + RTP,
  // encryption, UDP, IPv6, Ethernet preamble/gap/FCS (134 bytes).
  const int wire_bytes = packetsize + 134;
  result_t result;
  result.expected = std::uint64_t(kbps) * duration_ms / (8 * wire_bytes);
  std::vector<unsigned char> packet(packetsize + 48, 0x5a);
  std::copy(token.begin(), token.end(), packet.begin()); // 32 ASCII hex bytes
  const auto start = steady_clock::now();
  std::uint32_t seq = 0;
  const auto send_until = [&](std::uint32_t end) {
    for (; seq < end; ++seq) {
      for (int byte = 0; byte < 4; ++byte) packet[32 + byte] = seq >> (24 - byte * 8);
      const auto retry_until = steady_clock::now() + send_retry_budget;
      bool retried = false;
      for (;;) {
        ec.clear();
        const auto bytes = socket.send_to(boost::asio::buffer(packet), destination, 0, ec);
        if (!ec && bytes == packet.size()) {
          ++result.sent;
          break;
        }
        if (ec) result.last_send_error = ec.value();
        if ((ec != boost::asio::error::would_block && ec != boost::asio::error::try_again &&
             ec != boost::asio::error::no_buffer_space) ||
            steady_clock::now() >= retry_until) {
          break;
        }
        if (!retried) {
          retried = true;
          ++result.send_retries;
        }
        std::this_thread::yield();
      }
    }
  };
  // Bound overload rather than accumulating an arbitrarily long send queue.
  const auto overloaded = [&] { return steady_clock::now() - start > milliseconds(duration_ms + 100); };
  if (burst.fps > 0) {
    const auto frames = (std::max)(std::uint32_t(1), std::uint32_t(std::uint64_t(burst.fps) * duration_ms / 1000));
    const auto group = burst_packets_per_ms(burst.pace_bps, packet.size(), destination.address().is_v6());
    for (std::uint32_t frame = 0; frame < frames && !overloaded(); ++frame) {
      const auto frame_start = start + nanoseconds(std::uint64_t(duration_ms) * 1'000'000 * frame / frames);
      const auto frame_end = std::uint32_t(std::uint64_t(result.expected) * (frame + 1) / frames);
      for (std::uint32_t quantum = 0; seq < frame_end && !overloaded(); ++quantum) {
        wait_until(frame_start + milliseconds(quantum));
        send_until((std::min)(frame_end, seq + group));
      }
    }
  }
  else {
    for (int tick = 0; tick < duration_ms; ++tick) {
      wait_until(start + milliseconds(tick));
      send_until(std::uint32_t(std::uint64_t(result.expected) * (tick + 1) / duration_ms));
      if (overloaded()) break;
    }
  }
  wait_until(start + milliseconds(duration_ms));
  result.elapsed_ms = duration<double, std::milli>(steady_clock::now() - start).count();
  return result;
}
} // namespace pyrowave::probe
