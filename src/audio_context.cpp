/**
 * @file src/audio_context.cpp
 * @brief Production context acquisition, routing and resumable restoration.
 */
#include "audio_context.h"

#include "audio_lifecycle_policy.h"
#include "audio_steam_toggle_policy.h"

namespace audio {
  context_manager_t::context_manager_t(context_environment_t environment):
      environment_ {std::move(environment)},
      shared_ {[this](audio_ctx_t &ctx) {
                 return start(ctx);
               },
               [this](audio_ctx_t &ctx) {
                 stop(ctx);
               }} {
  }

  void context_manager_t::log(std::string_view message) {
    if (environment_.log) {
      environment_.log(message);
    }
  }

  audio_ctx_ref_t context_manager_t::acquire() {
    return shared_.ref();
  }

  std::shared_ptr<void> context_manager_t::before_display_change(bool audio_enabled) {
    if (!audio_enabled) {
      return {};
    }
    auto ref = acquire();
    if (!ref || !ref->control) {
      return {};
    }
    return std::make_shared<audio_ctx_ref_t>(std::move(ref));
  }

  int context_manager_t::assign(audio_ctx_t &ctx, const std::string &sink, bool capture_only) {
    if (!ctx.control) {
      return -1;
    }
    if (ctx.sink_flag->exchange(true, std::memory_order_acquire)) {
      return 0;
    }
    if (capture_only) {
      return ctx.control->set_capture_sink(sink);
    }
    ctx.restore_sink = ctx.sink.host != sink;
    return ctx.restore_sink ? ctx.control->set_sink(sink) : 0;
  }

  bool context_manager_t::steam_audio_auto_toggle_enabled() const {
    return environment_.steam_audio_auto_toggle && environment_.steam_audio_auto_toggle();
  }

  void context_manager_t::on_steam_toggle_connect(audio_ctx_t &ctx) {
    if (!ctx.control) {
      return;
    }

    const auto toggle = steam_toggle::evaluate_toggle(
      steam_toggle_state_,
      steam_toggle::session_event_e::CONNECT,
      steam_audio_auto_toggle_enabled()
    );
    if (toggle.should_snapshot) {
      // Record what the host used before we touch anything, so the terminal
      // end can put the user's own endpoints back instead of guessing.
      ctx.control->snapshot_default_devices();
    }
    if (toggle.action != steam_toggle::toggle_action_e::enable) {
      return;
    }
    if (ctx.control->enable_steam_audio_devices() == 0) {
      ctx.restore_steam_devices = true;
      return;
    }

    // The device boundary refused. Roll the state machine back so a later
    // disconnect does not try to hide endpoints we never enabled.
    (void) steam_toggle::evaluate_toggle(
      steam_toggle_state_,
      steam_toggle::session_event_e::DISCONNECT,
      steam_audio_auto_toggle_enabled()
    );
  }

  void context_manager_t::on_steam_toggle_disconnect(audio_ctx_t &ctx) {
    if (!ctx.restore_steam_devices) {
      return;
    }
    ctx.restore_steam_devices = false;
    if (!ctx.control) {
      return;
    }

    const auto toggle = steam_toggle::evaluate_toggle(
      steam_toggle_state_,
      steam_toggle::session_event_e::DISCONNECT,
      steam_audio_auto_toggle_enabled()
    );
    if (toggle.action == steam_toggle::toggle_action_e::disable) {
      ctx.control->disable_steam_audio_devices();
    }
  }

  int context_manager_t::start(audio_ctx_t &ctx) {
    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);
    if (environment_.retain_for_app) {
      state_.wait_for_restore();
      retained_t retained;
      if (lifecycle::can_reclaim_retained_audio(true, environment_.app_active(), state_.terminal_pending()) && state_.reclaim(retained, environment_.app_active())) {
        ctx.control = std::move(retained.control);
        ctx.sink = std::move(retained.sink);
        ctx.restore_sink = retained.restore_sink;
        ctx.restore_steam_devices = retained.restore_steam_devices;
        ctx.sink_flag->store(true, std::memory_order_release);
        log("Audio retained for resumable app; reclaimed on reconnect.");
        return 0;
      }
    }
    ctx.restore_sink = false;
    ctx.restore_steam_devices = false;
    ctx.control = environment_.create_control();
    if (ctx.control) {
      on_steam_toggle_connect(ctx);
      if (auto sink = ctx.control->sink_info()) {
        ctx.sink = std::move(*sink);
        return 0;
      }
      // No usable sink means no stream; do not leave the host with Steam
      // endpoints enabled that nothing will ever clean up.
      on_steam_toggle_disconnect(ctx);
      ctx.control.reset();
    }
    log("There will be no audio");
    return 0;
  }

  void context_manager_t::restore(audio_ctx_t &ctx) {
    if (!ctx.control || (!ctx.restore_sink && !ctx.restore_steam_devices)) {
      return;
    }
    if (ctx.restore_sink) {
      const auto sink = ctx.sink.host.empty() ? environment_.configured_sink() : ctx.sink.host;
      if (!sink.empty()) {
        ctx.control->restore_sink(sink);
      }
      ctx.control->reset_default_device(sink);
    }
    on_steam_toggle_disconnect(ctx);
  }

  void context_manager_t::stop(audio_ctx_t &ctx) {
    if (!ctx.restore_sink && !ctx.restore_steam_devices) {
      return;
    }
    if (environment_.retain_for_app &&
        lifecycle::final_owner_release_action(ctx.restore_sink || ctx.restore_steam_devices, environment_.app_active(), state_.terminal_pending()) == lifecycle::release_action_e::retain) {
      retained_t value {std::move(ctx.control), std::move(ctx.sink), ctx.restore_sink, ctx.restore_steam_devices};
      if (state_.retain(value, environment_.app_active())) {
        log("Audio retained for resumable app after final transport owner ended.");
        return;
      }
      ctx.control = std::move(value.control);
      ctx.sink = std::move(value.sink);
      ctx.restore_sink = value.restore_sink;
      ctx.restore_steam_devices = value.restore_steam_devices;
    }
    log("Restoring audio state after terminal app end.");
    restore(ctx);
  }

  void context_manager_t::app_started() {
    if (environment_.retain_for_app) {
      state_.app_started();
    }
  }

  void context_manager_t::app_termination_requested() {
    if (!environment_.retain_for_app) {
      return;
    }
    auto retained = state_.begin_terminal();
    if (!retained) {
      return;
    }
    audio_ctx_t ctx;
    ctx.control = std::move(retained->control);
    ctx.sink = std::move(retained->sink);
    ctx.restore_sink = retained->restore_sink;
    ctx.restore_steam_devices = retained->restore_steam_devices;
    auto completion = util::fail_guard([this] {
      state_.complete_terminal_restore();
    });
    log("Restoring retained audio state after terminal app end.");
    restore(ctx);
  }

  void context_manager_t::owner_acquired() {
    if (environment_.retain_for_app) {
      state_.owner_acquired();
    }
  }

  void context_manager_t::owner_released() {
    if (environment_.retain_for_app) {
      state_.owner_released();
    }
  }

  void join_rtsp_capture(std::shared_ptr<void> &hold, std::thread &video, std::thread &audio, const std::function<void()> &control_end, const std::function<std::shared_ptr<void>()> &watchdog, const std::function<void(join_stage_e)> &stage) {
    join_capture_and_release(hold, [&] {
      // The watchdog covers only joins. Endpoint restoration on final release
      // can call Windows policy RPCs and must run after watchdog destruction.
      auto deadline = watchdog();
      stage(join_stage_e::video);
      video.join();
      stage(join_stage_e::audio);
      if (audio.joinable()) {
        audio.join();
      }
      stage(join_stage_e::control);
      control_end();
    });
  }

  void join_webrtc_capture(std::shared_ptr<void> &hold, std::thread &audio, const std::function<void()> &after_join) {
    join_capture_and_release(hold, [&] {
      if (audio.joinable()) {
        audio.join();
      }
      if (after_join) {
        after_join();
      }
    });
  }
}  // namespace audio
