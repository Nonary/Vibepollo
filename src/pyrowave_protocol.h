/**
 * @file src/pyrowave_protocol.h
 * @brief Wire constants for PyroWave over the GameStream/Sunshine protocol.
 *
 * The contract is docs/pyrowave-protocol.md, shared with the moonlight-qt fork.
 * The capability bits and bitStreamFormat value match the Aurora/Solarflare
 * PyroWave implementation. Vibeshine builds against upstream moonlight-common-c,
 * so the host-side constants live here rather than in Limelight.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pyrowave::protocol {
  // ServerCodecModeSupport bits advertised in /serverinfo.
  constexpr std::uint32_t SCM_PYROWAVE = 0x00800000;  ///< 8-bit 4:2:0
  constexpr std::uint32_t SCM_PYROWAVE_444 = 0x01000000;  ///< 8-bit 4:4:4
  constexpr std::uint32_t SCM_PYROWAVE_HDR10 = 0x02000000;  ///< 10-bit 4:2:0 (HDR10 on an HDR display)
  constexpr std::uint32_t SCM_PYROWAVE_HDR10_444 = 0x04000000;  ///< 10-bit 4:4:4
  constexpr std::uint32_t SCM_MASK_PYROWAVE = SCM_PYROWAVE | SCM_PYROWAVE_444 | SCM_PYROWAVE_HDR10 | SCM_PYROWAVE_HDR10_444;

  /// `x-nv-vqos[0].bitStreamFormat` value selecting PyroWave (0/1/2 are H.264/HEVC/AV1).
  constexpr int BITSTREAM_FORMAT = 3;

  /// First eight hex digits of the vendored pyrowave commit (third-party/pyrowave/VENDOR.txt).
  /// The PyroWave bitstream carries no version field, so both ends must match.
  constexpr std::string_view BITSTREAM_ID = "186f0393";

  /// RTSP DESCRIBE capability marker. No RTP payload type 99 is ever sent.
  constexpr std::string_view DESCRIBE_RTPMAP = "a=rtpmap:99 PYROWAVE/90000";
  /// RTSP DESCRIBE attribute carrying BITSTREAM_ID.
  constexpr std::string_view DESCRIBE_BITSTREAM_ATTRIBUTE = "a=x-ss-pyrowave.bitstream:";

  // RTSP ANNOUNCE attributes sent by PyroWave clients.
  constexpr std::string_view ANNOUNCE_ADAPTIVE_FEC = "x-ss-video[0].pyrowaveAdaptiveFec";  ///< Aurora; presence selects record framing
  constexpr std::string_view ANNOUNCE_ADAPTIVE_BITRATE = "x-ss-video[0].pyrowaveAdaptiveBitrate";  ///< Aurora
  constexpr std::string_view ANNOUNCE_FEATURES = "x-ss-video[0].pyrowaveFeatures";  ///< Bitmask of FEATURE_*

  /// Client parses record framing with padding records. Record-framed frames are
  /// always laid out for partial decoding, so no bit asks for that (0x2, once
  /// reserved for it, is ignored).
  constexpr std::uint32_t FEATURE_RECORD_FRAMING = 0x1;

  /// First word of an in-band padding record: `0xFFFFFFFF, N, N zero words`.
  constexpr std::uint32_t PADDING_MAGIC = 0xFFFFFFFFu;
  /// Size of the smallest padding record (magic and count, no zero words).
  constexpr std::size_t PADDING_RECORD_MIN_BYTES = 8;

  /// PyroWave packet boundary used for the length-prefixed compatibility framing.
  constexpr std::size_t LENGTH_PREFIXED_PACKET_BOUNDARY = 1024;

  /// Video payload bytes per RTP shard are `packetSize - SHARD_OVERHEAD_BYTES`
  /// (stream.cpp: blocksize = packetSize + MAX_RTP_HEADER_SIZE, minus the 32-byte
  /// video_packet_raw_t header). stream.cpp static_asserts this.
  constexpr int SHARD_OVERHEAD_BYTES = 16;
  /// The short frame header occupies the start of the first shard's payload.
  constexpr std::size_t FRAME_HEADER_BYTES = 8;

  /// `NV_VIDEO_PACKET::extraFlags` bit set on a record-framed shard whose frame data
  /// starts with a record, where a client that lost a record header resumes parsing.
  constexpr std::uint8_t EXTRA_FLAG_RECORD_START = 0x80;
}  // namespace pyrowave::protocol
