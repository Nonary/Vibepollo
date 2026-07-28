/**
 * @file src/platform/windows/mic_write.cpp
 * @brief WASAPI render backend implementation for Steam Streaming Microphone.
 */

#include "mic_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <thread>
#include <vector>

#include <Audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <synchapi.h>

// Must come after mmdeviceapi.h
#include "PolicyConfig.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace {

  // Local PKEY definitions — avoid relying on audio.cpp's anonymous-namespace definitions.
  const PROPERTYKEY local_PKEY_Device_FriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
  };

  /**
   * @brief Waveformat used with IPolicyConfig::SetDeviceFormat.
   *
   * THE CRITICAL FIX: SubFormat must be KSDATAFORMAT_SUBTYPE_PCM here.
   * Using IEEE_FLOAT for SetDeviceFormat causes WASAPI Initialize to fail
   * with 0x88890008 (AUDCLNT_E_UNSUPPORTED_FORMAT) on Steam Streaming Microphone.
   */
  WAVEFORMATEXTENSIBLE make_recommended_steam_mic_device_waveformat() {
    WAVEFORMATEXTENSIBLE wfx {};
    wfx.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels            = 2;
    wfx.Format.nSamplesPerSec       = 48000;
    wfx.Format.wBitsPerSample       = 32;
    wfx.Format.nBlockAlign          = 8;
    wfx.Format.nAvgBytesPerSec      = 384000;
    wfx.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;  // <-- PCM for SetDeviceFormat
    return wfx;
  }

  /**
   * @brief Waveformat used with IAudioClient::Initialize.
   *
   * After setting the device format to PCM, WASAPI Initialize succeeds with
   * IEEE_FLOAT — this is the render stream format.
   */
  WAVEFORMATEXTENSIBLE make_required_steam_mic_render_waveformat() {
    WAVEFORMATEXTENSIBLE wfx {};
    wfx.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels            = 2;
    wfx.Format.nSamplesPerSec       = 48000;
    wfx.Format.wBitsPerSample       = 32;
    wfx.Format.nBlockAlign          = 8;
    wfx.Format.nAvgBytesPerSec      = 384000;
    wfx.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;  // <-- float for Initialize
    return wfx;
  }

}  // namespace

namespace platf::audio {

  int mic_write_wasapi_t::init() {
    // Create device enumerator
    IMMDeviceEnumerator *raw_enum = nullptr;
    auto hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
      IID_IMMDeviceEnumerator, (void **)&raw_enum);
    if (FAILED(hr) || !raw_enum) {
      BOOST_LOG(warning) << "[mic] mic_write_wasapi: CoCreateInstance(DeviceEnumerator) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return -1;
    }
    device_enum.reset(raw_enum);

    // Find the Steam Streaming Microphone render endpoint
    std::wstring device_id;
    if (!find_target_device(device_id)) {
      BOOST_LOG(warning) << "[mic] mic_write_wasapi: Steam Streaming Microphone render endpoint not found"sv;
      return -1;
    }

    // Normalize device format to PCM (critical — must run before Initialize)
    ensure_recommended_steam_mic_format(device_id);

    // Initialize WASAPI with IEEE_FLOAT render stream
    if (!initialize_device(device_id)) {
      return -1;
    }

    return 0;
  }

  bool mic_write_wasapi_t::find_target_device(std::wstring &out_device_id) {
    // Match a Steam mic render endpoint by friendly-name substring within a given
    // device-state mask. Returns the device id of the first match and records its
    // friendly name in target_device_name.
    auto search_mask = [&](DWORD state_mask, std::wstring &id_out) -> bool {
      IMMDeviceCollection *raw_collection = nullptr;
      auto hr = device_enum->EnumAudioEndpoints(eRender, state_mask, &raw_collection);
      if (FAILED(hr) || !raw_collection) {
        return false;
      }
      util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>> collection(raw_collection);

      UINT count = 0;
      collection->GetCount(&count);

      for (UINT i = 0; i < count; ++i) {
        IMMDevice *raw_device = nullptr;
        if (FAILED(collection->Item(i, &raw_device)) || !raw_device) continue;
        util::safe_ptr<IMMDevice, release_com<IMMDevice>> device(raw_device);

        IPropertyStore *raw_prop = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_prop)) || !raw_prop) continue;
        util::safe_ptr<IPropertyStore, release_com<IPropertyStore>> prop(raw_prop);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (FAILED(prop->GetValue(local_PKEY_Device_FriendlyName, &pv)) ||
            pv.vt != VT_LPWSTR || !pv.pwszVal) {
          PropVariantClear(&pv);
          continue;
        }
        std::wstring name(pv.pwszVal);
        PropVariantClear(&pv);

        // Case-insensitive substring match against autodetect patterns
        std::wstring name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::towlower);

        bool matched = false;
        for (const auto &pattern : autodetect_patterns) {
          std::wstring pat_lower = pattern;
          std::transform(pat_lower.begin(), pat_lower.end(), pat_lower.begin(), ::towlower);
          if (name_lower.find(pat_lower) != std::wstring::npos) {
            matched = true;
            break;
          }
        }
        if (!matched) continue;

        WCHAR *raw_id = nullptr;
        if (SUCCEEDED(device->GetId(&raw_id)) && raw_id) {
          id_out = std::wstring(raw_id);
          CoTaskMemFree(raw_id);
          target_device_name = platf::to_utf8(name);
          return true;
        }
      }
      return false;
    };

    // 1. Fast path: an ACTIVE endpoint is ready to use.
    if (search_mask(DEVICE_STATE_ACTIVE, out_device_id)) {
      BOOST_LOG(info) << "[mic] found Steam mic render endpoint: "sv << target_device_name;
      return true;
    }

    // 2. Self-heal: the Steam Streaming Microphone render endpoint (the loopback
    //    sink we write decoded mic audio into) can be left disabled — e.g. a user
    //    disabling it in Sound settings, or a Steam Remote Play toggle. WASAPI then
    //    won't enumerate it as ACTIVE and passthrough silently dies. Look in the
    //    recoverable inactive states and re-enable via IPolicyConfig. NOTPRESENT is
    //    excluded: a driver-absent endpoint can't be re-enabled by visibility, and
    //    matching a stale not-present entry first would falsely abort the heal.
    std::wstring inactive_id;
    if (search_mask(DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, inactive_id)) {
      BOOST_LOG(warning) << "[mic] Steam mic render endpoint ["sv << target_device_name
                         << "] is present but inactive — attempting to re-enable it."sv;
      if (try_enable_endpoint(inactive_id)) {
        // Endpoint visibility/state propagation through MMDevice is asynchronous;
        // poll for the endpoint to surface as ACTIVE rather than assuming a delay.
        for (int attempt = 0; attempt < 20; ++attempt) {
          if (search_mask(DEVICE_STATE_ACTIVE, out_device_id)) {
            BOOST_LOG(info) << "[mic] re-enabled and found Steam mic render endpoint: "sv << target_device_name;
            return true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
      BOOST_LOG(warning) << "[mic] could not re-enable the Steam mic render endpoint — enable "
                            "'Speakers (Steam Streaming Microphone)' in Windows Sound settings, or "
                            "toggle Steam Remote Play microphone."sv;
      return false;
    }

    return false;
  }

  bool mic_write_wasapi_t::try_enable_endpoint(const std::wstring &device_id) {
    IPolicyConfig *raw_policy = nullptr;
    auto hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
      IID_IPolicyConfig, (void **)&raw_policy);
    if (FAILED(hr) || !raw_policy) {
      BOOST_LOG(warning) << "[mic] try_enable_endpoint: CoCreateInstance(IPolicyConfig) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>> policy(raw_policy);

    hr = policy->SetEndpointVisibility(device_id.c_str(), TRUE);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] try_enable_endpoint: SetEndpointVisibility(TRUE) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    return true;
  }

  void mic_write_wasapi_t::ensure_recommended_steam_mic_format(const std::wstring &device_id) {
    IPolicyConfig *raw_policy = nullptr;
    auto hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
      IID_IPolicyConfig, (void **)&raw_policy);
    if (FAILED(hr) || !raw_policy) {
      BOOST_LOG(warning) << "[mic] ensure_format: CoCreateInstance(IPolicyConfig) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return;
    }
    util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>> policy(raw_policy);

    // Set render endpoint to PCM (the critical fix — not IEEE_FLOAT)
    auto device_fmt = make_recommended_steam_mic_device_waveformat();
    auto device_id_copy = device_id;
    WAVEFORMATEXTENSIBLE prev_fmt {};
    auto render_hr = policy->SetDeviceFormat(device_id_copy.c_str(),
      reinterpret_cast<WAVEFORMATEX *>(&device_fmt),
      reinterpret_cast<WAVEFORMATEX *>(&prev_fmt));

    if (SUCCEEDED(render_hr)) {
      BOOST_LOG(info) << "[mic] Changed Steam microphone render device format for ["sv
                      << target_device_name << "] to [pcm, 32-bit, 48000 Hz, 2ch]"sv;
    } else {
      BOOST_LOG(warning) << "[mic] SetDeviceFormat render: 0x"sv
                         << util::hex(render_hr).to_string_view()
                         << " (format may already be correct)"sv;
    }

    // Also normalize the paired capture endpoint
    IMMDeviceCollection *raw_captures = nullptr;
    if (FAILED(device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &raw_captures))) return;
    util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>> captures(raw_captures);

    UINT count = 0;
    captures->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice *raw_dev = nullptr;
      if (FAILED(captures->Item(i, &raw_dev)) || !raw_dev) continue;
      util::safe_ptr<IMMDevice, release_com<IMMDevice>> cap_dev(raw_dev);

      IPropertyStore *raw_prop = nullptr;
      if (FAILED(cap_dev->OpenPropertyStore(STGM_READ, &raw_prop)) || !raw_prop) continue;
      util::safe_ptr<IPropertyStore, release_com<IPropertyStore>> prop(raw_prop);

      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (FAILED(prop->GetValue(local_PKEY_Device_FriendlyName, &pv)) ||
          pv.vt != VT_LPWSTR || !pv.pwszVal) {
        PropVariantClear(&pv);
        continue;
      }
      std::wstring cap_name(pv.pwszVal);
      PropVariantClear(&pv);

      // Match capture endpoint against steam patterns or generic "Steam Streaming" substring
      std::wstring cap_lower = cap_name;
      std::transform(cap_lower.begin(), cap_lower.end(), cap_lower.begin(), ::towlower);

      bool is_steam_cap = false;
      for (const auto &pattern : autodetect_patterns) {
        std::wstring pat_lower = pattern;
        std::transform(pat_lower.begin(), pat_lower.end(), pat_lower.begin(), ::towlower);
        if (cap_lower.find(pat_lower) != std::wstring::npos) {
          is_steam_cap = true;
          break;
        }
      }
      if (!is_steam_cap && cap_lower.find(L"steam streaming") != std::wstring::npos) {
        is_steam_cap = true;
      }

      if (is_steam_cap) {
        WCHAR *raw_cap_id = nullptr;
        if (FAILED(cap_dev->GetId(&raw_cap_id)) || !raw_cap_id) continue;
        std::wstring cap_id(raw_cap_id);
        CoTaskMemFree(raw_cap_id);

        auto cap_fmt = make_recommended_steam_mic_device_waveformat();
        WAVEFORMATEXTENSIBLE prev {};
        auto cap_hr = policy->SetDeviceFormat(cap_id.c_str(),
          reinterpret_cast<WAVEFORMATEX *>(&cap_fmt),
          reinterpret_cast<WAVEFORMATEX *>(&prev));

        if (SUCCEEDED(cap_hr)) {
          BOOST_LOG(info) << "[mic] Changed Steam microphone capture device format for ["sv
                          << platf::to_utf8(cap_name) << "] to [pcm, 32-bit, 48000 Hz, 2ch]"sv;
        } else {
          BOOST_LOG(warning) << "[mic] SetDeviceFormat capture: 0x"sv
                             << util::hex(cap_hr).to_string_view()
                             << " (format may already be correct)"sv;
        }
      }
    }
  }

  bool mic_write_wasapi_t::initialize_device(const std::wstring &device_id) {
    IMMDevice *raw_device = nullptr;
    auto hr = device_enum->GetDevice(device_id.c_str(), &raw_device);
    if (FAILED(hr) || !raw_device) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetDevice failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    util::safe_ptr<IMMDevice, release_com<IMMDevice>> device(raw_device);

    IAudioClient *raw_client = nullptr;
    hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&raw_client);
    if (FAILED(hr) || !raw_client) {
      BOOST_LOG(warning) << "[mic] initialize_device: Activate failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    audio_client.reset(raw_client);

    // Use IEEE_FLOAT for WASAPI render stream (after PCM SetDeviceFormat above).
    // Buffer duration in REFERENCE_TIME (100-nanosecond units): ms * 10000.
    // Uses mic_buffer_ms from config (default 50ms). Previously hardcoded to 100ms.
    const REFERENCE_TIME buffer_duration =
      static_cast<REFERENCE_TIME>(config::audio.mic_buffer_ms) * 10000LL;
    auto render_fmt = make_required_steam_mic_render_waveformat();
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buffer_duration, 0,
      reinterpret_cast<WAVEFORMATEX *>(&render_fmt), nullptr);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: Initialize failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }

    hr = audio_client->GetBufferSize(&buffer_frame_count);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetBufferSize failed"sv;
      return false;
    }

    IAudioRenderClient *raw_render = nullptr;
    hr = audio_client->GetService(IID_IAudioRenderClient, (void **)&raw_render);
    if (FAILED(hr) || !raw_render) {
      BOOST_LOG(warning) << "[mic] initialize_device: GetService(IAudioRenderClient) failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }
    audio_render = raw_render;

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!evt) {
      BOOST_LOG(warning) << "[mic] initialize_device: CreateEvent failed"sv;
      return false;
    }
    render_event.reset(evt);
    audio_client->SetEventHandle(render_event.get());

    hr = audio_client->Start();
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[mic] initialize_device: Start failed 0x"sv
                         << util::hex(hr).to_string_view();
      return false;
    }

    BOOST_LOG(info) << "[mic] WASAPI init / render target: "sv << target_device_name
                    << " buf="sv << buffer_frame_count << " frames"sv;

    // Only spawn the render thread on first init. When called from render_loop's
    // WASAPI re-init path, render_thread is joinable (we ARE the thread).
    // Assigning over a joinable std::thread calls std::terminate() — guard against that.
    if (!render_thread.joinable()) {
      stop_render_thread.store(false);
      render_dead.store(false);
      render_thread = std::thread(&mic_write_wasapi_t::render_loop, this);
    }
    return true;
  }

  int mic_write_wasapi_t::write_pcm(const float *samples, std::uint32_t frame_count) {
    if (!render_event || stop_render_thread.load(std::memory_order_acquire) ||
        render_dead.load(std::memory_order_acquire)) {
      if (render_dead.load(std::memory_order_acquire) && !render_dead_logged) {
        render_dead_logged = true;
        BOOST_LOG(warning) << "[mic] write_pcm: render thread dead — mic audio being dropped"sv;
      }
      return -1;
    }
    {
      std::lock_guard<std::mutex> lk(queue_mutex);
      for (std::uint32_t i = 0; i < frame_count; ++i) {
        pending_frames.push_back(std::clamp(samples[i], -1.0f, 1.0f));
      }
      // Cap at 1 second to bound latency
      if (pending_frames.size() > 48000) {
        auto trim = pending_frames.size() - 48000;
        pending_frames.erase(pending_frames.begin(), pending_frames.begin() + (std::ptrdiff_t) trim);
      }
    }
    if (!first_packet_written_logged) {
      first_packet_written_logged = true;
      BOOST_LOG(info) << "[mic] first PCM write to Steam mic backend"sv;
    }
    SetEvent(render_event.get());
    return 0;
  }

  void mic_write_wasapi_t::render_loop() {
    auto coinit_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    if (FAILED(coinit_hr) && coinit_hr != RPC_E_CHANGED_MODE) {
      BOOST_LOG(error) << "[mic] render_loop: CoInitializeEx failed 0x"sv
                       << util::hex(coinit_hr).to_string_view();
      return;
    }
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    static constexpr std::size_t kPrebufFrames = 960 * 2;  // 2 Opus packets — matches mic_buffer_packets default

    while (!stop_render_thread.load(std::memory_order_acquire)) {
      WaitForSingleObject(render_event.get(), 20);
      if (stop_render_thread.load(std::memory_order_acquire)) break;

      if (!playout_started) {
        std::size_t qsz;
        { std::lock_guard<std::mutex> lk(queue_mutex); qsz = pending_frames.size(); }
        if (qsz < kPrebufFrames) {
          if (!playout_wait_logged) {
            playout_wait_logged = true;
            BOOST_LOG(info) << "[mic] Steam mic: waiting for prebuffer..."sv;
          }
          continue;
        }
        playout_started = true;
        BOOST_LOG(info) << "[mic] Steam mic playout started (prebuffer: "sv << qsz << " frames)"sv;
      }

      UINT32 padding = 0;
      HRESULT pad_hr = audio_client->GetCurrentPadding(&padding);
      if (FAILED(pad_hr)) {
        BOOST_LOG(warning) << "[mic] GetCurrentPadding failed 0x"sv
                           << util::hex(pad_hr).to_string_view()
                           << " — attempting WASAPI re-init"sv;
        // Stop the existing client before re-init
        if (audio_client) audio_client->Stop();
        if (audio_render) { audio_render->Release(); audio_render = nullptr; }
        audio_client.reset();
        playout_started = false;
        playout_wait_logged = false;
        // Re-find device and re-initialize
        std::wstring device_id;
        if (!find_target_device(device_id) || !initialize_device(device_id)) {
          BOOST_LOG(error) << "[mic] WASAPI re-init failed — render dead"sv;
          render_dead.store(true, std::memory_order_release);
          break;
        }
        BOOST_LOG(info) << "[mic] WASAPI re-init succeeded — resuming render"sv;
        // Clear stale audio queued before the re-init to prevent replaying it
        // into the freshly-opened WASAPI session.
        { std::lock_guard<std::mutex> lk(queue_mutex); pending_frames.clear(); }
        continue;
      }

      auto avail = buffer_frame_count - padding;
      if (avail == 0) continue;

      UINT32 to_write;
      { std::lock_guard<std::mutex> lk(queue_mutex); to_write = std::min(avail, (UINT32) pending_frames.size()); }
      if (to_write == 0) continue;

      BYTE *buf = nullptr;
      if (FAILED(audio_render->GetBuffer(to_write, &buf)) || !buf) continue;

      auto *dst = reinterpret_cast<float *>(buf);
      {
        std::lock_guard<std::mutex> lk(queue_mutex);
        for (UINT32 f = 0; f < to_write; ++f) {
          const float s = pending_frames.front();
          pending_frames.pop_front();
          dst[f * 2]     = s;
          dst[f * 2 + 1] = s;
        }
      }
      audio_render->ReleaseBuffer(to_write, 0);
    }

    if (SUCCEEDED(coinit_hr)) CoUninitialize();
  }

  mic_write_wasapi_t::~mic_write_wasapi_t() {
    stop_render_thread.store(true);
    if (render_event) SetEvent(render_event.get());
    if (render_thread.joinable()) render_thread.join();
    if (audio_client) audio_client->Stop();
    if (audio_render) {
      audio_render->Release();
      audio_render = nullptr;
    }
    // render_event auto-closes via CloseHandle
    // audio_client auto-releases
    // device_enum auto-releases
  }

}  // namespace platf::audio
