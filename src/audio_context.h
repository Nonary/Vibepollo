/**
 * @file src/audio_context.h
 * @brief Shared audio ownership used by capture and pending display preparation.
 */
#pragma once
#include "audio_lifecycle_state.h"
#include "platform/audio.h"
#include "thread_safe.h"

#include <functional>
#include <string_view>
#include <thread>

namespace audio {
  struct audio_ctx_t {
    std::unique_ptr<std::atomic_bool> sink_flag;
    std::unique_ptr<platf::audio_control_t> control;
    bool restore_sink {false};
    platf::sink_t sink;
  };

  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;

  // Only process/configuration and device construction are injected. Context
  // retention, handoff, first assignment and restoration are implemented here.
  struct context_environment_t {
    std::function<std::unique_ptr<platf::audio_control_t>()> create_control;
    std::function<bool()> app_active;
    std::function<std::string()> configured_sink;
    std::function<void(std::string_view)> log;
    bool retain_for_app {true};
  };

  class context_manager_t {
  public:
    explicit context_manager_t(context_environment_t environment);
    context_manager_t(const context_manager_t &) = delete;
    context_manager_t &operator=(const context_manager_t &) = delete;

    audio_ctx_ref_t acquire();
    std::shared_ptr<void> before_display_change(bool audio_enabled);
    std::shared_ptr<void> before_display_change(bool audio_enabled, bool input_only) {
      return input_only ? std::shared_ptr<void> {} : before_display_change(audio_enabled);
    }
    int assign(audio_ctx_t &ctx, const std::string &sink, bool capture_only);
    void app_started();
    void app_termination_requested();
    void owner_acquired();
    void owner_released();

  private:
    struct retained_t {
      std::unique_ptr<platf::audio_control_t> control;
      platf::sink_t sink;
      bool restore_sink {false};
    };

    int start(audio_ctx_t &ctx);
    void stop(audio_ctx_t &ctx);
    void restore(audio_ctx_t &ctx);
    void log(std::string_view message);

    context_environment_t environment_;
    lifecycle::state_t<retained_t> state_;
    safe::shared_t<audio_ctx_t> shared_;
  };

  // Both transports use this boundary for blocking joins. A failed join keeps
  // the hold; a completed join releases it before shared display cleanup.
  template<class Join>
  void join_capture_and_release(std::shared_ptr<void> &hold, Join &&join) {
    std::forward<Join>(join)();
    hold.reset();
  }

  enum class join_stage_e {
    video,
    audio,
    control
  };
  void join_rtsp_capture(std::shared_ptr<void> &hold, std::thread &video, std::thread &audio, const std::function<void()> &control_end, const std::function<std::shared_ptr<void>()> &watchdog, const std::function<void(join_stage_e)> &stage);
  void join_webrtc_capture(std::shared_ptr<void> &hold, std::thread &audio, const std::function<void()> &after_join = {});
}  // namespace audio
