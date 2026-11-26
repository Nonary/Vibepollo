/**
 * @file src/pyrowave_protocol.h
 * @brief Wire constants for PyroWave over the GameStream/Sunshine protocol.
 *
 * The contract is docs/pyrowave-protocol.md, shared with the moonlight-qt fork.
 * The original capability bits and bitStreamFormat value matched the Aurora/Solarflare
 * PyroWave implementation. Upstream Sunshine (#5194) and moonlight-common-c (#175)
 * have since assigned SCM_MPEG2 = 0x00800000, SCM_H263P = 0x02000000 and
 * bitStreamFormat 3/4 to MPEG-2/H.263+, so PyroWave moved to unused values. The host
 * advertises and accepts both during the transition (docs/pyrowave-protocol.md).
 * Vibepollo builds against upstream moonlight-common-c, so the host-side constants
 * live here rather than in Limelight.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pyrowave::protocol {
  // ServerCodecModeSupport bits advertised in /serverinfo. Bits 27-30: clear of the
  // codec bits upstream assigns from the bottom up, and bit 31 stays unused because
  // clients parse the decimal value into a signed 32-bit int.
  constexpr std::uint32_t SCM_PYROWAVE = 0x08000000;  ///< 8-bit 4:2:0
  constexpr std::uint32_t SCM_PYROWAVE_444 = 0x10000000;  ///< 8-bit 4:4:4
  constexpr std::uint32_t SCM_PYROWAVE_HDR10 = 0x20000000;  ///< 10-bit 4:2:0 (HDR10 on an HDR display)
  constexpr std::uint32_t SCM_PYROWAVE_HDR10_444 = 0x40000000;  ///< 10-bit 4:4:4
  constexpr std::uint32_t SCM_MASK_PYROWAVE = SCM_PYROWAVE | SCM_PYROWAVE_444 | SCM_PYROWAVE_HDR10 | SCM_PYROWAVE_HDR10_444;

  // Pre-move bits, still advertised for existing clients during the transition.
  // 0x00800000 and 0x02000000 are SCM_MPEG2 and SCM_H263P upstream: stop advertising
  // these before this host ever advertises MPEG-2 or H.263+.
  constexpr std::uint32_t LEGACY_SCM_PYROWAVE = 0x00800000;
  constexpr std::uint32_t LEGACY_SCM_PYROWAVE_444 = 0x01000000;
  constexpr std::uint32_t LEGACY_SCM_PYROWAVE_HDR10 = 0x02000000;
  constexpr std::uint32_t LEGACY_SCM_PYROWAVE_HDR10_444 = 0x04000000;
  constexpr std::uint32_t LEGACY_SCM_MASK_PYROWAVE =
    LEGACY_SCM_PYROWAVE | LEGACY_SCM_PYROWAVE_444 | LEGACY_SCM_PYROWAVE_HDR10 | LEGACY_SCM_PYROWAVE_HDR10_444;

  /// Session format this host uses internally for PyroWave (`monitor.videoFormat`).
  /// Unchanged so the encoder/capture checks need no edits.
  constexpr int BITSTREAM_FORMAT = 3;
  /// `x-nv-vqos[0].bitStreamFormat` a client sends to select PyroWave. Upstream uses
  /// 0-4 (H.264/HEVC/AV1/MPEG-2/H.263+); 100 leaves room for its growth.
  constexpr int WIRE_BITSTREAM_FORMAT = 100;
  /// Pre-move wire value, still accepted from existing clients (MPEG-2 upstream).
  constexpr int LEGACY_WIRE_BITSTREAM_FORMAT = 3;

  /// Maps an ANNOUNCE bitStreamFormat to the session format. Both PyroWave wire
  /// values select PyroWave while this host offers no MPEG-2.
  constexpr int session_video_format(int wire_format) {
    return wire_format == WIRE_BITSTREAM_FORMAT || wire_format == LEGACY_WIRE_BITSTREAM_FORMAT ?
             BITSTREAM_FORMAT :
             wire_format;
  }

  static_assert((SCM_MASK_PYROWAVE & 0x80000000u) == 0, "bit 31 breaks signed ServerCodecModeSupport parsing");
  static_assert((SCM_MASK_PYROWAVE & LEGACY_SCM_MASK_PYROWAVE) == 0, "new and legacy PyroWave bits must be disjoint");
  static_assert((SCM_MASK_PYROWAVE & 0x02FF0301u) == 0,
                "PyroWave must avoid upstream H.264/HEVC/AV1/4:4:4/MPEG-2/H.263+ bits");
  static_assert(session_video_format(WIRE_BITSTREAM_FORMAT) == BITSTREAM_FORMAT &&
                  session_video_format(LEGACY_WIRE_BITSTREAM_FORMAT) == BITSTREAM_FORMAT &&
                  session_video_format(0) == 0 && session_video_format(1) == 1 && session_video_format(2) == 2,
                "PyroWave wire mapping must not change standard codecs");

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
  /// The video header has two bits for the block count and ten for data shards.
  constexpr std::size_t MAX_FEC_BLOCKS = 1u << 2;
  constexpr std::size_t MAX_DATA_SHARDS_PER_BLOCK = (1u << 10) - 1;
  /// PyroWave headers contain two 32-bit words.
  constexpr std::size_t SEQUENCE_HEADER_BYTES = 2 * sizeof(std::uint32_t);
  constexpr std::size_t BLOCK_HEADER_BYTES = 2 * sizeof(std::uint32_t);
}  // namespace pyrowave::protocol
