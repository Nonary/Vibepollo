/**
 * @file sunshine_wgc_capture.cpp
 * @brief Windows Graphics Capture helper process for Sunshine.
 *
 * This standalone executable provides Windows Graphics Capture functionality
 * for the main Sunshine streaming process. It runs as a separate process to
 * isolate WGC operations and handle secure desktop scenarios. The helper
 * communicates with the main process via a control pipe, shared D3D11 texture,
 * shared frame metadata, and a frame-ready event.
 */

#define WIN32_LEAN_AND_MEAN

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/ipc/pipes.h"
#include "src/platform/windows/wgc_damage_tracker.h"
#include "src/utility.h"  // For RAII utilities

// platform includes
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>  // For IInspectable
#include <KnownFolders.h>
#include <ShellScalingApi.h>  // For DPI awareness
#include <ShlObj.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>  // For ApiInformation
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Manual declaration for CreateDirect3D11DeviceFromDXGIDevice if missing
extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
}

/**
 * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
 * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
 * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
 */
struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
  __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
  IDirect3DDxgiInterfaceAccess: ::IUnknown {
  virtual HRESULT __stdcall GetInterface(REFIID id, IUnknown **object) = 0;
};

#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
};

template<>
constexpr auto __mingw_uuidof<IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}

static constexpr GUID GUID__IDirect3DSurface = {
  0x0BF4A146,
  0x13C1,
  0x4694,
  {0xBE, 0xE3, 0x7A, 0xBF, 0x15, 0xEA, 0xF5, 0x86}
};

template<>
constexpr auto __mingw_uuidof<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>() -> GUID const & {
  return GUID__IDirect3DSurface;
}
#endif

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace platf::dxgi;

// GPU scheduling priority definitions for optimal capture performance under high GPU load
enum class D3DKMT_SchedulingPriorityClass : LONG {
  IDLE = 0,
  BELOW_NORMAL = 1,
  NORMAL = 2,
  ABOVE_NORMAL = 3,
  HIGH = 4,
  REALTIME = 5
};

using PD3DKMTSetProcessSchedulingPriorityClass = LONG(__stdcall *)(HANDLE, LONG);

/**
 * @brief D3D11 device creation flags for the WGC helper process.
 *
 * NOTE: This constant is NOT shared with the main process. It is defined here separately
 * because including display.h would pull in too many dependencies for this standalone helper.
 * If you change this flag, you MUST update it in both this file and src/platform/windows/display.h.
 */
constexpr UINT D3D11_CREATE_DEVICE_FLAGS = 0;

/**
 * @brief Initial log level for the helper process.
 */
const int INITIAL_LOG_LEVEL = 2;

/**
 * @brief Global configuration data received from the main process.
 */
constexpr uint32_t DEFAULT_WGC_IPC_FLAGS =
  platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST |
  platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE;
static platf::dxgi::config_data_t g_config = {0, 0, 0, L"", {0, 0}, 10000, 60, 1, 2, DEFAULT_WGC_IPC_FLAGS};
static std::mutex g_config_mutex;
static std::condition_variable g_config_cv;

/**
 * @brief Flag indicating whether configuration data has been received from main process.
 */
static bool g_config_received = false;

/**
 * @brief Global communication pipe for sending session closed notifications.
 */
static std::weak_ptr<AsyncNamedPipe> g_communication_pipe_weak;

/**
 * @brief Global Windows event hook for desktop switch detection.
 */
static safe_winevent_hook g_desktop_switch_hook = nullptr;

/**
 * @brief Flag indicating if a secure desktop has been detected.
 */
static bool g_secure_desktop_detected = false;

/**
 * @brief Set when the GraphicsCaptureItem reports it was closed (display mode change,
 * DWM restart, monitor removal). FrameArrived never fires again after this, so the
 * helper must exit and let the main process reinitialize capture.
 */
static std::atomic<bool> g_capture_item_closed {false};
static winrt::handle g_pipe_disconnected_event;
static winrt::handle g_capture_item_closed_event;

/**
 * @brief Pending verification deadline for desktop switches that were not immediately secure.
 */
static std::optional<std::chrono::steady_clock::time_point> g_pending_secure_desktop_check_deadline;

/**
 * @brief Grace window for confirming secure desktop after a desktop switch event.
 *
 * EVENT_SYSTEM_DESKTOPSWITCH can fire before OpenInputDesktop reflects the final target desktop.
 * Keep a short follow-up probe window so we still notify the main process when the switch settles
 * onto secure desktop, without forcing a reinit for ordinary non-secure desktop churn.
 */
constexpr auto SECURE_DESKTOP_CONFIRMATION_WINDOW = std::chrono::seconds(1);

void signal_capture_item_closed() {
  g_capture_item_closed.store(true, std::memory_order_release);
  if (g_capture_item_closed_event) {
    SetEvent(g_capture_item_closed_event.get());
  }
}

void signal_pipe_disconnected() {
  if (g_pipe_disconnected_event) {
    SetEvent(g_pipe_disconnected_event.get());
  }
}

void notify_main_process_about_secure_desktop() {
  if (g_secure_desktop_detected) {
    return;
  }

  g_secure_desktop_detected = true;
  g_pending_secure_desktop_check_deadline.reset();
  BOOST_LOG(info) << "Secure desktop detected - sending notification to main process";

  if (auto pipe = g_communication_pipe_weak.lock(); pipe && pipe->is_connected()) {
    uint8_t msg = SECURE_DESKTOP_MSG;
    pipe->send(std::span<const uint8_t>(&msg, 1));
    BOOST_LOG(info) << "Queued desktop switch reinit notification to main process";
  } else {
    BOOST_LOG(warning) << "Desktop switch detected, but the main process pipe is not connected";
  }
}

void poll_pending_secure_desktop_transition() {
  if (!g_pending_secure_desktop_check_deadline) {
    return;
  }

  if (platf::dxgi::is_secure_desktop_active()) {
    BOOST_LOG(info) << "Secure desktop became active shortly after desktop switch";
    notify_main_process_about_secure_desktop();
    return;
  }

  if (std::chrono::steady_clock::now() >= *g_pending_secure_desktop_check_deadline) {
    BOOST_LOG(debug) << "Desktop switch settled without entering secure desktop; ignoring reinit";
    g_pending_secure_desktop_check_deadline.reset();
  }
}

/**
 * @brief System initialization class to handle DPI, threading, and MMCSS setup.
 *
 * This class manages critical system-level initialization for optimal capture performance:
 * - Sets DPI awareness to handle high-DPI displays correctly
 * - Elevates thread priority for better capture timing
 * - Configures MMCSS (Multimedia Class Scheduler Service) for audio/video workloads
 * - Initializes WinRT apartment for Windows Graphics Capture APIs
 */
class SystemInitializer {
private:
  safe_mmcss_handle _mmcss_handle = nullptr;  ///< Handle for MMCSS thread characteristics
  bool _dpi_awareness_set = false;  ///< Flag indicating if DPI awareness was successfully set
  bool _thread_priority_set = false;  ///< Flag indicating if thread priority was elevated
  bool _mmcss_characteristics_set = false;  ///< Flag indicating if MMCSS characteristics were set
  bool _gpu_priority_set = false;  ///< Flag indicating if GPU scheduling priority was set

public:
  /**
   * @brief Initializes DPI awareness for the process.
   *
   * Attempts to set per-monitor DPI awareness to handle high-DPI displays correctly.
   * First tries the newer SetProcessDpiAwarenessContext API, then falls back to
   * SetProcessDpiAwareness if the newer API is not available.
   *
   * @return true if DPI awareness was successfully set, false otherwise.
   */
  bool initialize_dpi_awareness() {
    // Try newer API first
    if (HMODULE user32_module = GetModuleHandleA("user32.dll")) {
      using set_process_dpi_awareness_context_fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
      auto set_process_dpi_awareness_context =
        reinterpret_cast<set_process_dpi_awareness_context_fn>(
          GetProcAddress(user32_module, "SetProcessDpiAwarenessContext")
        );

      if (set_process_dpi_awareness_context &&
          set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        _dpi_awareness_set = true;
        return true;
      }
    }

    // Fallback to older API (Win 8.1+)
    if (SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
      _dpi_awareness_set = true;
      return true;
    }

    BOOST_LOG(warning) << "Failed to set DPI awareness, display scaling issues may occur";
    return false;
  }

  /**
   * @brief Elevates the current thread priority for better capture performance.
   *
   * Sets the thread priority to THREAD_PRIORITY_HIGHEST to reduce latency
   * and improve capture frame timing consistency.
   *
   * @return true if thread priority was successfully elevated, false otherwise.
   */
  bool initialize_thread_priority() {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
      BOOST_LOG(error) << "Failed to set thread priority: " << GetLastError();
      return false;
    }
    _thread_priority_set = true;
    return true;
  }

  /**
   * @brief Configures MMCSS (Multimedia Class Scheduler Service) characteristics.
   *
   * Registers the thread with MMCSS for multimedia workload scheduling.
   * First attempts "Pro Audio" task profile, then falls back to "Games" profile.
   * This helps ensure consistent timing for capture operations.
   * Additionally sets MMCSS thread relative priority to maximum for best performance.
   *
   * @return true if MMCSS characteristics were successfully set, false otherwise.
   */
  bool initialize_mmcss_characteristics() {
    DWORD task_idx = 0;
    HANDLE raw_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    if (!raw_handle) {
      raw_handle = AvSetMmThreadCharacteristicsW(L"Games", &task_idx);
      if (!raw_handle) {
        BOOST_LOG(error) << "Failed to set MMCSS characteristics: " << GetLastError();
        return false;
      }
    }

    // Set MMCSS thread relative priority to maximum (AVRT_PRIORITY_HIGH = 2)
    if (!AvSetMmThreadPriority(raw_handle, AVRT_PRIORITY_HIGH)) {
      BOOST_LOG(warning) << "Failed to set MMCSS thread priority: " << GetLastError();
      // Don't fail completely as the basic MMCSS registration still works
    }

    _mmcss_handle.reset(raw_handle);
    _mmcss_characteristics_set = true;
    return true;
  }

  /**
   * @brief Sets GPU scheduling priority for optimal capture performance under high GPU load.
   *
   * Uses HIGH rather than REALTIME so capture work remains responsive without
   * preempting the game's rendering queue under GPU pressure.
   *
   * @return true if GPU priority was successfully set, false otherwise.
   */
  bool initialize_gpu_scheduling_priority() {
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (!gdi32) {
      BOOST_LOG(warning) << "Failed to get gdi32.dll handle for GPU priority adjustment";
      return false;
    }

    auto d3dkmt_set_process_priority = reinterpret_cast<PD3DKMTSetProcessSchedulingPriorityClass>(GetProcAddress(gdi32, "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!d3dkmt_set_process_priority) {
      BOOST_LOG(warning) << "D3DKMTSetProcessSchedulingPriorityClass not available, GPU priority not set";
      return false;
    }

    auto priority = static_cast<LONG>(D3DKMT_SchedulingPriorityClass::HIGH);

    HRESULT hr = d3dkmt_set_process_priority(GetCurrentProcess(), priority);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "Failed to set GPU scheduling priority to HIGH: " << hr
                         << " (may require administrator privileges for optimal performance)";
      return false;
    }

    BOOST_LOG(info) << "GPU scheduling priority set to HIGH for balanced capture performance";
    _gpu_priority_set = true;
    return true;
  }

  /**
   * @brief Initializes WinRT apartment for Windows Graphics Capture APIs.
   *
   * Sets up the WinRT apartment as multi-threaded to support Windows Graphics
   * Capture operations from background threads.
   *
   * @return true always (WinRT initialization typically succeeds).
   */
  bool initialize_winrt_apartment() const {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    return true;
  }

  /**
   * @brief Initializes all system components for optimal capture performance.
   *
   * Calls all initialization methods in sequence:
   * - DPI awareness configuration
   * - Thread priority elevation
   * - GPU scheduling priority setup
   * - MMCSS characteristics setup
   * - WinRT apartment initialization
   *
   * @return true if all initialization steps succeeded, false if any failed.
   */
  bool initialize_all() {
    bool success = true;
    success &= initialize_dpi_awareness();
    success &= initialize_thread_priority();
    success &= initialize_gpu_scheduling_priority();
    success &= initialize_mmcss_characteristics();
    success &= initialize_winrt_apartment();
    return success;
  }

  /**
   * @brief Checks if DPI awareness was successfully set.
   * @return true if DPI awareness is configured, false otherwise.
   */
  bool is_dpi_awareness_set() const {
    return _dpi_awareness_set;
  }

  /**
   * @brief Checks if thread priority was successfully elevated.
   * @return true if thread priority is elevated, false otherwise.
   */
  bool is_thread_priority_set() const {
    return _thread_priority_set;
  }

  /**
   * @brief Checks if MMCSS characteristics were successfully set.
   * @return true if MMCSS characteristics are configured, false otherwise.
   */
  bool is_mmcss_characteristics_set() const {
    return _mmcss_characteristics_set;
  }

  /**
   * @brief Checks if GPU scheduling priority was successfully set.
   * @return true if GPU scheduling priority is configured, false otherwise.
   */
  bool is_gpu_priority_set() const {
    return _gpu_priority_set;
  }

  /**
   * @brief Destructor for SystemInitializer.
   *
   * RAII automatically releases MMCSS resources.
   */
  ~SystemInitializer() noexcept = default;
};

/**
 * @brief D3D11 device management class to handle device creation and WinRT interop.
 *
 * This class manages D3D11 device and context creation, as well as the WinRT
 * interop device required for Windows Graphics Capture. It handles the complex
 * process of bridging between traditional D3D11 APIs and WinRT capture APIs.
 */
class D3D11DeviceManager {
private:
  winrt::com_ptr<ID3D11Device> _device;  ///< D3D11 device for graphics operations
  winrt::com_ptr<ID3D11DeviceContext> _context;  ///< D3D11 device context for rendering
  D3D_FEATURE_LEVEL _feature_level;  ///< D3D feature level supported by the device
  winrt::com_ptr<IDXGIDevice> _dxgi_device;  ///< DXGI device interface for WinRT interop
  winrt::com_ptr<::IDirect3DDevice> _interop_device;  ///< Intermediate interop device
  IDirect3DDevice _winrt_device = nullptr;  ///< WinRT Direct3D device for capture integration

public:
  /**
   * @brief Creates a D3D11 device and context for graphics operations.
   *
   * Creates a hardware-accelerated D3D11 device using the specified adapter.
   * The device is used for texture operations and WinRT interop.
   * Also sets GPU thread priority to 7 for optimal capture performance.
   * Uses the same D3D11_CREATE_DEVICE_FLAGS as the main Sunshine process.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if device creation succeeded, false otherwise.
   */
  bool create_device(const LUID &adapter_luid) {
    // Feature levels to try, matching the main process
    D3D_FEATURE_LEVEL featureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1
    };

    // Find the adapter matching the LUID
    winrt::com_ptr<IDXGIAdapter1> adapter;
    if (adapter_luid.HighPart != 0 || adapter_luid.LowPart != 0) {
      // Non-zero LUID provided, find the matching adapter
      winrt::com_ptr<IDXGIFactory1> factory;
      HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
      if (FAILED(hr)) {
        BOOST_LOG(error) << "Failed to create DXGI factory for adapter lookup";
        return false;
      }

      IDXGIAdapter1 *raw_adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &raw_adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIAdapter1> test_adapter;
        test_adapter.attach(raw_adapter);
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(test_adapter->GetDesc1(&desc))) {
          if (desc.AdapterLuid.HighPart == adapter_luid.HighPart &&
              desc.AdapterLuid.LowPart == adapter_luid.LowPart) {
            adapter = test_adapter;
            BOOST_LOG(info) << "Found matching adapter: " << wide_to_utf8(desc.Description);
            break;
          }
        }
      }

      if (!adapter) {
        BOOST_LOG(warning) << "Could not find adapter with LUID "
                           << std::hex << adapter_luid.HighPart << ":" << adapter_luid.LowPart
                           << std::dec << ", using default adapter";
      }
    } else {
      BOOST_LOG(info) << "Using default adapter (no LUID specified)";
    }

    // Create the D3D11 device using the same flags as the main process
    HRESULT hr = D3D11CreateDevice(
      adapter.get(),  // nullptr if no specific adapter found
      adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_FLAGS,
      featureLevels,
      ARRAYSIZE(featureLevels),
      D3D11_SDK_VERSION,
      _device.put(),
      &_feature_level,
      _context.put()
    );

    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create D3D11 device: " << std::hex << hr << std::dec;
      return false;
    }

    // Set GPU thread priority to 7 for optimal capture performance under high GPU load
    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (_dxgi_device) {
      hr = _dxgi_device->SetGPUThreadPriority(7);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to set GPU thread priority to 7: " << hr
                           << " (may require administrator privileges for optimal performance)";
      } else {
        BOOST_LOG(info) << "GPU thread priority set to 7 for optimal capture performance";
      }
    } else {
      BOOST_LOG(warning) << "Failed to query DXGI device for GPU thread priority setting";
    }

    return true;
  }

  /**
   * @brief Creates WinRT interop device from the D3D11 device.
   *
   * Bridges the D3D11 device to WinRT APIs required for Windows Graphics Capture.
   * This involves:
   * - Querying DXGI device interface from D3D11 device
   * - Creating Direct3D interop device using WinRT factory
   * - Converting to WinRT IDirect3DDevice interface
   *
   * @return true if WinRT interop device creation succeeded, false otherwise.
   */
  bool create_winrt_interop() {
    if (!_device) {
      return false;
    }

    _dxgi_device = _device.try_as<IDXGIDevice>();
    if (!_dxgi_device) {
      BOOST_LOG(error) << "Failed to get DXGI device";
      return false;
    }

    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(_dxgi_device.get(), reinterpret_cast<::IInspectable **>(winrt::put_abi(_interop_device)));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create interop device";
      return false;
    }

    _winrt_device = _interop_device.try_as<IDirect3DDevice>();
    if (!_winrt_device) {
      BOOST_LOG(error) << "Failed to query IDirect3DDevice from interop device";
      return false;
    }
    return true;
  }

  /**
   * @brief Initializes the D3D11 device and WinRT interop device.
   *
   * Calls create_device() with the specified adapter LUID, then create_winrt_interop() to create the WinRT interop device.
   *
   * @param adapter_luid LUID of the adapter to use, or all zeros for default adapter.
   * @return true if both device and interop device are successfully created, false otherwise.
   */
  bool initialize_all(const LUID &adapter_luid) {
    return create_device(adapter_luid) && create_winrt_interop();
  }

  /**
   * @brief Gets the underlying D3D11 device com_ptr.
   *
   * @return Pointer to the managed ID3D11Device, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11Device> &get_device() const {
    return _device;
  }

  /**
   * @brief Gets the underlying D3D11 device context com_ptr.
   *
   * @return Pointer to the managed ID3D11DeviceContext, or empty if not initialized.
   */
  const winrt::com_ptr<ID3D11DeviceContext> &get_context() const {
    return _context;
  }

  /**
   * @brief Gets the WinRT IDirect3DDevice for Windows Graphics Capture interop.
   *
   * @return The WinRT IDirect3DDevice, or nullptr if not initialized.
   */
  IDirect3DDevice get_winrt_device() const {
    return _winrt_device;
  }

  /**
   * @brief Destructor for D3D11DeviceManager.
   *
   * RAII automatically releases device and context resources.
   */
  ~D3D11DeviceManager() noexcept = default;
};

/**
 * @brief Monitor and display management class to handle monitor enumeration and selection.
 *
 * This class manages monitor detection, selection, and configuration for capture operations.
 * It handles scenarios with multiple monitors and provides resolution configuration based
 * on monitor capabilities and user preferences.
 */
class DisplayManager {
private:
  HMONITOR _selected_monitor = nullptr;  ///< Handle to the selected monitor for capture
  MONITORINFO _monitor_info = {sizeof(MONITORINFO)};  ///< Information about the selected monitor
  UINT _width = 0;  ///< Final capture width in pixels
  UINT _height = 0;  ///< Final capture height in pixels

public:
  /**
   * @brief Selects a monitor for capture based on the provided configuration.
   *
   * If a display name is specified in the config, attempts to find and select the monitor matching that name.
   * If not found, or if no display name is provided, falls back to selecting the primary monitor.
   *
   * @param config The configuration data containing the desired display name (if any).
   * @return true if a monitor was successfully selected; false if monitor selection failed.
   */
  bool select_monitor(const platf::dxgi::config_data_t &config) {
    if (config.display_name[0] != L'\0') {
      auto find_monitor_by_name = [&](const wchar_t *target_name) -> HMONITOR {
        struct EnumData {
          const wchar_t *target_name;
          HMONITOR found_monitor;
        };

        EnumData enum_data = {target_name, nullptr};

        auto enum_proc = +[](HMONITOR h_mon, HDC /*hdc*/, RECT * /*rc*/, LPARAM l_param) {
          auto *data = static_cast<EnumData *>(reinterpret_cast<void *>(l_param));
          if (MONITORINFOEXW m_info = {sizeof(MONITORINFOEXW)}; GetMonitorInfoW(h_mon, &m_info) && wcsncmp(m_info.szDevice, data->target_name, 32) == 0) {
            data->found_monitor = h_mon;
            return FALSE;  // Stop enumeration
          }
          return TRUE;
        };

        EnumDisplayMonitors(nullptr, nullptr, enum_proc, static_cast<LPARAM>(reinterpret_cast<std::uintptr_t>(&enum_data)));
        return enum_data.found_monitor;
      };

      _selected_monitor = find_monitor_by_name(config.display_name);
      if (!_selected_monitor) {
        // During virtual display topology transitions, monitor enumeration can lag briefly.
        // Wait a short amount of time before falling back so we avoid capture bouncing.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (!_selected_monitor && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          _selected_monitor = find_monitor_by_name(config.display_name);
        }
      }
      if (!_selected_monitor) {
        BOOST_LOG(warning) << "Could not find monitor with name '" << winrt::to_string(config.display_name) << "', falling back to primary.";
      }
    }

    if (!_selected_monitor) {
      _selected_monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
      if (!_selected_monitor) {
        BOOST_LOG(error) << "Failed to get primary monitor";
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Retrieves information about the currently selected monitor.
   *
   * This function queries the Windows API to obtain monitor information for the selected monitor,
   * including its dimensions. The width and height are stored as fallback values for later use.
   *
   * @return true if monitor information was successfully retrieved; false otherwise.
   */
  bool get_monitor_info() {
    if (!_selected_monitor) {
      return false;
    }

    if (!GetMonitorInfo(_selected_monitor, &_monitor_info)) {
      BOOST_LOG(error) << "Failed to get monitor info";
      return false;
    }

    return true;
  }

  /**
   * @brief Creates a Windows Graphics Capture item for the selected monitor.
   *
   * Uses the IGraphicsCaptureItemInterop interface to create a GraphicsCaptureItem
   * corresponding to the currently selected monitor. This item is required for
   * initiating Windows Graphics Capture sessions.
   *
   * @param[out] item Reference to a GraphicsCaptureItem that will be set on success.
   * @return true if the GraphicsCaptureItem was successfully created; false otherwise.
   */
  bool create_graphics_capture_item(GraphicsCaptureItem &item) {
    if (!_selected_monitor) {
      return false;
    }

    auto activation_factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    HRESULT hr = activation_factory->CreateForMonitor(_selected_monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem for monitor: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Calculates the final capture resolution based on config, monitor info, and WGC item size.
   *
   * Chooses resolution in this order:
   *
   * - Uses config width/height if valid.
   *
   * - Otherwise uses monitor logical size.
   *
   * - If WGC item size differs significantly (DPI scaling), uses WGC physical size to avoid cropping/zoom.
   *
   * @param config The configuration data with requested width/height.
   * @param config_received True if config data was received.
   * @param item The GraphicsCaptureItem for the selected monitor.
   */
  void configure_capture_resolution(const GraphicsCaptureItem &item) {
    // Get actual WGC item size to ensure we capture full desktop (fixes zoomed display issue)
    auto item_size = item.Size();
    _height = item_size.Height;
    _width = item_size.Width;
  }

  /**
   * @brief Gets the handle to the selected monitor.
   * @return HMONITOR handle of the selected monitor, or nullptr if none selected.
   */
  HMONITOR get_selected_monitor() const {
    return _selected_monitor;
  }

  /**
   * @brief Gets the configured capture width.
   * @return Capture width in pixels.
   */
  UINT get_width() const {
    return _width;
  }

  /**
   * @brief Gets the configured capture height.
   * @return Capture height in pixels.
   */
  UINT get_height() const {
    return _height;
  }
};

/**
 * @brief Shared resource management class to handle texture, memory mapping, and events.
 *
 * This class manages shared D3D11 resources for inter-process communication with the main
 * Sunshine process. It creates a shared texture with a keyed mutex plus a shared metadata
 * view and frame-ready event for realtime cross-process delivery.
 */
class SharedResourceManager {
private:
  std::array<winrt::com_ptr<ID3D11Texture2D>, platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT> _shared_textures;  ///< Shared D3D11 texture ring for frame data
  std::array<winrt::com_ptr<IDXGIKeyedMutex>, platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT> _keyed_mutexes;  ///< Keyed mutexes for synchronization
  std::array<winrt::handle, platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT> _shared_handles;  ///< Shared handles for cross-process sharing
  winrt::handle _frame_ready_event;  ///< Auto-reset event signaled after each published frame
  winrt::handle _frame_metadata_mapping;  ///< Shared memory containing latest frame metadata
  platf::dxgi::frame_metadata_t *_frame_metadata = nullptr;  ///< Mapped frame metadata view
  UINT _width = 0;  ///< Texture width in pixels
  UINT _height = 0;  ///< Texture height in pixels

public:
  /**
   * @brief Default constructor for SharedResourceManager.
   */
  SharedResourceManager() = default;

  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  SharedResourceManager(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  SharedResourceManager &operator=(const SharedResourceManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  SharedResourceManager(SharedResourceManager &&) = delete;

  /**
   * @brief Deleted move assignment operator to prevent resource transfer issues.
   */
  SharedResourceManager &operator=(SharedResourceManager &&) = delete;

  /**
   * @brief Creates a shared D3D11 texture with keyed mutex for inter-process sharing.
   *
   * @param device Pointer to the D3D11 device used for texture creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if the texture was successfully created; false otherwise.
   */
  bool create_shared_texture(const winrt::com_ptr<ID3D11Device> &device, size_t slot, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    _width = texture_width;
    _height = texture_height;

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = _width;
    tex_desc.Height = _height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // Use NT shared handles exclusively
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, _shared_textures[slot].put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create NT shared texture: " << hr;
      return false;
    }
    return true;
  }

  /**
   * @brief Acquires a keyed mutex interface from the shared texture for synchronization.
   * @return true if the keyed mutex was successfully acquired; false otherwise.
   */
  bool create_keyed_mutex(size_t slot) {
    if (!_shared_textures[slot]) {
      return false;
    }

    _keyed_mutexes[slot] = _shared_textures[slot].try_as<IDXGIKeyedMutex>();
    if (!_keyed_mutexes[slot]) {
      BOOST_LOG(error) << "Failed to get keyed mutex";
      return false;
    }
    return true;
  }

  /**
   * @brief Creates a shared handle for the texture resource.
   *
   * @return true if the handle was successfully created; false otherwise.
   */
  bool create_shared_handle(size_t slot) {
    if (!_shared_textures[slot]) {
      BOOST_LOG(error) << "Cannot create shared handle - no shared texture available";
      return false;
    }

    winrt::com_ptr<IDXGIResource1> dxgi_resource1 = _shared_textures[slot].try_as<IDXGIResource1>();
    if (!dxgi_resource1) {
      BOOST_LOG(error) << "Failed to query DXGI resource1 interface";
      return false;
    }

    // Create the shared handle
    HRESULT hr = dxgi_resource1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, _shared_handles[slot].put());
    if (FAILED(hr)) {
      BOOST_LOG(error) << "Failed to create shared handle: " << hr;
      return false;
    }
    return true;
  }

  bool create_frame_signal() {
    _frame_ready_event = winrt::handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!_frame_ready_event) {
      BOOST_LOG(error) << "Failed to create frame-ready event: " << GetLastError();
      return false;
    }

    _frame_metadata_mapping = winrt::handle(CreateFileMappingW(
      INVALID_HANDLE_VALUE,
      nullptr,
      PAGE_READWRITE,
      0,
      static_cast<DWORD>(sizeof(platf::dxgi::frame_metadata_t)),
      nullptr
    ));
    if (!_frame_metadata_mapping) {
      BOOST_LOG(error) << "Failed to create frame metadata mapping: " << GetLastError();
      return false;
    }

    _frame_metadata = static_cast<platf::dxgi::frame_metadata_t *>(MapViewOfFile(
      _frame_metadata_mapping.get(),
      FILE_MAP_READ | FILE_MAP_WRITE,
      0,
      0,
      sizeof(platf::dxgi::frame_metadata_t)
    ));
    if (!_frame_metadata) {
      BOOST_LOG(error) << "Failed to map frame metadata view: " << GetLastError();
      return false;
    }

    _frame_metadata->sequence = 0;
    _frame_metadata->frame_id = 0;
    _frame_metadata->frame_qpc = 0;
    _frame_metadata->texture_slot = 0;
    for (auto &slot : _frame_metadata->slots) {
      slot.state = static_cast<LONG>(platf::dxgi::wgc_texture_slot_state_e::free);
      slot.reserved = 0;
      slot.frame_id = 0;
      slot.frame_qpc = 0;
    }
    return true;
  }

  /**
   * @brief Initializes all shared resource components: texture, keyed mutex, and handle.
   *
   * @param device Pointer to the D3D11 device used for resource creation.
   * @param texture_width Width of the texture in pixels.
   * @param texture_height Height of the texture in pixels.
   * @param format DXGI format for the texture.
   * @return true if all resources were successfully initialized; false otherwise.
   */
  bool initialize_all(const winrt::com_ptr<ID3D11Device> &device, UINT texture_width, UINT texture_height, DXGI_FORMAT format) {
    for (size_t slot = 0; slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      if (!create_shared_texture(device, slot, texture_width, texture_height, format) ||
          !create_keyed_mutex(slot) ||
          !create_shared_handle(slot)) {
        return false;
      }
    }
    return create_frame_signal();
  }

  /**
   * @brief Gets the shared handle data for inter-process sharing.
   * @return shared_handle_data_t struct containing the shared handle and texture dimensions.
   */
  platf::dxgi::shared_handle_data_t get_shared_handle_data() const {
    platf::dxgi::shared_handle_data_t data = {};
    for (size_t slot = 0; slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      data.texture_handles[slot] = const_cast<HANDLE>(_shared_handles[slot].get());
    }
    data.frame_event_handle = const_cast<HANDLE>(_frame_ready_event.get());
    data.frame_metadata_handle = const_cast<HANDLE>(_frame_metadata_mapping.get());
    data.width = _width;
    data.height = _height;
    return data;
  }

  struct texture_slot_reservation_t {
    size_t slot = platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT;
    LONG previous_state = static_cast<LONG>(platf::dxgi::wgc_texture_slot_state_e::free);
  };

  std::optional<texture_slot_reservation_t> reserve_texture_slot() {
    if (!_frame_metadata) {
      return std::nullopt;
    }

    const auto ready_state = static_cast<LONG>(platf::dxgi::wgc_texture_slot_state_e::ready);
    for (size_t slot = 0; slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      if (platf::dxgi::transition_wgc_texture_slot(
            _frame_metadata->slots[slot],
            platf::dxgi::wgc_texture_slot_state_e::free,
            platf::dxgi::wgc_texture_slot_state_e::writing
          )) {
        return texture_slot_reservation_t {
          .slot = slot,
          .previous_state = static_cast<LONG>(platf::dxgi::wgc_texture_slot_state_e::free)
        };
      }
    }

    // A ready slot has not been claimed by Sunshine yet. Reclaim the oldest
    // one so a stalled consumer still receives the newest available frame.
    size_t oldest_slot = platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT;
    LONG64 oldest_frame_id = (std::numeric_limits<LONG64>::max)();
    for (size_t slot = 0; slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      if (platf::dxgi::wgc_texture_slot_state(_frame_metadata->slots[slot]) != ready_state) {
        continue;
      }
      const auto frame_id = InterlockedCompareExchange64(&_frame_metadata->slots[slot].frame_id, 0, 0);
      if (frame_id < oldest_frame_id) {
        oldest_frame_id = frame_id;
        oldest_slot = slot;
      }
    }

    if (oldest_slot < platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT &&
        platf::dxgi::transition_wgc_texture_slot(
          _frame_metadata->slots[oldest_slot],
          platf::dxgi::wgc_texture_slot_state_e::ready,
          platf::dxgi::wgc_texture_slot_state_e::writing
        )) {
      return texture_slot_reservation_t {.slot = oldest_slot, .previous_state = ready_state};
    }

    return std::nullopt;
  }

  void abandon_texture_slot(const texture_slot_reservation_t &reservation) {
    if (!_frame_metadata || reservation.slot >= platf::dxgi::WGC_IPC_TEXTURE_SLOT_COUNT) {
      return;
    }

    (void) platf::dxgi::transition_wgc_texture_slot(
      _frame_metadata->slots[reservation.slot],
      platf::dxgi::wgc_texture_slot_state_e::writing,
      static_cast<platf::dxgi::wgc_texture_slot_state_e>(reservation.previous_state)
    );
  }

  void publish_frame_metadata(uint64_t frame_qpc, size_t texture_slot) {
    if (!_frame_metadata) {
      return;
    }

    InterlockedIncrement64(&_frame_metadata->sequence);
    const auto frame_id = InterlockedCompareExchange64(&_frame_metadata->frame_id, 0, 0) + 1;
    auto &slot = _frame_metadata->slots[texture_slot];
    InterlockedExchange64(&slot.frame_qpc, static_cast<LONG64>(frame_qpc));
    InterlockedExchange64(&slot.frame_id, frame_id);
    InterlockedExchange(&slot.state, static_cast<LONG>(platf::dxgi::wgc_texture_slot_state_e::ready));
    InterlockedExchange64(&_frame_metadata->frame_qpc, static_cast<LONG64>(frame_qpc));
    InterlockedExchange64(&_frame_metadata->texture_slot, static_cast<LONG64>(texture_slot));
    InterlockedExchange64(&_frame_metadata->frame_id, frame_id);
    InterlockedIncrement64(&_frame_metadata->sequence);
  }

  void signal_frame_ready() {
    if (!_frame_ready_event) {
      return;
    }

    SetEvent(_frame_ready_event.get());
  }

  /**
   * @brief Gets the underlying shared D3D11 texture pointer.
   * @return Pointer to the managed ID3D11Texture2D, or nullptr if not initialized.
   */
  const winrt::com_ptr<ID3D11Texture2D> &get_shared_texture(size_t slot) const {
    return _shared_textures[slot];
  }

  /**
   * @brief Gets the keyed mutex interface com_ptr for the shared texture.
   * @return const winrt::com_ptr<IDXGIKeyedMutex>& (may be empty if not initialized).
   */
  const winrt::com_ptr<IDXGIKeyedMutex> &get_keyed_mutex(size_t slot) const {
    return _keyed_mutexes[slot];
  }

  /**
   * @brief Destructor for SharedResourceManager.
   * Uses RAII to automatically dispose resources
   */
  ~SharedResourceManager() {
    if (_frame_metadata) {
      UnmapViewOfFile(_frame_metadata);
      _frame_metadata = nullptr;
    }
  }
};

/**
 * @brief WGC capture management class to handle frame pool, capture session, and frame processing.
 *
 * This class manages the core Windows Graphics Capture functionality including:
 * - Frame pool creation and management with dynamic buffer sizing
 * - Capture session lifecycle management
 * - Frame arrival event handling and processing
 * - Frame rate optimization and adaptive buffering
 * - Integration with shared texture resources and event signaling for inter-process communication
 */
struct WgcCaptureDependencies {
  // Required devices/resources
  IDirect3DDevice winrt_device;  // WinRT Direct3D device (value-type COM handle)
  GraphicsCaptureItem graphics_item;  // Target capture item
  SharedResourceManager &resource_manager;  // Shared inter-process texture/mutex manager
  winrt::com_ptr<ID3D11DeviceContext> d3d_context;  // D3D11 context for copies
};

class WgcCaptureManager {
private:
  static constexpr auto WGC_DIAGNOSTICS_LOG_INTERVAL = std::chrono::seconds(15);
  static constexpr auto WGC_DRAIN_LOG_INTERVAL = std::chrono::seconds(5);
  static constexpr uint64_t WGC_TIMING_SAMPLE_PERIOD = 120;

  std::atomic<bool> _shutting_down {false};
  Direct3D11CaptureFramePool _frame_pool = nullptr;  ///< WinRT frame pool for capture operations
  GraphicsCaptureSession _capture_session = nullptr;  ///< WinRT capture session for monitor/window capture
  winrt::event_token _frame_arrived_token {};  ///< Event token for frame arrival notifications
  std::optional<WgcCaptureDependencies> _deps;  ///< Dependencies for frame processing

  uint32_t _initial_buffer_size = 1;  ///< Minimum steady-state frame buffer size for this stream
  uint32_t _current_buffer_size = 1;  ///< Current frame buffer size for dynamic adjustment
  uint32_t _max_buffer_size = 4;  ///< Maximum allowed buffer size for adaptive growth
  static constexpr uint32_t ABSOLUTE_MAX_BUFFER_SIZE = 4;  ///< Hard cap to bound latency/VRAM

  std::deque<std::chrono::steady_clock::time_point> _drop_timestamps;  ///< Timestamps of recent frame drops for analysis
  std::atomic<int> _outstanding_frames {0};  ///< Number of frames currently being processed
  std::atomic<int> _peak_outstanding {0};  ///< Peak number of outstanding frames (for monitoring)
  std::atomic<int64_t> _last_frame_pool_pressure_us {0};  ///< Last source drop/drain pressure signal
  std::atomic<int64_t> _last_drain_log_us {0};  ///< Last rate-limited drain log timestamp
  std::atomic<int64_t> _last_diagnostics_log_us {0};  ///< Last aggregate diagnostics log timestamp
  std::chrono::steady_clock::time_point _last_quiet_start = std::chrono::steady_clock::now();  ///< Last time frame processing became quiet
  std::chrono::steady_clock::time_point _last_buffer_check = std::chrono::steady_clock::now();  ///< Last time buffer size was checked
  std::atomic<uint64_t> _captured_frames {0};
  std::atomic<uint64_t> _frame_pool_empty_drops {0};
  std::atomic<uint64_t> _drained_pool_frames {0};
  std::atomic<uint64_t> _ring_slot_drops {0};
  std::atomic<uint64_t> _slow_context_waits {0};
  std::atomic<uint64_t> _slow_mutex_waits {0};
  std::atomic<uint64_t> _slow_shared_mutex_holds {0};
  std::atomic<uint64_t> _slow_copy_submissions {0};
  std::atomic<uint64_t> _published_frames {0};
  std::atomic<uint64_t> _full_copies {0};
  std::atomic<uint64_t> _partial_copies {0};
  std::atomic<uint64_t> _no_change_skips {0};
  std::atomic<uint64_t> _copied_pixels {0};
  std::atomic<uint64_t> _timing_samples {0};
  std::atomic<uint64_t> _sampled_context_wait_us {0};
  std::atomic<uint64_t> _sampled_mutex_wait_us {0};
  std::atomic<uint64_t> _sampled_mutex_hold_us {0};
  std::atomic<uint64_t> _sampled_copy_submit_us {0};
  uint64_t _last_diagnostics_captured_frames = 0;
  uint64_t _last_diagnostics_published_frames = 0;
  uint64_t _last_diagnostics_empty_drops = 0;
  uint64_t _last_diagnostics_drained_frames = 0;
  uint64_t _last_diagnostics_ring_slot_drops = 0;
  uint64_t _last_diagnostics_slow_context = 0;
  uint64_t _last_diagnostics_slow_mutex = 0;
  uint64_t _last_diagnostics_slow_hold = 0;
  uint64_t _last_diagnostics_slow_copy = 0;
  uint64_t _last_diagnostics_full_copies = 0;
  uint64_t _last_diagnostics_partial_copies = 0;
  uint64_t _last_diagnostics_no_change_skips = 0;
  uint64_t _last_diagnostics_copied_pixels = 0;
  uint64_t _last_diagnostics_timing_samples = 0;
  uint64_t _last_diagnostics_context_wait_us = 0;
  uint64_t _last_diagnostics_mutex_wait_us = 0;
  uint64_t _last_diagnostics_mutex_hold_us = 0;
  uint64_t _last_diagnostics_copy_submit_us = 0;
  std::mutex _d3d_context_mutex;
  std::mutex _damage_mutex;
  DXGI_FORMAT _capture_format = DXGI_FORMAT_UNKNOWN;  ///< DXGI format for captured frames
  UINT _height = 0;  ///< Capture height in pixels
  UINT _width = 0;  ///< Capture width in pixels
  wgc_texture_ring_damage_tracker_t _damage_tracker;
  std::atomic<bool> _dirty_regions_enabled {false};

public:
  /**
   * @brief Constructor for WgcCaptureManager.
   *
   * Initializes the capture manager for the given dimensions and capture format and
   * stores the dependencies bundle used during capture operations.
   *
   * @param capture_format DXGI format for captured frames.
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   * @param deps Bundle of dependencies: winrt IDirect3DDevice, GraphicsCaptureItem,
   *             reference to SharedResourceManager and D3D11 context com_ptr.
   */
  WgcCaptureManager(DXGI_FORMAT capture_format, UINT width, UINT height, WgcCaptureDependencies deps):
      _deps(std::move(deps)),
      _capture_format(capture_format),
      _height(height),
      _width(width),
      _damage_tracker(width, height) {
    _max_buffer_size = std::clamp<uint32_t>(
      g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : ABSOLUTE_MAX_BUFFER_SIZE,
      1,
      ABSOLUTE_MAX_BUFFER_SIZE
    );
    _initial_buffer_size = std::clamp<uint32_t>(
      g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
      1,
      _max_buffer_size
    );
    _current_buffer_size = _initial_buffer_size;

    const auto now = std::chrono::steady_clock::now();
    _last_quiet_start = now;
    _last_buffer_check = now;
    _last_diagnostics_log_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  }

  /**
   * @brief Destructor for WgcCaptureManager.
   *
   * Automatically cleans up capture session and frame pool resources.
   */
  ~WgcCaptureManager() noexcept {
    shutdown();
  }

  void shutdown() noexcept {
    if (_shutting_down.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    cleanup_capture_session();
    cleanup_frame_pool();

    // Ensure any in-flight FrameArrived callbacks have finished before destructing.
    // This avoids use-after-free on shutdown paths triggered by display reinit/HDR changes.
    auto last_log = std::chrono::steady_clock::now();
    while (_outstanding_frames.load(std::memory_order_acquire) > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_log > std::chrono::seconds(2)) {
        last_log = now;
        BOOST_LOG(warning) << "Waiting for " << _outstanding_frames.load(std::memory_order_relaxed)
                           << " in-flight frame(s) to finish before shutdown...";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

private:
  void cleanup_capture_session() noexcept {
    try {
      if (_capture_session) {
        _capture_session.Close();
        _capture_session = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _capture_session.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _capture_session.Close()";
    }
  }

  void cleanup_frame_pool() noexcept {
    try {
      if (_frame_pool) {
        if (_frame_arrived_token.value != 0) {
          _frame_pool.FrameArrived(_frame_arrived_token);  // Remove handler
          _frame_arrived_token.value = 0;
        }
        _frame_pool.Close();
        _frame_pool = nullptr;
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "Exception during _frame_pool.Close(): " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      BOOST_LOG(error) << "Unknown exception during _frame_pool.Close()";
    }
  }

public:
  /**
   * @brief Deleted copy constructor to prevent resource duplication.
   */
  WgcCaptureManager(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted copy assignment operator to prevent resource duplication.
   */
  WgcCaptureManager &operator=(const WgcCaptureManager &) = delete;

  /**
   * @brief Deleted move constructor to prevent resource transfer issues.
   */
  WgcCaptureManager(WgcCaptureManager &&) = delete;
  WgcCaptureManager &operator=(WgcCaptureManager &&) = delete;

  /**
   * @brief Recreates the frame pool with a new buffer size for dynamic adjustment.
   * @param buffer_size The number of frames to buffer.
   * @returns true if the frame pool was recreated successfully, false otherwise.
   */
  bool create_or_adjust_frame_pool(uint32_t buffer_size) {
    if (!_deps || !_deps->winrt_device || _capture_format == DXGI_FORMAT_UNKNOWN) {
      return false;
    }

    if (_frame_pool) {
      // Use the proper Recreate method instead of closing and re-creating
      try {
        _frame_pool.Recreate(
          _deps->winrt_device,
          (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
          buffer_size,
          SizeInt32 {static_cast<int32_t>(_width), static_cast<int32_t>(_height)}
        );

        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool recreated with buffer size: " << buffer_size;
        return true;
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(error) << "Failed to recreate frame pool: " << ex.code() << " - " << winrt::to_string(ex.message());
        return false;
      }
    } else {
      // Initial creation case - create new frame pool
      _frame_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        _deps->winrt_device,
        (_capture_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized,
        buffer_size,
        SizeInt32 {static_cast<int32_t>(_width), static_cast<int32_t>(_height)}
      );

      if (_frame_pool) {
        _current_buffer_size = buffer_size;
        BOOST_LOG(info) << "Frame pool created with buffer size: " << buffer_size;
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Processes a frame when the frame arrived event is triggered.
   * @param sender The frame pool that triggered the event.
   */
  void process_frame(Direct3D11CaptureFramePool const &sender) {
    // Track callback invocations
    static std::atomic<uint64_t> callback_count {0};
    auto cc = ++callback_count;
    if (cc == 1) {
      BOOST_LOG(info) << "First FrameArrived callback invoked";
    }

    Direct3D11CaptureFrame frame = nullptr;
    uint32_t drained_frames = 0;

    try {
      if (drain_to_latest()) {
        while (auto next_frame = sender.TryGetNextFrame()) {
          if (frame) {
            accumulate_dirty_regions(frame);
            ++drained_frames;
          }
          frame = std::move(next_frame);
        }
      } else {
        frame = sender.TryGetNextFrame();
      }
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "WinRT error retrieving WGC frame: " << ex.code() << " - " << winrt::to_string(ex.message());
      return;
    }

    if (!frame) {
      // Frame drop detected - record timestamp for sliding window analysis
      auto now = std::chrono::steady_clock::now();
      _drop_timestamps.push_back(now);
      record_frame_pool_pressure(now);

      const auto total_empty_drops = _frame_pool_empty_drops.fetch_add(1, std::memory_order_relaxed) + 1;
      if (total_empty_drops <= 5 || total_empty_drops % 120 == 0) {
        BOOST_LOG(info) << "WGC frame pool returned no frame"
                        << " (recent drops in 5s window=" << _drop_timestamps.size()
                        << ", total=" << total_empty_drops << ")";
      }
    } else {
      // Frame successfully retrieved
      try {
        accumulate_dirty_regions(frame);
        auto surface = frame.Surface();

        // Get frame timing information from the WGC frame
        uint64_t frame_qpc = frame.SystemRelativeTime().count();
        const bool sample_timing = record_frame_arrival(drained_frames);
        queue_frame_for_delivery(std::move(frame), surface, frame_qpc, sample_timing);
      } catch (const winrt::hresult_error &ex) {
        // Log error
        BOOST_LOG(error) << "WinRT error in frame processing: " << ex.code() << " - " << winrt::to_string(ex.message());
      }
    }

    // Check if we need to adjust frame buffer size
    check_and_adjust_frame_buffer();
  }

private:
  bool drain_to_latest() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) != 0;
  }

  bool allow_buffer_decrease() const {
    return (g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) != 0;
  }

  static int64_t steady_time_us(const std::chrono::steady_clock::time_point &time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
  }

  void record_frame_pool_pressure(const std::chrono::steady_clock::time_point &now) {
    _last_frame_pool_pressure_us.store(steady_time_us(now), std::memory_order_relaxed);
  }

  static bool should_log_rate_limited(std::atomic<int64_t> &last_log_us, const std::chrono::steady_clock::time_point &now, std::chrono::microseconds interval) {
    const auto now_us = steady_time_us(now);
    auto last_us = last_log_us.load(std::memory_order_relaxed);
    while (last_us == 0 || now_us - last_us >= interval.count()) {
      if (last_log_us.compare_exchange_weak(last_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  double expected_frame_ms() const {
    return 1000.0 / static_cast<double>(std::max(1, g_config.target_fps));
  }

  double approximate_extra_pool_latency_ms(uint32_t buffer_size) const {
    return buffer_size > _initial_buffer_size ?
             static_cast<double>(buffer_size - _initial_buffer_size) * expected_frame_ms() :
             0.0;
  }

  bool record_frame_arrival(uint32_t drained_frames) {
    const auto captured_frames = _captured_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    if (drained_frames > 0) {
      const auto now = std::chrono::steady_clock::now();
      record_frame_pool_pressure(now);
      const auto total_drained = _drained_pool_frames.fetch_add(drained_frames, std::memory_order_relaxed) + drained_frames;
      if (should_log_rate_limited(_last_drain_log_us, now, std::chrono::duration_cast<std::chrono::microseconds>(WGC_DRAIN_LOG_INTERVAL))) {
        BOOST_LOG(info) << "WGC drained " << drained_frames << " queued frame(s) from frame pool"
                        << " (total_drained=" << total_drained
                        << ", buffer=" << _current_buffer_size << "/" << _max_buffer_size
                        << ", approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size) << ")";
      }
    }

    return captured_frames == 1 || captured_frames % WGC_TIMING_SAMPLE_PERIOD == 0;
  }

  void disable_dirty_regions(std::string_view reason) {
    if (!_dirty_regions_enabled.exchange(false, std::memory_order_acq_rel)) {
      return;
    }

    std::lock_guard lock(_damage_mutex);
    _damage_tracker.invalidate();
    BOOST_LOG(warning) << "WGC dirty-region capture disabled; using full copies (" << reason << ')';
  }

  void accumulate_dirty_regions(const Direct3D11CaptureFrame &frame) {
    if (!_dirty_regions_enabled.load(std::memory_order_acquire)) {
      return;
    }

    try {
      auto frame2 = frame.try_as<IDirect3D11CaptureFrame2>();
      if (!frame2 || frame2.DirtyRegionMode() != GraphicsCaptureDirtyRegionMode::ReportOnly) {
        disable_dirty_regions("frame did not report ReportOnly damage");
        return;
      }

      std::lock_guard lock(_damage_mutex);
      if (!_dirty_regions_enabled.load(std::memory_order_relaxed)) {
        return;
      }
      for (const auto &region : frame2.DirtyRegions()) {
        const wgc_damage_rect_t damage {
          .x = region.X,
          .y = region.Y,
          .width = region.Width,
          .height = region.Height
        };
        if (!_damage_tracker.accumulate(std::span<const wgc_damage_rect_t> {&damage, 1})) {
          _dirty_regions_enabled.store(false, std::memory_order_release);
          BOOST_LOG(warning) << "WGC reported an invalid dirty region; using full copies";
          return;
        }
      }
    } catch (const winrt::hresult_error &ex) {
      disable_dirty_regions("dirty-region query failed");
      BOOST_LOG(debug) << "WGC dirty-region query failed: " << ex.code() << " - " << winrt::to_string(ex.message());
    } catch (...) {
      disable_dirty_regions("dirty-region query threw");
    }
  }

  void maybe_log_capture_diagnostics(const std::chrono::steady_clock::time_point &now) {
    const auto now_us = steady_time_us(now);
    auto last_log_us = _last_diagnostics_log_us.load(std::memory_order_relaxed);
    const auto interval_us = std::chrono::duration_cast<std::chrono::microseconds>(WGC_DIAGNOSTICS_LOG_INTERVAL).count();

    while (now_us - last_log_us >= interval_us) {
      if (!_last_diagnostics_log_us.compare_exchange_weak(last_log_us, now_us, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        continue;
      }

      const double interval_s = static_cast<double>(now_us - last_log_us) / 1000000.0;
      if (interval_s <= 0.0) {
        return;
      }

      const auto captured = _captured_frames.load(std::memory_order_relaxed);
      const auto published = _published_frames.load(std::memory_order_relaxed);
      const auto empty_drops = _frame_pool_empty_drops.load(std::memory_order_relaxed);
      const auto drained = _drained_pool_frames.load(std::memory_order_relaxed);
      const auto ring_slot_drops = _ring_slot_drops.load(std::memory_order_relaxed);
      const auto slow_context = _slow_context_waits.load(std::memory_order_relaxed);
      const auto slow_mutex = _slow_mutex_waits.load(std::memory_order_relaxed);
      const auto slow_hold = _slow_shared_mutex_holds.load(std::memory_order_relaxed);
      const auto slow_copy = _slow_copy_submissions.load(std::memory_order_relaxed);
      const auto full_copies = _full_copies.load(std::memory_order_relaxed);
      const auto partial_copies = _partial_copies.load(std::memory_order_relaxed);
      const auto no_change_skips = _no_change_skips.load(std::memory_order_relaxed);
      const auto copied_pixels = _copied_pixels.load(std::memory_order_relaxed);
      const auto timing_samples = _timing_samples.load(std::memory_order_relaxed);
      const auto context_wait_us = _sampled_context_wait_us.load(std::memory_order_relaxed);
      const auto mutex_wait_us = _sampled_mutex_wait_us.load(std::memory_order_relaxed);
      const auto mutex_hold_us = _sampled_mutex_hold_us.load(std::memory_order_relaxed);
      const auto copy_submit_us = _sampled_copy_submit_us.load(std::memory_order_relaxed);

      const auto captured_delta = captured - _last_diagnostics_captured_frames;
      const auto published_delta = published - _last_diagnostics_published_frames;
      const auto empty_drop_delta = empty_drops - _last_diagnostics_empty_drops;
      const auto drained_delta = drained - _last_diagnostics_drained_frames;
      const auto ring_slot_drop_delta = ring_slot_drops - _last_diagnostics_ring_slot_drops;
      const auto slow_context_delta = slow_context - _last_diagnostics_slow_context;
      const auto slow_mutex_delta = slow_mutex - _last_diagnostics_slow_mutex;
      const auto slow_hold_delta = slow_hold - _last_diagnostics_slow_hold;
      const auto slow_copy_delta = slow_copy - _last_diagnostics_slow_copy;
      const auto full_copy_delta = full_copies - _last_diagnostics_full_copies;
      const auto partial_copy_delta = partial_copies - _last_diagnostics_partial_copies;
      const auto no_change_skip_delta = no_change_skips - _last_diagnostics_no_change_skips;
      const auto copied_pixels_delta = copied_pixels - _last_diagnostics_copied_pixels;
      const auto timing_sample_delta = timing_samples - _last_diagnostics_timing_samples;
      const auto context_wait_us_delta = context_wait_us - _last_diagnostics_context_wait_us;
      const auto mutex_wait_us_delta = mutex_wait_us - _last_diagnostics_mutex_wait_us;
      const auto mutex_hold_us_delta = mutex_hold_us - _last_diagnostics_mutex_hold_us;
      const auto copy_submit_us_delta = copy_submit_us - _last_diagnostics_copy_submit_us;

      _last_diagnostics_captured_frames = captured;
      _last_diagnostics_published_frames = published;
      _last_diagnostics_empty_drops = empty_drops;
      _last_diagnostics_drained_frames = drained;
      _last_diagnostics_ring_slot_drops = ring_slot_drops;
      _last_diagnostics_slow_context = slow_context;
      _last_diagnostics_slow_mutex = slow_mutex;
      _last_diagnostics_slow_hold = slow_hold;
      _last_diagnostics_slow_copy = slow_copy;
      _last_diagnostics_full_copies = full_copies;
      _last_diagnostics_partial_copies = partial_copies;
      _last_diagnostics_no_change_skips = no_change_skips;
      _last_diagnostics_copied_pixels = copied_pixels;
      _last_diagnostics_timing_samples = timing_samples;
      _last_diagnostics_context_wait_us = context_wait_us;
      _last_diagnostics_mutex_wait_us = mutex_wait_us;
      _last_diagnostics_mutex_hold_us = mutex_hold_us;
      _last_diagnostics_copy_submit_us = copy_submit_us;

      const auto copied_pixel_ratio = captured_delta > 0 ?
                                        static_cast<double>(copied_pixels_delta) /
                                          (static_cast<double>(captured_delta) * static_cast<uint64_t>(_width) * _height) :
                                        0.0;
      const auto sampled_average_ms = [timing_sample_delta](uint64_t total_us) {
        return timing_sample_delta > 0 ?
                 static_cast<double>(total_us) / timing_sample_delta / 1000.0 :
                 0.0;
      };

      BOOST_LOG(info) << "WGC capture diagnostics: interval_s=" << interval_s
                      << " buffer=" << _current_buffer_size << "/" << _max_buffer_size
                      << " approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(_current_buffer_size)
                      << " capture_fps=" << (static_cast<double>(captured_delta) / interval_s)
                      << " publish_fps=" << (static_cast<double>(published_delta) / interval_s)
                      << " drained=" << drained_delta
                      << " empty_drops=" << empty_drop_delta
                      << " ring_slot_drops=" << ring_slot_drop_delta
                      << " full_copies=" << full_copy_delta
                      << " partial_copies=" << partial_copy_delta
                      << " no_change_skips=" << no_change_skip_delta
                      << " copied_pixel_ratio=" << copied_pixel_ratio
                      << " timing_samples=" << timing_sample_delta
                      << " sampled_context_wait_ms=" << sampled_average_ms(context_wait_us_delta)
                      << " sampled_mutex_wait_ms=" << sampled_average_ms(mutex_wait_us_delta)
                      << " sampled_mutex_hold_ms=" << sampled_average_ms(mutex_hold_us_delta)
                      << " sampled_copy_submit_ms=" << sampled_average_ms(copy_submit_us_delta)
                      << " slow_context=" << slow_context_delta
                      << " slow_mutex=" << slow_mutex_delta
                      << " slow_shared_hold=" << slow_hold_delta
                      << " slow_copy=" << slow_copy_delta;
      return;
    }
  }

  /**
   * @brief Prunes old frame drop timestamps from the sliding window.
   * @param now Current timestamp for comparison.
   */
  void prune_old_drop_timestamps(const std::chrono::steady_clock::time_point &now) {
    while (!_drop_timestamps.empty() && now - _drop_timestamps.front() > std::chrono::seconds(5)) {
      _drop_timestamps.pop_front();
    }
  }

  /**
   * @brief Checks if buffer size should be increased due to recent frame drops.
   * @param now Current timestamp.
   * @return true if buffer was increased, false otherwise.
   */
  bool try_increase_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (_drop_timestamps.size() >= 2 && _current_buffer_size < _max_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size + 1;
      BOOST_LOG(info) << "Detected " << _drop_timestamps.size() << " frame drops in 5s window, increasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _drop_timestamps.clear();  // Reset after adjustment
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }
    return false;
  }

  /**
   * @brief Checks if buffer size should be decreased due to sustained quiet period.
   * @param now Current timestamp.
   * @return true if buffer was decreased, false otherwise.
   */
  bool try_decrease_buffer_size(const std::chrono::steady_clock::time_point &now) {
    if (!allow_buffer_decrease()) {
      return false;
    }

    constexpr auto quiet_period = std::chrono::seconds(30);
    const auto last_pressure_us = _last_frame_pool_pressure_us.load(std::memory_order_relaxed);
    const auto quiet_period_us = std::chrono::duration_cast<std::chrono::microseconds>(quiet_period).count();
    const bool recent_pool_pressure = last_pressure_us > 0 &&
                                      steady_time_us(now) - last_pressure_us < quiet_period_us;
    bool is_quiet = _drop_timestamps.empty() &&
                    !recent_pool_pressure &&
                    _peak_outstanding.load() <= static_cast<int>(_current_buffer_size) - 1;

    if (!is_quiet) {
      _last_quiet_start = now;  // Reset quiet timer
      return false;
    }

    // Check if we've been quiet for 30 seconds
    if (now - _last_quiet_start >= quiet_period && _current_buffer_size > _initial_buffer_size) {
      uint32_t new_buffer_size = _current_buffer_size - 1;
      BOOST_LOG(info) << "Sustained quiet period (30s) with peak occupancy " << _peak_outstanding.load()
                      << " <= " << (_current_buffer_size - 1) << ", decreasing buffer from "
                      << _current_buffer_size << " to " << new_buffer_size
                      << " (approx_extra_pool_latency_ms=" << approximate_extra_pool_latency_ms(new_buffer_size) << ")";
      create_or_adjust_frame_pool(new_buffer_size);
      _peak_outstanding = 0;  // Reset peak tracking
      _last_quiet_start = now;  // Reset quiet timer
      return true;
    }

    return false;
  }

  /**
   * @brief Copies the WGC frame directly into an immediately available shared IPC slot.
   * @param frame The WGC frame object, kept alive until the copy has been submitted.
   * @param surface The captured D3D11 surface.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   */
  void queue_frame_for_delivery(Direct3D11CaptureFrame frame, winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface surface, uint64_t frame_qpc, bool sample_timing) {
    if (!_deps) {
      return;
    }

    // Get DXGI access
    winrt::com_ptr<IDirect3DDxgiInterfaceAccess> ia;
    if (FAILED(winrt::get_unknown(surface)->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), ia.put_void()))) {
      BOOST_LOG(error) << "Failed to query IDirect3DDxgiInterfaceAccess";
      return;
    }

    // Get underlying texture
    winrt::com_ptr<ID3D11Texture2D> frame_tex;
    if (FAILED(ia->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<::IUnknown **>(frame_tex.put_void())))) {
      BOOST_LOG(error) << "Failed to get ID3D11Texture2D from interface";
      return;
    }

    // Each IPC slot has an independent keyed mutex. A busy consumer only makes
    // this callback skip that slot; it never makes WGC retain a frame-pool buffer.
    copy_frame_to_shared_texture(frame_tex, frame_qpc, sample_timing);
  }

  /**
   * @brief Copies the captured texture to the shared texture and signals the main process.
   * @param frame_tex The captured D3D11 texture.
   * @param frame_qpc The QPC timestamp from when the frame was captured.
   */
  void copy_frame_to_shared_texture(const winrt::com_ptr<ID3D11Texture2D> &frame_tex, uint64_t frame_qpc, bool sample_timing) {
    if (!_deps || !frame_tex) {
      return;
    }

    // Serialize all helper D3D work before taking the cross-process keyed
    // mutex, keeping the shared-slot critical section to one copy submission.
    const auto context_wait_start = sample_timing ? std::optional {std::chrono::steady_clock::now()} : std::nullopt;
    std::unique_lock context_lock(_d3d_context_mutex);
    const auto context_wait = context_wait_start ?
                                std::optional {std::chrono::steady_clock::now() - *context_wait_start} :
                                std::nullopt;

    std::unique_lock damage_lock(_damage_mutex);
    const auto reservation = _deps->resource_manager.reserve_texture_slot();
    if (!reservation) {
      const auto ring_slot_drops = _ring_slot_drops.fetch_add(1, std::memory_order_relaxed) + 1;
      if (ring_slot_drops <= 5 || ring_slot_drops % 120 == 0) {
        BOOST_LOG(debug) << "All WGC IPC texture-ring slots are leased; dropping frame"
                         << " (count=" << ring_slot_drops << ")";
      }
      return;
    }
    const auto texture_slot = reservation->slot;

    auto copy_plan = _dirty_regions_enabled.load(std::memory_order_acquire) ?
                       _damage_tracker.plan_for_slot(texture_slot) :
                       wgc_damage_copy_plan_t {.kind = wgc_damage_copy_kind_e::full};
    if (copy_plan.kind == wgc_damage_copy_kind_e::skip) {
      _deps->resource_manager.abandon_texture_slot(*reservation);
      _no_change_skips.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const auto mutex_wait_start = sample_timing ? std::optional {std::chrono::steady_clock::now()} : std::nullopt;
    const HRESULT hr = _deps->resource_manager.get_keyed_mutex(texture_slot)->AcquireSync(0, 0);
    const auto mutex_wait = mutex_wait_start ?
                              std::optional {std::chrono::steady_clock::now() - *mutex_wait_start} :
                              std::nullopt;
    if (hr != S_OK && hr != WAIT_ABANDONED) {
      _deps->resource_manager.abandon_texture_slot(*reservation);
      if (hr != WAIT_TIMEOUT) {
        BOOST_LOG(error) << "Failed to acquire WGC IPC slot mutex: " << std::format(": 0x{:08X}", hr);
      }
      return;
    }
    if (hr == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Keyed mutex was abandoned; continuing with lock held";
    }

    // A concurrent frame may have made dirty-region reporting invalid after
    // this callback selected its plan. Prefer one conservative full copy over
    // publishing a texture with any untracked pixels.
    if (!_dirty_regions_enabled.load(std::memory_order_acquire)) {
      copy_plan = {.kind = wgc_damage_copy_kind_e::full};
    }

    const auto shared_mutex_hold_start = sample_timing ? std::optional {std::chrono::steady_clock::now()} : std::nullopt;
    auto release_shared_mutex = util::fail_guard([&]() {
      const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex(texture_slot)->ReleaseSync(0);
      if (FAILED(rel_hr)) {
        BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      }
      _deps->resource_manager.abandon_texture_slot(*reservation);
    });

    // WGC frame surfaces cannot be shared directly. A full copy initializes a
    // ring slot; otherwise a single accumulated damage bounding box refreshes
    // all changes that slot missed while it was leased.
    const auto copy_start = sample_timing ? std::optional {std::chrono::steady_clock::now()} : std::nullopt;
    auto *destination = _deps->resource_manager.get_shared_texture(texture_slot).get();
    if (copy_plan.kind == wgc_damage_copy_kind_e::full) {
      _deps->d3d_context->CopyResource(destination, frame_tex.get());
    } else {
      const auto &rect = copy_plan.rect;
      const D3D11_BOX source_box {
        static_cast<UINT>(rect.x),
        static_cast<UINT>(rect.y),
        0,
        static_cast<UINT>(rect.x + rect.width),
        static_cast<UINT>(rect.y + rect.height),
        1
      };
      _deps->d3d_context->CopySubresourceRegion(destination, 0, source_box.left, source_box.top, 0, frame_tex.get(), 0, &source_box);
    }
    const auto copy_submit = copy_start ?
                               std::optional {std::chrono::steady_clock::now() - *copy_start} :
                               std::nullopt;

    const HRESULT rel_hr = _deps->resource_manager.get_keyed_mutex(texture_slot)->ReleaseSync(0);
    release_shared_mutex.disable();
    if (FAILED(rel_hr)) {
      BOOST_LOG(warning) << "Failed to release mutex key 0: " << std::format(": 0x{:08X}", rel_hr);
      _deps->resource_manager.abandon_texture_slot(*reservation);
      return;
    }
    const auto shared_mutex_hold = shared_mutex_hold_start ?
                                     std::optional {std::chrono::steady_clock::now() - *shared_mutex_hold_start} :
                                     std::nullopt;
    _damage_tracker.mark_copied(texture_slot);
    damage_lock.unlock();
    context_lock.unlock();

    const uint64_t frame_pixels = static_cast<uint64_t>(_width) * _height;
    if (copy_plan.kind == wgc_damage_copy_kind_e::full) {
      _full_copies.fetch_add(1, std::memory_order_relaxed);
      _copied_pixels.fetch_add(frame_pixels, std::memory_order_relaxed);
    } else {
      const uint64_t copied_pixels = static_cast<uint64_t>(copy_plan.rect.width) * copy_plan.rect.height;
      _partial_copies.fetch_add(1, std::memory_order_relaxed);
      _copied_pixels.fetch_add(copied_pixels, std::memory_order_relaxed);
    }

    // Publish after releasing the keyed mutex. Once the slot is visible as ready,
    // Sunshine can lease it directly to encoder consumers without a second copy.
    _deps->resource_manager.publish_frame_metadata(frame_qpc, texture_slot);
    _deps->resource_manager.signal_frame_ready();
    const auto published = _published_frames.fetch_add(1, std::memory_order_relaxed) + 1;

    if (sample_timing) {
      const auto context_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(*context_wait).count();
      const auto mutex_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(*mutex_wait).count();
      const auto shared_mutex_hold_us = std::chrono::duration_cast<std::chrono::microseconds>(*shared_mutex_hold).count();
      const auto copy_submit_us = std::chrono::duration_cast<std::chrono::microseconds>(*copy_submit).count();
      _timing_samples.fetch_add(1, std::memory_order_relaxed);
      _sampled_context_wait_us.fetch_add(context_wait_us, std::memory_order_relaxed);
      _sampled_mutex_wait_us.fetch_add(mutex_wait_us, std::memory_order_relaxed);
      _sampled_mutex_hold_us.fetch_add(shared_mutex_hold_us, std::memory_order_relaxed);
      _sampled_copy_submit_us.fetch_add(copy_submit_us, std::memory_order_relaxed);
      if (context_wait_us > 1000) {
        _slow_context_waits.fetch_add(1, std::memory_order_relaxed);
      }
      if (mutex_wait_us > 1000) {
        _slow_mutex_waits.fetch_add(1, std::memory_order_relaxed);
      }
      if (shared_mutex_hold_us > 1000) {
        _slow_shared_mutex_holds.fetch_add(1, std::memory_order_relaxed);
      }
      if (copy_submit_us > 1000) {
        _slow_copy_submissions.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Log first frame and frame 100 for quick sanity checks, but avoid long-running spam.
    if (published == 1 || published == 100) {
      BOOST_LOG(info) << "Published frame " << published << " to main process";
    }
  }

public:
  /**
   * @brief Creates a capture session for the specified capture item.
   * @returns true if the session was created successfully, false otherwise.
   */
  bool create_capture_session() {
    if (!_frame_pool) {
      return false;
    }

    if (!_deps || !_deps->graphics_item) {
      BOOST_LOG(error) << "Cannot create capture session: missing capture item";
      return false;
    }

    _frame_arrived_token = _frame_pool.FrameArrived([this](Direct3D11CaptureFramePool const &sender, winrt::Windows::Foundation::IInspectable const &) {
      if (_shutting_down.load(std::memory_order_acquire)) {
        return;
      }

      struct outstanding_guard_t {
        std::atomic<int> &counter;
        const int value;

        explicit outstanding_guard_t(std::atomic<int> &c):
            counter(c),
            value(c.fetch_add(1, std::memory_order_acq_rel) + 1) {}

        ~outstanding_guard_t() {
          counter.fetch_sub(1, std::memory_order_acq_rel);
        }
      } guard {_outstanding_frames};

      int prev_peak = _peak_outstanding.load(std::memory_order_relaxed);
      while (guard.value > prev_peak &&
             !_peak_outstanding.compare_exchange_weak(prev_peak, guard.value, std::memory_order_release, std::memory_order_relaxed)) {
      }

      process_frame(sender);
    });

    try {
      _capture_session = _frame_pool.CreateCaptureSession(_deps->graphics_item);
    } catch (const winrt::hresult_error &ex) {
      BOOST_LOG(error) << "CreateCaptureSession threw: " << ex.code() << " - " << winrt::to_string(ex.message());
      return false;
    } catch (...) {
      BOOST_LOG(error) << "CreateCaptureSession threw an unknown exception";
      return false;
    }

    if (!_capture_session) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureSession (returned null)";
      return false;
    }

    auto session3 = _capture_session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
    if (!session3) {
      BOOST_LOG(warning) << "IGraphicsCaptureSession3 not available; skipping IsBorderRequired(false)";
    } else {
      try {
        session3.IsBorderRequired(false);
      } catch (const winrt::hresult_error &ex) {
        BOOST_LOG(warning) << "IsBorderRequired(false) failed (continuing without it): " << ex.code() << " - " << winrt::to_string(ex.message());
      } catch (...) {
        BOOST_LOG(warning) << "IsBorderRequired(false) threw an unknown exception (continuing without it)";
      }
    }

    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
          L"Windows.Foundation.UniversalApiContract",
          19
        )) {
      auto session4 = _capture_session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession4>();
      if (session4) {
        try {
          session4.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportOnly);
          _dirty_regions_enabled.store(true, std::memory_order_release);
          BOOST_LOG(info) << "WGC dirty-region mode set to ReportOnly";
        } catch (const winrt::hresult_error &ex) {
          BOOST_LOG(warning) << "Failed to enable WGC ReportOnly dirty regions; using full copies: "
                             << ex.code() << " - " << winrt::to_string(ex.message());
        } catch (...) {
          BOOST_LOG(warning) << "Failed to enable WGC ReportOnly dirty regions; using full copies";
        }
      } else {
        BOOST_LOG(debug) << "IGraphicsCaptureSession4 unavailable; using full WGC copies";
      }
    } else {
      BOOST_LOG(debug) << "WGC dirty regions require UniversalApiContract 19; using full copies";
    }

    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
      if (g_config.min_update_interval_100ns > 0) {
        _capture_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan {g_config.min_update_interval_100ns});
        BOOST_LOG(info) << "WGC MinUpdateInterval set to " << g_config.min_update_interval_100ns << " ticks";
      } else {
        BOOST_LOG(info) << "WGC MinUpdateInterval left at system default";
      }
    }

    return true;
  }

  /**
   * @brief Starts the capture session if available.
   */
  void start_capture() const {
    if (_capture_session) {
      _capture_session.StartCapture();
      BOOST_LOG(info) << "Helper process started. Capturing frames using WGC...";
    }
  }

private:
  void check_and_adjust_frame_buffer() {
    auto now = std::chrono::steady_clock::now();

    // Check every 1 second for buffer adjustments
    if (auto time_since_last_check = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_buffer_check);
        time_since_last_check.count() < 1000) {
      return;
    }

    _last_buffer_check = now;

    // 1) Prune old drop timestamps (older than 5 seconds)
    prune_old_drop_timestamps(now);

    // 2) Try to increase buffer count if we have recent drops
    if (try_increase_buffer_size(now)) {
      maybe_log_capture_diagnostics(now);
      return;
    }

    // 3) Try to decrease buffer count if we've been quiet
    try_decrease_buffer_size(now);
    maybe_log_capture_diagnostics(now);
  }
};

/**
 * @brief Callback procedure for desktop switch events.
 *
 * This function handles EVENT_SYSTEM_DESKTOPSWITCH events to detect when the system
 * transitions to or from secure desktop mode (such as UAC prompts or lock screens).
 * If secure desktop is not active yet, it starts a short confirmation window so we can
 * still catch the secure-desktop transition after the event settles.
 *
 * @param h_win_event_hook Handle to the event hook (unused).
 * @param event The event type that occurred.
 * @param hwnd Handle to the window (unused).
 * @param id_object Object identifier (unused).
 * @param id_child Child object identifier (unused).
 * @param dw_event_thread Thread that generated the event (unused).
 * @param dwms_event_time Time the event occurred (unused).
 */
void CALLBACK desktop_switch_hook_proc(HWINEVENTHOOK /*h_win_event_hook*/, DWORD event, HWND /*hwnd*/, LONG /*id_object*/, LONG /*id_child*/, DWORD /*dw_event_thread*/, DWORD /*dwms_event_time*/) {
  if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
    BOOST_LOG(info) << "Desktop switch detected!";

    const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
    BOOST_LOG(info) << "Desktop switch - Secure desktop: " << (secure_desktop_active ? "YES" : "NO");

    if (secure_desktop_active) {
      notify_main_process_about_secure_desktop();
    } else if (!secure_desktop_active && g_secure_desktop_detected) {
      BOOST_LOG(info) << "Returned to normal desktop";
      g_secure_desktop_detected = false;
      g_pending_secure_desktop_check_deadline.reset();
    } else {
      g_pending_secure_desktop_check_deadline = std::chrono::steady_clock::now() + SECURE_DESKTOP_CONFIRMATION_WINDOW;
      BOOST_LOG(debug) << "Desktop switch was not immediately secure; waiting briefly for confirmation";
    }
  }
}

std::filesystem::path get_roaming_log_directory() {
  PWSTR roaming_path = nullptr;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roaming_path);
  if (FAILED(hr) || roaming_path == nullptr) {
    if (roaming_path) {
      CoTaskMemFree(roaming_path);
    }
    return {};
  }

  std::filesystem::path result(roaming_path);
  CoTaskMemFree(roaming_path);
  result /= L"Sunshine";

  std::error_code ec;
  std::filesystem::create_directories(result, ec);
  if (ec) {
    return {};
  }

  return result;
}

/**
 * @brief Helper function to get the WGC helper log path.
 *
 * Logs live under Roaming/Sunshine so they sit with the other Sunshine logs.
 * When session-mode logging is enabled, the returned file path is used to derive
 * the base name and output directory (typically Roaming/Sunshine/logs).
 * Falls back to the system temporary directory (or current directory) if the roaming
 * folder cannot be resolved.
 *
 * @return Path to the log file as a UTF-8 string.
 */
std::string get_temp_log_path() {
  if (auto roaming_dir = get_roaming_log_directory(); !roaming_dir.empty()) {
    auto log_file = roaming_dir / L"sunshine_wgc_helper.log";
    return wide_to_utf8(log_file.wstring());
  }

  std::wstring temp_path(MAX_PATH, L'\0');
  if (auto len = GetTempPathW(MAX_PATH, temp_path.data()); len == 0 || len > MAX_PATH) {
    return "sunshine_wgc_helper.log";
  }
  temp_path.resize(wcslen(temp_path.data()));
  std::wstring wlog = temp_path + L"sunshine_wgc_helper.log";
  return wide_to_utf8(wlog);
}

/**
 * @brief Helper function to handle IPC messages from the main process.
 *
 * Processes incoming messages from the main Sunshine process via named pipe:
 *
 * - Configuration messages: Receives and stores config_data_t structure with display settings
 *
 * @param message The received message bytes from the named pipe.
 *
 */
void handle_ipc_message(std::span<const uint8_t> message) {
  // Handle config data message
  if (message.size() == sizeof(platf::dxgi::config_data_t)) {
    std::lock_guard lock(g_config_mutex);
    if (g_config_received) {
      return;
    }

    memcpy(&g_config, message.data(), sizeof(platf::dxgi::config_data_t));
    g_config_received = true;
    // If log_level in config differs from current, update log filter
    if (INITIAL_LOG_LEVEL != g_config.log_level) {
      // Update log filter to new log level
      boost::log::core::get()->set_filter(
        severity >= g_config.log_level
      );
      BOOST_LOG(info) << "Log level updated from config: " << g_config.log_level;
    }
    BOOST_LOG(info) << "Received config data: hdr: " << g_config.dynamic_range
                    << ", display: '" << winrt::to_string(g_config.display_name) << "'"
                    << ", adapter LUID: " << std::hex << g_config.adapter_luid.HighPart
                    << ":" << g_config.adapter_luid.LowPart << std::dec
                    << ", target_fps: " << g_config.target_fps
                    << ", min_update_interval_100ns: " << g_config.min_update_interval_100ns
                    << ", initial_buffers: " << g_config.initial_frame_buffer_size
                    << ", max_buffers: " << g_config.max_frame_buffer_size
                    << ", force_sdr_capture: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) ? "yes" : "no")
                    << ", drain_to_latest: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_DRAIN_TO_LATEST) ? "yes" : "no")
                    << ", allow_buffer_decrease: " << ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE) ? "yes" : "no");
    g_config_cv.notify_all();
  }
}

/**
 * @brief Helper function to setup the communication pipe with the main process.
 *
 * Configures the AsyncNamedPipe with callback functions for message handling:
 * - on_message: Delegates to handle_ipc_message() for processing received data
 * - on_error: Handles pipe communication errors (currently empty handler)
 *
 * @param pipe Reference to the AsyncNamedPipe to configure.
 *
 * @return true if the pipe was successfully configured and started, false otherwise.
 */
bool setup_pipe_callbacks(AsyncNamedPipe &pipe) {
  auto on_message = [](std::span<const uint8_t> message) {
    handle_ipc_message(message);
  };

  auto on_error = [](std::string_view /*err*/) {
    // Error handler, intentionally left empty or log as needed
  };

  auto on_broken_pipe = []() {
    signal_pipe_disconnected();
  };

  return pipe.start(on_message, on_error, on_broken_pipe);
}

/**
 * @brief Helper function to process window messages for desktop hooks.
 *
 * @param shutdown_requested Reference to shutdown flag that may be set if WM_QUIT is received.
 * @return true if messages were processed, false if shutdown was requested.
 */
bool process_window_messages(bool &shutdown_requested) {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (msg.message == WM_QUIT) {
      shutdown_requested = true;
      return false;
    }
  }
  return true;
}

DWORD helper_wait_timeout_ms() {
  constexpr DWORD connection_health_interval_ms = 1000;
  if (!g_pending_secure_desktop_check_deadline) {
    return connection_health_interval_ms;
  }

  const auto remaining = *g_pending_secure_desktop_check_deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return 0;
  }

  const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining + std::chrono::microseconds(999)).count();
  return static_cast<DWORD>((std::min) (int64_t {connection_health_interval_ms}, (std::max) (int64_t {1}, remaining_ms)));
}

/**
 * @brief Helper function to setup desktop switch hook for secure desktop detection.
 *
 * @return true if the hook was successfully installed, false otherwise.
 */
bool setup_desktop_switch_hook() {
  BOOST_LOG(info) << "Setting up desktop switch hook...";

  HWINEVENTHOOK raw_hook = SetWinEventHook(
    EVENT_SYSTEM_DESKTOPSWITCH,
    EVENT_SYSTEM_DESKTOPSWITCH,
    nullptr,
    desktop_switch_hook_proc,
    0,
    0,
    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
  );

  if (!raw_hook) {
    BOOST_LOG(error) << "Failed to set up desktop switch hook: " << GetLastError();
    return false;
  }

  g_desktop_switch_hook.reset(raw_hook);
  BOOST_LOG(info) << "Desktop switch hook installed successfully";
  return true;
}

/**
 * @brief Main application entry point for the Windows Graphics Capture helper process.
 *
 * This standalone executable serves as a helper process for Sunshine's Windows Graphics
 * Capture functionality. The main function performs these key operations:
 *
 * 1. **System Initialization**: Sets up logging, DPI awareness, thread priority, and MMCSS
 * 2. **IPC Setup**: Establishes named pipe communication with the main Sunshine process
 * 3. **D3D11 Device Creation**: Creates hardware-accelerated D3D11 device and WinRT interop
 * 4. **Monitor Selection**: Identifies and configures the target monitor for capture
 * 5. **Shared Resource Creation**: Sets up shared D3D11 texture for inter-process frame sharing
 * 6. **WGC Setup**: Initializes Windows Graphics Capture frame pool and capture session
 * 7. **Desktop Monitoring**: Sets up hooks to detect secure desktop transitions
 * 8. **Main Loop**: Processes window messages and handles capture events until shutdown
 *
 * @param argc Number of command line arguments. Expects at least 2 arguments.
 * @param argv Array of command line argument strings. argv[1] should contain the pipe name GUID.
 * @return 0 on successful completion, 1 on initialization failure.
 */
int main(int argc, char *argv[]) {
  // Set up default config and log level
  // Use session-mode logs so rare startup deadlocks don't get overwritten by the next helper run.
  auto log_deinit = logging::init(2, get_temp_log_path());

  // Check command line arguments for pipe names
  if (argc < 2) {
    BOOST_LOG(error) << "Usage: " << argv[0] << " <pipe_name_guid>";
    return 1;
  }

  std::string pipe_name = argv[1];
  BOOST_LOG(info) << "Using pipe name: " << pipe_name;

  g_pipe_disconnected_event = winrt::handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  g_capture_item_closed_event = winrt::handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!g_pipe_disconnected_event || !g_capture_item_closed_event) {
    BOOST_LOG(error) << "Failed to create helper wait events: " << GetLastError();
    return 1;
  }

  // Initialize system settings (DPI awareness, thread priority, MMCSS)
  SystemInitializer system_initializer;
  if (!system_initializer.initialize_all()) {
    BOOST_LOG(error) << "System initialization failed, exiting...";
    return 1;
  }

  // Debug: Verify system settings
  BOOST_LOG(info) << "DPI awareness set: " << (system_initializer.is_dpi_awareness_set() ? "YES" : "NO");
  BOOST_LOG(info) << "Thread priority set: " << (system_initializer.is_thread_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "GPU scheduling priority set: " << (system_initializer.is_gpu_priority_set() ? "YES" : "NO");
  BOOST_LOG(info) << "MMCSS characteristics set: " << (system_initializer.is_mmcss_characteristics_set() ? "YES" : "NO");

  BOOST_LOG(info) << "Starting Windows Graphics Capture helper process...";

  // Create named pipe for communication with main process using provided pipe name
  AnonymousPipeFactory pipe_factory;

  auto comm_pipe = pipe_factory.create_client(pipe_name);
  auto pipe_shared = std::make_shared<AsyncNamedPipe>(std::move(comm_pipe));
  g_communication_pipe_weak = pipe_shared;  // Store weak reference for desktop hook callback

  if (!setup_pipe_callbacks(*pipe_shared)) {
    BOOST_LOG(error) << "Failed to start communication pipe";
    return 1;
  }

  constexpr auto max_wait = std::chrono::milliseconds(5000);
  {
    std::unique_lock lock(g_config_mutex);
    if (!g_config_cv.wait_for(lock, max_wait, []() {
          return g_config_received;
        })) {
      BOOST_LOG(error) << "Timed out waiting for config data from main process (" << max_wait.count() << "ms)";
      return 1;
    }
  }

  // Create D3D11 device and context using the same adapter as the main process
  D3D11DeviceManager d3d11_manager;
  if (!d3d11_manager.initialize_all(g_config.adapter_luid)) {
    BOOST_LOG(error) << "D3D11 device initialization failed, exiting...";
    return 1;
  }

  // Monitor management
  DisplayManager display_manager;
  if (!display_manager.select_monitor(g_config)) {
    BOOST_LOG(error) << "Monitor selection failed, exiting...";
    return 1;
  }

  if (!display_manager.get_monitor_info()) {
    BOOST_LOG(error) << "Failed to get monitor info, exiting...";
    return 1;
  }

  // Create GraphicsCaptureItem for monitor using interop.
  // Virtual display topology can churn briefly while Sunshine is applying display settings,
  // so retry monitor selection/item creation before treating startup as fatal.
  GraphicsCaptureItem item = nullptr;
  if (!display_manager.create_graphics_capture_item(item)) {
    constexpr int kMaxCreateItemAttempts = 8;
    bool created = false;
    for (int attempt = 1; attempt <= kMaxCreateItemAttempts && !created; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      if (!display_manager.select_monitor(g_config)) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": monitor selection failed";
        continue;
      }
      if (!display_manager.get_monitor_info()) {
        BOOST_LOG(warning) << "GraphicsCaptureItem retry " << attempt
                           << "/" << kMaxCreateItemAttempts
                           << ": failed to query monitor info";
        continue;
      }
      if (display_manager.create_graphics_capture_item(item)) {
        BOOST_LOG(info) << "GraphicsCaptureItem created after retry " << attempt
                        << "/" << kMaxCreateItemAttempts;
        created = true;
        break;
      }
    }
    if (!created) {
      BOOST_LOG(error) << "Failed to create GraphicsCaptureItem after retries";
      return 1;
    }
  }

  // Calculate final resolution based on config and monitor info
  display_manager.configure_capture_resolution(item);

  // WGC silently stops delivering frames once the capture item closes; display mode
  // changes, DWM restarts and monitor removals all do this (especially on Windows 10).
  // Exit so the main process notices the broken pipe and reinitializes capture instead
  // of freezing on the last delivered frame.
  item.Closed([](GraphicsCaptureItem const &, winrt::Windows::Foundation::IInspectable const &) {
    BOOST_LOG(warning) << "GraphicsCaptureItem closed; shutting down so capture can be reinitialized";
    signal_capture_item_closed();
  });

  // Use FP16 whenever the stream is HDR or the target output is already in
  // Advanced Color, except when the main process asks for SDR-compatible
  // capture so RTX HDR/TrueHDR can synthesize the HDR frame itself.
  DXGI_FORMAT capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  const bool force_sdr_capture =
    g_config_received &&
    ((g_config.flags & platf::dxgi::WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT) != 0);
  if (g_config_received && !force_sdr_capture && (g_config.dynamic_range || g_config.advanced_color_capture)) {
    capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  }

  // Create shared resource manager for texture, keyed mutex, and metadata
  SharedResourceManager shared_resource_manager;
  if (!shared_resource_manager.initialize_all(d3d11_manager.get_device(), display_manager.get_width(), display_manager.get_height(), capture_format)) {
    return 1;
  }

  // Send shared handle data via named pipe to main process
  platf::dxgi::shared_handle_data_t handle_data = shared_resource_manager.get_shared_handle_data();
  BOOST_LOG(info) << "Prepared shared texture-ring handle message - Size: " << sizeof(handle_data) << " bytes, first handle: 0x" << std::hex << reinterpret_cast<uintptr_t>(handle_data.texture_handles[0]) << std::dec;
  std::span<const uint8_t> handle_message(reinterpret_cast<const uint8_t *>(&handle_data), sizeof(handle_data));

  // Wait for connection and send the handle data
  BOOST_LOG(info) << "Waiting for main process to connect...";
  {
    constexpr int connect_timeout_ms = 5000;
    int connect_waited_ms = 0;
    while (!pipe_shared->is_connected() && connect_waited_ms < connect_timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      connect_waited_ms += 50;
    }

    if (!pipe_shared->is_connected()) {
      BOOST_LOG(error) << "Timed out waiting for main process to connect (" << connect_timeout_ms << "ms)";
      return 1;
    }
  }
  BOOST_LOG(info) << "Connected! Sending duplicated handle data...";
  pipe_shared->send(handle_message);
  BOOST_LOG(info) << "Duplicated handle data sent successfully to main process";

  // Create dependencies for capture manager
  WgcCaptureDependencies deps {
    d3d11_manager.get_winrt_device(),
    item,
    shared_resource_manager,
    d3d11_manager.get_context()
  };

  // Create WGC capture manager
  WgcCaptureManager wgc_capture_manager {capture_format, display_manager.get_width(), display_manager.get_height(), std::move(deps)};
  const auto initial_frame_buffer_size = std::clamp<uint32_t>(
    g_config.initial_frame_buffer_size ? g_config.initial_frame_buffer_size : 1,
    1,
    std::max<uint32_t>(1, g_config.max_frame_buffer_size ? g_config.max_frame_buffer_size : 1)
  );
  if (!wgc_capture_manager.create_or_adjust_frame_pool(initial_frame_buffer_size)) {
    BOOST_LOG(error) << "Failed to create frame pool";
    return 1;
  }

  if (!wgc_capture_manager.create_capture_session()) {
    BOOST_LOG(error) << "Failed to create capture session";
    return 1;
  }

  // Set up desktop switch hook for secure desktop detection
  setup_desktop_switch_hook();

  wgc_capture_manager.start_capture();

  // Wait on desktop-switch messages and helper shutdown signals rather than
  // polling at 1 kHz. A one-second timeout remains as a connection-health
  // fallback for pipe implementations that cannot signal remote closure.
  bool shutdown_requested = false;
  const std::array wait_handles {
    g_pipe_disconnected_event.get(),
    g_capture_item_closed_event.get()
  };
  while (!shutdown_requested && !g_capture_item_closed.load(std::memory_order_acquire)) {
    if (!pipe_shared->is_connected()) {
      break;
    }

    poll_pending_secure_desktop_transition();
    const auto wait_result = MsgWaitForMultipleObjectsEx(
      static_cast<DWORD>(wait_handles.size()),
      wait_handles.data(),
      helper_wait_timeout_ms(),
      QS_ALLINPUT,
      MWMO_INPUTAVAILABLE
    );
    if (wait_result == WAIT_FAILED) {
      BOOST_LOG(error) << "MsgWaitForMultipleObjectsEx failed: " << GetLastError();
      break;
    }
    if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_OBJECT_0 + 1) {
      break;
    }
    if (wait_result == WAIT_OBJECT_0 + wait_handles.size()) {
      if (!process_window_messages(shutdown_requested)) {
        break;
      }
    }
  }

  if (g_capture_item_closed.load(std::memory_order_acquire)) {
    BOOST_LOG(info) << "Capture item closed, shutting down...";
  } else {
    BOOST_LOG(info) << "Main process disconnected, shutting down...";
  }

  pipe_shared->stop();

  // Flush logs before exit
  boost::log::core::get()->flush();

  BOOST_LOG(info) << "WGC Helper process terminated";

  return 0;
}
