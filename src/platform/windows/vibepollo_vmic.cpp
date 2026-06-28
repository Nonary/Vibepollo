/**
 * @file src/platform/windows/vibepollo_vmic.cpp
 * @brief vibepollo_vmic_t implementation.
 */

#include "vibepollo_vmic.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf::audio {

  int vibepollo_vmic_t::init() {
    auto backend = std::make_unique<mic_write_wasapi_t>();

    // If mic_sink is configured, use it as the primary search pattern so
    // find_target_device honours the user's explicit render endpoint choice.
    // Fall back to well-known Steam Streaming Microphone names if mic_sink
    // is empty or doesn't match any device.
    std::vector<std::wstring> patterns;
    if (!config::audio.mic_sink.empty()) {
      patterns.push_back(platf::from_utf8(config::audio.mic_sink));
      backend->requested_device_name = config::audio.mic_sink;
    } else {
      backend->requested_device_name = "Steam Streaming Microphone";
    }
    // Always include the canonical fallback names so the backend can
    // auto-discover the device even when mic_sink uses a shorter alias.
    patterns.push_back(L"Steam Streaming Microphone");
    patterns.push_back(L"Speakers (Steam Streaming Microphone)");
    backend->autodetect_patterns = std::move(patterns);

    if (backend->init() != 0) {
      log_missing_driver_once();
      return -1;
    }

    speaker_backend = std::move(backend);
    return 0;
  }

  int vibepollo_vmic_t::write_pcm(const float *samples, std::uint32_t frame_count) {
    if (!speaker_backend) return -1;
    return speaker_backend->write_pcm(samples, frame_count);
  }

  void vibepollo_vmic_t::log_missing_driver_once() {
    if (!missing_driver_logged) {
      missing_driver_logged = true;
      BOOST_LOG(warning) << "[mic] Steam Streaming Microphone render endpoint not found — "
                            "is Steam running on this machine? Mic passthrough disabled."sv;
    }
  }

}  // namespace platf::audio
