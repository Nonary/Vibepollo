/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
#include <algorithm>
#include <mutex>
#include <thread>

// Use the build's existing FFmpeg linkage for optional Dolby audio encoding.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "dolby_session_worker.h"
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

  /**
   * @brief Encode one interleaved stereo or 5.1 float PCM frame as an AC-3-family access unit.
   */
  class ffmpeg_audio_encoder_t {
  public:
    ffmpeg_audio_encoder_t() = default;
    ffmpeg_audio_encoder_t(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t &operator=(const ffmpeg_audio_encoder_t &) = delete;
    ffmpeg_audio_encoder_t(ffmpeg_audio_encoder_t &&) = delete;
    ffmpeg_audio_encoder_t &operator=(ffmpeg_audio_encoder_t &&) = delete;

    ~ffmpeg_audio_encoder_t() {
      if (packet_) {
        av_packet_free(&packet_);
      }
      if (frame_) {
        av_frame_free(&frame_);
      }
      if (context_) {
        avcodec_free_context(&context_);
      }
    }

    /**
     * @brief Initialize the selected FFmpeg encoder for stereo or 5.1 PCM.
     *
     * @param codec_id FFmpeg codec identifier for AC-3 or E-AC-3.
     * @param bit_rate Target encoded bitrate in bits per second.
     * @param channels Number of channels; only 2 and 6 are supported.
     * @return True if the encoder context and frame buffers initialize successfully.
     */
    bool initialize(AVCodecID codec_id, std::int64_t bit_rate, int channels) {
      if (channels != 2 && channels != 6) {
        return false;
      }

      const auto *codec = avcodec_find_encoder(codec_id);
      if (!codec) {
        return false;
      }

      context_ = avcodec_alloc_context3(codec);
      if (!context_) {
        return false;
      }

      context_->bit_rate = bit_rate;
      context_->sample_rate = SAMPLE_RATE;
      context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
      context_->time_base = AVRational {1, SAMPLE_RATE};
      context_->thread_count = 1;
      channels_ = channels;

      if (const auto channel_layout = channels == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_5POINT1_BACK; av_channel_layout_from_mask(&context_->ch_layout, channel_layout) < 0 || avcodec_open2(context_, codec, nullptr) < 0) {
        return false;
      }

      // AC-3 and E-AC-3 use 1536 samples per frame here (32 ms at 48 kHz).
      if (context_->frame_size != 1536) {
        return false;
      }

      frame_ = av_frame_alloc();
      if (!frame_) {
        return false;
      }

      frame_->format = context_->sample_fmt;
      frame_->sample_rate = context_->sample_rate;
      frame_->nb_samples = context_->frame_size;
      if (av_channel_layout_copy(&frame_->ch_layout, &context_->ch_layout) < 0 || av_frame_get_buffer(frame_, 0) < 0) {
        return false;
      }

      packet_ = av_packet_alloc();
      return packet_ != nullptr;
    }

    /**
     * @brief Encode an interleaved floating-point PCM frame into a Dolby access unit.
     *
     * @param interleaved Interleaved float PCM samples for one complete frame.
     * @param output Destination buffer for the encoded access unit.
     * @return Encoded access-unit size in bytes, or -1 if input validation or encoding fails.
     */
    int encode(const std::vector<float> &interleaved, buffer_t &output) {
      if (!frame_ || interleaved.size() != static_cast<std::size_t>(frame_->nb_samples * channels_)) {
        return -1;
      }

      if (av_frame_make_writable(frame_) < 0) {
        return -1;
      }

      for (int channel = 0; channel < channels_; ++channel) {
        auto *destination = static_cast<float *>(static_cast<void *>(frame_->extended_data[channel]));
        for (int sample = 0; sample < frame_->nb_samples; ++sample) {
          destination[sample] = interleaved[static_cast<std::size_t>(sample * channels_ + channel)];
        }
      }

      frame_->pts = next_pts_;
      next_pts_ += frame_->nb_samples;

      if (avcodec_send_frame(context_, frame_) < 0 || avcodec_receive_packet(context_, packet_) < 0) {
        av_packet_unref(packet_);
        return -1;
      }

      const auto packet_size = packet_->size;
      if (packet_size <= 0 || static_cast<std::size_t>(packet_size) > output.size()) {
        av_packet_unref(packet_);
        return -1;
      }

      std::copy_n(packet_->data, packet_size, std::begin(output));
      av_packet_unref(packet_);
      return packet_size;
    }

  private:
    AVCodecContext *context_ {nullptr};
    AVFrame *frame_ {nullptr};
    AVPacket *packet_ {nullptr};
    std::int64_t next_pts_ {0};
    int channels_ {0};
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

    if (config.use_ac3 || config.use_eac3) {
      const auto codec_id = config.use_eac3 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
      const auto codec_name = config.use_eac3 ? "E-AC-3"sv : "AC-3"sv;
      const auto bitrate_kbps = dolby_bitrate_kbps(config.use_eac3, config.high_quality_dolby, config.channels);
      ffmpeg_audio_encoder_t encoder;

      // The worker's control flow is shared with the CPU-only regression test.
      // FFmpeg and packet delivery remain production callbacks.
      dolby::run_encoder_worker(
        *samples,
        encoder,
        [&](auto &active_encoder) {
          if (bitrate_kbps <= 0 || !active_encoder.initialize(codec_id, static_cast<std::int64_t>(bitrate_kbps) * 1000, config.channels)) {
            BOOST_LOG(error) << "Couldn't initialize the linked FFmpeg "sv << codec_name << " encoder"sv;
            return false;
          }
          BOOST_LOG(info) << codec_name << " initialized: "sv << config.channels
                          << " channels, 48 kHz, "sv << bitrate_kbps << " kbit/s"sv;
          return true;
        },
        [](auto &active_encoder, const auto &sample, buffer_t &packet) {
          return active_encoder.encode(sample, packet);
        },
        [] {
          // Reserve enough room for the largest supported 640 kbit/s AC-3 access unit.
          return buffer_t {4096};
        },
        [&](const auto &sample, buffer_t packet, int bytes) {
          if (webrtc_stream::has_active_sessions() && channel_data == nullptr) {
            // Dolby frame duration is fixed at 1536 samples at 48 kHz (32 ms).
            webrtc_stream::submit_audio_frame(sample, SAMPLE_RATE, config.channels, 1536);
          }
          packet.fake_resize(bytes);
          packets->raise(channel_data, std::move(packet));
        }
      );
      return;
    }

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
      dolby::finish_capture_session(
        samples,
        thread,
        [&]() { shutdown_event->view(); },
        [&]() { mic.reset(); }
      );
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

  bool ac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_AC3) != nullptr;
  }

  bool eac3_available() {
    return avcodec_find_encoder(AV_CODEC_ID_EAC3) != nullptr;
  }

  int dolby_bitrate_kbps(bool eac3, bool high_quality, int channels) {
    if (channels == 2) return high_quality ? 192 : 128;
    if (channels == 6) {
      if (eac3) return high_quality ? 384 : 192;
      return high_quality ? 640 : 384;
    }
    return 0;
  }

  dolby_codec_e configure_dolby_audio(
    config_t &config,
    std::string_view offered,
    std::string_view accepted,
    bool ac3_encoder_available,
    bool eac3_encoder_available,
    bool high_quality
  ) {
    using enum audio::dolby_codec_e;
    config.use_ac3 = false;
    config.use_eac3 = false;
    config.high_quality_dolby = false;
    const bool supported_channels = config.channels == 2 || config.channels == 6;
    auto selected = opus;
    if (supported_channels && offered == "AC3" && accepted == offered && ac3_encoder_available) {
      selected = ac3;
    } else if (supported_channels && offered == "EAC3" && accepted == offered && eac3_encoder_available) {
      selected = eac3;
    }
    if (selected != opus) {
      config.use_ac3 = selected == ac3;
      config.use_eac3 = selected == eac3;
      config.packetDuration = 32;
      config.flags[config_t::HIGH_QUALITY] = true;
      config.flags[config_t::CUSTOM_SURROUND_PARAMS] = false;
      config.high_quality_dolby = high_quality;
    }
    return selected;
  }

  std::string_view dolby_codec_name(dolby_codec_e codec) {
    using enum audio::dolby_codec_e;
    switch (codec) {
      case ac3: return "AC3";
      case eac3: return "EAC3";
      case opus:
      default: return "Opus";
    }
  }

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
