/**
 * @file src/pyrowave_policy.h
 * @brief Pure PyroWave host policy: frame framing and the per-frame byte budget.
 *
 * Nothing here touches a GPU or Sunshine's runtime, so it is shared by the
 * Windows encoder, the unit tests and the standalone encoder harness. See
 * docs/pyrowave-protocol.md for the wire format.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pyrowave::policy {
  /// Frame payload capacity from the negotiated packet size and video header fields.
  std::size_t max_frame_bytes(int packetsize, bool critical_fec = false);

  /// Codec budget leaving enough space for compatibility packet length prefixes.
  std::size_t max_bitstream_bytes(int packetsize, bool length_prefixed, bool critical_fec = false);

  /// Packets per pacing quantum, using the routed link speed or stream bitrate.
  std::size_t pacing_packets_per_ms(
    std::uint64_t link_bps, int bitrate_kbps,
    std::size_t payload_bytes, std::size_t wire_bytes,
    std::size_t frame_bytes = 0, int framerate = 0
  );

  enum class framing_e {
    records,  ///< Concatenated PyroWave records with padding records aligned to RTP shards.
    length_prefixed,  ///< `[u32 count] { [u32 size] [packet] }`, for the azafrob/dimizago clients.
  };

  /**
   * @brief Choose the frame framing for a PyroWave session.
   * @param client_sent_adaptive_fec The client sent `x-ss-video[0].pyrowaveAdaptiveFec` (any value).
   * @param client_features Value of `x-ss-video[0].pyrowaveFeatures`, if sent.
   */
  framing_e select_framing(bool client_sent_adaptive_fec, std::optional<std::uint32_t> client_features);

  /**
   * @brief Frame bytes carried by one RTP shard for the negotiated `packetSize`.
   * @return 0 when record alignment is impossible (unknown size, too small, or not
   *         a multiple of four bytes); the frame is then written without padding.
   */
  std::size_t shard_payload_bytes(int packetsize);

  /// A PyroWave packet inside an encoded bitstream. Layout-compatible with `pyrowave_packet`.
  struct packet_t {
    std::size_t offset;
    std::size_t size;
  };

  struct record_frame_stats_t {
    std::size_t block_records = 0;
    std::size_t padding_records = 0;
    std::size_t padding_bytes = 0;
    std::size_t oversized_records = 0;  ///< Records too large to share a shard with padding, which span shards.
    std::size_t reordered_records = 0;  ///< Records placed ahead of an earlier record to fill a shard.
    /// Frame bytes through the last record of the coarsest wavelet level (at least the
    /// sequence header). The shards holding them are the frame's critical shards.
    std::size_t critical_bytes = 0;
  };

  /**
   * @brief Number of blocks in PyroWave's coarsest wavelet level for a frame size.
   *
   * PyroWave indexes these first (block indices below this count). A receiver cannot
   * decode a frame that lost any of them, so they are sent before finer blocks.
   */
  std::uint32_t coarse_block_count(std::uint32_t width, std::uint32_t height);

  /**
   * @brief Append a record-framed frame to `out`.
   *
   * `bitstream` is one PyroWave packet holding the whole frame: the 8-byte sequence
   * header followed by every block record (`pyrowave_encoder_packetize` with an
   * unlimited boundary). The layout lets a receiver decode a frame that lost packets
   * (docs/pyrowave-protocol.md):
   *
   * 1. The sequence header.
   * 2. The coarsest wavelet level, which a receiver cannot do without: its shards
   *    (`critical_bytes`) are the ones stream.cpp protects with parity.
   * 3. Every other record.
   *
   * Within groups 2 and 3, oversized records (too large to share a shard with a
   * padding record) come first, in frame order. They span shards; a minimal padding
   * record goes before one that would otherwise end 4 bytes before a boundary. The
   * group's other records follow, packed first-fit: a shard remainder takes the
   * earliest record of the group that fits it, and a padding record fills it only
   * when none fits. None of them crosses a shard boundary and no 4-byte remainder
   * (too small for a padding record) is left, so every shard after the finer
   * level's oversized records starts with a record.
   *
   * Measured padding is well under 1% of the frame, against about 20% when records
   * keep strict order.
   *
   * @param shard_payload Result of shard_payload_bytes(); 0 copies the bitstream unchanged.
   * @param frame_limit If set, omit padding when it would exceed this capacity.
   * @return Statistics, or std::nullopt when `bitstream` is not a valid PyroWave frame
   *         (nothing is appended then).
   */
  std::optional<record_frame_stats_t> write_record_frame(
    std::span<const std::uint8_t> bitstream,
    std::size_t shard_payload,
    std::vector<std::uint8_t> &out,
    std::size_t frame_limit = 0
  );

  /// Append a length-prefixed frame (`[u32 LE count] { [u32 LE size] [bytes] }`) to `out`.
  void write_length_prefixed_frame(
    std::span<const packet_t> packets,
    const std::uint8_t *bitstream,
    std::vector<std::uint8_t> &out
  );

  /// Result of parsing a record-framed frame; used by tests and diagnostics.
  struct record_frame_info_t {
    bool valid = false;
    std::string error;
    bool has_sequence_header = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool chroma444 = false;
    std::uint32_t total_blocks = 0;  ///< From the sequence header.
    std::size_t block_records = 0;
    std::size_t padding_records = 0;
    std::size_t padding_bytes = 0;
    std::size_t misaligned_records = 0;  ///< Ordinary block or padding records that cross a shard boundary.
    std::size_t out_of_order_records = 0;  ///< Block records whose block_index is lower than the previous one.
    std::size_t oversized_records = 0;  ///< Records too large to share a shard with a padding record.
    std::size_t late_oversized_records = 0;  ///< Oversized records after an ordinary record of their level.
    std::size_t late_coarse_records = 0;  ///< Coarsest-level records after a finer one.
    std::size_t critical_bytes = 0;  ///< Frame bytes through the last coarsest-level record.
    std::vector<std::uint8_t> stripped;  ///< The frame with padding records removed.
  };

  /**
   * @brief Which shards of a record-framed frame start with a record.
   *
   * Shard 0 holds the frame from its first byte (after the frame header) and so starts
   * with the sequence header; shard s > 0 starts at frame byte s * shard_payload -
   * FRAME_HEADER_BYTES. A record here is a sequence header, block or padding record.
   * @return One entry per shard, or empty when the frame cannot be walked.
   */
  std::vector<bool> record_start_shards(std::span<const std::uint8_t> frame, std::size_t shard_payload);

  /**
   * @brief Parse a record-framed frame the way a receiver does.
   * @param shard_payload Shard size used for the alignment statistics; 0 skips them.
   */
  record_frame_info_t inspect_record_frame(std::span<const std::uint8_t> frame, std::size_t shard_payload);

  /// One FEC block of a PyroWave frame.
  struct fec_block_t {
    std::size_t data_shards;
    int fec_percentage;
  };

  /// stream.cpp sends a frame in at most this many FEC blocks (2 bits in the header).
  constexpr std::size_t MAX_FEC_BLOCKS = 4;
  /// Data shards one FEC block can index (10 bits in the header).
  constexpr std::size_t MAX_FEC_BLOCK_SHARDS = 1023;
  /// Data plus parity shards one Reed-Solomon block (GF(2^8)) can hold.
  constexpr std::size_t MAX_REED_SOLOMON_SHARDS = 255;

  /**
   * @brief Split a PyroWave frame into FEC blocks.
   *
   * Only the frame's first `critical_shards` (the sequence header and the coarsest
   * wavelet level) get parity, as block 0 at `critical_fec_percentage` with at least
   * `min_parity_shards` parity shards. Losing any of them costs the whole frame,
   * while a lost finer record only blurs its area. The rest is split evenly into up
   * to three blocks without FEC. Parity is skipped when the percentage is 0, there is
   * nothing critical, or block 0 would exceed a Reed-Solomon block; the frame is then
   * split as before, into up to four blocks without FEC.
   *
   * Blocks are listed in order and their shards add up to `total_shards`. A frame
   * too large for the blocks gets blocks over MAX_FEC_BLOCK_SHARDS, which the caller
   * reports; the encoder's frame budget keeps that from happening.
   */
  std::vector<fec_block_t> plan_fec_blocks(std::size_t total_shards, std::size_t critical_shards, int critical_fec_percentage, std::size_t min_parity_shards);

  /// Parity shards stream.cpp's FEC encoder adds to a block of `data_shards`.
  std::size_t parity_shards(std::size_t data_shards, int fec_percentage, std::size_t min_parity_shards);

  /**
   * @brief Per-frame byte budget for an intra-only codec.
   *
   * Each submitted frame, including repeats, gets the bytes earned since the
   * previous submission. The initial budget uses the negotiated frame rate.
   * The transport capacity bounds accumulation after stalls.
   */
  class budget_t {
  public:
    budget_t(int framerate, int bitrate_kbps, std::size_t max_frame_bytes);

    void set_bitrate(int bitrate_kbps);

    /// Report an encoding attempt, including repeats of the previous capture.
    void on_frame(std::chrono::steady_clock::time_point when);

    /// Current budget in bytes, aligned to codec words; zero means skip this frame.
    [[nodiscard]] std::size_t bytes_per_frame() const {
      return budget;
    }

    [[nodiscard]] double frame_fps() const {
      return frame_interval > 0.0 ? 1.0 / frame_interval : 0.0;
    }

  private:
    void update();

    int bitrate_kbps;
    std::size_t max_frame_bytes;
    double frame_interval;
    std::optional<std::chrono::steady_clock::time_point> last_frame;
    std::size_t budget = 0;
  };
}  // namespace pyrowave::policy
