/**
 * @file src/aurora_link/audio_default.cpp
 * @brief Save and restore the Windows default playback device around a session.
 *
 * WHY: the host forces its own virtual sink as the default render endpoint for the
 * duration of a stream and does not reliably put the previous one back afterwards --
 * so a session ends with the machine's sound still pointed at a sink nobody is
 * listening to. The session hook snapshots the default on the way in and restores it
 * on the way out.
 *
 * This replaces audio_default.py, the LAST Python dependency in the host stack. That
 * script needed a per-machine Python install plus comtypes/pycaw, which is exactly
 * what a fork cannot ship (see aurora_link.h).
 *
 * THE SNAPSHOT STORES ID **AND** FRIENDLY NAME, AND THE NAME IS NOT REDUNDANT.
 * NVIDIA's HD Audio re-enumerates its endpoints with fresh GUIDs whenever the display
 * topology or HDR mode changes -- and this stack changes display topology on every
 * single session (the VDD comes up, the physical panel is detached, and both revert
 * at the end). So the endpoint id saved 40 minutes ago routinely no longer exists by
 * the time we restore, failing with "Element not found". The friendly-name fallback
 * is the common path here, not a rare one. It matches among ACTIVE render endpoints
 * only, because the GPU leaves NOTPRESENT ghost duplicates of the same name behind.
 *
 * Mechanism is IPolicyConfig::SetDefaultEndpoint, the same undocumented interface the
 * Sound control panel uses and the same one rename.cpp walks. Read the long comment
 * in rename.cpp before touching any of this: the interface is dispatched BY VTABLE
 * SLOT deliberately (an abstract C++ interface class is devirtualised into a crash at
 * -O3), and the GUIDs are spelled out rather than resolved via __uuidof.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// WIN32_LEAN_AND_MEAN drops mmsystem, and the IPolicyConfig vtable needs WAVEFORMATEX
// to sit at the right slots even though nothing here calls those methods.
#include <mmreg.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <cstdio>
#include <string>

#include "aurora_link.h"

namespace aurora_link {

  namespace {

    // {870af99c-171d-4f9e-af0d-e63df40c2bc9}
    const CLSID CLSID_CPolicyConfigClient_ = {
      0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
    // {f8679f50-850a-41cf-9c72-430f290290c8}
    const IID IID_IPolicyConfig_ = {
      0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
    const CLSID CLSID_MMDeviceEnumerator__ = {
      0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    const IID IID_IMMDeviceEnumerator__ = {
      0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};

    /** Slot 13 of IPolicyConfig: SetDefaultEndpoint(PCWSTR deviceId, ERole role). */
    constexpr int policy_slot_set_default_endpoint = 13;
    using policy_set_default_fn = HRESULT(STDMETHODCALLTYPE *)(void *, PCWSTR, int);

    policy_set_default_fn policy_set_default(void *policy) {
      return reinterpret_cast<policy_set_default_fn>(
        (*reinterpret_cast<void ***>(policy))[policy_slot_set_default_endpoint]);
    }

    /**
     * All three roles, always. Windows keeps a separate default for eConsole,
     * eMultimedia and eCommunications; setting only eConsole leaves the machine
     * half-restored in a way that shows up later as "the game has sound but Discord
     * does not", which is a miserable thing to trace back to here.
     */
    constexpr int roles[] = {0, 1, 2};  // eConsole, eMultimedia, eCommunications

    std::wstring endpoint_name(IMMDevice *dev) {
      IPropertyStore *props = nullptr;
      if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)) || props == nullptr) {
        return {};
      }
      PROPVARIANT v;
      PropVariantInit(&v);
      std::wstring out;
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        out = v.pwszVal;
      }
      PropVariantClear(&v);
      props->Release();
      return out;
    }

    /** Find an ACTIVE render endpoint whose friendly name matches exactly. */
    std::wstring id_for_name(IMMDeviceEnumerator *enumerator, const std::wstring &want) {
      IMMDeviceCollection *coll = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || coll == nullptr) {
        return {};
      }
      UINT count = 0;
      coll->GetCount(&count);
      std::wstring found;
      for (UINT i = 0; i < count && found.empty(); ++i) {
        IMMDevice *dev = nullptr;
        if (FAILED(coll->Item(i, &dev)) || dev == nullptr) {
          continue;
        }
        if (endpoint_name(dev) == want) {
          LPWSTR id = nullptr;
          if (SUCCEEDED(dev->GetId(&id)) && id != nullptr) {
            found = id;
            CoTaskMemFree(id);
          }
        }
        dev->Release();
      }
      coll->Release();
      return found;
    }

    FILE *open_utf16(const std::wstring &path, const wchar_t *mode) {
      return _wfopen(path.c_str(), mode);
    }

  }  // namespace

  bool save_default_endpoint(const std::wstring &path) {
    const bool com_here = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator__, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator__, reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
      fprintf(stderr, "aurora-link: device enumerator unavailable (hr=0x%08lx)\n", static_cast<unsigned long>(hr));
      if (com_here) CoUninitialize();
      return false;
    }

    IMMDevice *dev = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (FAILED(hr) || dev == nullptr) {
      // No default render endpoint at all is a legitimate state (every device
      // disabled). Nothing to restore later, so say so and succeed quietly.
      fprintf(stderr, "aurora-link: no default playback device to save (hr=0x%08lx)\n",
              static_cast<unsigned long>(hr));
      enumerator->Release();
      if (com_here) CoUninitialize();
      return false;
    }

    std::wstring id;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(dev->GetId(&raw)) && raw != nullptr) {
      id = raw;
      CoTaskMemFree(raw);
    }
    const std::wstring name = endpoint_name(dev);
    dev->Release();
    enumerator->Release();
    if (com_here) CoUninitialize();

    if (id.empty()) {
      fprintf(stderr, "aurora-link: default playback device has no id\n");
      return false;
    }
    // NEVER SAVE A VIRTUAL SINK AS "THE USER'S SPEAKERS". The host moves the default to its virtual sink (Steam
    // Streaming Speakers) when a stream starts without host audio -- since 2026-10-02 also for Eclipse's
    // this-PC-as-co-op-host -- and this hook can run after that switch. It then "restored" the virtual sink at
    // session end, leaving the PC silent (measured 2026-10-02 on SKYNET). Keep the previous save instead; the host
    // restores the real default itself.
    if (name.find(L"Steam Streaming") != std::wstring::npos) {
      wprintf(L"aurora-link: default is the virtual sink [%ls]; keeping the previously saved device\n", name.c_str());
      return true;
    }

    FILE *f = open_utf16(path, L"w, ccs=UTF-8");
    if (f == nullptr) {
      fprintf(stderr, "aurora-link: cannot write the audio snapshot\n");
      return false;
    }
    fwprintf(f, L"%ls\n%ls\n", id.c_str(), name.c_str());
    fclose(f);
    wprintf(L"aurora-link: saved default playback device [%ls]\n", name.c_str());
    return true;
  }

  bool restore_default_endpoint(const std::wstring &path) {
    FILE *f = open_utf16(path, L"r, ccs=UTF-8");
    if (f == nullptr) {
      // No snapshot => session-start never ran, or this is a first run. Not an error:
      // restoring nothing is the correct outcome, and failing here would make the
      // session-end hook look broken.
      return true;
    }
    wchar_t idbuf[1024] = {0}, namebuf[1024] = {0};
    const bool got_id = fgetws(idbuf, 1024, f) != nullptr;
    const bool got_name = fgetws(namebuf, 1024, f) != nullptr;
    fclose(f);
    if (!got_id) {
      return true;
    }
    std::wstring id(idbuf), name(got_name ? namebuf : L"");
    for (std::wstring *s : {&id, &name}) {
      while (!s->empty() && (s->back() == L'\n' || s->back() == L'\r')) {
        s->pop_back();
      }
    }
    if (id.empty()) {
      return true;
    }

    const bool com_here = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator__, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator__, reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
      fprintf(stderr, "aurora-link: device enumerator unavailable (hr=0x%08lx)\n", static_cast<unsigned long>(hr));
      if (com_here) CoUninitialize();
      return false;
    }

    void *policy = nullptr;
    hr = CoCreateInstance(CLSID_CPolicyConfigClient_, nullptr, CLSCTX_ALL, IID_IPolicyConfig_, &policy);
    if (FAILED(hr) || policy == nullptr) {
      fprintf(stderr, "aurora-link: IPolicyConfig unavailable (hr=0x%08lx), default device left as-is\n",
              static_cast<unsigned long>(hr));
      enumerator->Release();
      if (com_here) CoUninitialize();
      return false;
    }
    auto set_default = policy_set_default(policy);

    // Try the exact id first, then fall back to the name. The id is preferred because
    // two endpoints can share a friendly name; the fallback exists because the id
    // routinely expires across the display changes this stack makes every session.
    bool ok = true;
    for (const int role : roles) {
      if (FAILED(set_default(policy, id.c_str(), role))) {
        ok = false;
        break;
      }
    }

    if (!ok && !name.empty()) {
      const std::wstring live = id_for_name(enumerator, name);
      if (!live.empty()) {
        ok = true;
        for (const int role : roles) {
          if (FAILED(set_default(policy, live.c_str(), role))) {
            ok = false;
            break;
          }
        }
        if (ok) {
          wprintf(L"aurora-link: restored default playback device by name [%ls] "
                  L"(the saved id had expired)\n", name.c_str());
        }
      }
    } else if (ok) {
      wprintf(L"aurora-link: restored default playback device [%ls]\n", name.c_str());
    }

    if (!ok) {
      fwprintf(stderr, L"aurora-link: could not restore the default playback device [%ls]\n", name.c_str());
    }

    reinterpret_cast<IUnknown *>(policy)->Release();
    enumerator->Release();
    if (com_here) CoUninitialize();
    return ok;
  }

}  // namespace aurora_link
