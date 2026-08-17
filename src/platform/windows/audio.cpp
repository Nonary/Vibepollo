/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// platform includes
#include <WinSock2.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>

// local includes
#include "src/config.h"
#include "src/audio_policy.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "utf_utils.h"

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#endif

namespace {

  constexpr auto SAMPLE_RATE = 48000;
#ifdef STEAM_DRIVER_SUBDIR
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";
#endif

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

  device_t default_device(device_enum_t &device_enum, ERole role = eConsole) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      role,
      &device
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  using role_device_ids_t = std::array<std::wstring, static_cast<std::size_t>(ERole_enum_count)>;

  struct pending_role_restore_t {
    ERole role;
    std::wstring preferred_id;
    std::wstring expected_current_id;
    bool fallback_transition = false;
  };

  using pending_role_restores_t = std::vector<pending_role_restore_t>;

  struct pending_role_restore_handoff_t {
    std::wstring steam_device_id;
    pending_role_restores_t role_restores;
    std::uint64_t assignment_epoch = 0;
  };

  constexpr std::size_t role_index(ERole role) {
    return static_cast<std::size_t>(role);
  }

  /**
   * @brief Lightweight IMMNotificationClient that signals a Win32 Event
   * when a non-ignored audio device becomes active or is added.
   * Used by reset_default_device() to wait for device arrival.
   */
  class device_arrival_notification_t: public ::IMMNotificationClient {
  public:
    /**
     * @param ignored_device_id Device ID to ignore in notifications (e.g., Steam Streaming Speakers).
     */
    explicit device_arrival_notification_t(const std::wstring &ignored_device_id):
        ignored_id(ignored_device_id) {
      arrival_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!arrival_event) {
        BOOST_LOG(warning) << "Failed to create device arrival event"sv;
      }
    }

    ~device_arrival_notification_t() {
      if (arrival_event) {
        CloseHandle(arrival_event);
      }
    }

    ULONG STDMETHODCALLTYPE AddRef() { return 1; }
    ULONG STDMETHODCALLTYPE Release() { return 1; }

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

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      if (arrival_event && !is_ignored(pwstrDeviceId)) {
        SetEvent(arrival_event);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
      if (dwNewState == DEVICE_STATE_ACTIVE && arrival_event && !is_ignored(pwstrDeviceId)) {
        SetEvent(arrival_event);
      }
      return S_OK;
    }

    /**
     * @brief Wait for the arrival event to be signaled.
     * @param timeout_ms Maximum time to wait in milliseconds.
     * @return true if signaled, false on timeout.
     */
    bool wait(HANDLE cancel_event, DWORD timeout_ms) {
      if (!arrival_event) {
        if (cancel_event) {
          WaitForSingleObject(cancel_event, timeout_ms);
        } else {
          Sleep(timeout_ms);
        }
        return false;
      }

      HANDLE wait_handles[2] {arrival_event, cancel_event};
      DWORD handle_count = cancel_event ? 2 : 1;
      auto result = WaitForMultipleObjects(handle_count, wait_handles, FALSE, timeout_ms);
      if (result == WAIT_OBJECT_0) {
        ResetEvent(arrival_event);
        return true;
      }
      return false;
    }

  private:
    bool is_ignored(LPCWSTR device_id) const {
      return device_id && !ignored_id.empty() && ignored_id == device_id;
    }

    HANDLE arrival_event = nullptr;
    std::wstring ignored_id;
  };

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
        if (capture_result == capture_e::timeout && continuous_audio) {
          // Write silence to sample_buf
          std::fill_n(sample_buf_pos, sample_size, 0.0f);
          sample_buf_pos += sample_size;
        } else if (capture_result != capture_e::ok) {
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

    int init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out, bool continuous) {
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
      continuous_audio = continuous;

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

      status = WaitForSingleObjectEx(audio_event.get(), default_latency_ms, FALSE);
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
    bool continuous_audio;

    HANDLE mmcss_task_handle = nullptr;
  };

  class audio_control_t: public ::platf::audio_control_t {
  public:
    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Capture each render role before the virtual sink replaces them. The
      // console path below retains its existing pending-restore behavior.
      auto default_device_ids = current_default_device_ids();
      auto &host_id = default_device_ids[role_index(eConsole)];
      if (host_id.empty()) {
        return std::nullopt;
      }

      const auto endpoint_catalog = active_render_endpoint_catalog();
      const auto steam_device_ids = steam_render_device_ids(endpoint_catalog);
      if (endpoint_catalog.complete && contains_device_id(steam_device_ids, host_id)) {
        std::vector<std::wstring> preferred_ids;
        if (const auto pending_id = pending_preferred_restore_id()) {
          preferred_ids.push_back(*pending_id);
        }
        if (const auto replacement = active_non_steam_render_endpoint(endpoint_catalog, preferred_ids)) {
          for (auto &default_id : default_device_ids) {
            if (contains_device_id(steam_device_ids, default_id)) {
              default_id = *replacement;
            }
          }
        }
      } else if (endpoint_catalog.complete) {
        clear_pending_preferred_restore();
      }

      sink.host = utf_utils::to_utf8(host_id.c_str());
      // Pre-populate the restore-cache so we have property snapshots even if
      // the device disappears before reset_default_device runs.
      for (const auto &device_id : default_device_ids) {
        if (!device_id.empty()) {
          (void) preferred_device_match_list(device_id);
        }
      }
      captured_default_device_ids = std::move(default_device_ids);

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      } else {
        match_list = match_all_fields(utf_utils::from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list, DEVICE_STATEMASK_ALL);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = utf_utils::to_utf8(matched->second);
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
      const auto match_list = match_all_fields(utf_utils::from_utf8(sink));
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
            auto device_id = utf_utils::from_utf8(current.substr(name.size(), current.size() - name.size()));
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
      auto mic = std::make_unique<mic_wasapi_t>();

      if (mic->init(sample_rate, frame_size, channels, continuous_audio)) {
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
        auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)));
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
      return set_sink(sink, host_mute_requested_);
    }

    int set_sink(const std::string &sink, bool mute_host) override {
      host_mute_requested_ = mute_host;
      if (host_mute_prepared_ && !host_mute_active_ && !rollback_host_mute_visibility()) {
        BOOST_LOG(error) << "Previous host-audio mute visibility rollback is incomplete"sv;
        return -1;
      }
      const bool starting_host_mute = mute_host && !host_mute_active_ && !host_mute_prepared_;
      if (starting_host_mute && !prepare_host_mute_visibility()) {
        BOOST_LOG(error) << "Couldn't establish the host-audio mute endpoint visibility transaction"sv;
        return -1;
      }

      auto device_id = set_format(sink);
      if (!device_id) {
        if (starting_host_mute) {
          (void) rollback_host_mute_visibility();
        }
        return -1;
      }

      // Cancel immediately before replacing the defaults so a failed format
      // setup leaves the existing recovery worker intact.
      const auto current_default_ids = current_default_device_ids();
      role_device_ids_t desired_device_ids;
      desired_device_ids.fill(*device_id);
      auto pending_restore_handoff = begin_policy_assignment(std::move(desired_device_ids));
      const auto assignment_epoch = pending_restore_handoff.assignment_epoch;
      pending_role_restores_t transferred_role_restores;
      if (!pending_restore_handoff.role_restores.empty()) {
        const auto transfer_catalog = active_render_endpoint_catalog();
        transferred_role_restores = normalize_pending_role_restores(
          std::move(pending_restore_handoff.role_restores),
          transfer_catalog.complete ?
            steam_render_device_ids(transfer_catalog) :
            std::vector<std::wstring> {},
          current_default_ids
        );
      }

      // The initial setup happens before microphone callbacks exist. Later
      // callbacks reapply the same sink, so capture defaults only on the first
      // assignment. If a previous session was still restoring a role, preserve
      // its preferred endpoint while the worker-owned fallback remains selected.
      if (assigned_device_id.empty()) {
        for (std::size_t index = 0; index < current_default_ids.size(); ++index) {
          if (!current_default_ids[index].empty()) {
            captured_default_device_ids[index] = current_default_ids[index];
          }
        }

        for (const auto &role_restore : transferred_role_restores) {
          const auto index = role_index(role_restore.role);
          captured_default_device_ids[index] = role_restore.preferred_id;
        }

        for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
          if (!captured_default_device_ids[static_cast<std::size_t>(x)].empty()) {
            BOOST_LOG(info) << "Captured pre-stream default endpoint for role ["sv << x
                            << "]: "sv
                            << utf_utils::to_utf8(captured_default_device_ids[static_cast<std::size_t>(x)].c_str());
          }
        }

        assigned_device_id = *device_id;
      }

      int failure {};
      bool assignment_active = true;
      pending_role_restores_t failed_role_restores;
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        const auto role = static_cast<ERole>(x);
        auto result = set_default_endpoint_for_assignment(assignment_epoch, role, *device_id);
        if (!result) {
          assignment_active = false;
          break;
        }
        const auto status = *result;
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          } else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
          for (const auto &role_restore : transferred_role_restores) {
            if (role_restore.role == role) {
              failed_role_restores.push_back(role_restore);
              break;
            }
          }
        }
      }

      if (assignment_active &&
          !failed_role_restores.empty() &&
          !pending_restore_handoff.steam_device_id.empty()) {
        // This role never transferred to the new stream, so keep its prior
        // recovery alive instead of dropping the preferred endpoint.
        start_pending_role_restore_task(
          pending_restore_handoff.steam_device_id,
          std::move(failed_role_restores),
          assignment_epoch
        );
      }

      if (!assignment_active || failure) {
        if (starting_host_mute) {
          // The assignment may have partially changed role defaults. Reuse
          // the normal role-scoped restore path before restoring visibility.
          (void) restore_sink(sink);
          assigned_device_id.clear();
          assigned_sink.clear();
        }
        return ::audio::policy::sink_assignment_result(assignment_active, failure);
      }

      if (starting_host_mute && !commit_host_mute_visibility()) {
        (void) restore_sink(sink);
        assigned_device_id.clear();
        assigned_sink.clear();
        return -1;
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (assignment_active && !failure) {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (policy_assignment_epoch_ref() == assignment_epoch) {
          assigned_sink = sink;
        }
      }

      return ::audio::policy::sink_assignment_result(assignment_active, failure);
    }

    int restore_sink(const std::string &) override {
      // Preserve the old teardown order: stop a pending retry before writing
      // the captured role defaults. Publish every intended role before the
      // calls so an older in-flight worker can repair to this assignment.
      const auto current_default_ids = current_default_device_ids();
      const bool visibility_restored = teardown_host_mute_visibility();
      if (!visibility_restored) {
        BOOST_LOG(warning) << "Host-audio mute endpoint visibility teardown was incomplete"sv;
      }
      auto desired_device_ids = current_default_ids;
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        const auto index = role_index(role);
        const auto &captured_device_id = captured_default_device_ids[index];
        if (!assigned_device_id.empty() &&
            current_default_ids[index] == assigned_device_id &&
            !captured_device_id.empty() &&
            captured_device_id != assigned_device_id) {
          desired_device_ids[index] = captured_device_id;
        }
      }
      pending_role_restore_handoff = begin_policy_assignment(std::move(desired_device_ids));
      const auto assignment_epoch = pending_role_restore_handoff.assignment_epoch;

      int failure {};
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        if (assigned_device_id.empty() ||
            current_default_ids[role_index(role)] != assigned_device_id) {
          continue;
        }

        const auto &captured_device_id = captured_default_device_ids[role_index(role)];
        if (captured_device_id.empty() || captured_device_id == assigned_device_id) {
          continue;
        }

        auto result = set_default_endpoint_for_assignment(
          assignment_epoch,
          role,
          captured_device_id
        );
        if (!result) {
          break;
        }
        const auto status = *result;
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Couldn't restore captured audio endpoint for role ["sv << x
                             << "]: 0x"sv << util::hex(status).to_string_view();
          ++failure;
        }
      }

      return failure || (visibility_restored ? 0 : 1);
    }

    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name
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

    audio_control_t::match_fields_list_t match_all_fields(const std::wstring &name) {
      return {
        {match_field_e::device_id, name},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {match_field_e::device_friendly_name, name},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {match_field_e::device_description, name},  // Digital Audio (S/PDIF)
        {match_field_e::adapter_friendly_name, name},  // High Definition Audio Device
      };
    }

    ::audio::policy::render_endpoint_catalog_t active_render_endpoint_catalog() {
      collection_t collection;
      const auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status) || !collection) {
        BOOST_LOG(error) << "Couldn't enumerate active render endpoints: [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return ::audio::policy::build_render_endpoint_catalog(false, {});
      }

      UINT count = 0;
      const auto count_status = collection->GetCount(&count);
      if (FAILED(count_status)) {
        BOOST_LOG(error) << "Couldn't count active render endpoints: [0x"sv
                         << util::hex(count_status).to_string_view() << ']';
        return ::audio::policy::build_render_endpoint_catalog(false, {});
      }

      bool complete = true;
      std::vector<::audio::policy::render_endpoint_t> endpoints;
      endpoints.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        audio::device_t device;
        if (FAILED(collection->Item(index, &device)) || !device) {
          complete = false;
          continue;
        }

        audio::wstring_t device_id;
        if (FAILED(device->GetId(&device_id)) || !device_id) {
          complete = false;
          continue;
        }

        std::string adapter_name;
        audio::prop_t properties;
        const auto property_status = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(property_status) && properties) {
          prop_var_t adapter_friendly_name;
          if (SUCCEEDED(properties->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop)) &&
              adapter_friendly_name.prop.vt == VT_LPWSTR &&
              adapter_friendly_name.prop.pwszVal &&
              adapter_friendly_name.prop.pwszVal[0] != L'\0') {
            adapter_name = utf_utils::to_utf8(adapter_friendly_name.prop.pwszVal);
          } else {
            complete = false;
          }
        } else {
          complete = false;
        }

        endpoints.push_back({
          utf_utils::to_utf8(device_id.get()),
          std::move(adapter_name),
          true,
        });
      }

      auto catalog = ::audio::policy::build_render_endpoint_catalog(complete, endpoints);
      if (!catalog.complete) {
        BOOST_LOG(warning) << "Active render endpoint discovery was incomplete; skipping Steam audio recovery policy writes"sv;
      }
      return catalog;
    }

    static std::vector<std::wstring> steam_render_device_ids(
      const ::audio::policy::render_endpoint_catalog_t &catalog
    ) {
      std::vector<std::wstring> ids;
      ids.reserve(catalog.steam_endpoint_ids.size());
      for (const auto &device_id : catalog.steam_endpoint_ids) {
        ids.push_back(utf_utils::from_utf8(device_id));
      }
      return ids;
    }

    static bool contains_device_id(
      const std::vector<std::wstring> &device_ids,
      const std::wstring &device_id
    ) {
      return !device_id.empty() &&
             std::find(device_ids.begin(), device_ids.end(), device_id) != device_ids.end();
    }

    std::optional<std::wstring> active_non_steam_render_endpoint(
      const ::audio::policy::render_endpoint_catalog_t &catalog,
      const std::vector<std::wstring> &preferred_ids
    ) {
      std::vector<std::string> preferred_utf8;
      preferred_utf8.reserve(preferred_ids.size());
      for (const auto &preferred_id : preferred_ids) {
        if (!preferred_id.empty()) {
          preferred_utf8.push_back(utf_utils::to_utf8(preferred_id.c_str()));
        }
      }

      const auto selected = ::audio::policy::select_eligible_non_steam_render_endpoint(
        catalog,
        preferred_utf8
      );
      if (!selected) {
        return std::nullopt;
      }
      return utf_utils::from_utf8(*selected);
    }

    static std::mutex &preferred_restore_cache_mutex_ref() {
      static std::mutex mutex;
      return mutex;
    }

    static std::unordered_map<std::wstring, match_fields_list_t> &preferred_restore_cache_ref() {
      static std::unordered_map<std::wstring, match_fields_list_t> cache;
      return cache;
    }

    static std::wstring &pending_preferred_restore_id_ref() {
      static std::wstring id;
      return id;
    }

    static std::optional<std::wstring> pending_preferred_restore_id() {
      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      const auto &id = pending_preferred_restore_id_ref();
      if (id.empty()) {
        return std::nullopt;
      }

      return id;
    }

    static void remember_pending_preferred_restore(const std::wstring &preferred_id, const std::wstring &steam_device_id) {
      if (preferred_id.empty() || preferred_id == steam_device_id) {
        return;
      }

      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      pending_preferred_restore_id_ref() = preferred_id;
    }

    static void clear_pending_preferred_restore(const std::wstring &preferred_id = {}) {
      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      auto &pending_id = pending_preferred_restore_id_ref();
      if (preferred_id.empty() || pending_id == preferred_id) {
        pending_id.clear();
      }
    }

    role_device_ids_t current_default_device_ids() {
      role_device_ids_t device_ids;
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        auto device = default_device(device_enum, role);
        if (!device) {
          continue;
        }

        audio::wstring_t id;
        if (SUCCEEDED(device->GetId(&id)) && id) {
          device_ids[role_index(role)] = id.get();
        }
      }
      return device_ids;
    }

    static pending_role_restores_t normalize_pending_role_restores(
      pending_role_restores_t role_restores,
      const std::vector<std::wstring> &steam_device_ids,
      const role_device_ids_t &current_default_ids
    ) {
      pending_role_restores_t normalized;
      normalized.reserve(role_restores.size());
      for (auto &role_restore : role_restores) {
        const auto &current_id = current_default_ids[role_index(role_restore.role)];
        if (role_restore.preferred_id.empty() || current_id.empty()) {
          continue;
        }

        const bool retained_endpoint =
          current_id == role_restore.expected_current_id ||
          current_id == role_restore.preferred_id;
        const bool retained_steam_topology =
          contains_device_id(steam_device_ids, role_restore.expected_current_id) &&
          contains_device_id(steam_device_ids, current_id);
        if (!retained_endpoint && !retained_steam_topology) {
          // A mismatch outside the product-owned Steam topology is a
          // newer user or system choice. Never replace it with the old target.
          continue;
        }

        // The live default is now the ownership guard for a restarted worker.
        role_restore.expected_current_id = current_id;
        role_restore.fallback_transition = false;
        normalized.push_back(std::move(role_restore));
      }
      return normalized;
    }

    static void append_match_field(match_fields_list_t &match_list, match_field_e field, const wchar_t *value) {
      if (value == nullptr || value[0] == L'\0') {
        return;
      }

      const std::wstring candidate {value};
      for (const auto &[existing_field, existing_value] : match_list) {
        if (existing_field == field && existing_value == candidate) {
          return;
        }
      }

      match_list.emplace_back(field, candidate);
    }

    std::optional<match_fields_list_t> preferred_device_match_list(const std::wstring &preferred_id) {
      {
        std::lock_guard lock {preferred_restore_cache_mutex_ref()};
        auto &cache = preferred_restore_cache_ref();
        const auto it = cache.find(preferred_id);
        if (it != cache.end()) {
          return it->second;
        }
      }

      audio::device_t device;
      if (FAILED(device_enum->GetDevice(preferred_id.c_str(), &device)) || !device) {
        return std::nullopt;
      }

      match_fields_list_t match_list;
      match_list.emplace_back(match_field_e::device_id, preferred_id);

      audio::prop_t prop;
      if (FAILED(device->OpenPropertyStore(STGM_READ, &prop)) || !prop) {
        return match_list;
      }

      prop_var_t device_friendly_name;
      prop_var_t adapter_friendly_name;
      prop_var_t device_desc;

      append_match_field(match_list, match_field_e::device_friendly_name,
        SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop)) ? device_friendly_name.prop.pwszVal : nullptr);
      append_match_field(match_list, match_field_e::device_description,
        SUCCEEDED(prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop)) ? device_desc.prop.pwszVal : nullptr);
      append_match_field(match_list, match_field_e::adapter_friendly_name,
        SUCCEEDED(prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop)) ? adapter_friendly_name.prop.pwszVal : nullptr);

      {
        std::lock_guard lock {preferred_restore_cache_mutex_ref()};
        preferred_restore_cache_ref()[preferred_id] = match_list;
      }

      return match_list;
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t> find_device_id(
      const match_fields_list_t &match_list,
      DWORD state_mask = DEVICE_STATE_ACTIVE
    ) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, state_mask, &collection);
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
     * If a preferred device is supplied, tries to restore that exact device,
     * keeping a background retry active if it is temporarily missing (e.g.,
     * HDMI/DP audio coming back after virtual display teardown). While a
     * preferred restore is pending, Steam speakers remain usable instead of
     * falling back to another endpoint.
     * @param preferred_device The endpoint device_id of the device to restore.
     */
    void reset_default_device(const std::string &preferred_device = {}) override {
      // A stream can assign a different endpoint to the console, multimedia,
      // and communications roles. Always keep its recovery role-scoped so the
      // legacy all-role fallback cannot overwrite a role that was restored.
      if (!assigned_device_id.empty()) {
        reset_failed_default_roles();
        return;
      }

      std::wstring preferred_id;
      if (!preferred_device.empty()) {
        preferred_id = utf_utils::from_utf8(preferred_device);
      }
      reset_default_device_impl(true, preferred_id);
    }

    /**
     * @brief Non-blocking variant of reset_default_device() for startup.
     * Tries once to move the default away from Steam speakers without waiting.
     */
    void reset_default_device_no_wait() {
      reset_default_device_impl(false, {});
    }

  private:
    using render_endpoint_states_t = std::vector<::audio::policy::render_endpoint_t>;

    std::optional<render_endpoint_states_t> render_endpoint_visibility_snapshot() {
      collection_t collection;
      const auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection);
      if (FAILED(status) || !collection) {
        BOOST_LOG(error) << "Couldn't enumerate render endpoint visibility state: [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      if (FAILED(collection->GetCount(&count))) {
        BOOST_LOG(error) << "Couldn't count render endpoint visibility state"sv;
        return std::nullopt;
      }

      render_endpoint_states_t endpoints;
      endpoints.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        audio::device_t device;
        if (FAILED(collection->Item(index, &device)) || !device) {
          return std::nullopt;
        }

        audio::wstring_t device_id;
        DWORD state = DEVICE_STATE_NOTPRESENT;
        if (FAILED(device->GetId(&device_id)) || !device_id || FAILED(device->GetState(&state))) {
          return std::nullopt;
        }

        audio::prop_t properties;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties) {
          return std::nullopt;
        }

        prop_var_t adapter_friendly_name;
        // Audio endpoint devnodes expose their human-readable adapter name as
        // PKEY_Device_FriendlyName. PKEY_DeviceInterface_FriendlyName is an
        // interface property and is empty when queried from IMMDevice's
        // endpoint property store, which would make the visibility snapshot
        // fail closed before any handoff writes occur.
        const auto friendly_name_status = properties->GetValue(PKEY_Device_FriendlyName, &adapter_friendly_name.prop);
        if (FAILED(friendly_name_status) ||
            adapter_friendly_name.prop.vt != VT_LPWSTR ||
            !adapter_friendly_name.prop.pwszVal ||
            adapter_friendly_name.prop.pwszVal[0] == L'\0') {
          // Stale disabled/unplugged endpoint records can legitimately lack a
          // friendly name. Their ID and original state are still sufficient to
          // restore visibility; only Steam endpoints need names for topology
          // classification, and the pair check below still fails closed if
          // either Steam half cannot be identified.
          BOOST_LOG(warning) << "Render endpoint has no friendly name; preserving it by ID/state: index ["
                             << index << "] id [" << utf_utils::to_utf8(device_id.get())
                             << "] HRESULT [0x" << util::hex(friendly_name_status).to_string_view() << ']';
        }

        endpoints.push_back({
          utf_utils::to_utf8(device_id.get()),
          adapter_friendly_name.prop.vt == VT_LPWSTR && adapter_friendly_name.prop.pwszVal ?
            utf_utils::to_utf8(adapter_friendly_name.prop.pwszVal) : std::string {},
          state == DEVICE_STATE_ACTIVE,
        });
      }

      return endpoints;
    }

    bool set_endpoint_visibility(const std::vector<std::string> &endpoint_ids, INT visible) {
      bool success = true;
      for (const auto &endpoint_id : endpoint_ids) {
        if (endpoint_id.empty()) {
          success = false;
          continue;
        }
        const auto endpoint_id_wide = utf_utils::from_utf8(endpoint_id);
        const auto status = policy->SetEndpointVisibility(endpoint_id_wide.c_str(), visible);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Couldn't set endpoint visibility for ["sv << endpoint_id
                             << "] to ["sv << visible << "]: 0x"
                             << util::hex(status).to_string_view();
          success = false;
        }
      }
      return success;
    }

    bool prepare_host_mute_visibility() {
      const auto snapshot = render_endpoint_visibility_snapshot();
      if (!snapshot) {
        return false;
      }

      std::vector<std::string> virtual_ids;
      bool has_speakers = false;
      bool has_microphone = false;
      for (const auto &endpoint : *snapshot) {
        if (!::audio::policy::is_steam_streaming_render_adapter(endpoint.adapter_name)) {
          continue;
        }
        virtual_ids.push_back(endpoint.id);
        has_speakers = has_speakers ||
          endpoint.adapter_name == "Steam Streaming Speakers" ||
          endpoint.adapter_name == "Speakers (Steam Streaming Speakers)";
        has_microphone = has_microphone ||
          endpoint.adapter_name == "Steam Streaming Microphone" ||
          endpoint.adapter_name == "Speakers (Steam Streaming Microphone)";
      }

      // Host mute owns both halves of the Steam topology. If either half is
      // missing, fail closed instead of leaving a partially managed topology.
      if (!has_speakers || !has_microphone) {
        BOOST_LOG(error) << "Steam render pair is incomplete; refusing host-audio mute transition"sv;
        return false;
      }

      const auto plan = ::audio::policy::plan_host_mute_visibility(true, *snapshot, virtual_ids);
      if (!plan) {
        return false;
      }

      host_mute_visibility_plan_ = *plan;
      host_mute_prepared_ = true;
      BOOST_LOG(info) << "Host-audio mute visibility plan: show Steam endpoints ["
                      << host_mute_visibility_plan_.show_on_connect.size()
                      << "], hide physical endpoints ["
                      << host_mute_visibility_plan_.hide_on_connect.size() << "]"sv;
      if (!set_endpoint_visibility(host_mute_visibility_plan_.show_on_connect, TRUE)) {
        rollback_host_mute_visibility();
        return false;
      }
      return true;
    }

    bool commit_host_mute_visibility() {
      if (!host_mute_prepared_) {
        return false;
      }
      if (!set_endpoint_visibility(host_mute_visibility_plan_.hide_on_connect, FALSE)) {
        rollback_host_mute_visibility();
        return false;
      }
      host_mute_active_ = true;
      return true;
    }

    bool rollback_host_mute_visibility() {
      if (!host_mute_prepared_) {
        return true;
      }
      // Restore the exact pre-transition state: physical endpoints that were
      // active become visible again, and Steam halves that were hidden remain
      // hidden. Best-effort cleanup is still attempted for every endpoint.
      const bool physical_visible = set_endpoint_visibility(
        host_mute_visibility_plan_.hide_on_connect,
        TRUE
      );
      const bool steam_restored = set_endpoint_visibility(
        host_mute_visibility_plan_.show_on_connect,
        FALSE
      );
      if (!physical_visible || !steam_restored) {
        return false;
      }

      host_mute_visibility_plan_ = {};
      host_mute_prepared_ = false;
      host_mute_active_ = false;
      return true;
    }

    bool teardown_host_mute_visibility() {
      if (!host_mute_prepared_) {
        return true;
      }
      if (!host_mute_active_) {
        return rollback_host_mute_visibility();
      }

      const bool steam_hidden = set_endpoint_visibility(
        host_mute_visibility_plan_.hide_on_teardown,
        FALSE
      );
      const bool physical_visible = set_endpoint_visibility(
        host_mute_visibility_plan_.show_on_teardown,
        TRUE
      );
      if (!steam_hidden || !physical_visible) {
        return false;
      }

      host_mute_visibility_plan_ = {};
      host_mute_prepared_ = false;
      host_mute_active_ = false;
      return true;
    }

    bool is_default_device(const std::wstring &device_id, ERole role = eConsole) {
      auto current_default_dev = default_device(device_enum, role);
      if (!current_default_dev) {
        return false;
      }

      audio::wstring_t current_default_id;
      if (FAILED(current_default_dev->GetId(&current_default_id)) || !current_default_id) {
        return false;
      }

      return device_id == current_default_id.get();
    }

    static std::mutex &pending_restore_mutex_ref() {
      static std::mutex mutex;
      return mutex;
    }

    static std::jthread &pending_restore_thread_ref() {
      static std::jthread thread;
      return thread;
    }

    using pending_restore_token_t = std::shared_ptr<std::atomic_bool>;

    static pending_restore_token_t &pending_restore_token_ref() {
      static pending_restore_token_t token;
      return token;
    }

    static pending_role_restores_t &pending_role_restores_ref() {
      static pending_role_restores_t role_restores;
      return role_restores;
    }

    static std::wstring &pending_restore_steam_device_id_ref() {
      static std::wstring steam_device_id;
      return steam_device_id;
    }

    static std::uint64_t &policy_assignment_epoch_ref() {
      static std::uint64_t epoch = 0;
      return epoch;
    }

    static role_device_ids_t &policy_assignment_desired_ids_ref() {
      static role_device_ids_t desired_ids;
      return desired_ids;
    }

    static void deactivate_pending_restore_worker_locked() {
      auto &token = pending_restore_token_ref();
      if (token) {
        token->store(false, std::memory_order_release);
        token.reset();
      }
      pending_role_restores_ref().clear();
      pending_restore_steam_device_id_ref().clear();
    }

    static bool pending_restore_worker_owns_state_locked(const pending_restore_token_t &token, std::uint64_t assignment_epoch) {
      return token &&
             token->load(std::memory_order_acquire) &&
             pending_restore_token_ref() == token &&
             pending_restore_thread_ref().get_id() == std::this_thread::get_id() &&
             policy_assignment_epoch_ref() == assignment_epoch;
    }

    static bool pending_restore_worker_can_write(
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (stop_token.stop_requested() || !token || !token->load(std::memory_order_acquire)) {
        return false;
      }

      std::scoped_lock lock(pending_restore_mutex_ref());
      return pending_restore_worker_owns_state_locked(token, assignment_epoch);
    }

    // The caller holds pending_restore_mutex_ref(). Only roll back the marker
    // when this worker is still the registered owner, so a failed handoff
    // cannot clear a newer worker's pending restore. Clear unconditionally:
    // a failed assignment can leave the old marker intact.
    static std::jthread rollback_pending_restore_worker_locked(const pending_restore_token_t &token) {
      if (pending_restore_token_ref() != token) {
        return {};
      }

      token->store(false, std::memory_order_release);
      clear_pending_preferred_restore();
      pending_role_restores_ref().clear();
      pending_restore_steam_device_id_ref().clear();

      auto failed_thread = std::move(pending_restore_thread_ref());
      pending_restore_token_ref().reset();
      return failed_thread;
    }

    static void retire_pending_restore_task(std::jthread old_thread) {
      if (!old_thread.joinable()) {
        return;
      }

      old_thread.request_stop();
      std::thread([old_thread = std::move(old_thread)]() mutable {
        old_thread.join();
      }).detach();
    }

    static pending_role_restore_handoff_t begin_policy_assignment(role_device_ids_t desired_ids) {
      std::jthread old_thread;
      pending_role_restore_handoff_t handoff;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        handoff.steam_device_id = std::move(pending_restore_steam_device_id_ref());
        handoff.role_restores = std::move(pending_role_restores_ref());
        deactivate_pending_restore_worker_locked();
        old_thread = std::move(pending_restore_thread_ref());

        auto &assignment_epoch = policy_assignment_epoch_ref();
        if (++assignment_epoch == 0) {
          ++assignment_epoch;
        }
        policy_assignment_desired_ids_ref() = std::move(desired_ids);
        handoff.assignment_epoch = assignment_epoch;
      }
      retire_pending_restore_task(std::move(old_thread));
      return handoff;
    }

    static bool policy_assignment_role_is_current(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return policy_assignment_epoch_ref() == assignment_epoch &&
             policy_assignment_desired_ids_ref()[role_index(role)] == desired_id;
    }

    static bool policy_assignment_is_current(std::uint64_t assignment_epoch) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return policy_assignment_epoch_ref() == assignment_epoch;
    }

    static bool publish_policy_assignment_role(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (policy_assignment_epoch_ref() != assignment_epoch) {
        return false;
      }
      policy_assignment_desired_ids_ref()[role_index(role)] = desired_id;
      return true;
    }

    static bool remember_pending_preferred_restore_for_assignment(
      const std::wstring &preferred_id,
      const std::wstring &steam_device_id,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (policy_assignment_epoch_ref() != assignment_epoch) {
        return false;
      }
      remember_pending_preferred_restore(preferred_id, steam_device_id);
      return true;
    }

    static void clear_pending_preferred_restore_for_assignment(
      std::uint64_t assignment_epoch,
      const std::wstring &preferred_id = {}
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (policy_assignment_epoch_ref() == assignment_epoch) {
        clear_pending_preferred_restore(preferred_id);
      }
    }

    void reassert_current_policy_assignment_role(ERole role) {
      // A policy call may return after a newer assignment has already written.
      // Follow the epoch until one repair lands for a still-current target.
      for (int attempt = 0; attempt < 3; ++attempt) {
        std::uint64_t assignment_epoch;
        std::wstring desired_id;
        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          assignment_epoch = policy_assignment_epoch_ref();
          desired_id = policy_assignment_desired_ids_ref()[role_index(role)];
        }
        if (desired_id.empty()) {
          return;
        }

        const auto status = policy->SetDefaultEndpoint(desired_id.c_str(), role);
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Couldn't reassert the current audio endpoint for role ["sv
                             << static_cast<int>(role) << "]: 0x"sv
                             << util::hex(status).to_string_view();
        }
        if (SUCCEEDED(status) &&
            policy_assignment_role_is_current(assignment_epoch, role, desired_id)) {
          return;
        }
      }
    }

    void reassert_current_policy_assignment() {
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        reassert_current_policy_assignment_role(static_cast<ERole>(x));
      }
    }

    std::optional<HRESULT> set_default_endpoint_for_assignment(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      if (!publish_policy_assignment_role(assignment_epoch, role, desired_id)) {
        return std::nullopt;
      }

      const auto status = policy->SetDefaultEndpoint(desired_id.c_str(), role);
      if (!policy_assignment_role_is_current(assignment_epoch, role, desired_id)) {
        reassert_current_policy_assignment_role(role);
        return std::nullopt;
      }
      return status;
    }

    bool publish_policy_assignment_role_for_worker(
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return false;
      }
      policy_assignment_desired_ids_ref()[role_index(role)] = desired_id;
      return true;
    }

    bool adopt_current_policy_endpoint_for_worker(
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role
    ) {
      std::wstring current_id;
      auto current_device = default_device(device_enum, role);
      if (current_device) {
        audio::wstring_t device_id;
        if (SUCCEEDED(current_device->GetId(&device_id)) && device_id) {
          current_id = device_id.get();
        }
      }
      return publish_policy_assignment_role_for_worker(
        token,
        assignment_epoch,
        role,
        current_id
      );
    }

    bool adopt_current_policy_endpoint_for_assignment(
      std::uint64_t assignment_epoch,
      ERole role
    ) {
      std::wstring current_id;
      auto current_device = default_device(device_enum, role);
      if (current_device) {
        audio::wstring_t device_id;
        if (SUCCEEDED(current_device->GetId(&device_id)) && device_id) {
          current_id = device_id.get();
        }
      }
      return publish_policy_assignment_role(assignment_epoch, role, current_id);
    }

    std::optional<HRESULT> set_default_endpoint_for_worker(
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      if (stop_token.stop_requested() ||
          !publish_policy_assignment_role_for_worker(token, assignment_epoch, role, desired_id)) {
        return std::nullopt;
      }

      const auto status = policy->SetDefaultEndpoint(desired_id.c_str(), role);
      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch) ||
          !policy_assignment_role_is_current(assignment_epoch, role, desired_id)) {
        reassert_current_policy_assignment_role(role);
        return std::nullopt;
      }
      return status;
    }

    static void start_pending_restore_task(
      const std::wstring &steam_device_id,
      const std::wstring &preferred_id,
      std::uint64_t assignment_epoch
    ) {
      const bool publish_preferred_id = !preferred_id.empty() && preferred_id != steam_device_id;
      auto token = std::make_shared<std::atomic_bool>(true);
      auto start_promise = std::make_shared<std::promise<bool>>();
      auto start_signal = start_promise->get_future().share();
      std::jthread old_thread;
      std::jthread new_thread;
      std::jthread failed_thread;
      try {
        // The thread is gated until the old worker is invalidated and this
        // worker's marker/token are registered below.
        new_thread = std::jthread([steam_device_id, preferred_id, publish_preferred_id, token, start_signal, assignment_epoch](std::stop_token stop_token) {
          if (!start_signal.get() || !pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
            return;
          }

          co_init_t co_init;
          audio_control_t restore_control;
          if (restore_control.init() != 0) {
            if (publish_preferred_id) {
              audio_control_t::clear_pending_preferred_restore_for_worker(
                preferred_id,
                token,
                assignment_epoch
              );
            }
            return;
          }
          restore_control.run_pending_restore_task(stop_token, steam_device_id, preferred_id, token, assignment_epoch);
        });

        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          if (policy_assignment_epoch_ref() != assignment_epoch) {
            token->store(false, std::memory_order_release);
            start_promise->set_value(false);
          } else {
            deactivate_pending_restore_worker_locked();
            old_thread = std::move(pending_restore_thread_ref());
            old_thread.request_stop();
            pending_restore_thread_ref() = std::move(new_thread);
            pending_restore_token_ref() = token;
            if (publish_preferred_id) {
              remember_pending_preferred_restore(preferred_id, steam_device_id);
            }
            start_promise->set_value(true);
          }
        }

      } catch (...) {
        token->store(false, std::memory_order_release);
        try {
          start_promise->set_value(false);
        } catch (...) {
        }
        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          failed_thread = rollback_pending_restore_worker_locked(token);
        }
        retire_pending_restore_task(std::move(new_thread));
        retire_pending_restore_task(std::move(failed_thread));
        retire_pending_restore_task(std::move(old_thread));
        throw;
      }
      retire_pending_restore_task(std::move(old_thread));
    }

    static void start_pending_role_restore_task(
      const std::wstring &steam_device_id,
      pending_role_restores_t role_restores,
      std::uint64_t assignment_epoch
    ) {
      auto published_role_restores = role_restores;
      std::optional<std::wstring> console_preferred_id;
      for (const auto &role_restore : role_restores) {
        if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
          console_preferred_id = role_restore.preferred_id;
          break;
        }
      }

      auto token = std::make_shared<std::atomic_bool>(true);
      auto start_promise = std::make_shared<std::promise<bool>>();
      auto start_signal = start_promise->get_future().share();
      std::jthread old_thread;
      std::jthread new_thread;
      std::jthread failed_thread;
      try {
        // The thread starts immediately, but it cannot initialize COM or
        // touch policy until the protected handoff below signals it.
        new_thread = std::jthread([steam_device_id, role_restores = std::move(role_restores), token, start_signal, assignment_epoch](std::stop_token stop_token) mutable {
          if (!start_signal.get() || !pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
            return;
          }

          co_init_t co_init;
          audio_control_t restore_control;
          if (restore_control.init() != 0) {
            audio_control_t::clear_pending_role_restores_for_worker(
              role_restores,
              token,
              assignment_epoch
            );
            return;
          }
          restore_control.run_pending_role_restore_task(stop_token, steam_device_id, std::move(role_restores), token, assignment_epoch);
        });

        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          if (policy_assignment_epoch_ref() != assignment_epoch) {
            token->store(false, std::memory_order_release);
            start_promise->set_value(false);
          } else {
            deactivate_pending_restore_worker_locked();
            old_thread = std::move(pending_restore_thread_ref());
            old_thread.request_stop();
            pending_restore_thread_ref() = std::move(new_thread);
            pending_restore_token_ref() = token;
            pending_restore_steam_device_id_ref() = steam_device_id;
            pending_role_restores_ref() = std::move(published_role_restores);
            if (console_preferred_id) {
              remember_pending_preferred_restore(*console_preferred_id, steam_device_id);
            } else {
              clear_pending_preferred_restore();
            }
            start_promise->set_value(true);
          }
        }

      } catch (...) {
        token->store(false, std::memory_order_release);
        try {
          start_promise->set_value(false);
        } catch (...) {
        }
        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          failed_thread = rollback_pending_restore_worker_locked(token);
        }
        retire_pending_restore_task(std::move(new_thread));
        retire_pending_restore_task(std::move(failed_thread));
        retire_pending_restore_task(std::move(old_thread));
        throw;
      }
      retire_pending_restore_task(std::move(old_thread));
    }

    void run_pending_restore_task(
      std::stop_token stop_token,
      const std::wstring &steam_device_id,
      const std::wstring &preferred_id,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      bool try_preferred_restore = !preferred_id.empty() && preferred_id != steam_device_id;
      bool retry_fallback_reset = true;

      device_arrival_notification_t arrival_notifier(steam_device_id);
      HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!cancel_event) {
        BOOST_LOG(warning) << "Failed to create background restore cancellation event"sv;
      }
      auto cancel_event_guard = util::fail_guard([&]() {
        if (cancel_event) {
          CloseHandle(cancel_event);
        }
      });
      std::optional<std::stop_callback<std::function<void()>>> stop_callback;
      if (cancel_event) {
        stop_callback.emplace(stop_token, [cancel_event]() {
          SetEvent(cancel_event);
        });
      }

      auto reg_status = device_enum->RegisterEndpointNotificationCallback(&arrival_notifier);
      const bool have_notifications = SUCCEEDED(reg_status);
      if (!have_notifications) {
        BOOST_LOG(warning) << "Failed to register device arrival notification for background restore: "sv
                           << util::hex(reg_status).to_string_view();
      }
      auto unreg_guard = util::fail_guard([&]() {
        if (have_notifications) {
          device_enum->UnregisterEndpointNotificationCallback(&arrival_notifier);
        }
      });

      if (try_preferred_restore) {
        BOOST_LOG(info) << "Waiting in background to restore the original default audio device"sv;
      } else {
        BOOST_LOG(info) << "Waiting in background for a non-Steam audio device to appear"sv;
      }

      while (pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        if (try_preferred_restore) {
          auto preferred_result = try_restore_preferred(
            preferred_id,
            steam_device_id,
            assignment_epoch,
            &stop_token,
            token
          );
          if (preferred_result == reset_result_e::success) {
            return;
          }
          if (preferred_result == reset_result_e::fatal) {
            clear_pending_preferred_restore_for_worker(
              preferred_id,
              token,
              assignment_epoch
            );
            return;
          }
          if (preferred_result == reset_result_e::inactive) {
            return;
          }

          arrival_notifier.wait(cancel_event, 1000);
          continue;
        }

        if (retry_fallback_reset) {
          auto fallback_result = try_reset_from_steam(
            steam_device_id,
            assignment_epoch,
            &stop_token,
            token
          );
          if (fallback_result == reset_result_e::fatal) {
            return;
          }
          if (fallback_result == reset_result_e::inactive) {
            return;
          }
          if (fallback_result == reset_result_e::success && !try_preferred_restore) {
            return;
          }
          if (fallback_result == reset_result_e::no_device) {
            retry_fallback_reset = false;
          }
        }

        // If notification registration failed, use the timed wait as a polling
        // backoff so a fallback endpoint that appears later is still retried.
        if (arrival_notifier.wait(cancel_event, 1000) || !have_notifications) {
          retry_fallback_reset = true;
        }
      }
    }

    void reset_failed_default_roles() {
      auto inherited_handoff = std::move(pending_role_restore_handoff);
      pending_role_restore_handoff = {};
      if (inherited_handoff.assignment_epoch == 0) {
        inherited_handoff = begin_policy_assignment(current_default_device_ids());
      } else if (!policy_assignment_is_current(inherited_handoff.assignment_epoch)) {
        return;
      }

      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        const auto current_default_ids = current_default_device_ids();
        auto role_restores = normalize_pending_role_restores(
          std::move(inherited_handoff.role_restores),
          {},
          current_default_ids
        );
        if (!assigned_device_id.empty()) {
          for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
            const auto role = static_cast<ERole>(x);
            const auto already_queued = std::any_of(
              role_restores.begin(),
              role_restores.end(),
              [role](const auto &restore) {
                return restore.role == role;
              }
            );
            if (already_queued || current_default_ids[role_index(role)] != assigned_device_id) {
              continue;
            }
            const auto &captured_id = captured_default_device_ids[role_index(role)];
            role_restores.push_back({
              role,
              captured_id == assigned_device_id ? std::wstring {} : captured_id,
              assigned_device_id,
            });
          }
        }
        if (!role_restores.empty()) {
          start_pending_role_restore_task(
            inherited_handoff.steam_device_id.empty() ?
              assigned_device_id :
              inherited_handoff.steam_device_id,
            std::move(role_restores),
            inherited_handoff.assignment_epoch
          );
        }
        return;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      std::wstring steam_device_id = inherited_handoff.steam_device_id;
      for (const auto &endpoint : catalog.endpoints) {
        if (endpoint.active && endpoint.adapter_name == "Steam Streaming Speakers") {
          steam_device_id = utf_utils::from_utf8(endpoint.id);
          break;
        }
      }
      if (steam_device_ids.empty()) {
        clear_pending_preferred_restore_for_assignment(
          inherited_handoff.assignment_epoch
        );
        return;
      }
      if (steam_device_id.empty()) {
        steam_device_id = steam_device_ids.front();
      }

      auto current_default_ids = current_default_device_ids();
      pending_role_restores_t role_restores;
      if (contains_device_id(steam_device_ids, assigned_device_id)) {
        for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
          const auto role = static_cast<ERole>(x);
          const auto &current_id = current_default_ids[role_index(role)];
          if (!contains_device_id(steam_device_ids, current_id)) {
            continue;
          }

          const auto &captured_device_id = captured_default_device_ids[role_index(role)];
          role_restores.push_back({
            role,
            contains_device_id(steam_device_ids, captured_device_id) ? std::wstring {} : captured_device_id,
            current_id,
          });
        }
      }

      auto inherited_role_restores = normalize_pending_role_restores(
        std::move(inherited_handoff.role_restores),
        steam_device_ids,
        current_default_ids
      );
      for (auto &inherited_restore : inherited_role_restores) {
        bool already_queued = false;
        for (const auto &role_restore : role_restores) {
          if (role_restore.role == inherited_restore.role) {
            already_queued = true;
            break;
          }
        }
        if (!already_queued) {
          role_restores.push_back(std::move(inherited_restore));
        }
      }

      if (role_restores.empty()) {
        clear_pending_preferred_restore_for_assignment(
          inherited_handoff.assignment_epoch
        );
        return;
      }

      // Audio policy calls can block in the Windows audio service, so keep
      // every role-specific fallback off the session thread.
      start_pending_role_restore_task(
        steam_device_id,
        std::move(role_restores),
        inherited_handoff.assignment_epoch
      );
    }

    void reset_default_device_impl(bool wait_for_device, const std::wstring &preferred_id) {
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      if (steam_device_ids.empty()) {
        return;
      }

      const auto current_default_ids = current_default_device_ids();
      auto assignment_handoff = begin_policy_assignment(current_default_ids);
      const auto assignment_epoch = assignment_handoff.assignment_epoch;

      std::wstring steam_device_id;
      const auto &console_id = current_default_ids[role_index(eConsole)];
      if (contains_device_id(steam_device_ids, console_id)) {
        steam_device_id = console_id;
      } else {
        for (const auto &current_id : current_default_ids) {
          if (contains_device_id(steam_device_ids, current_id)) {
            steam_device_id = current_id;
            break;
          }
        }
      }
      if (steam_device_id.empty()) {
        clear_pending_preferred_restore_for_assignment(assignment_epoch);
        return;
      }

      // Avoid restoring back to Steam speakers if that's somehow what got
      // recorded as the original host sink.
      std::wstring effective_preferred_id = preferred_id;
      if (effective_preferred_id.empty() || contains_device_id(steam_device_ids, effective_preferred_id)) {
        auto pending_preferred_id = pending_preferred_restore_id();
        if (pending_preferred_id) {
          effective_preferred_id = *pending_preferred_id;
        }
      }
      bool try_preferred_restore =
        !effective_preferred_id.empty() &&
        !contains_device_id(steam_device_ids, effective_preferred_id);

      if (try_preferred_restore) {
        if (!remember_pending_preferred_restore_for_assignment(
              effective_preferred_id,
              steam_device_id,
              assignment_epoch)) {
          return;
        }

        auto result = try_restore_preferred(
          effective_preferred_id,
          steam_device_id,
          assignment_epoch
        );
        if (result == reset_result_e::success) {
          return;
        }
        if (result == reset_result_e::fatal) {
          clear_pending_preferred_restore_for_assignment(
            assignment_epoch,
            effective_preferred_id
          );
        } else if (result == reset_result_e::inactive) {
          return;
        } else if (wait_for_device) {
          start_pending_restore_task(
            steam_device_id,
            effective_preferred_id,
            assignment_epoch
          );
          return;
        } else {
          return;
        }
      }

      // Keep policy writes off the session audio thread so a stalled audio
      // service call cannot prevent session teardown from completing.
      if (wait_for_device) {
        start_pending_restore_task(steam_device_id, {}, assignment_epoch);
        return;
      }

      (void) try_reset_from_steam(steam_device_id, assignment_epoch);
    }

    enum class reset_result_e {
      success,       ///< A non-Steam device was set as default
      no_device,     ///< No non-Steam device is available yet (retriable)
      inactive,      ///< A superseded background worker must not write
      fatal,         ///< Unrecoverable failure (do not retry)
    };

    /**
     * @brief Retires the pending preferred-restore marker for whichever owner
     * is running, so the background worker and the session-thread assignment
     * release it under exactly the same ownership rules.
     */
    static void clear_pending_preferred_restore_for_caller(
      const std::wstring &preferred_id,
      std::uint64_t assignment_epoch,
      const std::stop_token *stop_token,
      const pending_restore_token_t &token
    ) {
      if (stop_token) {
        clear_pending_preferred_restore_for_worker(
          preferred_id,
          token,
          assignment_epoch
        );
      } else {
        clear_pending_preferred_restore_for_assignment(
          assignment_epoch,
          preferred_id
        );
      }
    }

    /**
     * @brief Attempts to set a specific device as the default for the roles
     * either Steam render endpoint still owns.
     * Used to restore the user's original default device after a streaming
     * session ends. Verifies the device is currently active before touching the
     * policy so we don't bind to a missing endpoint. Only the roles that are
     * still assigned to the Steam topology are rewritten. A role the user points at
     * another endpoint (commonly a separate default communications headset) is
     * adopted as-is and must never be overwritten with the preferred endpoint.
     * @param preferred_id Endpoint device_id of the device to restore.
     * @param steam_device_id The Steam endpoint that initiated recovery.
     * @return success if every Steam-owned role was restored or released,
     *         no_device if the device isn't active right now, fatal if the
     *         policy call rejected it.
     */
    reset_result_e try_restore_preferred(
      const std::wstring &preferred_id,
      const std::wstring &steam_device_id,
      std::uint64_t assignment_epoch,
      const std::stop_token *stop_token = nullptr,
      const pending_restore_token_t &token = {}
    ) {
      (void) steam_device_id;
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return reset_result_e::no_device;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      const auto current_default_ids = current_default_device_ids();

      // Record which roles are actually on Steam before resolving anything.
      // Every other role belongs to the user and stays untouched by this
      // restore, exactly like the role-aware fallback reset.
      std::vector<ERole> steam_roles;
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        const auto role = static_cast<ERole>(x);
        if (contains_device_id(steam_device_ids, current_default_ids[role_index(role)])) {
          steam_roles.push_back(role);
        }
      }
      if (steam_roles.empty()) {
        // Steam speakers no longer hold any role, so this restore is already
        // satisfied. Retire the marker instead of waiting for the preferred
        // endpoint just to overwrite the newer defaults with it.
        clear_pending_preferred_restore_for_caller(
          preferred_id,
          assignment_epoch,
          stop_token,
          token
        );
        return reset_result_e::success;
      }

      auto match_list = preferred_device_match_list(preferred_id);
      if (!match_list) {
        return reset_result_e::no_device;
      }

      auto matched = find_device_id(*match_list);
      if (!matched) {
        return reset_result_e::no_device;
      }

      const auto &resolved_id = matched->second;
      const auto eligible_preferred = active_non_steam_render_endpoint(catalog, {resolved_id});
      if (!eligible_preferred || *eligible_preferred != resolved_id) {
        return reset_result_e::no_device;
      }

      int failure = 0;
      int restored = 0;
      for (const auto role : steam_roles) {
        // Re-check ownership immediately before writing. The user may have
        // moved this role while the preferred endpoint was re-enumerated, so
        // adopt that newer default instead of replacing it.
        const auto guarded_default_ids = current_default_device_ids();
        if (!contains_device_id(steam_device_ids, guarded_default_ids[role_index(role)])) {
          const bool adopted =
            stop_token ?
              adopt_current_policy_endpoint_for_worker(token, assignment_epoch, role) :
              adopt_current_policy_endpoint_for_assignment(assignment_epoch, role);
          if (!adopted) {
            return reset_result_e::inactive;
          }
          continue;
        }

        std::optional<HRESULT> result;
        if (stop_token) {
          result = set_default_endpoint_for_worker(
            *stop_token,
            token,
            assignment_epoch,
            role,
            resolved_id
          );
        } else {
          result = set_default_endpoint_for_assignment(
            assignment_epoch,
            role,
            resolved_id
          );
        }
        if (!result) {
          return reset_result_e::inactive;
        }
        const auto hr = *result;
        if (FAILED(hr)) {
          BOOST_LOG(warning) << "Couldn't restore preferred audio endpoint for role ["sv << static_cast<int>(role)
                             << "]: 0x"sv << util::hex(hr).to_string_view();
          ++failure;
          continue;
        }
        ++restored;
      }

      if (failure) {
        return reset_result_e::fatal;
      }

      if (restored) {
        if (resolved_id != preferred_id) {
          BOOST_LOG(info) << "Restored original default audio device via re-enumerated endpoint"sv;
        } else {
          BOOST_LOG(info) << "Restored original default audio device"sv;
        }
      }
      clear_pending_preferred_restore_for_caller(
        preferred_id,
        assignment_epoch,
        stop_token,
        token
      );
      return reset_result_e::success;
    }

    /**
     * @brief Moves every Steam-owned default role to an active physical endpoint.
     * Steam's full-duplex driver exposes both "Steam Streaming Speakers" and
     * "Steam Streaming Microphone" as render endpoints. Explicitly selecting a
     * catalog-proven non-Steam endpoint prevents Windows from promoting the
     * microphone half during speaker teardown.
     * @param steam_device_id The Steam endpoint that initiated recovery.
     * @return Result indicating success, retriable failure, or fatal failure.
     */
    reset_result_e try_reset_from_steam(
      const std::wstring &steam_device_id,
      std::uint64_t assignment_epoch,
      const std::stop_token *stop_token = nullptr,
      const pending_restore_token_t &token = {}
    ) {
      if ((stop_token &&
           !pending_restore_worker_can_write(*stop_token, token, assignment_epoch)) ||
          (!stop_token && !policy_assignment_is_current(assignment_epoch))) {
        return reset_result_e::inactive;
      }

      (void) steam_device_id;
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return reset_result_e::no_device;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      if (steam_device_ids.empty()) {
        return reset_result_e::success;
      }

      const auto current_default_ids = current_default_device_ids();
      std::vector<ERole> steam_roles;
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        const auto role = static_cast<ERole>(x);
        if (contains_device_id(steam_device_ids, current_default_ids[role_index(role)])) {
          steam_roles.push_back(role);
        }
      }
      if (steam_roles.empty()) {
        return reset_result_e::success;
      }

      const bool assignment_active =
        stop_token ?
          pending_restore_worker_can_write(*stop_token, token, assignment_epoch) :
          policy_assignment_is_current(assignment_epoch);
      if (!assignment_active) {
        reassert_current_policy_assignment();
        return reset_result_e::inactive;
      }

      bool no_device = false;
      int failure = 0;
      for (const auto role : steam_roles) {
        std::vector<std::wstring> preferred_ids {
          captured_default_device_ids[role_index(role)],
        };
        if (const auto pending_id = pending_preferred_restore_id()) {
          preferred_ids.push_back(*pending_id);
        }
        const auto fallback_device_id = active_non_steam_render_endpoint(catalog, preferred_ids);
        if (!fallback_device_id) {
          no_device = true;
          continue;
        }

        // Check the live role again immediately before writing. A non-Steam
        // endpoint is a newer user/system choice and must remain untouched.
        const auto guarded_default_ids = current_default_device_ids();
        if (!contains_device_id(steam_device_ids, guarded_default_ids[role_index(role)])) {
          const bool adopted =
            stop_token ?
              adopt_current_policy_endpoint_for_worker(token, assignment_epoch, role) :
              adopt_current_policy_endpoint_for_assignment(assignment_epoch, role);
          if (!adopted) {
            return reset_result_e::inactive;
          }
          continue;
        }

        std::optional<HRESULT> result;
        if (stop_token) {
          result = set_default_endpoint_for_worker(
            *stop_token,
            token,
            assignment_epoch,
            role,
            *fallback_device_id
          );
        } else {
          result = set_default_endpoint_for_assignment(
            assignment_epoch,
            role,
            *fallback_device_id
          );
        }
        if (!result) {
          return reset_result_e::inactive;
        }
        const auto status = *result;
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Couldn't set new default audio endpoint for role ["sv << static_cast<int>(role) << "]: 0x"sv << util::hex(status).to_string_view();
          ++failure;
        }
      }

      if (failure) {
        return reset_result_e::fatal;
      }
      if (no_device) {
        return reset_result_e::no_device;
      }

      BOOST_LOG(info) << "Successfully reset default audio device"sv;
      return reset_result_e::success;
    }

    enum class role_restore_result_e {
      restored,
      no_device,
      no_longer_owned,
      failed,
      inactive,
    };

    static void clear_pending_preferred_restore_for_worker(
      const std::wstring &preferred_id,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (preferred_id.empty()) {
        return;
      }

      std::scoped_lock lock(pending_restore_mutex_ref());
      if (pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        clear_pending_preferred_restore(preferred_id);
      }
    }

    static bool update_pending_role_restore_for_worker(
      const pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      bool publish_expected_endpoint = true
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return false;
      }

      for (auto &published_restore : pending_role_restores_ref()) {
        if (published_restore.role == role_restore.role) {
          published_restore = role_restore;
          if (publish_expected_endpoint) {
            policy_assignment_desired_ids_ref()[role_index(role_restore.role)] =
              role_restore.expected_current_id;
          }
          return true;
        }
      }

      return false;
    }

    static void clear_pending_role_restore_for_worker(
      const pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return;
      }

      auto &published_restores = pending_role_restores_ref();
      for (auto it = published_restores.begin(); it != published_restores.end(); ++it) {
        if (it->role == role_restore.role) {
          published_restores.erase(it);
          break;
        }
      }
      if (published_restores.empty()) {
        pending_restore_steam_device_id_ref().clear();
      }

      if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
        clear_pending_preferred_restore(role_restore.preferred_id);
      }
    }

    static void clear_pending_role_restores_for_worker(
      const pending_role_restores_t &role_restores,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return;
      }

      pending_role_restores_ref().clear();
      pending_restore_steam_device_id_ref().clear();
      for (const auto &role_restore : role_restores) {
        if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
          clear_pending_preferred_restore(role_restore.preferred_id);
        }
      }
    }

    role_restore_result_e try_restore_pending_role(
      const pending_role_restore_t &role_restore,
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return role_restore_result_e::inactive;
      }

      if (!is_default_device(role_restore.expected_current_id, role_restore.role)) {
        if (!adopt_current_policy_endpoint_for_worker(
              token,
              assignment_epoch,
              role_restore.role)) {
          return role_restore_result_e::inactive;
        }
        return role_restore_result_e::no_longer_owned;
      }
      if (role_restore.preferred_id.empty()) {
        return role_restore_result_e::no_device;
      }

      auto match_list = preferred_device_match_list(role_restore.preferred_id);
      if (!match_list) {
        return role_restore_result_e::no_device;
      }

      auto matched = find_device_id(*match_list);
      if (!matched) {
        return role_restore_result_e::no_device;
      }

      // The user may have selected another device while the preferred endpoint
      // was being re-enumerated. Never write over that newer choice.
      if (!is_default_device(role_restore.expected_current_id, role_restore.role)) {
        if (!adopt_current_policy_endpoint_for_worker(
              token,
              assignment_epoch,
              role_restore.role)) {
          return role_restore_result_e::inactive;
        }
        return role_restore_result_e::no_longer_owned;
      }
      auto result = set_default_endpoint_for_worker(
        stop_token,
        token,
        assignment_epoch,
        role_restore.role,
        matched->second
      );
      if (!result) {
        return role_restore_result_e::inactive;
      }
      const auto status = *result;
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Couldn't restore captured audio endpoint for role ["sv
                           << static_cast<int>(role_restore.role) << "]: 0x"sv
                           << util::hex(status).to_string_view();
        return role_restore_result_e::failed;
      }

      if (matched->second != role_restore.preferred_id) {
        BOOST_LOG(info) << "Restored captured audio endpoint via re-enumerated device for role ["sv
                        << static_cast<int>(role_restore.role) << ']';
      } else {
        BOOST_LOG(info) << "Restored captured audio endpoint for role ["sv
                        << static_cast<int>(role_restore.role) << ']';
      }
      return role_restore_result_e::restored;
    }

    reset_result_e try_reset_pending_roles_from_steam(
      const std::wstring &steam_device_id,
      pending_role_restores_t &role_restores,
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return reset_result_e::inactive;
      }

      (void) steam_device_id;
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return reset_result_e::no_device;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      if (steam_device_ids.empty()) {
        return reset_result_e::success;
      }

      std::vector<std::size_t> steam_role_indexes;
      for (std::size_t i = 0; i < role_restores.size(); ++i) {
        const auto &role_restore = role_restores[i];
        const auto live_ids = current_default_device_ids();
        if (contains_device_id(steam_device_ids, role_restore.expected_current_id) &&
            contains_device_id(steam_device_ids, live_ids[role_index(role_restore.role)])) {
          steam_role_indexes.push_back(i);
        }
      }
      if (steam_role_indexes.empty()) {
        return reset_result_e::success;
      }

      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return reset_result_e::inactive;
      }

      // Publish ownership of the explicit fallback transition before policy
      // writes begin. A new stream can then normalize the transferred record
      // against the selected non-Steam endpoint without waiting here.
      for (const auto index : steam_role_indexes) {
        auto &role_restore = role_restores[index];
        role_restore.fallback_transition = true;
        if (!update_pending_role_restore_for_worker(
              role_restore,
              token,
              assignment_epoch,
              false)) {
          return reset_result_e::inactive;
        }
      }

      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        reassert_current_policy_assignment();
        return reset_result_e::inactive;
      }

      bool no_device = false;
      int failure = 0;
      for (const auto index : steam_role_indexes) {
        if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
          return reset_result_e::inactive;
        }

        auto &role_restore = role_restores[index];
        const auto fallback_device_id = active_non_steam_render_endpoint(
          catalog,
          {role_restore.preferred_id}
        );

        if (!fallback_device_id) {
          role_restore.fallback_transition = false;
          if (!update_pending_role_restore_for_worker(
                role_restore,
                token,
                assignment_epoch)) {
            reassert_current_policy_assignment_role(role_restore.role);
            return reset_result_e::inactive;
          }
          no_device = true;
          continue;
        }

        // Check the role again immediately before writing so a user-selected
        // non-Steam endpoint cannot be replaced by our fallback.
        const auto live_ids = current_default_device_ids();
        if (!contains_device_id(steam_device_ids, live_ids[role_index(role_restore.role)])) {
          if (!adopt_current_policy_endpoint_for_worker(
                token,
                assignment_epoch,
                role_restore.role)) {
            return reset_result_e::inactive;
          }
          role_restore.expected_current_id.clear();
          role_restore.fallback_transition = false;
          clear_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch
          );
          continue;
        }
        auto result = set_default_endpoint_for_worker(
          stop_token,
          token,
          assignment_epoch,
          role_restore.role,
          *fallback_device_id
        );
        if (!result) {
          return reset_result_e::inactive;
        }
        const auto status = *result;
        if (FAILED(status)) {
          BOOST_LOG(warning) << "Couldn't set new default audio endpoint for role ["sv
                             << static_cast<int>(role_restore.role) << "]: 0x"sv
                             << util::hex(status).to_string_view();
          role_restore.fallback_transition = false;
          if (!update_pending_role_restore_for_worker(
                role_restore,
                token,
                assignment_epoch)) {
            reassert_current_policy_assignment_role(role_restore.role);
            return reset_result_e::inactive;
          }
          ++failure;
          continue;
        }

        role_restore.expected_current_id = *fallback_device_id;
        role_restore.fallback_transition = false;
        if (!update_pending_role_restore_for_worker(
              role_restore,
              token,
              assignment_epoch)) {
          reassert_current_policy_assignment_role(role_restore.role);
          return reset_result_e::inactive;
        }
      }

      if (failure) {
        BOOST_LOG(warning) << "Keeping "sv << failure
                           << " failed role-specific audio fallback reset(s) queued for retry"sv;
        return reset_result_e::success;
      }
      return no_device ? reset_result_e::no_device : reset_result_e::success;
    }

    void run_pending_role_restore_task(
      std::stop_token stop_token,
      const std::wstring &steam_device_id,
      pending_role_restores_t role_restores,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      device_arrival_notification_t arrival_notifier(steam_device_id);
      HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!cancel_event) {
        BOOST_LOG(warning) << "Failed to create background restore cancellation event"sv;
      }
      auto cancel_event_guard = util::fail_guard([&]() {
        if (cancel_event) {
          CloseHandle(cancel_event);
        }
      });
      std::optional<std::stop_callback<std::function<void()>>> stop_callback;
      if (cancel_event) {
        stop_callback.emplace(stop_token, [cancel_event]() {
          SetEvent(cancel_event);
        });
      }

      auto reg_status = device_enum->RegisterEndpointNotificationCallback(&arrival_notifier);
      const bool have_notifications = SUCCEEDED(reg_status);
      if (!have_notifications) {
        BOOST_LOG(warning) << "Failed to register device arrival notification for background restore: "sv
                           << util::hex(reg_status).to_string_view();
      }
      auto unreg_guard = util::fail_guard([&]() {
        if (have_notifications) {
          device_enum->UnregisterEndpointNotificationCallback(&arrival_notifier);
        }
      });

      BOOST_LOG(info) << "Waiting in background to restore failed audio roles"sv;
      bool retry_fallback_reset = true;
      while (pending_restore_worker_can_write(stop_token, token, assignment_epoch) &&
             !role_restores.empty()) {
        const auto catalog = active_render_endpoint_catalog();
        if (!catalog.complete) {
          arrival_notifier.wait(cancel_event, 1000);
          continue;
        }
        const auto steam_device_ids = steam_render_device_ids(catalog);
        bool needs_fallback = false;
        for (auto it = role_restores.begin(); it != role_restores.end();) {
          const auto result = try_restore_pending_role(
            *it,
            stop_token,
            token,
            assignment_epoch
          );
          if (result == role_restore_result_e::restored || result == role_restore_result_e::no_longer_owned) {
            clear_pending_role_restore_for_worker(
              *it,
              token,
              assignment_epoch
            );
            it = role_restores.erase(it);
            continue;
          }
          if (result == role_restore_result_e::inactive) {
            return;
          }
          if (result == role_restore_result_e::failed) {
            // A direct restore can fail even after the endpoint is visible.
            // While the role is still product-owned Steam, fall back from it
            // instead of leaving that role stuck there.
            if (!contains_device_id(steam_device_ids, it->expected_current_id)) {
              // Keep retrying a captured endpoint while the exact fallback we
              // selected remains active. Only a newer live choice releases it.
              if (is_default_device(it->expected_current_id, it->role)) {
                ++it;
                continue;
              }
              if (!adopt_current_policy_endpoint_for_worker(
                    token,
                    assignment_epoch,
                    it->role)) {
                return;
              }
              clear_pending_role_restore_for_worker(
                *it,
                token,
                assignment_epoch
              );
              it = role_restores.erase(it);
              continue;
            }
            needs_fallback = true;
            ++it;
            continue;
          }

          needs_fallback = needs_fallback ||
                           contains_device_id(steam_device_ids, it->expected_current_id);
          ++it;
        }

        if (needs_fallback && retry_fallback_reset) {
          const auto fallback_result = try_reset_pending_roles_from_steam(
            steam_device_id,
            role_restores,
            stop_token,
            token,
            assignment_epoch
          );
          if (fallback_result == reset_result_e::fatal) {
            clear_pending_role_restores_for_worker(
              role_restores,
              token,
              assignment_epoch
            );
            return;
          }
          if (fallback_result == reset_result_e::inactive) {
            return;
          }
          if (fallback_result == reset_result_e::no_device) {
            // Do not keep hiding and re-enabling Steam every second when no
            // replacement endpoint exists. A device-arrival notification
            // below re-enables this targeted fallback attempt.
            retry_fallback_reset = false;
          }
        }

        // Roles without a captured endpoint need only the immediate fallback.
        // Any role with a captured endpoint stays queued until that endpoint
        // returns, but only while its expected fallback remains selected.
        for (auto it = role_restores.begin(); it != role_restores.end();) {
          if (it->expected_current_id.empty() ||
              (it->preferred_id.empty() &&
               !contains_device_id(steam_device_ids, it->expected_current_id))) {
            clear_pending_role_restore_for_worker(
              *it,
              token,
              assignment_epoch
            );
            it = role_restores.erase(it);
          } else {
            ++it;
          }
        }
        if (role_restores.empty()) {
          return;
        }

        // If notification registration failed, use the timed wait as a polling
        // backoff so a fallback endpoint that appears later is still retried.
        if (arrival_notifier.wait(cancel_event, 1000) || !have_notifications) {
          retry_fallback_reset = true;
        }
      }
    }

  public:

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

      // Capture each role separately because installing the driver may replace
      // only some of the current policy endpoints.
      const auto old_default_ids = current_default_device_ids();

      // Install the Steam Streaming Speakers driver
      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // Restore only roles that Windows moved to either render half of the
        // newly installed Steam topology. Recheck immediately before each
        // write so a concurrent user choice wins.
        const auto catalog = active_render_endpoint_catalog();
        if (catalog.complete) {
          const auto steam_device_ids = steam_render_device_ids(catalog);
          const auto current_ids = current_default_device_ids();
          for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
            const auto role = static_cast<ERole>(x);
            const auto &old_default_id = old_default_ids[role_index(role)];
            if (old_default_id.empty() ||
                contains_device_id(steam_device_ids, old_default_id) ||
                !contains_device_id(steam_device_ids, current_ids[role_index(role)])) {
              continue;
            }

            const auto status = policy->SetDefaultEndpoint(old_default_id.c_str(), role);
            if (FAILED(status)) {
              BOOST_LOG(warning) << "Couldn't restore pre-install audio endpoint for role ["sv
                                 << x << "]: 0x"sv
                                 << util::hex(status).to_string_view();
            }
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

    ~audio_control_t() override {
      // A policy write can fail transiently while the audio topology is being
      // torn down. Keep the transaction owned until the inverse writes have
      // succeeded, and make a few final retries while COM/policy are alive.
      for (int attempt = 0; attempt < 3 && host_mute_prepared_; ++attempt) {
        const bool cleaned = host_mute_active_ ?
          teardown_host_mute_visibility() :
          rollback_host_mute_visibility();
        if (!cleaned) {
          BOOST_LOG(warning) << "Retrying incomplete host-audio mute visibility cleanup"sv;
        }
      }
      if (host_mute_prepared_) {
        BOOST_LOG(error) << "Host-audio mute visibility cleanup remained incomplete at control destruction"sv;
      }
    }

    policy_t policy;
    audio::device_enum_t device_enum;
    role_device_ids_t captured_default_device_ids;
    pending_role_restore_handoff_t pending_role_restore_handoff;
    std::string assigned_sink;
    std::wstring assigned_device_id;
    ::audio::policy::host_mute_visibility_plan_t host_mute_visibility_plan_;
    bool host_mute_requested_ = false;
    bool host_mute_prepared_ = false;
    bool host_mute_active_ = false;
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
    if (config::audio.install_steam_drivers &&
        !control->find_device_id(control->match_steam_speakers(), DEVICE_STATEMASK_ALL)) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
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
      audio_ctrl.reset_default_device_no_wait();
    }

    return co_init;
  }
}  // namespace platf
