/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
#include <algorithm>
#include <mutex>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "audio_lifecycle_policy.h"
#include "audio_lifecycle_state.h"
#include "audio_policy.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "thread_safe.h"
#include "utility.h"
#include "webrtc_stream.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  namespace {
    context_manager_t &context_manager() {
      static context_manager_t manager {{
        [] { return platf::audio_control(); },
        [] { return proc::proc.current_app_id() > 0; },
        [] { return config::audio.sink; },
        [](std::string_view message) {
          if (message == "There will be no audio") BOOST_LOG(warning) << message;
          else BOOST_LOG(info) << message;
        },
#if defined(_WIN32) || defined(__linux__)
        true,
#else
        false,
#endif
      }};
      return manager;
    }
  }  // namespace

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      if (webrtc_stream::has_active_sessions() && channel_data == nullptr) {
        webrtc_stream::submit_audio_frame(*sample, stream.sampleRate, stream.channelCount, frame_size);
      }

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream || config.input_only) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    audio_owner_acquired();
    auto owner_release = util::fail_guard([]() {
      audio_owner_released();
    });

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
      return;
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    policy::sink_catalog_t sinks {ref->sink.host, std::nullopt, std::nullopt, std::nullopt};
    if (ref->sink.null) {
      sinks.stereo = ref->sink.null->stereo;
      sinks.surround51 = ref->sink.null->surround51;
      sinks.surround71 = ref->sink.null->surround71;
    }
    const auto sink = policy::select_stream_sink(
      sinks,
      config::audio.sink,
      config::audio.virtual_sink,
      stream.channelCount,
      config.flags[config_t::HOST_AUDIO]
    );

    BOOST_LOG(info) << "Selected audio sink: "sv << sink;

    bool capture_only = false;
#ifdef _WIN32
    capture_only = policy::capture_sink_without_routing(config::audio.sink_capture_only, config::audio.sink, config::audio.virtual_sink, sink);
#endif
    if (context_manager().assign(*ref.get(), sink, capture_only)) return;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    std::shared_ptr<sample_queue_t::element_type> samples;
    std::thread thread;
    if (!config.bypass_opus) {
      samples = std::make_shared<sample_queue_t::element_type>(30);
      thread = std::thread {encodeThread, samples, config, channel_data};
    }

    auto fg = util::fail_guard([&]() {
      if (samples) {
        samples->stop();
        if (thread.joinable()) {
          thread.join();
        }
      }

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      policy::sample_status_e policy_status;
      switch (status) {
        case platf::capture_e::ok:
          policy_status = policy::sample_status_e::ok;
          break;
        case platf::capture_e::timeout:
          policy_status = policy::sample_status_e::timeout;
          break;
        case platf::capture_e::reinit:
          policy_status = config::audio.auto_capture ?
                            policy::sample_status_e::reinitialize :
                            policy::sample_status_e::timeout;
          break;
        case platf::capture_e::interrupted:
          policy_status = policy::sample_status_e::interrupted;
          break;
        case platf::capture_e::error:
          policy_status = policy::sample_status_e::error;
          break;
      }
      const auto action = policy::sample_action(policy_status);
      switch (action) {
        case policy::sample_action_e::emit:
          break;
        case policy::sample_action_e::retry:
          continue;
        case policy::sample_action_e::reacquire:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        case policy::sample_action_e::stop:
          return;
      }

      if (config.bypass_opus) {
        if (channel_data == nullptr) {
          webrtc_stream::submit_audio_frame(sample_buffer, stream.sampleRate, stream.channelCount, frame_size);
        }
      } else {
        samples->raise(std::move(sample_buffer));
      }
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    return context_manager().acquire();
  }

  std::shared_ptr<void> capture_before_display_change(bool input_only) {
#ifdef _WIN32
    return context_manager().before_display_change(config::audio.stream, input_only);
#else
    (void) input_only;
    return {};
#endif
  }

  void app_termination_requested() { context_manager().app_termination_requested(); }
  void app_started() { context_manager().app_started(); }
  void audio_owner_acquired() { context_manager().owner_acquired(); }
  void audio_owner_released() { context_manager().owner_released(); }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    return policy::stream_index(channels, quality);
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    policy::stream_layout_t base {
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      {}
    };
    policy::stream_layout_t custom {
      params.channelCount,
      params.streams,
      params.coupledStreams,
      {}
    };
    std::copy_n(params.mapping, custom.mapping.size(), custom.mapping.begin());
    const auto selected = policy::apply_custom_layout(base, custom);
    stream.channelCount = selected.channels;
    stream.streams = selected.streams;
    stream.coupledStreams = selected.coupled_streams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
