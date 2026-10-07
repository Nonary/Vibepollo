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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

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
    std::timed_mutex g_truehdr_mutex;

    // Set once a shim call has overrun its budget and been abandoned. The abandoned
    // call still owns g_truehdr_mutex and may never give it back, so every later
    // caller has to fail fast rather than queue behind it: one wedged feature create
    // used to freeze the encode loop of every client sharing this process.
    std::atomic<bool> g_shim_wedged {false};
    std::atomic<bool> g_wedge_skip_logged {false};

    // Generous, because it only has to distinguish "slow" from "never".
    constexpr auto kCreateTimeout = std::chrono::seconds(8);
    // A convert cannot wait on a peer: missing the deadline just means this frame
    // takes the plain SDR->PQ path.
    constexpr auto kConvertLockTimeout = std::chrono::milliseconds(250);
    constexpr auto kReleaseLockTimeout = std::chrono::seconds(5);

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

  struct nv_truehdr_t::init_state_t {
    std::mutex mutex;
    std::chrono::steady_clock::time_point started_at {std::chrono::steady_clock::now()};
    void *handle = nullptr;
    bool completed = false;
    bool abandoned = false;
  };

  nv_truehdr_t::~nv_truehdr_t() {
    release();
  }

  bool nv_truehdr_t::init(ID3D11Device *device) {
    if (!device) {
      return false;
    }
    if (initialized) {
      return true;
    }
    if (init_failed || g_shim_wedged.load(std::memory_order_acquire)) {
      return false;
    }

    // Creating the NGX feature reaches into D3D11 and the display stack. On a virtual
    // display whose mode is mid-change -- which is what an alt-tab out of a fullscreen
    // game looks like -- that call can block in the kernel for as long as the mode
    // change takes. Run it on a helper thread and poll from later frames so the encode
    // thread keeps streaming through SDR-to-PQ while creation is pending. A timed-out
    // or released owner marks the result abandoned; the helper destroys a late handle
    // instead of publishing it back into a dead session.
    //
    // The helper thread holds its own reference: once we abandon it, the encoder that
    // asked for the feature is free to drop the device while the call is still inside
    // the driver.
    if (!init_state) {
      init_state = std::make_shared<init_state_t>();
      device->AddRef();
      std::thread {[device, state = init_state]() {
        void *handle = nullptr;
        {
          std::scoped_lock lock {g_truehdr_mutex};
          if (resolve_shim_locked()) {
            handle = g_create(device);
          }
        }
        device->Release();

        void *abandoned_handle = nullptr;
        {
          std::scoped_lock lock {state->mutex};
          if (state->abandoned) {
            abandoned_handle = handle;
          } else {
            state->handle = handle;
          }
          state->completed = true;
        }
        // The owner can disappear while feature creation is still inside the
        // driver. In that case, clean up on this detached worker instead of
        // publishing a handle that nobody can release.
        if (abandoned_handle && g_destroy) {
          g_destroy(abandoned_handle);
        }
      }}.detach();
      BOOST_LOG(debug) << "RTX HDR: TrueHDR feature creation started asynchronously; "
                          "streaming through SDR-to-PQ until it is ready.";
      return false;
    }

    auto state = init_state;
    {
      std::scoped_lock lock {state->mutex};
      if (!state->completed) {
        if (std::chrono::steady_clock::now() - state->started_at < kCreateTimeout) {
          return false;
        }
        state->abandoned = true;
        init_state.reset();
        init_failed = true;
        g_shim_wedged.store(true, std::memory_order_release);
        BOOST_LOG(error) << "RTX HDR: TrueHDR feature creation did not complete within "
                         << std::chrono::duration_cast<std::chrono::seconds>(kCreateTimeout).count()
                         << "s (display stack likely mid-mode-change); abandoning it and disabling "
                            "TrueHDR for this process. Streaming continues on the SDR-to-PQ path.";
        return false;
      }
      shim_handle = std::exchange(state->handle, nullptr);
    }
    init_state.reset();
    if (!shim_handle) {
      init_failed = true;
      BOOST_LOG(info) << "RTX HDR: TrueHDR unavailable on this GPU/driver/runtime.";
      return false;
    }
    initialized = true;
    BOOST_LOG(info) << "RTX HDR: TrueHDR feature ready -- SDR->HDR synthesis available.";
    return true;
  }

  ID3D11Texture2D *nv_truehdr_t::convert(ID3D11Texture2D *sdr_input, const truehdr_params_t &params) {
    if (!initialized || !sdr_input || g_shim_wedged.load(std::memory_order_acquire)) {
      return nullptr;
    }
    std::unique_lock lock {g_truehdr_mutex, std::defer_lock};
    if (!lock.try_lock_for(kConvertLockTimeout)) {
      // Someone else is inside the shim and not coming out on any schedule we can
      // rely on. Waiting would spread their stall to this client's encode loop.
      if (!g_wedge_skip_logged.exchange(true, std::memory_order_acq_rel)) {
        BOOST_LOG(warning) << "RTX HDR: TrueHDR is busy in another session; streaming this frame "
                              "through the SDR-to-PQ path instead of waiting on it.";
      }
      return nullptr;
    }
    return g_convert(shim_handle, sdr_input, params.contrast, params.saturation,
                     params.middle_gray, params.peak_brightness);
  }

  void nv_truehdr_t::release() {
    if (init_state) {
      auto state = std::move(init_state);
      void *completed_handle = nullptr;
      {
        std::scoped_lock lock {state->mutex};
        state->abandoned = true;
        if (state->completed) {
          completed_handle = std::exchange(state->handle, nullptr);
        }
      }
      if (completed_handle && g_destroy) {
        g_destroy(completed_handle);
      }
    }

    // Snapshot+clear the handle under the lock, then call the shim destroy WITHOUT g_truehdr_mutex
    // held. VBSTrueHDR_Destroy releases the encoder's D3D11 device, and on a virtual display whose
    // mode is mid-change (alt-tab) or that is being removed (disconnect), that device's
    // DestroyDriverInstance -> D3DKMTDestroyHwQueue can block in the kernel indefinitely. Holding
    // the process-global mutex across that hang froze every other client's init()/convert(). This
    // is still only ever reached from ~nv_truehdr_t on the single owning thread (nv_truehdr_t is a
    // copy-deleted unique_ptr member), so the snapshot cannot race a concurrent release() of the
    // same object, and the NGX same-thread shutdown constraint (nv_truehdr.h) is preserved.
    void *local_handle = nullptr;
    destroy_fn local_destroy = nullptr;
    if (shim_handle) {
      std::unique_lock lock {g_truehdr_mutex, std::defer_lock};
      // A wedged shim never returns the mutex, and calling into it would hang this
      // thread too. Drop our own state and leak the feature instead.
      const bool locked = !g_shim_wedged.load(std::memory_order_acquire) &&
                          lock.try_lock_for(kReleaseLockTimeout);
      if (locked && initialized && shim_handle && g_destroy) {
        local_handle = shim_handle;
        local_destroy = g_destroy;
      } else if (!locked && shim_handle) {
        BOOST_LOG(warning) << "RTX HDR: abandoning the TrueHDR feature without destroying it "
                              "because the shim is wedged.";
      }
      shim_handle = nullptr;
      initialized = false;
    }
    if (local_handle && local_destroy) {
      local_destroy(local_handle);
    }
  }

}  // namespace platf::dxgi
