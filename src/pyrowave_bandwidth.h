#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>

// Keep identical in Vibeshine and Moonlight. Wire budgets include Ethernet
// preamble/gap/FCS, IPv6, UDP, RTP and encrypted-video headers. Compression
// savings are never needed to meet the cap.
namespace pyrowave::bandwidth {
struct transport_t {
    int packetsize = 1392;
    int critical_fec_percentage = 20;
    std::size_t min_parity_shards = 2;
    std::size_t envelope_bytes = 0; // Independent compression has a raw fallback.
};
constexpr std::size_t wire_overhead = 134;
constexpr std::size_t frame_header = 8;
constexpr double audio_control_kbps = 3572; // 8 HQ channels, 50% audio FEC + control.
constexpr double delivery_target_ms = 4.0;

inline std::size_t max_critical_parity(const transport_t& t, std::size_t packets = 255) {
    std::size_t result = 0;
    if (t.critical_fec_percentage <= 0) return result;
    for (std::size_t critical = 1; critical <= (std::min)(packets, std::size_t(255)); ++critical) {
        const auto parity = (std::max)((critical * std::size_t(t.critical_fec_percentage) + 99) / 100, t.min_parity_shards);
        if (critical + parity <= 255) result = (std::max)(result, parity);
    }
    return result;
}
inline std::size_t image_bytes(std::size_t wire_bytes, const transport_t& t) {
    if (t.packetsize <= 16) return 0;
    const auto packets = wire_bytes / (std::size_t(t.packetsize) + wire_overhead);
    const auto parity = max_critical_parity(t, packets);
    if (packets <= parity) return 0;
    const auto data = (packets - parity) * std::size_t(t.packetsize - 16);
    const auto headers = frame_header + t.envelope_bytes;
    return data > headers ? (data - headers) & ~std::size_t(3) : 0;
}
inline std::size_t wire_bytes(std::size_t image, const transport_t& t) {
    if (t.packetsize <= 16 || !image) return 0;
    const auto payload = std::size_t(t.packetsize - 16);
    const auto data = (image + frame_header + t.envelope_bytes + payload - 1) / payload;
    // Reserve only one critical block, never a percentage of every detail byte.
    return (data + max_critical_parity(t)) * (std::size_t(t.packetsize) + wire_overhead);
}
inline double image_kbps(double total_kbps, int fps, const transport_t& t) {
    if (fps <= 0 || total_kbps <= audio_control_kbps) return 0;
    return image_bytes(std::size_t((total_kbps - audio_control_kbps) * 125 / fps), t) * fps / 125.0;
}
inline double total_kbps(double image_kbps, int fps, const transport_t& t) {
    if (fps <= 0 || image_kbps <= 0) return 0;
    return wire_bytes(std::size_t(std::ceil(image_kbps * 125 / fps)), t) * fps / 125.0 + audio_control_kbps;
}
inline double wire_ms(double total_kbps, int fps, double link_mbps) {
    if (fps <= 0 || link_mbps <= 0) return INFINITY;
    return total_kbps / (fps * link_mbps);
}
inline std::uint64_t pacing_link_bps(std::uint64_t host, std::uint64_t receiver) {
    return host && receiver ? (std::min)(host, receiver) : (std::max)(host, receiver);
}
inline double paced_link_mbps(double measured_mbps, const transport_t& t) {
    if (measured_mbps <= 0 || t.packetsize <= 16) return 0;
    const auto bytes = t.packetsize + wire_overhead;
    // Sender paces whole packets in 1 ms groups. Include that quantization.
    return std::floor(measured_mbps * 1000 / (8 * bytes)) * 8 * bytes / 1000;
}
} // namespace pyrowave::bandwidth
