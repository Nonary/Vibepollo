/**
 * @file src/platform/windows/nv_truehdr.cpp
 * @brief Runtime loader for the MSVC-built TrueHDR shim DLL.
 *
 * Sunshine/Vibeshine builds with MinGW, but the NVIDIA RTX Video SDK ships MSVC libraries,
 * so the NGX TrueHDR work lives in a small MSVC-compiled shim (vibeshine_truehdr.dll, built
 * from tools/truehdr_shim). We load it the same way the rest of the codebase loads optional
 * NVIDIA components (CUDA, NVML): LoadLibrary + GetProcAddress, no link-time dependency. If
 * the shim or the RTX Video runtime isn't present, TrueHDR simply reports unavailable.
 *
 * The shim exposes a tiny C ABI operating on the D3D11 COM pointers we already hold (COM
 * vtables are ABI-stable across the MinGW/MSVC boundary).
 */

// this include
#include "nv_truehdr.h"

// local includes
#include "src/logging.h"

#include <mutex>

namespace platf::dxgi {

  namespace {
    // Shim C ABI (see tools/truehdr_shim/truehdr_shim.cpp).
    using create_fn = void *(__cdecl *) (ID3D11Device *device);
    using convert_fn = ID3D11Texture2D *(__cdecl *) (void *handle, ID3D11Texture2D *sdr_input,
                                                     int contrast, int saturation, int middle_gray, int peak_brightness);
    using destroy_fn = void(__cdecl *) (void *handle);

    HMODULE g_shim = nullptr;
    create_fn g_create = nullptr;
    convert_fn g_convert = nullptr;
    destroy_fn g_destroy = nullptr;
    bool g_resolve_attempted = false;
    // NGX TrueHDR uses process/device global runtime state. Multiple clients may have
    // separate encoder devices, so serialize all shim entry points across the process.
    std::mutex g_truehdr_mutex;

    // Resolve the shim exports once per process. The DLL is shipped next to sunshine.exe,
    // so the default search path (which includes the executable directory) finds it.
    bool resolve_shim_locked() {
      if (g_resolve_attempted) {
        return g_shim != nullptr;
      }
      g_resolve_attempted = true;

      g_shim = LoadLibraryW(L"vibeshine_truehdr.dll");
      if (!g_shim) {
        BOOST_LOG(warning) << "RTX HDR: vibeshine_truehdr.dll not found or failed to load (GetLastError="
                           << GetLastError() << "); RTX HDR disabled.";
        return false;
      }
      g_create = (create_fn) GetProcAddress(g_shim, "VBSTrueHDR_Create");
      g_convert = (convert_fn) GetProcAddress(g_shim, "VBSTrueHDR_Convert");
      g_destroy = (destroy_fn) GetProcAddress(g_shim, "VBSTrueHDR_Destroy");
      if (!g_create || !g_convert || !g_destroy) {
        BOOST_LOG(warning) << "RTX HDR: vibeshine_truehdr.dll is missing expected exports.";
        FreeLibrary(g_shim);
        g_shim = nullptr;
        return false;
      }
      return true;
    }
  }  // namespace

  nv_truehdr_t::~nv_truehdr_t() {
    release();
  }

  bool nv_truehdr_t::init(ID3D11Device *device) {
    if (!device) {
      return false;
    }

    std::scoped_lock lock {g_truehdr_mutex};
    if (initialized) {
      return true;
    }
    if (!resolve_shim_locked()) {
      return false;
    }

    shim_handle = g_create(device);
    if (!shim_handle) {
      BOOST_LOG(info) << "RTX HDR: TrueHDR unavailable on this GPU/driver/runtime.";
      return false;
    }
    initialized = true;
    BOOST_LOG(info) << "RTX HDR: TrueHDR feature ready -- SDR->HDR synthesis available.";
    return true;
  }

  ID3D11Texture2D *nv_truehdr_t::convert(ID3D11Texture2D *sdr_input, const truehdr_params_t &params) {
    std::scoped_lock lock {g_truehdr_mutex};
    if (!initialized || !sdr_input) {
      return nullptr;
    }
    return g_convert(shim_handle, sdr_input, params.contrast, params.saturation,
                     params.middle_gray, params.peak_brightness);
  }

  void nv_truehdr_t::release() {
    std::scoped_lock lock {g_truehdr_mutex};
    if (initialized && shim_handle && g_destroy) {
      g_destroy(shim_handle);
    }
    shim_handle = nullptr;
    initialized = false;
  }

}  // namespace platf::dxgi
