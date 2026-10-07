/**
 * @file src/pyrowave_policy.cpp
 * @brief Pure PyroWave host policy: frame framing and the per-frame byte budget.
 */
#include "pyrowave_policy.h"
#include "pyrowave_bandwidth.h"

#include "pyrowave_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace pyrowave::policy {
  namespace {
    using namespace pyrowave::protocol;

    std::uint32_t read_u32(const std::uint8_t *p) {
      return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }

    void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
      out.push_back(std::uint8_t(value & 0xff));
      out.push_back(std::uint8_t((value >> 8) & 0xff));
      out.push_back(std::uint8_t((value >> 16) & 0xff));
      out.push_back(std::uint8_t((value >> 24) & 0xff));
    }

    void append_padding(std::vector<std::uint8_t> &out, std::size_t bytes) {
      // bytes >= PADDING_RECORD_MIN_BYTES and a multiple of four.
      append_u32(out, PADDING_MAGIC);
      append_u32(out, std::uint32_t(bytes / 4 - 2));
      out.resize(out.size() + (bytes - PADDING_RECORD_MIN_BYTES), 0);
    }

    struct record_t {
      std::size_t offset;
      std::size_t size;
      std::uint32_t block_index;
    };

    /// Records too large to share a shard with a padding record. A record of a shard
    /// less 4 bytes would always strand a 4-byte remainder, so it counts too.
    bool oversized(std::size_t record_bytes, std::size_t shard_payload) {
      return record_bytes + PADDING_RECORD_MIN_BYTES > shard_payload;
    }

    /// Bytes left in the current shard at frame byte `position` (never 0).
    std::size_t shard_remaining(std::size_t position, std::size_t shard_payload) {
      return shard_payload - (position + FRAME_HEADER_BYTES) % shard_payload;
    }

    /**
     * Finds the earliest remaining record no larger than a limit, in O(log) time.
     *
     * Records are bucketed by size in words (payload_words is 12 bits), each bucket
     * holding its record indices in frame order. A min segment tree over the buckets
     * keeps each bucket's earliest remaining index, so "earliest record of at most N
     * words" is a prefix-minimum query. Records are only ever removed from the front of
     * their bucket: both the query and the frame-order cursor pick a bucket's earliest.
     */
    class fit_index_t {
    public:
      static constexpr std::size_t NONE = std::numeric_limits<std::size_t>::max();

      explicit fit_index_t(const std::vector<std::uint32_t> &words):
          heads(LEAVES + 1, 0),
          ends(LEAVES, 0),
          order(words.size()),
          tree(2 * LEAVES, NONE) {
        // Counting sort of the record indices by size, stable in frame order.
        for (const auto w : words) {
          ++heads[w + 1];
        }
        for (std::size_t b = 0; b < LEAVES; b++) {
          heads[b + 1] += heads[b];
          ends[b] = heads[b + 1];
        }
        std::vector<std::size_t> fill(heads.begin(), heads.end() - 1);
        for (std::size_t i = 0; i < words.size(); i++) {
          order[fill[words[i]]++] = i;
        }
        for (std::size_t b = 0; b < LEAVES; b++) {
          tree[LEAVES + b] = heads[b] < ends[b] ? order[heads[b]] : NONE;
        }
        for (std::size_t n = LEAVES - 1; n > 0; n--) {
          tree[n] = std::min(tree[2 * n], tree[2 * n + 1]);
        }
      }

      /// Earliest remaining record of at most `max_words` words, or NONE.
      [[nodiscard]] std::size_t earliest_fitting(std::size_t max_words) const {
        if (max_words == 0) {
          return NONE;
        }
        std::size_t best = NONE;
        std::size_t lo = LEAVES;
        std::size_t hi = LEAVES + std::min(max_words, LEAVES - 1) + 1;
        while (lo < hi) {
          if (lo & 1) {
            best = std::min(best, tree[lo++]);
          }
          if (hi & 1) {
            best = std::min(best, tree[--hi]);
          }
          lo >>= 1;
          hi >>= 1;
        }
        return best;
      }

      /// Earliest remaining record of exactly `words` words, or NONE.
      [[nodiscard]] std::size_t earliest_exact(std::uint32_t words) const {
        return words < LEAVES ? tree[LEAVES + words] : NONE;
      }

      /// Remove the earliest remaining record of `words` words.
      void remove(std::uint32_t words) {
        const auto head = ++heads[words];
        std::size_t n = LEAVES + words;
        tree[n] = head < ends[words] ? order[head] : NONE;
        for (n >>= 1; n > 0; n >>= 1) {
          tree[n] = std::min(tree[2 * n], tree[2 * n + 1]);
        }
      }

    private:
      static constexpr std::size_t LEAVES = 4096;

      std::vector<std::size_t> heads;  ///< Per size: position of the earliest remaining record in `order`.
      std::vector<std::size_t> ends;  ///< Per size: end of its run in `order`.
      std::vector<std::size_t> order;  ///< Record indices grouped by size, in frame order.
      std::vector<std::size_t> tree;
    };
  }  // namespace

  std::uint32_t coarse_block_count(std::uint32_t width, std::uint32_t height) {
    // PyroWave pads each dimension to 32 pixels (at least 128), then five wavelet
    // levels halve it; the coarsest level holds four bands of three components.
    const auto blocks = [](std::uint32_t extent) {
      const std::uint32_t aligned = std::max<std::uint32_t>((extent + 31) / 32 * 32, 128);
      return ((aligned >> 5) + 31) / 32;
    };
    return blocks(width) * blocks(height) * 4 * 3;
  }

  framing_e select_framing(bool client_sent_adaptive_fec, std::optional<std::uint32_t> client_features) {
    if (client_sent_adaptive_fec) {
      return framing_e::records;
    }
    if (client_features && (*client_features & FEATURE_RECORD_FRAMING)) {
      return framing_e::records;
    }
    return framing_e::length_prefixed;
  }

  std::size_t shard_payload_bytes(int packetsize) {
    if (packetsize <= SHARD_OVERHEAD_BYTES) {
      return 0;
    }
    const auto shard = std::size_t(packetsize - SHARD_OVERHEAD_BYTES);
    if (shard < FRAME_HEADER_BYTES + SEQUENCE_HEADER_BYTES + PADDING_RECORD_MIN_BYTES || shard % 4 != 0) {
      return 0;
    }
    return shard;
  }

  std::optional<record_frame_stats_t> write_record_frame(
    std::span<const std::uint8_t> bitstream,
    std::size_t shard_payload,
    std::vector<std::uint8_t> &out,
    std::size_t frame_limit
  ) {
    // Split the frame into its sequence header and block records.
    if (bitstream.size() < 8 || (read_u32(bitstream.data()) >> 31) == 0 ||
        (frame_limit && bitstream.size() > frame_limit)) {
      return std::nullopt;
    }
    std::vector<record_t> records;
    for (std::size_t position = 8; position < bitstream.size();) {
      if (bitstream.size() - position < 8) {
        return std::nullopt;
      }
      const std::uint32_t word0 = read_u32(bitstream.data() + position);
      const std::uint32_t payload_words = (word0 >> 16) & 0xfff;
      if ((word0 >> 31) != 0 || payload_words < 2 || std::size_t(payload_words) * 4 > bitstream.size() - position) {
        return std::nullopt;
      }
      records.push_back({position, std::size_t(payload_words) * 4, read_u32(bitstream.data() + position + 4) >> 8});
      position += records.back().size;
    }

    record_frame_stats_t stats;
    stats.block_records = records.size();
    const std::size_t frame_start = out.size();
    out.reserve(frame_start + bitstream.size());
    out.insert(out.end(), bitstream.begin(), bitstream.begin() + 8);

    if (!shard_payload) {
      out.insert(out.end(), bitstream.begin() + 8, bitstream.end());
      return stats;
    }

    const std::uint32_t sequence_word = read_u32(bitstream.data());
    const std::uint32_t coarse_blocks = coarse_block_count((sequence_word & 0x3fff) + 1, ((sequence_word >> 14) & 0x3fff) + 1);

    // Frame byte p sits at offset p + FRAME_HEADER_BYTES of the shard stream,
    // because the short frame header precedes the frame in the first shard.
    const auto remaining = [&]() {
      return shard_remaining(out.size() - frame_start, shard_payload);
    };
    const auto place = [&](const record_t &record) {
      out.insert(out.end(), bitstream.begin() + record.offset, bitstream.begin() + record.offset + record.size);
    };
    const auto pad = [&](std::size_t bytes) {
      append_padding(out, bytes);
      ++stats.padding_records;
      stats.padding_bytes += bytes;
    };

    // The coarsest wavelet level goes first: a receiver cannot decode a frame that
    // lost any of it, so those shards (critical_bytes) are the ones worth parity.
    // Then everything else. Within each group, oversized records come first and span
    // shards wherever they start; each is shifted by a minimal padding record when it
    // would end 4 bytes before a boundary, since nothing could fill that remainder.
    // The group's other records follow, packed first-fit without crossing a shard
    // boundary, so every shard after the finer level's oversized records starts with
    // a record.
    std::vector<std::size_t> coarse;
    std::vector<std::size_t> fine;
    for (std::size_t i = 0; i < records.size(); i++) {
      (records[i].block_index < coarse_blocks ? coarse : fine).push_back(i);
    }

    for (const auto *group : {&coarse, &fine}) {
      std::vector<std::size_t> ordinary;
      for (const auto i : *group) {
        if (oversized(records[i].size, shard_payload)) {
          if (records[i].size % shard_payload == remaining() - 4) {
            pad(PADDING_RECORD_MIN_BYTES);
          }
          place(records[i]);
          ++stats.oversized_records;
        } else {
          ordinary.push_back(i);
        }
      }

      std::vector<std::uint32_t> words;
      words.reserve(ordinary.size());
      for (const auto i : ordinary) {
        words.push_back(std::uint32_t(records[i].size / 4));
      }

      fit_index_t fit(words);
      std::vector<bool> placed(ordinary.size(), false);
      std::size_t earliest = 0;
      for (std::size_t left = ordinary.size(); left != 0;) {
        while (placed[earliest]) {
          ++earliest;
        }
        const std::size_t room = remaining();

        std::size_t chosen = fit.earliest_fitting(room / 4);
        if (chosen != fit_index_t::NONE && room - std::size_t(words[chosen]) * 4 == 4) {
          // That would strand 4 bytes, too few for a padding record. Take a record
          // that fills the shard exactly or leaves room for a padding record.
          const std::size_t exact = fit.earliest_exact(std::uint32_t(room / 4));
          const std::size_t roomy = room >= 16 ? fit.earliest_fitting((room - 8) / 4) : fit_index_t::NONE;
          chosen = std::min(exact, roomy);
        }
        if (chosen == fit_index_t::NONE) {
          // Never 4: shards start with at least 64 bytes and no placement leaves 4.
          pad(room);
          continue;
        }

        if (chosen != earliest) {
          ++stats.reordered_records;
        }
        placed[chosen] = true;
        fit.remove(words[chosen]);
        --left;
        place(records[ordinary[chosen]]);
      }

      if (group == &coarse) {
        stats.critical_bytes = out.size() - frame_start;
      }
    }

    if (frame_limit && out.size() - frame_start > frame_limit) {
      out.resize(frame_start);
      out.insert(out.end(), bitstream.begin(), bitstream.begin() + 8);
      stats = {};
      stats.block_records = records.size();
      for (const auto *group : {&coarse, &fine}) {
        for (const auto i : *group) {
          place(records[i]);
        }
        if (group == &coarse) {
          stats.critical_bytes = out.size() - frame_start;
        }
      }
    }
    return stats;
  }

  void write_length_prefixed_frame(
    std::span<const packet_t> packets,
    const std::uint8_t *bitstream,
    std::vector<std::uint8_t> &out
  ) {
    std::size_t total = 4;
    for (const auto &packet : packets) {
      total += 4 + packet.size;
    }
    out.reserve(out.size() + total);

    append_u32(out, std::uint32_t(packets.size()));
    for (const auto &packet : packets) {
      append_u32(out, std::uint32_t(packet.size));
      out.insert(out.end(), bitstream + packet.offset, bitstream + packet.offset + packet.size);
    }
  }

  record_frame_info_t inspect_record_frame(std::span<const std::uint8_t> frame, std::size_t shard_payload) {
    record_frame_info_t info;
    std::size_t position = 0;
    std::uint32_t last_block_index = 0;

    auto fail = [&](std::string error) {
      info.valid = false;
      info.error = std::move(error);
      return info;
    };

    std::uint32_t coarse_blocks = 0;
    bool seen_ordinary_coarse = false;
    bool seen_ordinary_fine = false;
    bool seen_fine = false;

    // Returns whether the record is oversized; ordinary records and padding must
    // stay inside one shard so that every shard after the oversized records
    // starts with a record.
    auto note_alignment = [&](std::size_t start, std::size_t size) {
      if (!shard_payload) {
        return false;
      }
      const std::size_t wire_start = start + FRAME_HEADER_BYTES;
      const std::size_t first_shard = wire_start / shard_payload;
      const std::size_t last_shard = (wire_start + size - 1) / shard_payload;
      if (oversized(size, shard_payload)) {
        ++info.oversized_records;
        return true;
      }
      if (first_shard != last_shard) {
        ++info.misaligned_records;
      }
      return false;
    };

    info.stripped.reserve(frame.size());
    while (position < frame.size()) {
      if (frame.size() - position < 8) {
        return fail("truncated record header at byte " + std::to_string(position));
      }
      const auto *record = frame.data() + position;
      const std::uint32_t word0 = read_u32(record);
      const std::uint32_t word1 = read_u32(record + 4);

      if (word0 == PADDING_MAGIC) {
        const std::size_t bytes = PADDING_RECORD_MIN_BYTES + std::size_t(word1) * 4;
        if (bytes > frame.size() - position) {
          return fail("padding record runs past the end of the frame at byte " + std::to_string(position));
        }
        ++info.padding_records;
        info.padding_bytes += bytes;
        note_alignment(position, bytes);
        position += bytes;
        continue;
      }

      const bool extended = (word0 >> 31) != 0;
      if (extended) {
        if (info.has_sequence_header) {
          return fail("second sequence header at byte " + std::to_string(position));
        }
        if (info.block_records != 0) {
          return fail("sequence header after block records");
        }
        info.has_sequence_header = true;
        info.width = (word0 & 0x3fff) + 1;
        info.height = ((word0 >> 14) & 0x3fff) + 1;
        info.total_blocks = word1 & 0xffffff;
        const std::uint32_t code = (word1 >> 24) & 0x3;
        if (code != 0) {
          return fail("sequence header code " + std::to_string(code) + " is not START_OF_FRAME");
        }
        info.chroma444 = ((word1 >> 26) & 0x1) != 0;
        coarse_blocks = coarse_block_count(info.width, info.height);
        info.critical_bytes = position + 8;
        note_alignment(position, 8);
        info.stripped.insert(info.stripped.end(), record, record + 8);
        position += 8;
        continue;
      }

      if (!info.has_sequence_header) {
        return fail("block record before the sequence header");
      }
      const std::uint32_t payload_words = (word0 >> 16) & 0xfff;
      if (payload_words < 2) {
        return fail("block record with payload_words " + std::to_string(payload_words) + " at byte " + std::to_string(position));
      }
      const std::size_t bytes = std::size_t(payload_words) * 4;
      if (bytes > frame.size() - position) {
        return fail("block record runs past the end of the frame at byte " + std::to_string(position));
      }
      const std::uint32_t block_index = word1 >> 8;
      if (info.block_records != 0 && block_index < last_block_index) {
        ++info.out_of_order_records;
      }
      last_block_index = block_index;
      ++info.block_records;
      if (shard_payload) {
        const bool coarse = block_index < coarse_blocks;
        if (coarse && seen_fine) {
          ++info.late_coarse_records;
        }
        if (note_alignment(position, bytes)) {
          // Each level's oversized records precede its ordinary ones
          if (coarse ? seen_ordinary_coarse : seen_ordinary_fine) {
            ++info.late_oversized_records;
          }
        } else if (coarse) {
          seen_ordinary_coarse = true;
        } else {
          seen_ordinary_fine = true;
        }
        if (!coarse) {
          seen_fine = true;
        } else {
          info.critical_bytes = position + bytes;
        }
      }
      info.stripped.insert(info.stripped.end(), record, record + bytes);
      position += bytes;
    }

    if (!info.has_sequence_header) {
      return fail("frame has no sequence header");
    }
    info.valid = true;
    return info;
  }

  std::vector<bool> record_start_shards(std::span<const std::uint8_t> frame, std::size_t shard_payload) {
    if (!shard_payload || frame.empty()) {
      return {};
    }
    std::vector<bool> starts((frame.size() + FRAME_HEADER_BYTES + shard_payload - 1) / shard_payload, false);
    for (std::size_t position = 0; position < frame.size();) {
      if (frame.size() - position < 8) {
        return {};
      }
      if ((position + FRAME_HEADER_BYTES) % shard_payload == 0 || position == 0) {
        starts[(position + FRAME_HEADER_BYTES) / shard_payload] = true;
      }
      const std::uint32_t word0 = read_u32(frame.data() + position);
      std::size_t bytes;
      if (word0 == PADDING_MAGIC) {
        bytes = PADDING_RECORD_MIN_BYTES + std::size_t(read_u32(frame.data() + position + 4)) * 4;
      } else if ((word0 >> 31) != 0) {
        bytes = 8;
      } else {
        bytes = std::size_t((word0 >> 16) & 0xfff) * 4;
        if (bytes < 8) {
          return {};
        }
      }
      if (bytes > frame.size() - position) {
        return {};
      }
      position += bytes;
    }
    return starts;
  }

  std::size_t parity_shards(std::size_t data_shards, int fec_percentage, std::size_t min_parity_shards) {
    if (fec_percentage <= 0) {
      return 0;
    }
    return std::max((data_shards * std::size_t(fec_percentage) + 99) / 100, min_parity_shards);
  }

  std::vector<fec_block_t> plan_fec_blocks(std::size_t total_shards, std::size_t critical_shards, int critical_fec_percentage, std::size_t min_parity_shards, int detail_fec_percentage, std::size_t extra_parity_budget) {
    std::vector<fec_block_t> blocks;
    if (total_shards == 0) {
      return blocks;
    }

    // Blocks without FEC are kept to Reed-Solomon size too, as for the other codecs,
    // so a block that stops arriving is delivered without waiting for a huge one.
    const auto split = [&](std::size_t shards, std::size_t max_blocks) {
      const std::size_t count = std::clamp<std::size_t>((shards + MAX_REED_SOLOMON_SHARDS - 1) / MAX_REED_SOLOMON_SHARDS, 1, max_blocks);
      const std::size_t per_block = (shards + count - 1) / count;
      for (std::size_t i = 0; i < count; i++) {
        blocks.push_back({i + 1 < count ? per_block : shards - per_block * (count - 1), 0});
      }
    };

    const std::size_t critical = std::min(critical_shards, total_shards);
    const bool protect = critical != 0 && critical_fec_percentage > 0 &&
                         critical + parity_shards(critical, critical_fec_percentage, min_parity_shards) <= MAX_REED_SOLOMON_SHARDS;
    if (!protect) {
      split(total_shards, MAX_FEC_BLOCKS);
      return blocks;
    }

    blocks.push_back({critical, critical_fec_percentage});
    detail_fec_percentage = std::clamp(detail_fec_percentage, 0, MAX_DETAIL_FEC_PERCENTAGE);
    if (detail_fec_percentage > 0 && extra_parity_budget > 0 && total_shards > critical) {
      const auto fine = total_shards - critical;
      // Bound the total extra overhead even when only part of a large frame can
      // be protected. Never spend credit accumulated across an idle period.
      auto budget = std::min(extra_parity_budget, parity_shards(fine, detail_fec_percentage, 0));
      const auto capacity = [&](int percentage) {
        std::size_t data = MAX_REED_SOLOMON_SHARDS;
        while (data && data + parity_shards(data, percentage, min_parity_shards) > MAX_REED_SOLOMON_SHARDS) {
          --data;
        }
        return data;
      };

      // Prefer covering all detail, reducing the rate if needed. Even splitting
      // avoids leaving the last block with a disproportionately high minimum FEC.
      for (int percentage = detail_fec_percentage; percentage > 0; --percentage) {
        const auto cap = capacity(percentage);
        if (!cap) {
          continue;
        }
        const auto count = (fine + cap - 1) / cap;
        if (count > MAX_FEC_BLOCKS - 1) {
          continue;
        }
        std::vector<fec_block_t> candidate = blocks;
        std::size_t parity = 0;
        for (std::size_t i = 0; i < count; ++i) {
          const auto data = fine / count + (i < fine % count ? 1 : 0);
          candidate.push_back({data, percentage});
          parity += parity_shards(data, percentage, min_parity_shards);
        }
        if (parity <= budget) {
          return candidate;
        }
      }

      // Large frames cannot fit entirely in Reed-Solomon blocks. Extend the
      // critical block first, then protect additional prefixes only when enough
      // block slots remain to carry every unprotected shard.
      const auto base_parity = parity_shards(critical, critical_fec_percentage, min_parity_shards);
      auto leading = critical;
      const auto leading_cap = std::min(total_shards, capacity(critical_fec_percentage));
      while (leading < leading_cap && parity_shards(leading + 1, critical_fec_percentage, min_parity_shards) - base_parity <= budget) {
        ++leading;
      }
      blocks.front().data_shards = leading;
      budget -= parity_shards(leading, critical_fec_percentage, min_parity_shards) - base_parity;
      auto remaining = total_shards - leading;
      const auto cap = capacity(detail_fec_percentage);
      while (remaining && blocks.size() < MAX_FEC_BLOCKS && cap) {
        auto data = std::min(remaining, cap);
        while (data && parity_shards(data, detail_fec_percentage, min_parity_shards) > budget) {
          --data;
        }
        const auto slots_after = MAX_FEC_BLOCKS - blocks.size() - 1;
        if (!data || remaining - data > slots_after * MAX_FEC_BLOCK_SHARDS) {
          break;
        }
        blocks.push_back({data, detail_fec_percentage});
        budget -= parity_shards(data, detail_fec_percentage, min_parity_shards);
        remaining -= data;
      }
      if (remaining) {
        split(remaining, MAX_FEC_BLOCKS - blocks.size());
      }
      return blocks;
    }
    if (total_shards > critical) {
      split(total_shards - critical, MAX_FEC_BLOCKS - 1);
    }
    return blocks;
  }

  detail_fec_controller_t::detail_fec_controller_t(int framerate):
      nominal_interval {framerate > 0 ? 1.0 / framerate : 0.0} {
    if (framerate <= 0) {
      throw std::invalid_argument("PyroWave requires a negotiated frame rate");
    }
  }

  detail_fec_t detail_fec_controller_t::observe(std::span<const std::uint8_t> frame, std::chrono::steady_clock::time_point when, int bitrate_kbps) {
    const auto reset = [&]() -> detail_fec_t {
      previous_blocks.clear();
      previous_record_bytes = 0;
      previous_time.reset();
      interval = stable_seconds = 0.0;
      return {};
    };
    const auto elapsed = previous_time ? std::chrono::duration<double>(when - *previous_time).count() : 0.0;
    if (elapsed > 0.0 && elapsed <= nominal_interval * 1.01) {
      // At negotiated cadence, allow 1% timing noise without an activity scan.
      // Start a fresh similarity history if cadence subsequently falls.
      previous_blocks.clear();
      previous_record_bytes = 0;
      previous_time = when;
      interval = elapsed;
      stable_seconds = 0.0;
      return {};
    }
    if (frame.size() < SEQUENCE_HEADER_BYTES) {
      return reset();
    }
    const auto word0 = read_u32(frame.data());
    const auto word1 = read_u32(frame.data() + 4);
    const auto count = word1 & 0xffffff;
    if (!(word0 & 0x80000000u) || word0 == PADDING_MAGIC || ((word1 >> 24) & 3) != 0 || !count || count > (frame.size() - 8) / 8) {
      return reset();
    }
    // Sequence and nonzero block count can change without changing the format.
    // Zero coefficient blocks are omitted, so block IDs need not be contiguous.
    const auto format = (std::uint64_t(word0 & 0x8fffffffu) << 32) | (word1 & 0xff000000u);
    std::unordered_map<std::uint32_t, std::uint64_t> hashes;
    hashes.reserve(count);
    std::size_t unchanged_bytes = 0;
    std::size_t record_bytes = 0;
    std::size_t records = 0;
    for (std::size_t position = 8; position < frame.size();) {
      if (frame.size() - position < 8) {
        return reset();
      }
      const auto *record = frame.data() + position;
      const auto header = read_u32(record);
      const auto index_word = read_u32(record + 4);
      const bool padding = header == PADDING_MAGIC;
      const auto bytes = padding ? 8 + std::size_t(index_word) * 4 : std::size_t((header >> 16) & 0xfff) * 4;
      if (bytes < 8 || bytes > frame.size() - position || (!padding && (header >> 31))) {
        return reset();
      }
      if (!padding) {
        const auto index = index_word >> 8;
        // Hash complete records, masking only the three sequence bits. Indexing
        // by block ID makes padding and the record packer's reordering irrelevant.
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t i = 0; i < bytes; ++i) {
          hash ^= i == 3 ? record[i] & 0x8f : record[i];
          hash *= 1099511628211ull;
        }
        if (!hashes.emplace(index, hash).second) {
          return reset();
        }
        const auto previous = previous_blocks.find(index);
        if (format == previous_format && previous != previous_blocks.end() && hash == previous->second) {
          unchanged_bytes += bytes;
        }
        record_bytes += bytes;
        ++records;
      }
      position += bytes;
    }
    if (records != count) {
      return reset();
    }
    const bool similar = double(unchanged_bytes) >= 0.75 * double(std::max(record_bytes, previous_record_bytes));
    previous_blocks = std::move(hashes);
    previous_record_bytes = record_bytes;
    previous_format = format;
    previous_time = when;
    if (elapsed <= 0.0 || elapsed > 0.25) {
      interval = stable_seconds = 0.0;
      return {};
    }
    interval = interval > 0.0 ? interval + (elapsed / (0.25 + elapsed)) * (elapsed - interval) : elapsed;
    // Require both current and smoothed cadence to be slow. A single hitch must
    // not enable parity; a return to negotiated FPS turns it off immediately.
    const auto cadence = std::min(interval, elapsed);
    stable_seconds = similar && cadence > nominal_interval * 1.01 ? stable_seconds + std::min(elapsed, 0.05) : 0.0;
    if (stable_seconds < 0.25) {
      return {};
    }
    detail_fec_t result;
    // Stable encoded frame size frees this fraction of a frame's payload budget
    // as cadence falls. The sender deducts wire overhead before spending it.
    result.percentage = int(std::clamp(100.0 * (cadence / nominal_interval - 1.0), 0.0, double(MAX_DETAIL_FEC_PERCENTAGE)));
    // Full wire cost (including base FEC) is deducted by the sender. This is a
    // per-frame allowance, not a token bucket; stalls cannot fund a large burst.
    result.frame_wire_budget = std::size_t(double(std::max(bitrate_kbps, 0)) * 125.0 * std::min(cadence, 0.05));
    return result;
  }

  std::size_t max_frame_bytes(int packetsize, bool critical_fec) {
    if (packetsize <= SHARD_OVERHEAD_BYTES) {
      return 0;
    }
    const auto max_shards = critical_fec ? 3000u : 4000u;
    return std::min<std::size_t>(max_shards, protocol::MAX_FEC_BLOCKS * protocol::MAX_DATA_SHARDS_PER_BLOCK) *
             std::size_t(packetsize - SHARD_OVERHEAD_BYTES) - FRAME_HEADER_BYTES;
  }

  std::size_t max_bitstream_bytes(int packetsize, bool length_prefixed, bool critical_fec) {
    auto capacity = max_frame_bytes(packetsize, critical_fec);
    if (length_prefixed) {
      // At worst each block and the sequence header occupy their own packet.
      // Each packet needs a length word, plus the frame's packet-count word.
      if (capacity < sizeof(std::uint32_t)) {
        return 0;
      }
      capacity = (capacity - sizeof(std::uint32_t)) /
                 (BLOCK_HEADER_BYTES + sizeof(std::uint32_t)) * BLOCK_HEADER_BYTES;
    }
    return capacity & ~std::size_t(3);
  }

  std::size_t pacing_packets_per_ms(
    std::uint64_t link_bps, int bitrate_kbps,
    std::size_t payload_bytes, std::size_t wire_bytes,
    std::size_t frame_bytes, int framerate,
    std::uint64_t calibrated_pace_bps
  ) {
    if (payload_bytes == 0 || wire_bytes == 0) {
      return 1;
    }
    // Link capacity includes network headers; the negotiated bitrate is payload.
    const auto frame_packets = (frame_bytes + payload_bytes - 1) / payload_bytes;
    const auto frame_allowance = (frame_packets * std::uint64_t(std::max(framerate, 0)) + 999) / 1000;
    const auto bitrate_allowance = (std::uint64_t(std::max(bitrate_kbps, 0)) + 8 * payload_bytes - 1) / (8 * payload_bytes);
    // Round demand up so quantizing to a pacing quantum cannot create a backlog.
    const auto demand = std::max(frame_allowance, bitrate_allowance);
    // The client measured this pace with frame-shaped UDP probes on this path.
    const auto calibrated_packets = calibrated_pace_bps / 8 / 1000 / wire_bytes;
    if (link_bps == 0) {
      return std::max<std::size_t>(1, std::max(calibrated_packets, demand));
    }
    // Round link capacity down so pacing never exceeds the reported link speed.
    const auto link_packets = link_bps / 8 / 1000 / wire_bytes;
    // A receiver's negotiated speed overstates what it can absorb back-to-back:
    // a USB 2.5GbE dock measured lossless at 2.2 Gbps but dropped 12-16 packet
    // runs at 2.35+. Uncalibrated, pace at 80% of the link.
    const auto target_packets = calibrated_pace_bps ? calibrated_packets : link_bps * 80 / 100 / 8 / 1000 / wire_bytes;
    return std::max<std::size_t>(1, std::min(link_packets, std::max(target_packets, demand)));
  }

  std::size_t frame_payload_budget(std::size_t wire_bytes, const wire_budget_t &transport) {
    return bandwidth::image_bytes(wire_bytes, {transport.packetsize, transport.critical_fec_percentage,
                                               transport.min_parity_shards, transport.envelope_bytes});
  }

  budget_t::budget_t(int framerate, int bitrate_kbps, std::size_t max_frame_bytes, bool stable_frame_size, std::optional<wire_budget_t> transport):
      transport {transport},
      bitrate_kbps {bitrate_kbps},
      max_frame_bytes {max_frame_bytes},
      nominal_interval {framerate > 0 ? 1.0 / framerate : 0.0},
      stable_frame_size {stable_frame_size},
      frame_interval {nominal_interval} {
    if (framerate <= 0) {
      throw std::invalid_argument("PyroWave requires a negotiated frame rate");
    }
    update();
  }

  void budget_t::set_bitrate(int kbps) {
    bitrate_kbps = kbps;
    update();
  }

  void budget_t::on_frame(std::chrono::steady_clock::time_point when) {
    if (last_frame) {
      if (when <= *last_frame) {
        budget = 0;
        return;
      }
      frame_interval = std::chrono::duration<double>(when - *last_frame).count();
    }
    last_frame = when;
    update();
  }

  void budget_t::update() {
    const auto budget_interval = stable_frame_size ? std::min(frame_interval, nominal_interval) : frame_interval;
    double bytes = std::max(0.0, double(bitrate_kbps) * 1000.0 * budget_interval / 8.0);
    if (transport) {
      bytes = double(frame_payload_budget(std::size_t(std::min(bytes, double(std::numeric_limits<std::uint32_t>::max()))), *transport));
    }
    if (max_frame_bytes) {
      bytes = std::min(bytes, double(max_frame_bytes));
    }
    bytes = std::min(bytes, double(std::numeric_limits<std::uint32_t>::max() & ~3u));
    budget = std::size_t(bytes) & ~std::size_t(3);
    if (budget < SEQUENCE_HEADER_BYTES + BLOCK_HEADER_BYTES) {
      budget = 0;
    }
  }
}  // namespace pyrowave::policy
