/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

// platform includes
#include <winsock2.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#else
  #warning No known Steam audio driver for this architecture
#endif

namespace {

  constexpr auto SAMPLE_RATE = 48000;
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";

  constexpr auto waveformat_mask_stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  constexpr auto waveformat_mask_surround51_with_backspeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

  constexpr auto waveformat_mask_surround51_with_sidespeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  constexpr auto waveformat_mask_surround71 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                              SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                                              SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  enum class sample_format_e {
    f32,
    s32,
    s24in32,
    s24,
    s16,
    _size,
  };

  constexpr WAVEFORMATEXTENSIBLE create_waveformat(sample_format_e sample_format, WORD channel_count, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE waveformat = {};

    switch (sample_format) {
      default:
      case sample_format_e::f32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s24in32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s24:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 24;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s16:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 16;
        waveformat.Samples.wValidBitsPerSample = 16;
        break;
    }

    static_assert((int) sample_format_e::_size == 5, "Unrecognized sample_format_e");

    waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveformat.Format.nChannels = channel_count;
    waveformat.Format.nSamplesPerSec = SAMPLE_RATE;

    waveformat.Format.nBlockAlign = waveformat.Format.nChannels * waveformat.Format.wBitsPerSample / 8;
    waveformat.Format.nAvgBytesPerSec = waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
    waveformat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    waveformat.dwChannelMask = channel_mask;

    return waveformat;
  }

  using virtual_sink_waveformats_t = std::vector<WAVEFORMATEXTENSIBLE>;

  /**
   * @brief List of supported waveformats for an N-channel virtual audio device
   * @tparam channel_count Number of virtual audio channels
   * @returns std::vector<WAVEFORMATEXTENSIBLE>
   * @note The list of virtual formats returned are sorted in preference order and the first valid
   *       format will be used. All bits-per-sample options are listed because we try to match
   *       this to the default audio device. See also: set_format() below.
   */
  template<WORD channel_count>
  virtual_sink_waveformats_t create_virtual_sink_waveformats() {
    if constexpr (channel_count == 2) {
      auto channel_mask = waveformat_mask_stereo;
      // The 32-bit formats are a lower priority for stereo because using one will disable Dolby/DTS
      // spatial audio mode if the user enabled it on the Steam speaker.
      return {
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
      };
    } else if (channel_count == 6) {
      auto channel_mask1 = waveformat_mask_surround51_with_backspeakers;
      auto channel_mask2 = waveformat_mask_surround51_with_sidespeakers;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask2),
      };
    } else if (channel_count == 8) {
      auto channel_mask = waveformat_mask_surround71;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
      };
    }
  }

  std::string waveformat_to_pretty_string(const WAVEFORMATEXTENSIBLE &waveformat) {
    std::string result = waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "F" :
                         waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_PCM        ? "S" :
                                                                                   "UNKNOWN";

    result += std::format("{} {} ", static_cast<int>(waveformat.Samples.wValidBitsPerSample), static_cast<int>(waveformat.Format.nSamplesPerSec));

    switch (waveformat.dwChannelMask) {
      case waveformat_mask_stereo:
        result += "2.0";
        break;

      case waveformat_mask_surround51_with_backspeakers:
        result += "5.1";
        break;

      case waveformat_mask_surround51_with_sidespeakers:
        result += "5.1 (sidespeakers)";
        break;

      case waveformat_mask_surround71:
        result += "7.1";
        break;

      default:
        result += std::format("{} channels (unrecognized)", static_cast<int>(waveformat.Format.nChannels));
        break;
    }

    return result;
  }

}  // namespace

using namespace std::literals;

namespace platf::audio {
  template<class T>
  void Release(T *p) {
    p->Release();
  }

  template<class T>
  void co_task_free(T *p) {
    CoTaskMemFree((LPVOID) p);
  }

  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  using device_t = util::safe_ptr<IMMDevice, Release<IMMDevice>>;
  using collection_t = util::safe_ptr<IMMDeviceCollection, Release<IMMDeviceCollection>>;
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;
  using audio_capture_t = util::safe_ptr<IAudioCaptureClient, Release<IAudioCaptureClient>>;
  using audio_render_t = util::safe_ptr<IAudioRenderClient, Release<IAudioRenderClient>>;
  using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
  using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;
  using policy_t = util::safe_ptr<IPolicyConfig, Release<IPolicyConfig>>;
  using prop_t = util::safe_ptr<IPropertyStore, Release<IPropertyStore>>;

  class co_init_t: public deinit_t {
  public:
    co_init_t() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    }

    ~co_init_t() override {
      CoUninitialize();
    }
  };

  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;
  };

  struct format_t {
    WORD channel_count;
    std::string name;
    int capture_waveformat_channel_mask;
    virtual_sink_waveformats_t virtual_sink_waveformats;
  };

  const std::array<const format_t, 3> formats = {
    format_t {
      2,
      "Stereo",
      waveformat_mask_stereo,
      create_virtual_sink_waveformats<2>(),
    },
    format_t {
      6,
      "Surround 5.1",
      waveformat_mask_surround51_with_backspeakers,
      create_virtual_sink_waveformats<6>(),
    },
    format_t {
      8,
      "Surround 7.1",
      waveformat_mask_surround71,
      create_virtual_sink_waveformats<8>(),
    },
  };

  audio_client_t make_audio_client(device_t &device, const format_t &format) {
    audio_client_t audio_client;
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate Device: [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);

    {
      wave_format_t mixer_waveformat;
      status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      // Prefer the native channel layout of captured audio device when channel counts match
      if (mixer_waveformat->nChannels == format.channel_count &&
          mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          mixer_waveformat->cbSize >= 22) {
        auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
        capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
      }

      BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                      << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                      << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
    }

    status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,  // Enable automatic resampling to 48 KHz
      0,
      0,
      (LPWAVEFORMATEX) &capture_waveformat,
      nullptr
    );

    if (status) {
      BOOST_LOG(error) << "Couldn't initialize audio client for ["sv << format.name << "]: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Audio capture format is "sv << logging::bracket(waveformat_to_pretty_string(capture_waveformat));

    return audio_client;
  }

  device_t default_device(device_enum_t &device_enum) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      eConsole,
      &device
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  class audio_notification_t: public ::IMMNotificationClient {
  public:
    audio_notification_t() {
    }

    // IUnknown implementation (unused by IMMDeviceEnumerator)
    ULONG STDMETHODCALLTYPE AddRef() {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      } else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
      if (flow == eRender) {
        default_render_device_changed_flag.store(true);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId,
      DWORD dwNewState
    ) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId,
      const PROPERTYKEY key
    ) {
      return S_OK;
    }

    /**
     * @brief Checks if the default rendering device changed and resets the change flag
     * @return `true` if the device changed since last call
     */
    bool check_default_render_device_changed() {
      return default_render_device_changed_flag.exchange(false);
    }

  private:
    std::atomic_bool default_render_device_changed_flag;
  };

  class mic_wasapi_t: public mic_t {
  public:
    capture_e sample(std::vector<float> &sample_out) override {
      auto sample_size = sample_out.size();

      // Refill the sample buffer if needed
      while (sample_buf_pos - std::begin(sample_buf) < sample_size) {
        auto capture_result = _fill_buffer();
        if (capture_result != capture_e::ok) {
          return capture_result;
        }
      }

      // Fill the output buffer with samples
      std::copy_n(std::begin(sample_buf), sample_size, std::begin(sample_out));

      // Move any excess samples to the front of the buffer
      std::move(&sample_buf[sample_size], sample_buf_pos, std::begin(sample_buf));
      sample_buf_pos -= sample_size;

      return capture_e::ok;
    }

    int init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;

        return -1;
      }

      HRESULT status;

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = device_enum->RegisterEndpointNotificationCallback(&endpt_notification);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't register endpoint notification [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      auto device = default_device(device_enum);
      if (!device) {
        return -1;
      }

      for (const auto &format : formats) {
        if (format.channel_count != channels_out) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << channels_out << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = channels_out;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      REFERENCE_TIME default_latency;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> {std::max(frames, frame_size) * 2 * channels_out};
      sample_buf_pos = std::begin(sample_buf);

      status = audio_client->GetService(IID_IAudioCaptureClient, (void **) &audio_capture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't initialize audio capture client [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = audio_client->SetEventHandle(audio_event.get());
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't set event handle [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(error) << "Couldn't associate audio capture thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't start recording [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      return 0;
    }

    ~mic_wasapi_t() override {
      if (device_enum) {
        device_enum->UnregisterEndpointNotificationCallback(&endpt_notification);
      }

      if (audio_client) {
        audio_client->Stop();
      }

      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

  private:
    capture_e _fill_buffer() {
      HRESULT status;

      // Total number of samples
      struct sample_aligned_t {
        std::uint32_t uninitialized;
        float *samples;
      } sample_aligned;

      // number of samples / number of channels
      struct block_aligned_t {
        std::uint32_t audio_sample_size;
      } block_aligned;

      // Check if the default audio device has changed
      if (endpt_notification.check_default_render_device_changed()) {
        // Invoke the audio_control_t's callback if it wants one
        if (default_endpt_changed_cb) {
          (*default_endpt_changed_cb)();
        }

        // Reinitialize to pick up the new default device
        return capture_e::reinit;
      }

      // Cap the wait to 100 ms regardless of what the device reports.
      // default_latency_ms is REFERENCE_TIME/1000 (100-ns → μs, then used as ms),
      // which can be arbitrarily large for virtual audio devices (e.g. VB-CABLE).
      // Without the cap, WaitForSingleObjectEx can block longer than the 10-second
      // session-teardown watchdog, causing a forced crash on every clean disconnect.
      constexpr DWORD MAX_CAPTURE_WAIT_MS = 100;
      status = WaitForSingleObjectEx(audio_event.get(),
        std::min<DWORD>(static_cast<DWORD>(default_latency_ms), MAX_CAPTURE_WAIT_MS), FALSE);
      switch (status) {
        case WAIT_OBJECT_0:
          break;
        case WAIT_TIMEOUT:
          return capture_e::timeout;
        default:
          BOOST_LOG(error) << "Couldn't wait for audio event: [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::error;
      }

      std::uint32_t packet_size {};
      for (
        status = audio_capture->GetNextPacketSize(&packet_size);
        SUCCEEDED(status) && packet_size > 0;
        status = audio_capture->GetNextPacketSize(&packet_size)
      ) {
        DWORD buffer_flags;
        status = audio_capture->GetBuffer(
          (BYTE **) &sample_aligned.samples,
          &block_aligned.audio_sample_size,
          &buffer_flags,
          nullptr,
          nullptr
        );

        switch (status) {
          case S_OK:
            break;
          case AUDCLNT_E_DEVICE_INVALIDATED:
            return capture_e::reinit;
          default:
            BOOST_LOG(error) << "Couldn't capture audio [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::error;
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          BOOST_LOG(debug) << "Audio capture signaled buffer discontinuity";
        }

        sample_aligned.uninitialized = std::end(sample_buf) - sample_buf_pos;
        auto n = std::min(sample_aligned.uninitialized, block_aligned.audio_sample_size * channels);

        if (n < block_aligned.audio_sample_size * channels) {
          BOOST_LOG(warning) << "Audio capture buffer overflow";
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill_n(sample_buf_pos, n, 0);
        } else {
          std::copy_n(sample_aligned.samples, n, sample_buf_pos);
        }

        sample_buf_pos += n;

        audio_capture->ReleaseBuffer(block_aligned.audio_sample_size);
      }

      if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
        return capture_e::reinit;
      }

      if (FAILED(status)) {
        return capture_e::error;
      }

      return capture_e::ok;
    }

  public:
    handle_t audio_event;

    device_enum_t device_enum;
    device_t device;
    audio_client_t audio_client;
    audio_capture_t audio_capture;

    audio_notification_t endpt_notification;
    std::optional<std::function<void()>> default_endpt_changed_cb;

    REFERENCE_TIME default_latency_ms;

    util::buffer_t<float> sample_buf;
    float *sample_buf_pos;
    int channels;

    HANDLE mmcss_task_handle = nullptr;
  };

  /**
   * WASAPI render client for mic passthrough.
   * Direct port of Logan's mic_write_wasapi_t (logabell/Apollo) adapted to
   * the platf::speaker_t interface.
   *
   * Flow:
   *   write(samples, count) → push mono samples to pending_frames + SetEvent
   *   render_loop (dedicated thread) → wakes on event or 20ms timeout
   *     → waits for playout prebuffer (4 packets = 3840 mono samples)
   *     → writes each mono sample duplicated to L+R stereo into WASAPI
   *   WASAPI started in init() BEFORE render thread launch (Logan's order)
   */
  class speaker_wasapi_t: public platf::speaker_t {
  public:
    // Called from control stream thread — queue + signal only, zero WASAPI ops.
    int write(const float *samples, std::uint32_t frame_count) override {
      if (!render_event) return -1;
      {
        std::lock_guard<std::mutex> lk(queue_mutex);
        for (std::uint32_t i = 0; i < frame_count; ++i) {
          pending_frames.push_back(std::clamp(samples[i], -1.0f, 1.0f));
        }
        // Cap to 1 second of mono audio to bound latency.
        if (pending_frames.size() > 48000) {
          auto trim = pending_frames.size() - 48000;
          pending_frames.erase(pending_frames.begin(),
                               pending_frames.begin() + (std::ptrdiff_t)trim);
        }
      }
      SetEvent(render_event);
      return 0;
    }

    int init(const std::wstring &device_id, int /*channel_count*/,
             std::uint32_t /*sample_rate*/) {

      // Step 1: SetDeviceFormat via IPolicyConfig BEFORE IMMDevice activation.
      // Logan's ensure_recommended_steam_mic_format() order.
      {
        policy_t pol;
        auto hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr,
          CLSCTX_ALL, IID_IPolicyConfig, (void **)&pol);
        if (SUCCEEDED(hr) && pol) {
          WAVEFORMATEXTENSIBLE wfx {};
          wfx.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
          wfx.Format.nChannels            = 2;
          wfx.Format.nSamplesPerSec       = 48000;
          wfx.Format.wBitsPerSample       = 32;
          wfx.Format.nBlockAlign          = 8;
          wfx.Format.nAvgBytesPerSec      = 48000 * 8;
          wfx.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
          wfx.Samples.wValidBitsPerSample = 32;
          wfx.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
          wfx.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
          WAVEFORMATEXTENSIBLE prev {};
          hr = pol->SetDeviceFormat(device_id.c_str(),
            reinterpret_cast<WAVEFORMATEX *>(&wfx),
            reinterpret_cast<WAVEFORMATEX *>(&prev));
          if (SUCCEEDED(hr)) {
            BOOST_LOG(info) << "[mic] SetDeviceFormat float32/2ch/48kHz: ok"sv;
          } else {
            BOOST_LOG(warning) << "[mic] SetDeviceFormat float32/2ch/48kHz failed 0x"sv
                               << util::hex(hr).to_string_view();
          }
        }
        else {
          BOOST_LOG(warning) << "[mic] IPolicyConfig unavailable 0x"sv
                             << util::hex(hr).to_string_view();
        }
      }

      // Step 2: Activate IAudioClient.
      device_enum_t dev_enum;
      auto status = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr,
        CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **)&dev_enum);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: CoCreateInstance MMDeviceEnumerator failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      device_t device;
      status = dev_enum->GetDevice(device_id.c_str(), &device);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: GetDevice failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      status = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                 (void **)&audio_client);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: Activate failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      // Step 3: Initialize with float32/2ch/48kHz + EVENTCALLBACK.
      // Logan's make_required_steam_mic_render_waveformat() format.
      WAVEFORMATEXTENSIBLE fmt {};
      fmt.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
      fmt.Format.nChannels            = 2;
      fmt.Format.nSamplesPerSec       = 48000;
      fmt.Format.wBitsPerSample       = 32;
      fmt.Format.nBlockAlign          = 8;
      fmt.Format.nAvgBytesPerSec      = 48000 * 8;
      fmt.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      fmt.Samples.wValidBitsPerSample = 32;
      fmt.dwChannelMask               = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
      fmt.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

      status = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        1000000LL,  // 100ms buffer in 100ns units
        0, reinterpret_cast<WAVEFORMATEX *>(&fmt), nullptr);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: Initialize failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      // Log actual mix format for diagnostics
      {
        WAVEFORMATEX *mix_fmt = nullptr;
        if (SUCCEEDED(audio_client->GetMixFormat(&mix_fmt)) && mix_fmt) {
          BOOST_LOG(info) << "[mic] WASAPI mix format: "
                          << mix_fmt->nChannels << "ch, "
                          << mix_fmt->nSamplesPerSec << "Hz, "
                          << mix_fmt->wBitsPerSample << "-bit, "
                          << "tag=0x" << util::hex(mix_fmt->wFormatTag).to_string_view();
          CoTaskMemFree(mix_fmt);
        }
      }

      // Step 4: GetBufferSize + GetService.
      status = audio_client->GetBufferSize(&buffer_frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: GetBufferSize failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      status = audio_client->GetService(IID_IAudioRenderClient, (void **)&audio_render);
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: GetService IAudioRenderClient failed 0x"sv
                         << util::hex(status).to_string_view();
        return -1;
      }

      // Step 5: Create event + SetEventHandle.
      render_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
      if (!render_event) {
        BOOST_LOG(error) << "speaker_wasapi_t: CreateEvent failed"sv;
        return -1;
      }

      status = audio_client->SetEventHandle(render_event);
      if (FAILED(status)) {
        BOOST_LOG(warning) << "[mic] SetEventHandle failed 0x"sv
                           << util::hex(status).to_string_view()
                           << " — render_loop will poll on 20ms timeout"sv;
      }

      // Step 6: Start WASAPI NOW, before render thread (Logan's order).
      // Device runs idle until playout_started is set in render_loop.
      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "speaker_wasapi_t: Start failed 0x"sv
                         << util::hex(status).to_string_view();
        CloseHandle(render_event);
        render_event = nullptr;
        return -1;
      }

      // Step 7: Launch render thread.
      stop_flag = false;
      render_thread = std::thread(&speaker_wasapi_t::render_loop, this);

      BOOST_LOG(info) << "[mic] speaker_wasapi_t ready — WASAPI running, "sv
                      << "render thread waiting for prebuffer"sv;
      return 0;
    }

    ~speaker_wasapi_t() override {
      stop_flag = true;
      if (render_event) SetEvent(render_event);
      if (render_thread.joinable()) render_thread.join();
      if (audio_client) audio_client->Stop();
      // audio_render released by safe_ptr destructor; render_event closed last.
      if (render_event) { CloseHandle(render_event); render_event = nullptr; }
    }

  private:
    static constexpr std::size_t kPrebufFrames = 960 * 4;  // 3840 mono samples (4 Opus packets)

    void render_loop() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);

      bool playout_started = false;

      while (!stop_flag) {
        WaitForSingleObject(render_event, 20);
        if (stop_flag) break;

        // WASAPI is already running. Delay writes until prebuffer is full.
        if (!playout_started) {
          std::size_t qsz;
          { std::lock_guard<std::mutex> lk(queue_mutex); qsz = pending_frames.size(); }
          if (qsz < kPrebufFrames) continue;
          playout_started = true;
          BOOST_LOG(info) << "[mic] Playout started (prebuffer: "sv << qsz << " mono frames)"sv;
        }

        UINT32 padding = 0;
        auto hr = audio_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "[mic] GetCurrentPadding failed 0x"sv
                             << util::hex(hr).to_string_view()
                             << " — render thread stopping"sv;
          break;
        }

        auto avail = buffer_frames - padding;
        if (avail == 0) continue;

        UINT32 to_write;
        {
          std::lock_guard<std::mutex> lk(queue_mutex);
          to_write = std::min(avail, (UINT32)pending_frames.size());
        }
        if (to_write == 0) continue;

        BYTE *buf = nullptr;
        hr = audio_render->GetBuffer(to_write, &buf);
        if (FAILED(hr) || !buf) continue;

        auto *dst = reinterpret_cast<float *>(buf);
        {
          std::lock_guard<std::mutex> lk(queue_mutex);
          for (UINT32 f = 0; f < to_write; ++f) {
            const float s = pending_frames.front();
            pending_frames.pop_front();
            dst[f * 2]     = s;  // L
            dst[f * 2 + 1] = s;  // R
          }
        }
        audio_render->ReleaseBuffer(to_write, 0);
      }

      CoUninitialize();
    }

    audio_client_t     audio_client;
    audio_render_t     audio_render;
    HANDLE             render_event { nullptr };
    UINT32             buffer_frames { 0 };
    std::atomic<bool>  stop_flag { false };
    std::thread        render_thread;
    std::mutex         queue_mutex;
    std::deque<float>  pending_frames;
  };

  class audio_control_t: public ::platf::audio_control_t {
  public:
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Fill host sink name with the device_id of the current default audio device.
      {
        auto device = default_device(device_enum);
        if (!device) {
          return std::nullopt;
        }

        audio::wstring_t id;
        device->GetId(&id);

        sink.host = to_utf8(id.get());
      }

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      } else {
        match_list = match_all_fields(from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = to_utf8(matched->second);
        // Also prepend format name (basically channel layout at the moment)
        // because we don't want to extend the platform interface.
        sink.null = std::make_optional(sink_t::null_t {
          "virtual-"s + formats[0].name + device_id,
          "virtual-"s + formats[1].name + device_id,
          "virtual-"s + formats[2].name + device_id,
        });
      } else if (!config::audio.virtual_sink.empty()) {
        BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink " << config::audio.virtual_sink;
      }

      return sink;
    }

    bool is_sink_available(const std::string &sink) override {
      const auto match_list = match_all_fields(from_utf8(sink));
      const auto matched = find_device_id(match_list);
      return static_cast<bool>(matched);
    }

    /**
     * @brief Extract virtual audio sink information possibly encoded in the sink name.
     * @param sink The sink name
     * @return A pair of device_id and format reference if the sink name matches
     *         our naming scheme for virtual audio sinks, `std::nullopt` otherwise.
     */
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>> extract_virtual_sink_info(const std::string &sink) {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string current = sink;
      auto prefix = "virtual-"sv;
      if (current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = from_utf8(current.substr(name.size(), current.size() - name.size()));
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
      auto mic = std::make_unique<mic_wasapi_t>();

      if (mic->init(sample_rate, frame_size, channels)) {
        return nullptr;
      }

      if (config::audio.keep_default) {
        // If this is a virtual sink, set a callback that will change the sink back if it's changed
        auto virtual_sink_info = extract_virtual_sink_info(assigned_sink);
        if (virtual_sink_info) {
          mic->default_endpt_changed_cb = [this] {
            BOOST_LOG(info) << "Resetting sink to ["sv << assigned_sink << "] after default changed";
            set_sink(assigned_sink);
          };
        }
      }

      return mic;
    }

    std::unique_ptr<platf::speaker_t> virtual_microphone(const std::string &device_name, int channels, std::uint32_t sample_rate) override {
      // "CABLE Input" is a render (playback) endpoint from WASAPI's perspective,
      // so find_device_id (which enumerates eRender) will locate it correctly.
      auto matched = find_device_id(match_all_fields(from_utf8(device_name)));
      if (!matched) {
        BOOST_LOG(warning) << "[mic] WARNING: no suitable mic sink — mic passthrough disabled for this session"sv;
        return nullptr;
      }

      auto spk = std::make_unique<speaker_wasapi_t>();
      if (spk->init(matched->second, channels, sample_rate)) {
        return nullptr;
      }

      return spk;
    }

    bool is_steam_mic_available() override {
      return static_cast<bool>(find_device_id(match_steam_microphone()));
    }

    /**
     * If the requested sink is a virtual sink, meaning no speakers attached to
     * the host, then we can seamlessly set the format to stereo and surround sound.
     *
     * Any virtual sink detected will be prefixed by:
     *    virtual-(format name)
     * If it doesn't contain that prefix, then the format will not be changed
     */
    std::optional<std::wstring> set_format(const std::string &sink) {
      if (sink.empty()) {
        return std::nullopt;
      }

      auto virtual_sink_info = extract_virtual_sink_info(sink);

      if (!virtual_sink_info) {
        // Sink name does not begin with virtual-(format name), hence it's not a virtual sink
        // and we don't want to change playback format of the corresponding device.
        // Also need to perform matching, sink name is not necessarily device_id in this case.
        auto matched = find_device_id(match_all_fields(from_utf8(sink)));
        if (matched) {
          return matched->second;
        } else {
          BOOST_LOG(error) << "Couldn't find audio sink " << sink;
          return std::nullopt;
        }
      }

      // When switching to a Steam virtual speaker device, try to retain the bit depth of the
      // default audio device. Switching from a 16-bit device to a 24-bit one has been known to
      // cause glitches for some users.
      int wanted_bits_per_sample = 32;
      auto current_default_dev = default_device(device_enum);
      if (current_default_dev) {
        audio::prop_t prop;
        prop_var_t current_device_format;

        if (SUCCEEDED(current_default_dev->OpenPropertyStore(STGM_READ, &prop)) && SUCCEEDED(prop->GetValue(PKEY_AudioEngine_DeviceFormat, &current_device_format.prop))) {
          auto *format = (WAVEFORMATEXTENSIBLE *) current_device_format.prop.blob.pBlobData;
          wanted_bits_per_sample = format->Samples.wValidBitsPerSample;
          BOOST_LOG(info) << "Virtual audio device will use "sv << wanted_bits_per_sample << "-bit to match default device"sv;
        }
      }

      auto &device_id = virtual_sink_info->first;
      auto &waveformats = virtual_sink_info->second.get().virtual_sink_waveformats;
      for (const auto &waveformat : waveformats) {
        // We're using completely undocumented and unlisted API,
        // better not pass objects without copying them first.
        auto device_id_copy = device_id;
        auto waveformat_copy = waveformat;
        auto waveformat_copy_pointer = reinterpret_cast<WAVEFORMATEX *>(&waveformat_copy);

        if (wanted_bits_per_sample != waveformat.Samples.wValidBitsPerSample) {
          continue;
        }

        WAVEFORMATEXTENSIBLE p {};
        if (SUCCEEDED(policy->SetDeviceFormat(device_id_copy.c_str(), waveformat_copy_pointer, (WAVEFORMATEX *) &p))) {
          BOOST_LOG(info) << "Changed virtual audio sink format to " << logging::bracket(waveformat_to_pretty_string(waveformat));
          return device_id;
        }
      }

      BOOST_LOG(error) << "Couldn't set virtual audio sink waveformat";
      return std::nullopt;
    }

    int set_sink(const std::string &sink) override {
      auto device_id = set_format(sink);
      if (!device_id) {
        return -1;
      }

      int failure {};
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        auto status = policy->SetDefaultEndpoint(device_id->c_str(), (ERole) x);
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          } else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
        }
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (!failure) {
        assigned_sink = sink;
      }

      return failure;
    }

    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name (exact)
      device_friendly_name_contains,  ///< Match endpoint friendly name (substring)
      adapter_friendly_name,  ///< Match adapter friendly name
      device_description,  ///< Match endpoint description
    };

    using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
    using matched_field_t = std::pair<match_field_e, std::wstring>;

    audio_control_t::match_fields_list_t match_steam_speakers() {
      return {
        {match_field_e::adapter_friendly_name, L"Steam Streaming Speakers"}
      };
    }

    // Steam Streaming Microphone render-side matching (eRender endpoint only).
    // Priority:
    //   1. device_friendly_name exact "Speakers (Steam Streaming Microphone)" — primary
    //   2. device_friendly_name exact "Steam Streaming Microphone" — locale-prefix fallback
    // Intentionally NOT using device_friendly_name_contains: the substring "Steam Streaming
    // Microphone" would also match the eCapture endpoint "Microphone (Steam Streaming
    // Microphone)" if Steam registers it in both flow directions.  Exact match on the
    // eRender name avoids silently opening the wrong endpoint.
    // Notes:
    //   adapter_friendly_name ({026e516e-b814-414b-83cd-856d6fef4822},2) is not populated
    //   by the Steam driver on this system; device_description (pid=2) is "Speakers" which
    //   is too broad to use as a discriminator.
    audio_control_t::match_fields_list_t match_steam_microphone() {
      return {
        {match_field_e::device_friendly_name, L"Speakers (Steam Streaming Microphone)"},
        {match_field_e::device_friendly_name, L"Steam Streaming Microphone"},
      };
    }

    audio_control_t::match_fields_list_t match_all_fields(const std::wstring &name) {
      return {
        {match_field_e::device_id, name},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {match_field_e::device_friendly_name, name},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {match_field_e::device_description, name},  // Digital Audio (S/PDIF)
        {match_field_e::adapter_friendly_name, name},  // High Definition Audio Device
      };
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t> find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        audio::device_t device;
        collection->Item(x, &device);

        audio::wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        audio::prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_friendly_name_contains:
                // substring match — handled below
                if (device_friendly_name.prop.pwszVal &&
                    std::wcsstr(device_friendly_name.prop.pwszVal, match_list[i].second.c_str())) {
                  matched[i] = device_id;
                }
                continue;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resets the default audio device from Steam Streaming Speakers.
     */
    void reset_default_device() {
      auto matched_steam = find_device_id(match_steam_speakers());
      if (!matched_steam) {
        return;
      }
      auto steam_device_id = matched_steam->second;

      {
        // Get the current default audio device (if present)
        auto current_default_dev = default_device(device_enum);
        if (!current_default_dev) {
          return;
        }

        audio::wstring_t current_default_id;
        current_default_dev->GetId(&current_default_id);

        // If Steam Streaming Speakers are already not default, we're done.
        if (steam_device_id != current_default_id.get()) {
          return;
        }
      }

      // Disable the Steam Streaming Speakers temporarily to allow the OS to pick a new default.
      auto hr = policy->SetEndpointVisibility(steam_device_id.c_str(), FALSE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to disable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // Get the newly selected default audio device
      auto new_default_dev = default_device(device_enum);

      // Enable the Steam Streaming Speakers again
      hr = policy->SetEndpointVisibility(steam_device_id.c_str(), TRUE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to enable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // If there's now no audio device, the Steam Streaming Speakers were the only device available.
      // There's no other device to set as the default, so just return.
      if (!new_default_dev) {
        return;
      }

      audio::wstring_t new_default_id;
      new_default_dev->GetId(&new_default_id);

      // Set the new default audio device
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        policy->SetDefaultEndpoint(new_default_id.get(), (ERole) x);
      }

      BOOST_LOG(info) << "Successfully reset default audio device"sv;
    }

    /**
     * @brief Installs the Steam Streaming Speakers driver, if present.
     * @return `true` if installation was successful.
     */
    bool install_steam_audio_drivers() {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a is missing DiInstallDriverW() even though the headers have it,
      // so we have to load it at runtime. It's Vista or later, so it will always be available.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "newdev.dll failed to load"sv;
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        BOOST_LOG(error) << "DiInstallDriverW() is missing"sv;
        return false;
      }

      // Get the current default audio device (if present)
      auto old_default_dev = default_device(device_enum);

      // Install the Steam Streaming Speakers driver
      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // If there was a previous default device, restore that original device as the
        // default output device just in case installing the new one changed it.
        if (old_default_dev) {
          audio::wstring_t old_default_id;
          old_default_dev->GetId(&old_default_id);

          for (int x = 0; x < (int) ERole_enum_count; ++x) {
            policy->SetDefaultEndpoint(old_default_id.get(), (ERole) x);
          }
        }

        return true;
      } else {
        auto err = GetLastError();
        switch (err) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Administrator privileges are required to install Steam Streaming Speakers"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Steam audio drivers not found. This is expected if you don't have Steam installed."sv;
            break;
          default:
            BOOST_LOG(warning) << "Failed to install Steam audio drivers: "sv << err;
            break;
        }

        return false;
      }
#else
      BOOST_LOG(warning) << "Unable to install Steam Streaming Speakers on unknown architecture"sv;
      return false;
#endif
    }

    int init() {
      auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void **) &policy
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create audio policy config: [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      return 0;
    }

    std::wstring find_capture_device_id(const std::wstring &name) {
      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) return {};
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        device_t device;
        if (FAILED(collection->Item(i, &device))) continue;
        prop_t prop;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &prop))) continue;
        prop_var_t pv;
        if (SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) &&
            pv.prop.vt == VT_LPWSTR && std::wcscmp(pv.prop.pwszVal, name.c_str()) == 0) {
          wstring_t id;
          if (SUCCEEDED(device->GetId(&id)))
            return std::wstring(id.get());
        }
      }
      return {};
    }

    std::string switch_default_capture_device(const std::string &device_name) override {
      // Enumerate all active capture devices for diagnostics
      {
        collection_t dbg_collection;
        if (SUCCEEDED(device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &dbg_collection))) {
          UINT dbg_count = 0;
          dbg_collection->GetCount(&dbg_count);
          BOOST_LOG(info) << "switch_default_capture_device: enumerating " << dbg_count
                          << " active capture device(s), looking for: \"" << device_name << "\"";
          for (UINT i = 0; i < dbg_count; ++i) {
            device_t dbg_dev;
            if (FAILED(dbg_collection->Item(i, &dbg_dev))) continue;
            prop_t dbg_prop;
            if (FAILED(dbg_dev->OpenPropertyStore(STGM_READ, &dbg_prop))) continue;
            prop_var_t dbg_pv;
            if (SUCCEEDED(dbg_prop->GetValue(PKEY_Device_FriendlyName, &dbg_pv.prop)) && dbg_pv.prop.vt == VT_LPWSTR)
              BOOST_LOG(info) << "  capture device[" << i << "]: \"" << to_utf8(dbg_pv.prop.pwszVal) << "\"";
          }
        }
      }

      device_t prev_dev;
      std::wstring prev_id;
      std::string prev_name;
      if (SUCCEEDED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &prev_dev))) {
        wstring_t id;
        if (SUCCEEDED(prev_dev->GetId(&id))) prev_id = id.get();
        prop_t prev_prop;
        if (SUCCEEDED(prev_dev->OpenPropertyStore(STGM_READ, &prev_prop))) {
          prop_var_t pv;
          if (SUCCEEDED(prev_prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) && pv.prop.vt == VT_LPWSTR)
            prev_name = to_utf8(pv.prop.pwszVal);
        }
      }
      auto target_id = find_capture_device_id(from_utf8(device_name));
      if (target_id.empty()) {
        BOOST_LOG(warning) << "switch_default_capture_device: not found: " << device_name;
        return {};
      }
      if (target_id == prev_id) return to_utf8(prev_id);
      for (int x = 0; x < (int) ERole_enum_count; ++x)
        policy->SetDefaultEndpoint(target_id.c_str(), (ERole) x);
      BOOST_LOG(info) << "[mic] default capture: '" << prev_name << "' -> '" << device_name << "'";
      // Verify the switch took effect; retry up to 3 times with 500 ms intervals.
      for (int retry = 0; retry < 3; ++retry) {
        device_t verify_dev;
        std::wstring verify_id;
        if (SUCCEEDED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &verify_dev))) {
          wstring_t vid;
          if (SUCCEEDED(verify_dev->GetId(&vid))) verify_id = vid.get();
        }
        if (verify_id == target_id) break;  // switch confirmed
        BOOST_LOG(info) << "[mic] switch_default_capture_device: verify failed (attempt " << (retry + 1) << "), retrying in 500 ms";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (int x = 0; x < (int) ERole_enum_count; ++x)
          policy->SetDefaultEndpoint(target_id.c_str(), (ERole) x);
        if (retry == 2) {
          BOOST_LOG(warning) << "[mic] switch_default_capture_device: switch did not take effect after 3 retries";
        }
      }
      return to_utf8(prev_id);
    }

    void restore_default_capture_device(const std::string &prev_id) override {
      if (prev_id.empty()) return;
      auto wide = from_utf8(prev_id);
      // Capture current (CABLE Output) and restored device names for logging.
      std::string current_name;
      {
        device_t cur_dev;
        if (SUCCEEDED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &cur_dev))) {
          prop_t prop;
          if (SUCCEEDED(cur_dev->OpenPropertyStore(STGM_READ, &prop))) {
            prop_var_t pv;
            if (SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) && pv.prop.vt == VT_LPWSTR)
              current_name = to_utf8(pv.prop.pwszVal);
          }
        }
      }
      std::string restored_name;
      {
        device_t restored_dev;
        if (SUCCEEDED(device_enum->GetDevice(wide.c_str(), &restored_dev))) {
          prop_t prop;
          if (SUCCEEDED(restored_dev->OpenPropertyStore(STGM_READ, &prop))) {
            prop_var_t pv;
            if (SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) && pv.prop.vt == VT_LPWSTR)
              restored_name = to_utf8(pv.prop.pwszVal);
          }
        }
      }
      for (int x = 0; x < (int) ERole_enum_count; ++x)
        policy->SetDefaultEndpoint(wide.c_str(), (ERole) x);
      BOOST_LOG(info) << "[mic] default capture restored: '" << current_name << "' -> '" << restored_name << "'";
    }

    std::string get_current_default_capture_name() override {
      device_t dev;
      if (FAILED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return {};
      prop_t prop;
      if (FAILED(dev->OpenPropertyStore(STGM_READ, &prop))) return {};
      prop_var_t pv;
      if (SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) && pv.prop.vt == VT_LPWSTR)
        return to_utf8(pv.prop.pwszVal);
      return {};
    }

    std::string get_current_default_capture_id() override {
      device_t dev;
      if (FAILED(device_enum->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) return {};
      wstring_t id;
      if (FAILED(dev->GetId(&id))) return {};
      return to_utf8(id.get());
    }

    void reset_default_capture_to_first_real() override {
      collection_t collection;
      if (FAILED(device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) return;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        device_t dev;
        if (FAILED(collection->Item(i, &dev))) continue;
        prop_t prop;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &prop))) continue;
        prop_var_t pv;
        if (FAILED(prop->GetValue(PKEY_Device_FriendlyName, &pv.prop)) || pv.prop.vt != VT_LPWSTR) continue;
        std::string name = to_utf8(pv.prop.pwszVal);
        if (name.find("CABLE") != std::string::npos) continue;
        wstring_t id;
        if (FAILED(dev->GetId(&id))) continue;
        for (int x = 0; x < (int) ERole_enum_count; ++x)
          policy->SetDefaultEndpoint(id.get(), (ERole) x);
        BOOST_LOG(info) << "Startup crash recovery: reset default capture to: " << name;
        return;
      }
      BOOST_LOG(warning) << "Startup crash recovery: no non-CABLE capture device found";
    }

    bool install_vbcable() {
      static std::atomic_bool attempted {false};
      if (attempted.exchange(true, std::memory_order_acq_rel)) return false;
      wchar_t module_path[MAX_PATH] = {};
      if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) return false;
      namespace fs = std::filesystem;
      fs::path exe_dir = fs::path(module_path).parent_path();
      fs::path script_path = exe_dir / L"drivers" / L"vbcable" / L"install.ps1";
      if (!fs::exists(script_path)) {
        BOOST_LOG(warning) << "VB-Cable install: script not found at " << platf::to_utf8(script_path.wstring());
        return false;
      }
      BOOST_LOG(info) << "VB-Audio CABLE not found; running installer...";
      std::wstring cmd = L"powershell.exe -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass"
                         L" -WindowStyle Hidden -File \"" + script_path.wstring() + L"\"";
      STARTUPINFOW si {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
      PROCESS_INFORMATION pi {};
      BOOL launched = FALSE;

      if (platf::is_running_as_system()) {
        // Vibepollo is running as the SYSTEM service. Use the SudoVDA elevation pattern:
        // acquire the logged-in user's linked admin token so the installer runs with
        // administrator rights inside the user's interactive session (not session 0),
        // which is required for audio device registration.
        HANDLE user_token = platf::retrieve_users_token(/*elevated=*/true);
        if (!user_token) {
          BOOST_LOG(warning) << "VB-Cable install: no active user session available; cannot elevate installer";
          return false;
        }

        STARTUPINFOW si_user {}; si_user.cb = sizeof(si_user); si_user.dwFlags = STARTF_USESHOWWINDOW; si_user.wShowWindow = SW_HIDE;
        launched = CreateProcessAsUserW(
          user_token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
          CREATE_NO_WINDOW, nullptr, exe_dir.wstring().c_str(), &si_user, &pi
        );
        const DWORD launch_err = GetLastError();

        CloseHandle(user_token);

        if (!launched) {
          BOOST_LOG(warning) << "VB-Cable install: elevated launch failed (error=" << launch_err << ")";
          return false;
        }
      }
      else {
        // Not running as SYSTEM — inherit current process token (elevation depends on
        // how Vibepollo was started; e.g. run-as-admin shortcut or UAC-elevated shell).
        launched = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                  nullptr, exe_dir.wstring().c_str(), &si, &pi);
        if (!launched) {
          BOOST_LOG(warning) << "VB-Cable install: failed to launch (error=" << GetLastError() << ")";
          return false;
        }
      }

      DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
      DWORD ec = 1;
      if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &ec);
      CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
      if (wait != WAIT_OBJECT_0 || ec != 0) {
        BOOST_LOG(warning) << "VB-Cable install: script exited with code " << ec;
        return false;
      }
      BOOST_LOG(info) << "VB-Audio CABLE installed successfully.";
      Sleep(3000);
      return true;
    }

    ~audio_control_t() override {}

    policy_t policy;
    audio::device_enum_t device_enum;
    std::string assigned_sink;
  };
}  // namespace platf::audio

namespace platf {

  // It's not big enough to justify it's own source file :/
  namespace dxgi {
    int init();
  }

  std::unique_ptr<audio_control_t> audio_control() {
    auto control = std::make_unique<audio::audio_control_t>();

    if (control->init()) {
      return nullptr;
    }

    // Install Steam Streaming Speakers if needed. We do this during audio_control() to ensure
    // the sink information returned includes the new Steam Streaming Speakers device.
    if (config::audio.install_steam_drivers && !control->find_device_id(control->match_steam_speakers())) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
    }

    // Install VB-Audio CABLE if mic passthrough is configured and CABLE Input (render) is not present.
    if (config::audio.install_vbcable && !config::audio.mic_sink.empty()) {
      if (!control->find_device_id(control->match_all_fields(from_utf8(config::audio.mic_sink)))) {
        control->install_vbcable();
      }
    }

    // Log VB-Audio CABLE version at startup for diagnostics.
    if (!config::audio.mic_sink.empty()) {
      // Read driver version from HKLM\SYSTEM\CurrentControlSet\services\VBAudioVACMME
      std::string cable_version = "unknown";
      HKEY hKey;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\services\\VBAudioVACMME",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t image_path[MAX_PATH] = {};
        DWORD sz = sizeof(image_path);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"ImagePath", nullptr, &type,
              reinterpret_cast<LPBYTE>(image_path), &sz) == ERROR_SUCCESS) {
          // Resolve SystemRoot variable in path
          std::wstring path(image_path);
          WCHAR sys_root[MAX_PATH];
          if (ExpandEnvironmentStringsW(path.c_str(), sys_root, MAX_PATH)) {
            DWORD handle = 0;
            DWORD info_size = GetFileVersionInfoSizeW(sys_root, &handle);
            if (info_size > 0) {
              std::vector<BYTE> buf(info_size);
              if (GetFileVersionInfoW(sys_root, handle, info_size, buf.data())) {
                VS_FIXEDFILEINFO *ffi = nullptr;
                UINT ffi_len = 0;
                if (VerQueryValueW(buf.data(), L"\\",
                      reinterpret_cast<LPVOID *>(&ffi), &ffi_len) && ffi) {
                  cable_version =
                    std::to_string(HIWORD(ffi->dwFileVersionMS)) + "." +
                    std::to_string(LOWORD(ffi->dwFileVersionMS)) + "." +
                    std::to_string(HIWORD(ffi->dwFileVersionLS)) + "." +
                    std::to_string(LOWORD(ffi->dwFileVersionLS));
                }
              }
            }
          }
        }
        RegCloseKey(hKey);
      }
      if (cable_version != "unknown") {
        BOOST_LOG(info) << "VB-Audio Virtual Cable version " << cable_version << " detected — mic passthrough ready";
      } else if (control->find_device_id(control->match_all_fields(from_utf8(config::audio.mic_sink)))) {
        BOOST_LOG(info) << "VB-Audio Virtual Cable detected (version unknown) — mic passthrough ready";
      } else {
        BOOST_LOG(warning) << "VB-Audio CABLE Input not found — mic passthrough will not work. "
                              "Install VB-Audio Virtual Cable from https://vb-audio.com/Cable/"sv;
      }
    }

    return control;
  }

  std::unique_ptr<deinit_t> init() {
    if (dxgi::init()) {
      return nullptr;
    }

    // Initialize COM
    auto co_init = std::make_unique<platf::audio::co_init_t>();

    // If Steam Streaming Speakers are currently the default audio device,
    // change the default to something else (if another device is available).
    audio::audio_control_t audio_ctrl;
    if (audio_ctrl.init() == 0) {
      audio_ctrl.reset_default_device();
    }

    return co_init;
  }
}  // namespace platf
