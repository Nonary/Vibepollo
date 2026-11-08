#include "src/platform/windows/audio_continuous_buffer.h"

#include <cstdlib>
#include <iostream>
#include <numeric>
using platf::audio_detail::continuous_buffer_t;
#define CHECK(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "failed: " #expr << " at line " << __LINE__ << "\n"; \
      std::exit(1); \
    } \
  } while (false)
constexpr std::int64_t epoch = 9000000000000;  // nonzero QPC uptime, not session time
constexpr std::int64_t ms = 10000;

// Replay 15 seconds of sample-contiguous capture with packet QPC jitter up to
// 47 frames, as measured in Vibepollo #703. Exercise actual output deadlines,
// ring wrap, nonzero 64-bit device positions, and exact interleaved sample order.
void verify_packet_jitter(std::uint32_t packet_frames, std::uint32_t channels) {
  constexpr std::uint32_t captured_frames = 480;
  constexpr int captures = 1500;
  constexpr std::uint64_t device_origin = (std::uint64_t(1) << 63) + 12345;
  continuous_buffer_t buffer(48000, channels, packet_frames, captured_frames, epoch);
  std::vector<float> input(captured_frames * channels), output(packet_frames * channels);
  std::size_t emitted = 0;
  const auto total_samples = std::size_t(captures) * input.size();
  for (int elapsed = 0; elapsed < captures * 10 + 100 && emitted < total_samples; ++elapsed) {
    auto now = epoch + elapsed * ms;
    if (elapsed % 10 == 0 && elapsed / 10 < captures) {
      auto n = elapsed / 10;
      std::iota(input.begin(), input.end(), float(std::size_t(n) * input.size() + 1));
      auto jitter_frames = n == 0 ? 0 : (n * 37 % 95) - 47;
      auto timestamp = now + std::int64_t(jitter_frames) * 10000000 / 48000;
      CHECK(buffer.put(timestamp, device_origin + std::uint64_t(n) * captured_frames,
                       captured_frames, input.data(), false));
    }
    buffer.advance(now);
    while (emitted < total_samples && buffer.take(now, output)) {
      for (float value : output) {
        CHECK(value == float(++emitted));
      }
    }
  }
  CHECK(emitted == total_samples);
}

int main() {
  verify_packet_jitter(240, 2);
  verify_packet_jitter(480, 8);

  // 5 ms output packets and a 10 ms WASAPI period. Exactly 200 output
  // packets/second, even with no events or with repeated empty events.
  continuous_buffer_t silence(48000, 2, 240, 480, epoch);
  std::vector<float> stereo(480, 99);
  CHECK(!silence.take(epoch + 29 * ms, stereo));
  for (int n = 0; n < 200; ++n) {
    auto now = epoch + (30 + n * 5) * ms;
    silence.advance(now);
    CHECK(silence.take(now, stereo));
    CHECK(std::all_of(stereo.begin(), stereo.end(), [](float x) {
      return x == 0;
    }));
    CHECK(!silence.take(now, stereo));
  }

  // A captured 10 ms packet delivered just before its deadline is split into
  // two 5 ms packets; the next deadline emits silence, never a duplicate.
  continuous_buffer_t delayed(48000, 2, 240, 480, epoch);
  std::vector<float> input(960);
  std::iota(input.begin(), input.end(), 1);
  CHECK(delayed.put(epoch, 0, 480, input.data(), false));
  CHECK(delayed.take(epoch + 30 * ms, stereo));
  CHECK(std::equal(stereo.begin(), stereo.end(), input.begin()));
  CHECK(delayed.take(epoch + 35 * ms, stereo));
  CHECK(std::equal(stereo.begin(), stereo.end(), input.begin() + 480));
  CHECK(delayed.put(epoch, 0, 480, input.data(), false));  // wholly late: drop
  CHECK(delayed.take(epoch + 40 * ms, stereo));
  CHECK(std::all_of(stereo.begin(), stereo.end(), [](float x) {
    return x == 0;
  }));

  // Gaps are replaceable until emitted; arrival ordering does not append
  // extra time or conceal a short earlier packet behind a future one.
  continuous_buffer_t reordered(48000, 2, 240, 480, epoch);
  CHECK(reordered.put(epoch + 10 * ms, 480, 240, input.data(), false));
  CHECK(reordered.put(epoch, 0, 240, input.data() + 480, false));
  CHECK(reordered.take(epoch + 30 * ms, stereo));
  CHECK(stereo.front() == 481);
  CHECK(reordered.take(epoch + 35 * ms, stereo));
  CHECK(stereo.front() == 0);
  CHECK(reordered.take(epoch + 40 * ms, stereo));
  CHECK(stereo.front() == 1);

  // Partial overlap trims frames already emitted; multichannel indexing is
  // frame-aligned and preserves the unplayed tail.
  continuous_buffer_t overlap(48000, 2, 240, 480, epoch);
  CHECK(overlap.take(epoch + 30 * ms, stereo));
  CHECK(overlap.put(epoch, 0, 480, input.data(), false));
  CHECK(overlap.take(epoch + 35 * ms, stereo));
  CHECK(stereo.front() == 481 && stereo.back() == 960);

  // Timestamp quantization of less than half a frame retains alignment.
  continuous_buffer_t jitter(48000, 2, 240, 480, epoch);
  CHECK(jitter.put(epoch + 4, 0, 240, input.data(), false));
  CHECK(jitter.take(epoch + 30 * ms, stereo));
  CHECK(stereo.front() == 1);

  // Silent WASAPI buffers may have a null pointer. Long gaps never allocate
  // more memory; a 10-second scheduler stall recovers with one current packet.
  continuous_buffer_t stall(48000, 2, 240, 480, epoch);
  CHECK(stall.put(epoch, 0, 240, nullptr, true));
  stall.advance(epoch + 10000 * ms);
  CHECK(stall.take(epoch + 10000 * ms, stereo));
  CHECK(!stall.take(epoch + 10000 * ms, stereo));
  CHECK(stall.wait_ms(epoch + 10000 * ms) == 5);
  CHECK(stall.put(epoch, 0, 480, input.data(), false));
  CHECK(!stall.put(epoch + 20000 * ms, 0, 240, input.data(), false));
  CHECK(!stall.put(-1, 0, 240, input.data(), false));

  // Ring reuse cannot replay sound from a previous lap; 8 channels and
  // non-divisible device/output periods also maintain the output duration.
  continuous_buffer_t surround(48000, 8, 240, 336, epoch);
  std::vector<float> surround_in(1920, 7), surround_out(1920);
  CHECK(surround.put(epoch, 0, 240, surround_in.data(), false));
  for (int n = 0; n < 450; ++n) {
    auto now = epoch + (24 + n * 5) * ms;
    surround.advance(now);
    CHECK(surround.take(now, surround_out));
    CHECK(surround_out.front() == (n == 0 ? 7 : 0));
  }
  // New endpoint/session: a new absolute QPC origin resets all state.
  continuous_buffer_t reset(48000, 2, 240, 480, epoch + 100000 * ms);
  CHECK(!reset.take(epoch + 100000 * ms, stereo));
  CHECK(reset.put(epoch + 100000 * ms, 0, 240, input.data(), false));
  CHECK(reset.take(epoch + 100030 * ms, stereo));
  CHECK(stereo.front() == 1);
  // Actual missing capture frames leave silence; resetting a device counter
  // or reporting a discontinuity starts a new QPC-anchored segment.
  continuous_buffer_t gaps(48000, 2, 240, 480, epoch);
  CHECK(gaps.put(epoch, 1000, 480, input.data(), false));
  CHECK(gaps.put(epoch + 20 * ms, 1960, 480, input.data(), false));
  for (int n = 0; n < 6; ++n) {
    CHECK(gaps.take(epoch + (30 + n * 5) * ms, stereo));
    CHECK(stereo.front() == (n == 2 || n == 3 ? 0 : n % 2 == 0 ? 1 : 481));
  }
  CHECK(gaps.put(epoch + 30 * ms, 0, 480, input.data(), false, true));
  CHECK(gaps.take(epoch + 60 * ms, stereo) && stereo.front() == 1);
  CHECK(gaps.take(epoch + 65 * ms, stereo) && stereo.front() == 481);
  // Discontinuity must also reanchor when the numeric position is contiguous.
  CHECK(gaps.put(epoch + 50 * ms, 480, 480, input.data(), false, true));
  CHECK(gaps.take(epoch + 70 * ms, stereo) && stereo.front() == 0);
  CHECK(gaps.take(epoch + 75 * ms, stereo) && stereo.front() == 0);
  CHECK(gaps.take(epoch + 80 * ms, stereo) && stereo.front() == 1);

  // Some loopback devices pause their counter when no application is playing.
  // Resume must anchor to current time even if that counter looks contiguous.
  continuous_buffer_t paused(48000, 2, 240, 480, epoch);
  CHECK(paused.put(epoch, 0, 480, input.data(), false));
  for (int n = 0; n < 200; ++n) {
    auto now = epoch + (30 + n * 5) * ms;
    paused.advance(now);
    CHECK(paused.take(now, stereo));
    CHECK(stereo.front() == (n == 0 ? 1 : n == 1 ? 481 : 0));
  }
  CHECK(paused.put(epoch + 1000 * ms, 480, 480, input.data(), false));
  CHECK(paused.take(epoch + 1030 * ms, stereo) && stereo.front() == 1);
  CHECK(paused.take(epoch + 1035 * ms, stereo) && stereo.front() == 481);
  CHECK(paused.put(epoch + 1010 * ms, 960, 480, nullptr, true));
  CHECK(paused.take(epoch + 1040 * ms, stereo) && stereo.front() == 0);
  // Reject arithmetic overflow or future timestamps without poisoning the
  // device anchor used by the following healthy packet.
  CHECK(!paused.put(epoch + 1020 * ms, UINT64_MAX, 480, input.data(), false));
  CHECK(!paused.put(epoch + 20000 * ms, 1440, 480, input.data(), false));
  CHECK(paused.put(epoch + 1020 * ms, 1440, 480, input.data(), false));

  std::cout << "continuous audio timeline tests passed\n";
}
