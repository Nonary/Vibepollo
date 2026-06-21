/**
 * @file tools/truehdr_shim/truehdr_shim.cpp
 * @brief MSVC-built shim DLL exposing NVIDIA TrueHDR (RTX Video SDK / NGX) over a tiny C ABI.
 *
 * Vibeshine builds with MinGW, but the RTX Video SDK ships MSVC libraries, so the NGX work
 * lives here and is compiled separately with MSVC (see build_truehdr_shim.ps1). The host
 * (src/platform/windows/nv_truehdr.cpp) loads this DLL with LoadLibrary/GetProcAddress and
 * passes the D3D11 COM pointers it already holds (vtables are ABI-stable across the boundary).
 *
 * Applies the same AI model as the NVIDIA App "RTX HDR" game filter to an SDR texture and
 * returns an FP16 scRGB HDR texture (1.0 == 80 nits), which Vibeshine's existing HDR convert
 * path consumes directly. Dials match the overlay (Contrast/Saturation/MiddleGray/Peak).
 */

#include <d3d11_4.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_truehdr.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_truehdr.h>

namespace {
  constexpr unsigned long long kNgxAppId = 0ULL;
  const wchar_t *kNgxAppPath = L".";

  struct ngx_device_runtime {
    ID3D11Device *device = nullptr;
    unsigned refs = 0;
  };

  std::mutex g_ngx_mutex;
  std::vector<ngx_device_runtime> g_ngx_devices;

  bool any_active_runtime_refs() {
    return std::any_of(g_ngx_devices.begin(), g_ngx_devices.end(), [](const auto &entry) {
      return entry.refs != 0;
    });
  }

  bool acquire_ngx_runtime_locked(ID3D11Device *device) {
    auto it = std::find_if(g_ngx_devices.begin(), g_ngx_devices.end(), [&](const auto &entry) {
      return entry.device == device;
    });
    if (it != g_ngx_devices.end()) {
      ++it->refs;
      return true;
    }

    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_Init(kNgxAppId, kNgxAppPath, device))) {
      return false;
    }

    device->AddRef();
    g_ngx_devices.push_back({device, 1});
    return true;
  }

  // Decrements the runtime ref for `device`. When no tracked device has active refs, shuts down the
  // NGX runtime for every tracked device -- still serialized under g_ngx_mutex so NGX Init/Shutdown1
  // can never race -- but DEFERS the D3D11 device Release()es into `deferred_releases`. Releasing a
  // device whose virtual display is mid-mode-change (alt-tab) or mid-removal wedges in the kernel
  // (final Release -> DestroyDriverInstance -> D3DKMTDestroyHwQueue); holding g_ngx_mutex across that
  // hang froze every other client's Create/Convert and permanently poisoned the lock. The caller
  // performs these Release()es after dropping the lock.
  void release_ngx_runtime_locked(ID3D11Device *device, std::vector<ID3D11Device *> &deferred_releases) {
    auto it = std::find_if(g_ngx_devices.begin(), g_ngx_devices.end(), [&](const auto &entry) {
      return entry.device == device;
    });
    if (it == g_ngx_devices.end()) {
      return;
    }
    if (it->refs != 0) {
      --it->refs;
    }
    if (any_active_runtime_refs()) {
      return;
    }

    for (auto &entry : g_ngx_devices) {
      if (entry.device) {
        NVSDK_NGX_D3D11_Shutdown1(entry.device);
        deferred_releases.push_back(entry.device);  // Release()d by the caller after g_ngx_mutex drops
      }
    }
    g_ngx_devices.clear();
  }

  class truehdr_impl {
  public:
    bool init_locked(ID3D11Device *device) {
      if (!device) {
        return false;
      }
      if (!acquire_ngx_runtime_locked(device)) {
        return false;
      }
      // init runs on a freshly created, healthy device, so a failure-path device Release here cannot
      // wedge; drain the deferred releases inline (still under g_ngx_mutex) rather than after the lock.
      std::vector<ID3D11Device *> deferred;
      auto drain = [&deferred]() {
        for (auto *d : deferred) {
          if (d) {
            d->Release();
          }
        }
        deferred.clear();
      };
      if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&params)) || !params) {
        release_ngx_runtime_locked(device, deferred);
        drain();
        return false;
      }
      int available = 0;
      params->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available);
      if (!available) {
        NVSDK_NGX_D3D11_DestroyParameters(params);
        params = nullptr;
        release_ngx_runtime_locked(device, deferred);
        drain();
        return false;
      }

      this->device = device;
      device->AddRef();
      device->GetImmediateContext(&context);
      if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D10Multithread), (void **) &multithread))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Enter();
      }

      NVSDK_NGX_Feature_Create_Params create_params = {};
      NVSDK_NGX_Result r = NGX_D3D11_CREATE_TRUEHDR_EXT(context, &feature, params, &create_params);
      if (multithread) {
        multithread->Leave();
      }
      if (NVSDK_NGX_FAILED(r)) {
        release_locked(deferred);
        drain();
        return false;
      }
      initialized = true;
      return true;
    }

    ID3D11Texture2D *convert_locked(ID3D11Texture2D *sdr_input, int contrast, int saturation, int middle_gray, int peak) {
      if (!initialized || !sdr_input) {
        return nullptr;
      }
      D3D11_TEXTURE2D_DESC in_desc = {};
      sdr_input->GetDesc(&in_desc);
      if (in_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
          in_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
          in_desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        return nullptr;
      }
      if (!ensure_output(in_desc.Width, in_desc.Height)) {
        return nullptr;
      }

      NVSDK_NGX_D3D11_TRUEHDR_Eval_Params eval = {};
      eval.pInput = sdr_input;
      eval.pOutput = output;
      eval.InputSubrectBR.Width = in_desc.Width;
      eval.InputSubrectBR.Height = in_desc.Height;
      eval.OutputSubrectBR.Width = out_w;
      eval.OutputSubrectBR.Height = out_h;
      eval.Contrast = (UINT) contrast;
      eval.Saturation = (UINT) saturation;
      eval.MiddleGray = (UINT) middle_gray;
      eval.MaxLuminance = (UINT) peak;

      if (multithread) {
        multithread->Enter();
      }
      NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_TRUEHDR_EXT(context, feature, params, &eval);
      if (multithread) {
        multithread->Leave();
      }
      return NVSDK_NGX_FAILED(r) ? nullptr : output;
    }

    // Releases everything EXCEPT the final D3D11 device Release()es, which are appended to
    // `deferred_releases` for the caller to perform after dropping g_ngx_mutex. The final device
    // Release is the call that wedges in the kernel on a mid-mode-change/removing virtual display,
    // and must not run while holding the process-global lock (it would freeze all other clients).
    // NGX ReleaseFeature/DestroyParameters/Shutdown1 stay under the lock so Init/Shutdown1 never race.
    void release_locked(std::vector<ID3D11Device *> &deferred_releases) {
      if (output) {
        output->Release();
        output = nullptr;
      }
      out_w = out_h = 0;
      if (feature) {
        NVSDK_NGX_D3D11_ReleaseFeature(feature);
        feature = nullptr;
      }
      if (params && device) {
        NVSDK_NGX_D3D11_DestroyParameters(params);
        params = nullptr;
      }
      if (device) {
        release_ngx_runtime_locked(device, deferred_releases);
      }
      if (multithread) {
        multithread->Release();
        multithread = nullptr;
      }
      if (context) {
        context->Release();
        context = nullptr;
      }
      if (device) {
        deferred_releases.push_back(device);  // wedging final Release; performed after the lock drops
        device = nullptr;
      }
      initialized = false;
    }

  private:
    bool ensure_output(UINT w, UINT h) {
      if (output && out_w == w && out_h == h) {
        return true;
      }
      if (output) {
        output->Release();
        output = nullptr;
      }
      D3D11_TEXTURE2D_DESC d = {};
      d.Width = w;
      d.Height = h;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // scRGB; what Vibeshine's HDR convert expects
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
      if (FAILED(device->CreateTexture2D(&d, nullptr, &output))) {
        output = nullptr;
        return false;
      }
      out_w = w;
      out_h = h;
      return true;
    }

    bool initialized = false;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D10Multithread *multithread = nullptr;
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    ID3D11Texture2D *output = nullptr;
    UINT out_w = 0, out_h = 0;
  };
}  // namespace

extern "C" {

  __declspec(dllexport) void *__cdecl VBSTrueHDR_Create(ID3D11Device *device) {
    std::scoped_lock lock(g_ngx_mutex);
    auto *impl = new truehdr_impl();
    if (!impl->init_locked(device)) {
      delete impl;
      return nullptr;
    }
    return impl;
  }

  __declspec(dllexport) ID3D11Texture2D *__cdecl VBSTrueHDR_Convert(
    void *handle, ID3D11Texture2D *sdr_input,
    int contrast, int saturation, int middle_gray, int peak_brightness) {
    if (!handle) {
      return nullptr;
    }
    std::scoped_lock lock(g_ngx_mutex);
    return static_cast<truehdr_impl *>(handle)->convert_locked(sdr_input, contrast, saturation, middle_gray, peak_brightness);
  }

  __declspec(dllexport) void __cdecl VBSTrueHDR_Destroy(void *handle) {
    if (handle) {
      auto *impl = static_cast<truehdr_impl *>(handle);
      std::vector<ID3D11Device *> deferred_releases;
      {
        std::scoped_lock lock(g_ngx_mutex);
        impl->release_locked(deferred_releases);
      }
      // Perform the wedging final device Release()es WITHOUT g_ngx_mutex held. On a virtual display
      // that is mid-mode-change (alt-tab) or being removed, the final ID3D11Device Release blocks in
      // the kernel (DestroyDriverInstance -> D3DKMTDestroyHwQueue); holding the process-global lock
      // across that hang is what froze every other client's Create/Convert and poisoned the lock.
      for (auto *d : deferred_releases) {
        if (d) {
          d->Release();
        }
      }
      delete impl;
    }
  }
}
