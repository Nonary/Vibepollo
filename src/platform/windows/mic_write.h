/**
 * @file src/platform/windows/mic_write.h
 * @brief WASAPI render backend for Steam Streaming Microphone.
 *
 * Provides mic_redirect_backend_t (abstract base) and mic_write_wasapi_t
 * (concrete WASAPI implementation). The critical fix over previous attempts:
 * SetDeviceFormat uses KSDATAFORMAT_SUBTYPE_PCM; WASAPI Initialize uses
 * KSDATAFORMAT_SUBTYPE_IEEE_FLOAT. Using IEEE_FLOAT for both failed.
 */
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include <mmdeviceapi.h>
#include <Audioclient.h>

#include "src/utility.h"

namespace platf::audio {

  template<class T>
  void release_com(T *p) {
    p->Release();
  }

  /**
   * @brief Abstract base for mic redirect backends.
   */
  class mic_redirect_backend_t {
  public:
    virtual int init() = 0;
    virtual int write_pcm(const float *samples, std::uint32_t frame_count) = 0;
    virtual const char *backend_id() const = 0;
    virtual ~mic_redirect_backend_t() = default;
  };

  /**
   * @brief WASAPI render backend that writes decoded PCM to Steam Streaming Microphone.
   *
   * init() discovers the Steam Streaming Microphone render endpoint, calls
   * IPolicyConfig::SetDeviceFormat with a PCM (not float!) waveformat to normalize
   * the endpoint, then initializes WASAPI with IEEE_FLOAT for the render stream.
   *
   * write_pcm() queues mono float32 samples and signals the render thread.
   * The render thread expands mono to stereo float32 and writes to WASAPI.
   */
  class mic_write_wasapi_t: public mic_redirect_backend_t {
  public:
    int init() override;
    int write_pcm(const float *samples, std::uint32_t frame_count) override;
    const char *backend_id() const override { return "mic_write_wasapi"; }
    ~mic_write_wasapi_t() override;

    // Set before calling init()
    std::vector<std::wstring> autodetect_patterns;
    std::string requested_device_name;

  private:
    bool find_target_device(std::wstring &out_device_id);
    bool try_enable_endpoint(const std::wstring &device_id);
    void ensure_recommended_steam_mic_format(const std::wstring &device_id);
    bool initialize_device(const std::wstring &device_id);
    void render_loop();

    util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>> device_enum;
    util::safe_ptr<IAudioClient, release_com<IAudioClient>> audio_client;
    IAudioRenderClient *audio_render = nullptr;
    UINT32 buffer_frame_count = 0;
    std::string backend_name;
    std::string target_device_name;
    bool first_packet_written_logged = false;
    bool render_dead_logged = false;
    util::safe_ptr_v2<void, BOOL, CloseHandle> render_event;
    std::mutex queue_mutex;
    std::deque<float> pending_frames;
    std::thread render_thread;
    std::atomic<bool> stop_render_thread {false};
    std::atomic<bool> render_dead {false};
    bool playout_started = false;
    bool playout_wait_logged = false;
  };

}  // namespace platf::audio
