/**
 * @file src/app_audio_termination.h
 * @brief Release a paused application's audio owner before display teardown.
 */
#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace audio {
  // Only guard copies/retirement hold this mutex; context/OS release and the
  // caller's lifecycle acquisition always happen outside it. Keeping it here
  // preserves launch_session_t layout and move semantics.
  inline std::mutex launch_audio_guard_mutex;

  // The RTSP I/O thread copies the original before taking lifecycle admission.
  // Synchronize that read with terminal retirement; the target is a fresh clone.
  template<class Session>
  void copy_launch_audio_guard(const Session &source, Session &target) {
    std::lock_guard lock(launch_audio_guard_mutex);
    target.audio_context_guard = source.audio_context_guard;
  }

  // The caller owns lifecycle admission, which protects the process's metadata
  // shared_ptr. Every RTSP accessor retains a strong owner of the original,
  // which may still have pending/I/O owners: do not modify its guard then. Native
  // launch metadata has no weak_ptr/shared_from_this resurrection path. The
  // count is only an ownership test; the mutex synchronizes guard access.
  //
  // Keep the metadata object, display_power_guard, cipher and every native field
  // until the existing process tail. An active capture uses its own cloned guard
  // and audio reference, so retiring this owner cannot bypass capture joins.
  template<class Session, class MarkTerminal>
  void begin_app_audio_termination(const std::shared_ptr<Session> &metadata,
                                   MarkTerminal &&mark_terminal) {
    std::shared_ptr<void> app_audio;
    if (metadata) {
      std::lock_guard lock(launch_audio_guard_mutex);
      if (metadata.use_count() == 1) {
        app_audio = std::move(metadata->audio_context_guard);
      }
    }
    // Drop outside the metadata mutex: final context release can call the OS.
    // While the app is still active, this first retains its control; the marker
    // then starts restoration before the caller's display cleanup.
    app_audio.reset();
    std::forward<MarkTerminal>(mark_terminal)();
  }
}  // namespace audio
