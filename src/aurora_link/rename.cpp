/**
 * @file src/aurora_link/rename.cpp
 * @brief Rename audio endpoints the way the Sound control panel does.
 *
 * WHY NOT THE REGISTRY
 * --------------------
 * Endpoint names live under HKLM\...\MMDevices\Audio\{Render,Capture}\{guid}\Properties,
 * but those keys deny writes to Administrators AND to SYSTEM. Only the audio service
 * writes them -- which is exactly why the Sound control panel can rename a device and
 * a script cannot. Taking ownership works, but it permanently loosens ACLs on a system
 * key and needs a manual, per-machine step. For something meant to ship in a fork,
 * "the installer quietly re-ACLs a system key" is not acceptable, and "rename it by
 * hand after install" is worse.
 *
 * WHAT THE CONTROL PANEL ACTUALLY DOES
 * ------------------------------------
 * It calls IPolicyConfig, an undocumented but long-stable COM interface implemented by
 * the audio service, which performs the write on our behalf with its own rights. That
 * makes the rename work from an elevated process, on any machine, at install time --
 * no ACL changes, no manual step. It must be ELEVATED: unprivileged callers get
 * E_ACCESSDENIED from the write while the reads still succeed.
 *
 * AND IT WRITES DeviceDesc, NOT FriendlyName -- see kDeviceDesc below. Writing
 * FriendlyName is refused for EVERY identity (measured: elevated administrator and
 * nt authority\system both get E_ACCESSDENIED), because FriendlyName is a composed,
 * derived value. That looks exactly like a permissions wall and is not one.
 *
 * CAVEAT, STATED PLAINLY: IPolicyConfig is undocumented. The GUIDs and vtable slots
 * below are the well-known Win10/11 layout. If a future Windows moves it, the layout
 * check at the call site refuses to write and renaming simply stops happening -- a
 * failure here is logged and ignored rather than being allowed to fail a session.
 * Nothing else depends on it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// WIN32_LEAN_AND_MEAN drops mmsystem, and the IPolicyConfig vtable below needs
// WAVEFORMATEX to sit at the right slots.
#include <mmreg.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <cstdio>
#include <string>
#include <vector>

#include "aurora_link.h"

namespace aurora_link {

  namespace {

    // {870af99c-171d-4f9e-af0d-e63df40c2bc9}
    const CLSID CLSID_CPolicyConfigClient = {
      0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
    // {f8679f50-850a-41cf-9c72-430f290290c8}
    const IID IID_IPolicyConfig = {
      0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

    // Declared explicitly rather than via __uuidof: GCC/MinGW's __uuidof support is
    // limited compared to MSVC, and when it fails to resolve it yields garbage that
    // CoCreateInstance happily accepts -- the first virtual call then faults. That was
    // an 0xC0000005 with no output, which is a miserable thing to debug from a crash
    // code alone. Explicit constants cannot silently degrade.
    const CLSID CLSID_MMDeviceEnumerator_ = {
      0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    const IID IID_IMMDeviceEnumerator_ = {
      0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};

    /**
     * Step tracing, on stderr because stderr is unbuffered: when this faults, any
     * stdout written first is still sitting in the CRT's block buffer and dies with
     * the process, which is how the first crash gave literally no output at all.
     * Kept rather than deleted -- this walks an UNDOCUMENTED vtable, so "which call
     * did it die on" is the only question worth asking when a Windows update moves it.
     */
    void trace(const char *what) {
      static const bool on = GetEnvironmentVariableA("AURORA_LINK_TRACE", nullptr, 0) != 0;
      if (on) {
        fprintf(stderr, "aurora-link/rename: %s\n", what);
        fflush(stderr);
      }
    }

    /**
     * IPolicyConfig's vtable. Dispatched BY SLOT, deliberately -- see below.
     *
     *   0-2  IUnknown
     *   3    GetMixFormat          (PCWSTR, WAVEFORMATEX **)
     *   4    GetDeviceFormat       (PCWSTR, INT, WAVEFORMATEX **)
     *   5    ResetDeviceFormat     (PCWSTR)
     *   6    SetDeviceFormat       (PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *)
     *   7    GetProcessingPeriod   (PCWSTR, INT, PINT64, PINT64)
     *   8    SetProcessingPeriod   (PCWSTR, PINT64)
     *   9    GetShareMode          (PCWSTR, DeviceShareMode *)
     *   10   SetShareMode          (PCWSTR, DeviceShareMode *)
     *   11   GetPropertyValue      (PCWSTR, BOOL, const PROPERTYKEY &, PROPVARIANT *)
     *   12   SetPropertyValue      (PCWSTR, BOOL, const PROPERTYKEY &, PROPVARIANT *)
     *   13   SetDefaultEndpoint    (PCWSTR, ERole)
     *   14   SetEndpointVisualName (PCWSTR, PCWSTR)
     *
     * WHY NOT AN ABSTRACT C++ INTERFACE CLASS, WHICH IS HOW EVERY EXAMPLE WRITES IT:
     * because at -O3 GCC miscompiles that into a crash. A local (internal-linkage)
     * class whose methods are all pure virtual lets GCC conclude it has seen the
     * whole hierarchy; since no visible class derives from it, it devirtualises the
     * call to __cxa_pure_virtual and we die at the first invocation. That assumption
     * is simply false for COM: the implementation is inside AudioSes.dll. It is
     * invisible at -O0 and appears at -O2/-O3, which makes it look like a data bug
     * rather than a codegen one. `-fno-devirtualize` also cures it, but a build flag
     * is easy to lose in a later CMake edit and MSVC would not need it; dispatching
     * by slot cannot regress. For an undocumented interface the slot index IS the
     * contract anyway, so writing it out is honest rather than obscure.
     *
     * NOTE THE `BOOL bFxStore` SECOND PARAMETER on the two property methods. The
     * widely-copied Vista-era declaration omits it, and omitting it is not a graceful
     * failure: the callee still reads a fourth argument, which then comes off
     * uninitialised stack and is dereferenced. Confirmed by disassembling
     * AudioSes.dll on Windows 11 26200 -- both slots store their third argument as a
     * 32-bit value (`mov %r8d,0x18(...)`, an int, not the PROPERTYKEY pointer) and
     * read a fourth from the stack, whereas the neighbouring GetMixFormat uses its
     * third as a 64-bit pointer. bFxStore selects the effects store; FALSE is the
     * device store, which is where the friendly name lives.
     */
    enum : int {
      policy_slot_get_property = 11,
      policy_slot_set_property = 12,
    };

    /**
     * WRITE DeviceDesc, NOT FriendlyName.
     *
     * FriendlyName is COMPOSED by the audio stack as "<DeviceDesc> (<adapter>)" --
     * it is a derived value, and writing it is refused with E_ACCESSDENIED for every
     * identity, including an elevated administrator AND nt authority\system (both
     * measured on Windows 11 26200, via a scheduled task for the SYSTEM case). That
     * is not a permissions problem to be escalated around; it is the wrong property.
     * DeviceDesc is the part the Sound control panel actually edits when you rename a
     * device, which is why the control panel appears to succeed where a script fails.
     *
     * Same fmtid as PKEY_Device_FriendlyName, pid 2 rather than 14. Spelled out for
     * the same reason as the CLSIDs above: no reliance on which import library
     * happens to define it.
     */
    const PROPERTYKEY kDeviceDesc = {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

    using policy_property_fn = HRESULT(STDMETHODCALLTYPE *)(void *, PCWSTR, BOOL, const PROPERTYKEY &, PROPVARIANT *);

    policy_property_fn policy_method(void *policy, int slot) {
      return reinterpret_cast<policy_property_fn>((*reinterpret_cast<void ***>(policy))[slot]);
    }

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

  }  // namespace

  bool rename_endpoints(const std::vector<std::pair<std::wstring, std::wstring>> &rules) {
    // Unbuffered so progress survives a fault: see trace() above.
    setvbuf(stdout, nullptr, _IONBF, 0);

    trace("CoInitializeEx");
    const bool com_here = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    trace("CoCreateInstance MMDeviceEnumerator");
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator_, reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
      fprintf(stderr, "aurora-link: device enumerator unavailable (hr=0x%08lx)\n", static_cast<unsigned long>(hr));
      if (com_here) CoUninitialize();
      return false;
    }

    trace("CoCreateInstance CPolicyConfigClient");
    void *policy = nullptr;
    hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                          IID_IPolicyConfig, &policy);
    if (FAILED(hr) || policy == nullptr) {
      // Undocumented interface missing => this Windows moved it. Not fatal.
      fprintf(stderr, "aurora-link: IPolicyConfig unavailable (hr=0x%08lx), endpoint names left as-is\n",
              static_cast<unsigned long>(hr));
      enumerator->Release();
      if (com_here) CoUninitialize();
      return false;
    }

    int renamed = 0;
    // DEVICE_STATE_DISABLED/NOTPRESENT included on purpose: the cable is hidden between
    // sessions, and we still want its name correct when it comes back.
    for (EDataFlow flow : {eRender, eCapture}) {
      trace(flow == eRender ? "EnumAudioEndpoints render" : "EnumAudioEndpoints capture");
      IMMDeviceCollection *coll = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATEMASK_ALL, &coll)) || coll == nullptr) {
        continue;
      }
      UINT count = 0;
      coll->GetCount(&count);
      trace("collection enumerated");
      for (UINT i = 0; i < count; ++i) {
        IMMDevice *dev = nullptr;
        if (FAILED(coll->Item(i, &dev)) || dev == nullptr) {
          continue;
        }
        trace("device: reading name");
        DWORD state = 0;
        dev->GetState(&state);
        LPWSTR id = nullptr;
        const std::wstring name = endpoint_name(dev);
        if (!name.empty() && SUCCEEDED(dev->GetId(&id))) {
          for (const auto &rule : rules) {
            if (name.find(rule.first) != std::wstring::npos) {
              if (GetEnvironmentVariableA("AURORA_LINK_TRACE", nullptr, 0) != 0) {
                fprintf(stderr, "aurora-link/rename: match state=0x%lx id=%ls\n",
                        static_cast<unsigned long>(state), id);
                fflush(stderr);
              }
              /* Read the same property back through IPolicyConfig BEFORE writing it.
               * The vtable is undocumented, so a wrong slot means calling some other
               * method with our arguments -- which faults rather than failing.
               * GetPropertyValue sits one slot before SetPropertyValue and is
               * harmless, so a sane answer here is the cheapest possible proof that
               * the layout still matches before we let it write anything. */
              trace("match -> IPolicyConfig::GetPropertyValue (layout check)");
              PROPVARIANT probe;
              PropVariantInit(&probe);
              const HRESULT phr = policy_method(policy, policy_slot_get_property)(
                policy, id, FALSE, PKEY_Device_FriendlyName, &probe);
              const bool layout_ok = SUCCEEDED(phr) && probe.vt == VT_LPWSTR && probe.pwszVal != nullptr && name == probe.pwszVal;
              if (GetEnvironmentVariableA("AURORA_LINK_TRACE", nullptr, 0) != 0) {
                fprintf(stderr, "aurora-link/rename: layout check hr=0x%08lx vt=%u ok=%d\n",
                        static_cast<unsigned long>(phr), probe.vt, static_cast<int>(layout_ok));
                fflush(stderr);
              }
              PropVariantClear(&probe);
              if (!layout_ok) {
                wprintf(L"aurora-link: IPolicyConfig layout not recognised, [%ls] left as-is\n", name.c_str());
                break;
              }

              trace("match -> InitPropVariantFromString");
              PROPVARIANT v;
              InitPropVariantFromString(rule.second.c_str(), &v);
              trace("match -> IPolicyConfig::SetPropertyValue");
              const HRESULT shr = policy_method(policy, policy_slot_set_property)(
                policy, id, FALSE, kDeviceDesc, &v);
              if (SUCCEEDED(shr)) {
                wprintf(L"aurora-link: renamed [%ls] -> [%ls]\n", name.c_str(), rule.second.c_str());
                ++renamed;
              } else if (shr == E_ACCESSDENIED) {
                // Needs admin. Worth naming, because the read check above succeeds
                // unelevated, so only the write reveals it.
                wprintf(L"aurora-link: rename denied (run as admin), [%ls] left as-is\n", name.c_str());
              } else {
                wprintf(L"aurora-link: rename FAILED for [%ls] (hr=0x%08lx)\n", name.c_str(),
                        static_cast<unsigned long>(shr));
              }
              PropVariantClear(&v);
              break;
            }
          }
          CoTaskMemFree(id);
        }
        dev->Release();
      }
      coll->Release();
    }

    static_cast<IUnknown *>(policy)->Release();
    enumerator->Release();
    if (com_here) CoUninitialize();

    printf("aurora-link: %d endpoint(s) renamed\n", renamed);
    return true;
  }

}  // namespace aurora_link
