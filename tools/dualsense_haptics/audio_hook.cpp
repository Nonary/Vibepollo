// SPDX-License-Identifier: GPL-3.0-or-later
#include "inject.h"
#include "attach.h"
#include "ipc.h"
#include "pcm.h"

#include <atomic>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <MinHook.h>
#include <mmdeviceapi.h>
#include <mutex>
#include <memory>
#include <propvarutil.h>
#include <string>
#include <shellapi.h>
#include <vector>

namespace {
  using namespace dualsense_haptics;
  constexpr GUID float_subtype {3, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
  constexpr GUID pcm_subtype {1, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
  shared_state *state = nullptr;
  std::wstring hook_path;
  std::wstring mapping_name;
  std::atomic<bool> initialized_hook {};
  struct launcher_session {
    std::wstring mapping_name;
    std::wstring directory;
    shared_state *state {};
    ~launcher_session() {
      if (state) {
        UnmapViewOfFile(state);
      }
    }
  };
  std::mutex launcher_mutex;
  std::shared_ptr<launcher_session> launcher_state;
  std::atomic<bool> launcher_mode {};
  std::mutex initialization_mutex;
  bool process_hooks_installed = false;
  const PROPERTYKEY container_key {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

  bool active(unsigned index) {
    return state && state->alive && index < slot_count && state->slots[index].active;
  }

  std::wstring endpoint_id(unsigned index) {
    return L"Vibepollo.DualSense.Audio." + std::to_wstring(index);
  }

  HRESULT copy_string(const std::wstring &s, wchar_t **out) {
    if (!out) {
      return E_POINTER;
    }
    *out = static_cast<wchar_t *>(CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t)));
    if (!*out) {
      return E_OUTOFMEMORY;
    }
    memcpy(*out, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
    return S_OK;
  }

  template<class Interface>
  class ref_counted: public Interface {
  public:
    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++refs;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      const auto count = --refs;
      if (!count) {
        delete this;
      }
      return count;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (iid != IID_IUnknown && iid != __uuidof(Interface)) {
        return E_NOINTERFACE;
      }
      *out = static_cast<Interface *>(this);
      AddRef();
      return S_OK;
    }

  protected:
    virtual ~ref_counted() = default;

  private:
    std::atomic<ULONG> refs {1};
  };

  class audio_client final: public IAudioClient, public IAudioRenderClient, public IAudioClock {
  public:
    explicit audio_client(unsigned index):
        index(index),
        generation(state->slots[index].generation) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (iid == IID_IUnknown || iid == __uuidof(IAudioClient)) {
        *out = static_cast<IAudioClient *>(this);
      } else if (iid == __uuidof(IAudioRenderClient)) {
        *out = static_cast<IAudioRenderClient *>(this);
      } else if (iid == __uuidof(IAudioClock)) {
        *out = static_cast<IAudioClock *>(this);
      } else {
        return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++refs;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      const auto count = --refs;
      if (!count) {
        delete this;
      }
      return count;
    }

    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME periodicity, const WAVEFORMATEX *format, LPCGUID) override {
      std::lock_guard lock(mutex);
      if (initialized) {
        return AUDCLNT_E_ALREADY_INITIALIZED;
      }
      if (!connected()) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      if (!supported(format)) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
      }
      if (mode != AUDCLNT_SHAREMODE_SHARED || periodicity || (flags & AUDCLNT_STREAMFLAGS_LOOPBACK)) {
        return E_INVALIDARG;
      }
      rate = format->nSamplesPerSec;
      floating = format->wBitsPerSample == 32;
      event_mode = flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
      capacity = static_cast<UINT32>(std::clamp<REFERENCE_TIME>(duration, 200000, 1000000) * rate / 10000000);
      buffer.resize(capacity * format->nBlockAlign);
      initialized = true;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32 *out) override {
      return get_initialized(out, capacity);
    }

    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME *out) override {
      return get_initialized(out, static_cast<REFERENCE_TIME>(capacity) * 10000000 / rate);
    }

    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32 *out) override {
      std::lock_guard lock(mutex);
      if (!out) {
        return E_POINTER;
      }
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (!connected()) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      advance();
      *out = static_cast<UINT32>(submitted - consumed);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *format, WAVEFORMATEX **closest) override {
      if (closest) {
        *closest = nullptr;
      }
      if (mode != AUDCLNT_SHAREMODE_SHARED) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
      }
      return supported(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      auto result = static_cast<WAVEFORMATEXTENSIBLE *>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
      if (!result) {
        return E_OUTOFMEMORY;
      }
      *result = {};
      result->Format = {WAVE_FORMAT_EXTENSIBLE, 4, 48000, 48000 * 16, 16, 32, 22};
      result->Samples.wValidBitsPerSample = 32;
      result->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
      result->SubFormat = float_subtype;
      *out = &result->Format;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME *normal, REFERENCE_TIME *minimum) override {
      if (!normal && !minimum) {
        return E_POINTER;
      }
      if (normal) {
        *normal = 50000;
      }
      if (minimum) {
        *minimum = 50000;
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Start() override {
      std::lock_guard lock(mutex);
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (!connected()) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      if (running) {
        return AUDCLNT_E_NOT_STOPPED;
      }
      if (event_mode && !event) {
        return AUDCLNT_E_EVENTHANDLE_NOT_SET;
      }
      if (event_mode && !timer && !CreateTimerQueueTimer(&timer, nullptr, [](void *ctx, BOOLEAN) {
            auto self = static_cast<audio_client *>(ctx);
            if (self->running && self->event) {
              SetEvent(self->event);
            }
          },
                                                         this,
                                                         0,
                                                         5,
                                                         WT_EXECUTEDEFAULT)) {
        return E_FAIL;
      }
      last_tick = GetTickCount64();
      running = true;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override {
      std::lock_guard lock(mutex);
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      advance();
      running = false;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
      std::lock_guard lock(mutex);
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (running) {
        return AUDCLNT_E_NOT_STOPPED;
      }
      if (outstanding) {
        return AUDCLNT_E_BUFFER_OPERATION_PENDING;
      }
      submitted = consumed = 0;
      converter.reset();
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE handle) override {
      std::lock_guard lock(mutex);
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (!event_mode) {
        return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
      }
      if (!handle || event) {
        return E_INVALIDARG;
      }
      if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &event, EVENT_MODIFY_STATE, FALSE, 0)) {
        return E_INVALIDARG;
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void **out) override {
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (iid != __uuidof(IAudioRenderClient) && iid != __uuidof(IAudioClock)) {
        if (out) {
          *out = nullptr;
        }
        return E_NOINTERFACE;
      }
      return QueryInterface(iid, out);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE **out) override {
      std::lock_guard lock(mutex);
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (!connected()) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      if (outstanding) {
        return AUDCLNT_E_OUT_OF_ORDER;
      }
      advance();
      if (frames > capacity - (submitted - consumed)) {
        return AUDCLNT_E_BUFFER_TOO_LARGE;
      }
      if (!frames) {
        return S_OK;
      }
      outstanding = frames;
      *out = buffer.data();
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override {
      std::lock_guard lock(mutex);
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      if (!outstanding) {
        return frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
      }
      if (frames > outstanding) {
        return AUDCLNT_E_INVALID_SIZE;
      }
      if (flags & ~AUDCLNT_BUFFERFLAGS_SILENT) {
        return E_INVALIDARG;
      }
      outstanding = 0;
      if (!connected()) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      advance();
      submitted += frames;
      converter.push(buffer.data(), frames, rate, floating, flags & AUDCLNT_BUFFERFLAGS_SILENT, [this](std::uint32_t, const auto &samples) {
        auto &s = state->slots[index];
        const auto sequence = static_cast<std::uint32_t>(InterlockedIncrement(&s.sequence)) - 1;
        // Never block an audio callback behind IPC or a stalled host.
        if (!try_lock(s)) {
          return;
        }
        if (s.write - s.read < queue_size) {
          s.packets[s.write % queue_size] = {sequence, GetTickCount(), generation, samples};
          ++s.write;
        }
        unlock(s);
      });
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64 *out) override {
      return get_initialized(out, static_cast<UINT64>(rate));
    }

    HRESULT STDMETHODCALLTYPE GetPosition(UINT64 *position, UINT64 *qpc) override {
      std::lock_guard lock(mutex);
      if (!position) {
        return E_POINTER;
      }
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      advance();
      *position = consumed;
      if (qpc) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        *qpc = static_cast<UINT64>(now.QuadPart * (10000000.0 / frequency.QuadPart));
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD *out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
      return S_OK;
    }

  private:
    ~audio_client() {
      running = false;
      if (timer) {
        DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
      }
      if (event) {
        CloseHandle(event);
      }
    }

    bool connected() const {
      return active(index) && state->slots[index].generation == generation;
    }

    static bool supported(const WAVEFORMATEX *f) {
      if (!f || f->nChannels != 4 || f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 192000) {
        return false;
      }
      WORD tag = f->wFormatTag;
      if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (f->cbSize < 22) {
          return false;
        }
        auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
        if (ext->Samples.wValidBitsPerSample != f->wBitsPerSample) {
          return false;
        }
        if (ext->SubFormat == float_subtype) {
          tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (ext->SubFormat == pcm_subtype) {
          tag = WAVE_FORMAT_PCM;
        } else {
          return false;
        }
      }
      if (!((tag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32) || (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16))) {
        return false;
      }
      return f->nBlockAlign == 4 * (f->wBitsPerSample / 8) && f->nAvgBytesPerSec == f->nSamplesPerSec * f->nBlockAlign;
    }

    template<class T>
    HRESULT get_initialized(T *out, T value) {
      if (!out) {
        return E_POINTER;
      }
      if (!initialized) {
        return AUDCLNT_E_NOT_INITIALIZED;
      }
      *out = value;
      return S_OK;
    }

    void advance() {
      if (!running) {
        return;
      }
      const auto tick = GetTickCount64();
      const auto elapsed = (tick - last_tick) * rate / 1000;
      if (elapsed) {
        consumed = std::min<UINT64>(submitted, consumed + elapsed);
        last_tick = tick;
      }
    }

    std::atomic<ULONG> refs {1};
    std::mutex mutex;
    unsigned index;
    LONG generation;
    unsigned rate {48000};
    UINT32 capacity {};
    UINT32 outstanding {};
    bool initialized {};
    bool floating {};
    bool event_mode {};
    std::atomic<bool> running {};
    HANDLE event {};
    HANDLE timer {};
    UINT64 submitted {}, consumed {}, last_tick {};
    std::vector<BYTE> buffer;
    packetizer converter;
  };

  class properties final: public ref_counted<IPropertyStore> {
  public:
    explicit properties(unsigned index):
        index(index) {}

    HRESULT STDMETHODCALLTYPE GetCount(DWORD *out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = 2;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAt(DWORD n, PROPERTYKEY *out) override {
      if (!out) {
        return E_POINTER;
      }
      if (n > 1) {
        return E_INVALIDARG;
      }
      *out = n ? container_key : PKEY_Device_FriendlyName;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT *out) override {
      if (!out) {
        return E_POINTER;
      }
      PropVariantInit(out);
      std::wstring value;
      if (IsEqualPropertyKey(key, container_key)) {
        wchar_t guid[40] {};
        const auto container = container_id(index);
        StringFromGUID2(container, guid, 40);
        value = guid;
      } else if (IsEqualPropertyKey(key, PKEY_Device_FriendlyName)) {
        value = L"Wireless Controller";
      } else {
        return S_OK;
      }
      const auto hr = copy_string(value, &out->pwszVal);
      if (SUCCEEDED(hr)) {
        out->vt = VT_LPWSTR;
      }
      return hr;
    }

    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
      return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Commit() override {
      return STG_E_ACCESSDENIED;
    }

  private:
    unsigned index;
  };

  class device final: public IMMDevice, public IMMEndpoint {
  public:
    explicit device(unsigned index):
        index(index) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (iid == IID_IUnknown || iid == __uuidof(IMMDevice)) {
        *out = static_cast<IMMDevice *>(this);
      } else if (iid == __uuidof(IMMEndpoint)) {
        *out = static_cast<IMMEndpoint *>(this);
      } else {
        return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++refs;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      const auto n = --refs;
      if (!n) {
        delete this;
      }
      return n;
    }

    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT *, void **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (!active(index)) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
      }
      if (iid != __uuidof(IAudioClient)) {
        return E_NOINTERFACE;
      }
      *out = static_cast<IAudioClient *>(new audio_client(index));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD mode, IPropertyStore **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (mode != STGM_READ) {
        return STG_E_ACCESSDENIED;
      }
      *out = new properties(index);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *out) override {
      return copy_string(endpoint_id(index), out);
    }

    HRESULT STDMETHODCALLTYPE GetState(DWORD *out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = active(index) ? DEVICE_STATE_ACTIVE : DEVICE_STATE_NOTPRESENT;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow *out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = eRender;
      return S_OK;
    }

  private:
    unsigned index;
    std::atomic<ULONG> refs {1};
  };

  class collection final: public ref_counted<IMMDeviceCollection> {
  public:
    collection(IMMDeviceCollection *real, bool render):
        real(real) {
      real->GetCount(&real_count);
      if (render) {
        for (unsigned i = 0; i < slot_count; ++i) {
          if (active(i)) {
            slots.push_back(i);
          }
        }
      }
    }

    HRESULT STDMETHODCALLTYPE GetCount(UINT *out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = real_count + slots.size();
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(UINT n, IMMDevice **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      if (n < real_count) {
        return real->Item(n, out);
      }
      if (n - real_count >= slots.size()) {
        return E_INVALIDARG;
      }
      *out = new device(slots[n - real_count]);
      return S_OK;
    }

  private:
    ~collection() override {
      real->Release();
    }

    IMMDeviceCollection *real;
    UINT real_count {};
    std::vector<unsigned> slots;
  };

  class enumerator final: public ref_counted<IMMDeviceEnumerator> {
  public:
    explicit enumerator(IMMDeviceEnumerator *real):
        real(real) {}

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection **out) override {
      if (!out) {
        return E_POINTER;
      }
      *out = nullptr;
      IMMDeviceCollection *devices = nullptr;
      const auto hr = real->EnumAudioEndpoints(flow, mask, &devices);
      if (FAILED(hr)) {
        return hr;
      }
      *out = new collection(devices, (flow == eRender || flow == eAll) && (mask & DEVICE_STATE_ACTIVE));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice **out) override {
      return real->GetDefaultAudioEndpoint(flow, role, out);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice **out) override {
      if (!id || !out) {
        return E_POINTER;
      }
      for (unsigned i = 0; i < slot_count; ++i) {
        if (active(i) && endpoint_id(i) == id) {
          *out = new device(i);
          return S_OK;
        }
      }
      return real->GetDevice(id, out);
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *client) override {
      return real->RegisterEndpointNotificationCallback(client);
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *client) override {
      return real->UnregisterEndpointNotificationCallback(client);
    }

  private:
    ~enumerator() override {
      real->Release();
    }

    IMMDeviceEnumerator *real;
  };

  decltype(&CoCreateInstance) real_create = nullptr;

  HRESULT WINAPI create(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *out) {
    auto hr = real_create(clsid, outer, context, iid, out);
    if (SUCCEEDED(hr) && !outer && clsid == __uuidof(MMDeviceEnumerator) && iid == __uuidof(IMMDeviceEnumerator) && out && *out) {
      *out = static_cast<IMMDeviceEnumerator *>(new enumerator(static_cast<IMMDeviceEnumerator *>(*out)));
    }
    return hr;
  }

  thread_local bool creating_process = false;
  decltype(&CreateProcessW) real_process_w = nullptr;
  decltype(&CreateProcessA) real_process_a = nullptr;

  bool finish_child(BOOL created, DWORD flags, PROCESS_INFORMATION *child) {
    if (!created) {
      return false;
    }
    std::shared_ptr<launcher_session> session;
    std::wstring child_mapping = mapping_name;
    bool should_inject = state && state->alive;
    if (launcher_mode) {
      {
        std::lock_guard lock(launcher_mutex);
        session = launcher_state;
      }
      wchar_t image[32768] {};
      DWORD length = 32768;
      should_inject = session && session->state->alive &&
                      QueryFullProcessImageNameW(child->hProcess, 0, image, &length) &&
                      under_directory(image, session->directory);
      if (should_inject) {
        child_mapping = session->mapping_name;
      }
    }
    if (!should_inject) {
      if (!(flags & CREATE_SUSPENDED)) {
        ResumeThread(child->hThread);
      }
      return true;
    }
    if (!inject(*child, hook_path, child_mapping, flags & CREATE_SUSPENDED)) {
      TerminateProcess(child->hProcess, 125);
      WaitForSingleObject(child->hProcess, 10000);
      CloseHandle(child->hThread);
      CloseHandle(child->hProcess);
      *child = {};
      SetLastError(ERROR_DLL_INIT_FAILED);
      return false;
    }
    return true;
  }

  BOOL WINAPI process_w(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    if (creating_process) {
      return real_process_w(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }
    if (flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS)) {
      if (launcher_mode) {
        return real_process_w(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
      }
      SetLastError(ERROR_NOT_SUPPORTED);
      return FALSE;
    }
    creating_process = true;
    const auto created = real_process_w(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, cwd, si, pi);
    creating_process = false;
    return finish_child(created, flags, pi);
  }

  BOOL WINAPI process_a(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    if (creating_process) {
      return real_process_a(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }
    if (flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS)) {
      if (launcher_mode) {
        return real_process_a(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
      }
      SetLastError(ERROR_NOT_SUPPORTED);
      return FALSE;
    }
    creating_process = true;
    const auto created = real_process_a(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, cwd, si, pi);
    creating_process = false;
    return finish_child(created, flags, pi);
  }

  bool prepare_process_hooks() {
    if (process_hooks_installed) {
      return true;
    }
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
      return false;
    }
    if (!real_process_w && MH_CreateHookApi(L"kernel32", "CreateProcessW", reinterpret_cast<void *>(&process_w), reinterpret_cast<void **>(&real_process_w)) != MH_OK) {
      return false;
    }
    if (!real_process_a && MH_CreateHookApi(L"kernel32", "CreateProcessA", reinterpret_cast<void *>(&process_a), reinterpret_cast<void **>(&real_process_a)) != MH_OK) {
      return false;
    }
    const auto unicode = MH_EnableHook(reinterpret_cast<void *>(&CreateProcessW));
    const auto ansi = MH_EnableHook(reinterpret_cast<void *>(&CreateProcessA));
    process_hooks_installed = (unicode == MH_OK || unicode == MH_ERROR_ENABLED) &&
                              (ansi == MH_OK || ansi == MH_ERROR_ENABLED);
    return process_hooks_installed;
  }

  bool resolve_hook_path() {
    if (!hook_path.empty()) {
      return true;
    }
    HMODULE module = nullptr;
    wchar_t path[32768] {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&resolve_hook_path), &module)) {
      return false;
    }
    const auto length = GetModuleFileNameW(module, path, 32768);
    if (!length || length >= 32768) {
      return false;
    }
    hook_path = path;
    return true;
  }
}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI VibepolloHapticsInitializeLauncher(void *argument) {
  std::lock_guard initialize_lock(initialization_mutex);
  if (!argument || state) {
    return 0;
  }
  const auto &config = *static_cast<const dualsense_haptics::launcher_config *>(argument);
  if (!config.mapping_name[0] || config.mapping_name[127] || !config.directory[0] || config.directory[32767]) {
    return 0;
  }
  auto session = std::make_shared<launcher_session>();
  session->mapping_name = config.mapping_name;
  wchar_t directory[32768] {};
  const auto length = GetFullPathNameW(config.directory, 32768, directory, nullptr);
  if (!length || length >= 32768) {
    return 0;
  }
  session->directory.assign(directory, length);
  while (!session->directory.empty() && (session->directory.back() == L'\\' || session->directory.back() == L'/')) {
    session->directory.pop_back();
  }
  // Never arm a whole drive or the Windows installation as a game scope.
  wchar_t windows[32768] {};
  if (session->directory.size() <= 3 || !GetWindowsDirectoryW(windows, 32768) ||
      _wcsicmp(session->directory.c_str(), windows) == 0 || under_directory(session->directory, windows)) {
    return 0;
  }
  const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, config.mapping_name);
  if (!mapping) {
    return 0;
  }
  session->state = static_cast<shared_state *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(shared_state)));
  CloseHandle(mapping);
  if (!session->state || session->state->signature != magic || !session->state->alive ||
      !resolve_hook_path()) {
    return 0;
  }
  {
    std::lock_guard lock(launcher_mutex);
    launcher_state = std::move(session);
  }
  launcher_mode = true;
  return prepare_process_hooks() ? 1 : 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI VibepolloHapticsPreparePlaynite(const wchar_t *mapping, const wchar_t *directory, BOOL steam) {
  if (!mapping || !directory || wcslen(mapping) >= 128 || wcslen(directory) >= 32768) {
    return 0;
  }
  dualsense_haptics::launcher_config config;
  wcscpy(config.mapping_name, mapping);
  wcscpy(config.directory, directory);
  if (!VibepolloHapticsInitializeLauncher(&config)) {
    return 0;
  }
  if (!steam) {
    return 1;
  }
  const auto deadline = GetTickCount64() + 10000;
  bool requested_start = false;
  do {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return 0;
    }
    PROCESSENTRY32W process {sizeof(process)};
    bool found = false, ok = true;
    if (Process32FirstW(snapshot, &process)) {
      do {
        if (_wcsicmp(process.szExeFile, L"steam.exe") == 0) {
          DWORD session = 0, ours = 0;
          if (ProcessIdToSessionId(process.th32ProcessID, &session) &&
              ProcessIdToSessionId(GetCurrentProcessId(), &ours) && session == ours) {
            found = true;
            ok = dualsense_haptics::attach_launcher(process.th32ProcessID, hook_path, config) && ok;
          }
        }
      } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    if (found) {
      if (ok) {
        return 1;
      }
      if (!requested_start) {
        return 0;
      }
    }
    if (!requested_start) {
      if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"steam://open/main", nullptr, nullptr, SW_HIDE)) <= 32) {
        return 0;
      }
      requested_start = true;
    }
    Sleep(100);
  } while (GetTickCount64() < deadline);
  return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI VibepolloHapticsInitialize(void *argument) {
  std::lock_guard initialize_lock(initialization_mutex);
  if (launcher_mode) {
    return 0;
  }
  if (state) {
    return initialized_hook ? 1 : 0;
  }
  wchar_t name[128] {};
  if (argument) {
    const auto config = static_cast<const wchar_t *>(argument);
    if (wcslen(config) >= 128) {
      return 0;
    }
    wcscpy(name, config);
  } else if (!GetEnvironmentVariableW(dualsense_haptics::environment_key, name, 128)) {
    return 0;
  }
  mapping_name = name;
  const auto mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (!mapping) {
    return 0;
  }
  state = static_cast<dualsense_haptics::shared_state *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dualsense_haptics::shared_state)));
  CloseHandle(mapping);
  if (!state || state->signature != dualsense_haptics::magic || !state->alive) {
    return 0;
  }
  if (!resolve_hook_path() || !prepare_process_hooks()) {
    return 0;
  }
  if (MH_CreateHookApi(L"ole32", "CoCreateInstance", reinterpret_cast<void *>(&create), reinterpret_cast<void **>(&real_create)) != MH_OK) {
    return 0;
  }
  initialized_hook = MH_EnableHook(reinterpret_cast<void *>(&CoCreateInstance)) == MH_OK;
  return initialized_hook ? 1 : 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI VibepolloHapticsBootstrap(void *argument) {
  auto config = static_cast<dualsense_haptics::bootstrap_config *>(argument);
  if (!config || !VibepolloHapticsInitialize(config->mapping_name)) {
    return 0;
  }
  auto entry = reinterpret_cast<void *>(config->entry);
  DWORD protection = 0;
  if (!VirtualProtect(entry, config->original.size(), PAGE_EXECUTE_READWRITE, &protection)) {
    return 0;
  }
  memcpy(entry, config->original.data(), config->original.size());
  DWORD ignored = 0;
  if (!VirtualProtect(entry, config->original.size(), protection, &ignored) || !FlushInstructionCache(GetCurrentProcess(), entry, config->original.size())) {
    return 0;
  }
  InterlockedExchange(&config->ready, 1);
  return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
