/**
 * @file src/pyrowave_policy.cpp
 * @brief Pure PyroWave host policy: frame framing and the per-frame byte budget.
 */
#include "pyrowave_policy.h"

#include "pyrowave_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pyrowave::policy {
  namespace {
    using namespace pyrowave::protocol;

    // Smallest shard worth aligning to. Below this, a single typical block no
    // longer fits and padding would only waste bandwidth.
    constexpr std::size_t MIN_ALIGNED_SHARD_BYTES = 64;

    // Smoothing of the measured capture interval, per captured frame.
    constexpr double CAPTURE_INTERVAL_SMOOTHING = 0.1;
    // A frame's budget never exceeds this multiple of the nominal budget.
    constexpr double MAX_BUDGET_BOOST = 2.0;
    constexpr std::size_t MIN_FRAME_BYTES = 4096;

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
    if (shard < MIN_ALIGNED_SHARD_BYTES || shard % 4 != 0) {
      return 0;
    }
    return shard;
  }

  std::optional<record_frame_stats_t> write_record_frame(
    std::span<const std::uint8_t> bitstream,
    std::size_t shard_payload,
    std::vector<std::uint8_t> &out
  ) {
    // Split the frame into its sequence header and block records.
    if (bitstream.size() < 8 || (read_u32(bitstream.data()) >> 31) == 0) {
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
    out.reserve(frame_start + bitstream.size() + (shard_payload ? bitstream.size() / 64 + shard_payload : 0));
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

  std::vector<fec_block_t> plan_fec_blocks(std::size_t total_shards, std::size_t critical_shards, int critical_fec_percentage, std::size_t min_parity_shards) {
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
    if (total_shards > critical) {
      split(total_shards - critical, MAX_FEC_BLOCKS - 1);
    }
    return blocks;
  }

  budget_t::budget_t(int framerate, int bitrate_kbps, std::size_t max_frame_bytes):
      framerate {framerate > 0 ? framerate : 60},
      bitrate_kbps {bitrate_kbps},
      max_frame_bytes {max_frame_bytes},
      capture_interval {1.0 / (framerate > 0 ? framerate : 60)} {
    update();
  }

  void budget_t::set_bitrate(int kbps) {
    bitrate_kbps = kbps;
    update();
  }

  void budget_t::on_new_capture(std::chrono::steady_clock::time_point when) {
    const double nominal = 1.0 / framerate;
    if (last_capture) {
      // Bounded, so a pause (static screen, loading) cannot drag the average out.
      const double interval = std::clamp(std::chrono::duration<double>(when - *last_capture).count(), nominal, nominal * MAX_BUDGET_BOOST);
      capture_interval += CAPTURE_INTERVAL_SMOOTHING * (interval - capture_interval);
    }
    last_capture = when;
    update();
  }

  void budget_t::update() {
    double bytes = std::max(0.0, double(bitrate_kbps) * 1000.0 * capture_interval / 8.0);
    if (max_frame_bytes) {
      bytes = std::min(bytes, double(max_frame_bytes));
    }
    auto result = std::max<std::size_t>(std::size_t(bytes), MIN_FRAME_BYTES);
    budget = result & ~std::size_t(3);
  }
}  // namespace pyrowave::policy
