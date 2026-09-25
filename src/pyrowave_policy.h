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
    std::size_t unpadded_gaps = 0;  ///< 4-byte shard remainders that a record had to straddle.
    std::size_t oversized_records = 0;  ///< Records larger than one shard, which span shards.
    std::size_t reordered_records = 0;  ///< Records placed ahead of an earlier record to fill a shard.
  };

  /**
   * @brief Append a record-framed frame to `out`.
   *
   * `bitstream` is one PyroWave packet holding the whole frame: the 8-byte sequence
   * header followed by every block record (`pyrowave_encoder_packetize` with an
   * unlimited boundary). The sequence header goes first. Records are then packed into
   * shards first-fit: each shard remainder takes the earliest remaining record that
   * fits it, so records stay roughly lowest-frequency-first but a later, smaller record
   * may fill a gap. A padding record fills a remainder only when no remaining record
   * fits it. A record larger than one shard, or placed at a 4-byte remainder (too small
   * for a padding record), straddles a shard boundary. Measured padding is well under 1%
   * of the frame, against about 20% when records keep strict order.
   *
   * @param shard_payload Result of shard_payload_bytes(); 0 copies the bitstream unchanged.
   * @return Statistics, or std::nullopt when `bitstream` is not a valid PyroWave frame
   *         (nothing is appended then).
   */
  std::optional<record_frame_stats_t> write_record_frame(
    std::span<const std::uint8_t> bitstream,
    std::size_t shard_payload,
    std::vector<std::uint8_t> &out
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
    std::size_t misaligned_records = 0;  ///< Records no larger than a shard that cross a shard boundary.
    std::size_t out_of_order_records = 0;  ///< Block records whose block_index is lower than the previous one.
    std::size_t oversized_records = 0;  ///< Records larger than a shard.
    std::vector<std::uint8_t> stripped;  ///< The frame with padding records removed.
  };

  /**
   * @brief Parse a record-framed frame the way a receiver does.
   * @param shard_payload Shard size used for the alignment statistics; 0 skips them.
   */
  record_frame_info_t inspect_record_frame(std::span<const std::uint8_t> frame, std::size_t shard_payload);

  /**
   * @brief Per-frame byte budget for an intra-only codec.
   *
   * Every PyroWave frame is coded on its own, so the stream bitrate divided by the
   * rate frames are actually captured gives each frame its share. When a game renders
   * below the stream rate (60 fps in a 120 fps stream), dividing by the stream rate
   * would leave half the bitrate unused. The capture interval is smoothed and bounded
   * to [1, 2] nominal frame intervals, so a frame never gets more than twice the
   * nominal budget. Adapted from dimizago's Vibepollo PyroWave encoder.
   */
  class budget_t {
  public:
    budget_t(int framerate, int bitrate_kbps, std::size_t max_frame_bytes);

    void set_bitrate(int bitrate_kbps);

    /// Report a newly captured frame (not a repeat of the previous capture).
    void on_new_capture(std::chrono::steady_clock::time_point when);

    /// Current budget in bytes, a multiple of four, at least 4096.
    [[nodiscard]] std::size_t bytes_per_frame() const {
      return budget;
    }

    [[nodiscard]] double capture_fps() const {
      return capture_interval > 0.0 ? 1.0 / capture_interval : 0.0;
    }

  private:
    void update();

    int framerate;
    int bitrate_kbps;
    std::size_t max_frame_bytes;
    double capture_interval;
    std::optional<std::chrono::steady_clock::time_point> last_capture;
    std::size_t budget = 0;
  };
}  // namespace pyrowave::policy
