/**
 * @file src/dolby_session_worker.h
 * @brief Testable production control flow for a per-session Dolby encoder worker.
 */
#pragma once

#include <utility>

namespace audio::dolby {

  /**
   * @brief Run one encoder worker while keeping failures local to its sample queue.
   *
   * The callbacks isolate FFmpeg and packet construction from the control flow.
   * Production supplies the FFmpeg encoder; tests supply a deterministic fake.
   * The shared packet queue is deliberately only reachable through publish.
   */
  template<typename Samples, typename Encoder, typename Initialize, typename Encode,
           typename MakePacket, typename Publish>
  bool run_encoder_worker(
    Samples &samples,
    Encoder &encoder,
    Initialize &&initialize,
    Encode &&encode,
    MakePacket &&make_packet,
    Publish &&publish
  ) {
    if (!initialize(encoder)) {
      samples.stop();
      return false;
    }

    while (auto sample = samples.pop()) {
      auto packet = make_packet();
      const auto bytes = encode(encoder, *sample, packet);
      if (bytes < 0) {
        samples.stop();
        return false;
      }

      publish(*sample, std::move(packet), bytes);
    }

    return true;
  }

  /**
   * @brief Stop and join a session's encoder before waiting for capture shutdown.
   *
   * The release callback runs after the worker has exited and shutdown has been
   * observed. Production uses it to release the session's microphone capture.
   */
  template<typename Samples, typename Thread, typename WaitForShutdown, typename ReleaseCapture>
  void finish_capture_session(
    Samples &samples,
    Thread &thread,
    WaitForShutdown &&wait_for_shutdown,
    ReleaseCapture &&release_capture
  ) {
    if (samples) {
      samples->stop();
    }
    if (thread.joinable()) {
      thread.join();
    }
    wait_for_shutdown();
    release_capture();
  }

}  // namespace audio::dolby
