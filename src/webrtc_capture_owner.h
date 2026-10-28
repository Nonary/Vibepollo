/**
 * @file src/webrtc_capture_owner.h
 * @brief Audio hold ownership across WebRTC capture start and thread teardown.
 */
#pragma once
#include "audio_context.h"

#include <array>
#include <cstdint>
#include <optional>

namespace webrtc_stream {
  struct CaptureThreadOwner {
    // Base members are destroyed after derived teardown threads. Within this
    // owner, the hold must also outlive both thread destructors if join throws.
    std::shared_ptr<void> audio_context_guard;
    std::thread video_thread;
    std::thread audio_thread;
  };

  struct WebRtcCaptureTeardown: CaptureThreadOwner {
    std::thread feedback_thread;
#ifdef SUNSHINE_ENABLE_WEBRTC
    std::thread media_thread;
#endif
    bool capture_was_active {false};
    std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes;

    void join_capture(const std::function<void()> &after_join = {}) {
      if (video_thread.joinable()) {
        video_thread.join();
      }
      audio::join_webrtc_capture(audio_context_guard, audio_thread, after_join);
    }
  };

  // Thread construction can throw after partial startup. Keep the pre-display
  // hold local until that boundary succeeds, rather than stranding it in the
  // published launch metadata or active owner when no audio thread was made.
  // Partial-start recovery of the other runtime state remains the caller's job.
  template<class StartThreads>
  void start_capture_with_audio_hold(CaptureThreadOwner &owner, std::shared_ptr<void> &pending_hold, StartThreads &&start_threads) {
    auto hold = std::move(pending_hold);
    std::forward<StartThreads>(start_threads)();
    owner.audio_context_guard = std::move(hold);
  }
}  // namespace webrtc_stream
