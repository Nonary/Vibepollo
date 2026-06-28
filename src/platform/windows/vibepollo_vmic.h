/**
 * @file src/platform/windows/vibepollo_vmic.h
 * @brief vibepollo_vmic_t — high-level mic redirect backend for Steam Streaming Microphone.
 *
 * Wraps mic_write_wasapi_t and provides the autodetect patterns for
 * Steam Streaming Microphone. This is the entry point called by audio_control_t.
 */
#pragma once

#include "mic_write.h"
#include <memory>

namespace platf::audio {

  class vibepollo_vmic_t: public mic_redirect_backend_t {
  public:
    int init() override;
    int write_pcm(const float *samples, std::uint32_t frame_count) override;
    const char *backend_id() const override { return "steam_streaming_microphone"; }
    ~vibepollo_vmic_t() override = default;

  private:
    void log_missing_driver_once();

    std::unique_ptr<mic_write_wasapi_t> speaker_backend;
    bool missing_driver_logged = false;
  };

}  // namespace platf::audio
