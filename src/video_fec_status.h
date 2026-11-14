/**
 * @file src/video_fec_status.h
 * @brief Parsing and de-duplication of client SS_FRAME_FEC_STATUS (0x5502) reports.
 *
 * moonlight-common-c sends one report per FEC block that needed recovery or was
 * dropped (Video.h, SS_FRAME_FEC_STATUS). Every multibyte field is big-endian,
 * unlike the control header.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace stream::pacing {
  /// Control message type of SS_FRAME_FEC_STATUS (client to host).
  inline constexpr std::uint16_t fec_status_packet_type = 0x5502;

  /// What one validated block report says about its FEC block.
  struct fec_block_t {
    enum class kind_e {
      clean,  ///< Every data packet arrived.
      recovered,  ///< `lost` of `data` packets were missing and parity rebuilt them.
      unrecoverable,  ///< Too few packets arrived to rebuild the block.
    };

    kind_e kind {kind_e::clean};
    std::uint16_t lost {};
    std::uint16_t data {};

    static constexpr fec_block_t clean_block() {
      return {};
    }

    static constexpr fec_block_t recovered_block(std::uint16_t lost, std::uint16_t data) {
      return {kind_e::recovered, lost, data};
    }

    static constexpr fec_block_t unrecoverable_block() {
      return {kind_e::unrecoverable, 0, 0};
    }

    constexpr bool operator==(const fec_block_t &) const = default;
  };

  struct fec_status_t {
    std::uint32_t frame {};
    std::uint16_t highest_received {};
    std::uint16_t next_contiguous {};
    std::uint16_t missing_before_highest {};
    std::uint16_t data {};
    std::uint16_t parity {};
    std::uint16_t received_data {};
    std::uint16_t received_parity {};
    std::uint8_t percentage {};
    std::uint8_t block {};
    std::uint8_t blocks {};

    constexpr bool operator==(const fec_status_t &) const = default;
  };

  inline constexpr std::size_t fec_status_size = 21;

  /// Parse and validate a 21-byte report, or return nothing for an invalid one.
  inline std::optional<fec_status_t> parse_fec_status(std::string_view payload) {
    if (payload.size() != fec_status_size) {
      return std::nullopt;
    }
    const auto byte = [&](std::size_t offset) {
      return std::uint32_t(std::uint8_t(payload[offset]));
    };
    const auto word = [&](std::size_t offset) {
      return std::uint16_t((byte(offset) << 8) | byte(offset + 1));
    };

    fec_status_t status;
    status.frame = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
    status.highest_received = word(4);
    status.next_contiguous = word(6);
    status.missing_before_highest = word(8);
    status.data = word(10);
    status.parity = word(12);
    status.received_data = word(14);
    status.received_parity = word(16);
    status.percentage = std::uint8_t(byte(18));
    status.block = std::uint8_t(byte(19));
    status.blocks = std::uint8_t(byte(20));

    if (status.blocks < 1 || status.blocks > 4 || status.block >= status.blocks) {
      return std::nullopt;
    }
    if (status.data < 1 || status.data > 1023) {
      return std::nullopt;
    }
    const std::uint32_t total = std::uint32_t(status.data) + status.parity;
    // Parity is the rounded-up percentage of data. Without FEC no shard limit
    // applies; with it, one Reed-Solomon block holds at most 255 shards.
    if (status.parity != (std::uint32_t(status.data) * status.percentage + 99) / 100 || (status.parity != 0 && total > 255)) {
      return std::nullopt;
    }
    if (status.received_data > status.data || status.received_parity > status.parity) {
      return std::nullopt;
    }
    const std::uint32_t received = std::uint32_t(status.received_data) + status.received_parity;
    if (status.missing_before_highest > total - received) {
      return std::nullopt;
    }
    // The sequence numbers are 16 bits wide and wrap.
    if (std::uint16_t(status.highest_received + 1 - status.next_contiguous) > total) {
      return std::nullopt;
    }
    return status;
  }

  /// A validated, de-duplicated report: which frame, and what happened to its block.
  struct fec_report_t {
    std::uint32_t frame {};
    fec_block_t block;
  };

  /// Validates reports against the frames actually sent and counts each frame's
  /// blocks once, so the pacing controller sees one report per FEC block.
  class fec_report_tracker_t {
  public:
    /// Recent wire frames retained for de-duplication. Bounds storage even when
    /// reports arrive out of order or the 32-bit frame counter wraps.
    static constexpr std::uint32_t window = 1024;

    /// Count one report and return what it says, or nothing for an invalid,
    /// unsent, stale or duplicate report.
    std::optional<fec_report_t> record(std::string_view payload, std::optional<std::uint32_t> last_sent) {
      ++reports_;
      const auto status = parse_fec_status(payload);
      if (!status || !last_sent || std::uint32_t(*last_sent - status->frame) >= window) {
        ++invalid_reports_;
        return std::nullopt;
      }
      const auto last = *last_sent;
      std::erase_if(frames_, [&](const frame_t &frame) {
        return std::uint32_t(last - frame.index) >= window;
      });

      auto entry = std::find_if(frames_.begin(), frames_.end(), [&](const frame_t &frame) {
        return frame.index == status->frame;
      });
      if (entry == frames_.end()) {
        frames_.push_back({status->frame, 0, status->blocks});
        entry = std::prev(frames_.end());
      }
      if (entry->block_count != status->blocks) {
        ++invalid_reports_;
        return std::nullopt;
      }
      const auto bit = std::uint8_t(1u << status->block);
      if (entry->blocks & bit) {
        ++duplicate_reports_;
        return std::nullopt;
      }
      entry->blocks |= bit;

      // These best-effort reports infer recovery, not a successful decode.
      const std::uint32_t received = std::uint32_t(status->received_data) + status->received_parity;
      if (received < status->data) {
        return fec_report_t {status->frame, fec_block_t::unrecoverable_block()};
      }
      if (status->received_data < status->data) {
        return fec_report_t {status->frame, fec_block_t::recovered_block(std::uint16_t(status->data - status->received_data), status->data)};
      }
      return fec_report_t {status->frame, fec_block_t::clean_block()};
    }

    std::uint64_t reports() const {
      return reports_;
    }

    std::uint64_t invalid_reports() const {
      return invalid_reports_;
    }

    std::uint64_t duplicate_reports() const {
      return duplicate_reports_;
    }

  private:
    struct frame_t {
      std::uint32_t index;
      std::uint8_t blocks;  ///< Bitmask of block indexes already reported.
      std::uint8_t block_count;
    };

    std::vector<frame_t> frames_;
    std::uint64_t reports_ {};
    std::uint64_t invalid_reports_ {};
    std::uint64_t duplicate_reports_ {};
  };
}  // namespace stream::pacing
