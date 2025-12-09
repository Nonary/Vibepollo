/**
 * @file src/platform/windows/ipc/ipc_session.cpp
 * @brief Implements the IPC session logic for Windows WGC capture integration.
 * Handles inter-process communication, shared texture setup, and frame synchronization
 * between the main process and the WGC capture helper process.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <span>
#include <string_view>
#include <thread>

// local includes
#include "config.h"
#include "ipc_session.h"
#include "misc_utils.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

// platform includes
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

namespace platf::dxgi {

  void ipc_session_t::handle_secure_desktop_message(std::span<const uint8_t> msg) {
    if (msg.size() == 1 && msg[0] == SECURE_DESKTOP_MSG) {
      // secure desktop detected
      BOOST_LOG(info) << "WGC can no longer capture the screen due to Secured Desktop, swapping to DXGI";
      _should_swap_to_dxgi = true;
    }
  }

  int ipc_session_t::init(const ::video::config_t &config, std::string_view display_name, ID3D11Device *device) {
    _process_helper = std::make_unique<ProcessHandler>();
    _config = config;
    _display_name = display_name;
    _device.copy_from(device);
    return 0;
  }

  void ipc_session_t::initialize_if_needed() {
    // Fast path: already successfully initialized
    if (_initialized) {
      return;
    }

    // Attempt to become the initializing thread
    bool expected = false;
    if (!_initializing.compare_exchange_strong(expected, true)) {
      // Another thread is initializing; wait until it finishes (either success or failure)
      while (_initializing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return;  // After wait, either initialized is true (success) or false (failure); caller can retry later
    }

    // We are the initializing thread now. Ensure we clear the flag on all exit paths.
    auto clear_initializing = util::fail_guard([this]() {
      _initializing = false;
    });

    // Check if properly initialized via init() first
    if (!_process_helper) {
      BOOST_LOG(debug) << "Cannot initialize_if_needed without prior init()";
      _initialized = false;
      return;
    }

    // Reset success flag before attempting
    _initialized = false;

    if (_pipe) {
      _pipe->stop();
      _pipe.reset();
    }
    _frame_queue_pipe.reset();
    _shared_texture = nullptr;
    _keyed_mutex = nullptr;
    _frame_ready = false;

    // Ensure previous helper is fully stopped before restarting. This avoids overlapping D3D11 allocations
    // across rapid re-inits that have been observed to destabilize the NVIDIA driver stack.
    stop_helper_process();

    // Give the driver a brief window to release resources if we just tore down.
    if (_last_helper_stop.time_since_epoch().count() != 0) {
      auto since_stop = std::chrono::steady_clock::now() - _last_helper_stop;
      if (since_stop < std::chrono::milliseconds(200)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200) - since_stop);
      }
    }

    // Flush any pending work on the capture device before creating a new shared texture.
    if (_device) {
      winrt::com_ptr<ID3D11DeviceContext> ctx;
      _device->GetImmediateContext(ctx.put());
      if (ctx) {
        ctx->Flush();
      }
    }

    // Get the directory of the main executable (Unicode-safe)
    std::wstring exePathBuffer(MAX_PATH, L'\0');
    GetModuleFileNameW(nullptr, exePathBuffer.data(), MAX_PATH);
    exePathBuffer.resize(wcslen(exePathBuffer.data()));
    std::filesystem::path mainExeDir = std::filesystem::path(exePathBuffer).parent_path();
    std::string pipe_guid = generate_guid();
    std::string frame_queue_pipe_guid = generate_guid();

    std::filesystem::path exe_path = mainExeDir / L"tools" / L"sunshine_wgc_capture.exe";
    // Pass both GUIDs as arguments, space-separated
    std::wstring arguments = platf::from_utf8(pipe_guid + " " + frame_queue_pipe_guid);

    if (!_process_helper->start(exe_path.wstring(), arguments)) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to start sunshine_wgc_capture executable at: " << exe_path.wstring()
                       << " (error code: " << err << ")";
      return;
    }

    auto on_message = [this](std::span<const uint8_t> msg) {
      if (msg.size() == 1) {
        handle_secure_desktop_message(msg);
      }
    };

    auto on_error = [](const std::string &err) {
      BOOST_LOG(error) << "Pipe error: " << err.c_str();
    };

    auto on_broken_pipe = [this]() {
      BOOST_LOG(warning) << "Broken pipe detected, forcing re-init";
      _force_reinit = true;
    };

    auto anon_connector = std::make_unique<AnonymousPipeFactory>();

    auto control_pipe = anon_connector->create_server(pipe_guid);
    auto frame_queue_pipe = anon_connector->create_server(frame_queue_pipe_guid);
    if (!control_pipe || !frame_queue_pipe) {
      BOOST_LOG(error) << "IPC pipe setup failed for WGC session; aborting";
      return;
    }

    control_pipe->wait_for_client_connection(5000);
    frame_queue_pipe->wait_for_client_connection(5000);

    if (!control_pipe->is_connected()) {
      BOOST_LOG(error) << "Helper failed to connect to control pipe within timeout";
      _process_helper->terminate();
      return;
    }

    if (!frame_queue_pipe->is_connected()) {
      BOOST_LOG(error) << "Helper failed to connect to frame queue pipe within timeout";
      _process_helper->terminate();
      return;
    }

    // Send config data to helper process
    config_data_t config_data = {};
    config_data.dynamic_range = _config.dynamicRange;
    config_data.log_level = config::sunshine.min_log_level;

    // Convert display_name (std::string) to wchar_t[32]
    if (!_display_name.empty()) {
      std::wstring wdisplay_name(_display_name.begin(), _display_name.end());
      wcsncpy_s(config_data.display_name, wdisplay_name.c_str(), 31);
      config_data.display_name[31] = L'\0';
    } else {
      config_data.display_name[0] = L'\0';
    }

    // We need to make sure helper uses the same adapter for now.
    // This won't be a problem in future versions when we add support for cross adapter capture.
    // But for now, it is required that we use the exact same one.
    if (_device) {
      try_get_adapter_luid(config_data.adapter_luid);
    } else {
      BOOST_LOG(warning) << "No D3D11 device available, helper will use default adapter";
      memset(&config_data.adapter_luid, 0, sizeof(LUID));
    }

    auto config_span = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&config_data), sizeof(config_data_t));
    if (!control_pipe->send(config_span, 5000)) {
      BOOST_LOG(error) << "Failed to send configuration data to helper process";
      _process_helper->terminate();
      return;
    }

    constexpr auto handle_wait_timeout = std::chrono::seconds(3);
    auto deadline = std::chrono::steady_clock::now() + handle_wait_timeout;
    std::array<uint8_t, sizeof(shared_handle_data_t)> control_buffer {};
    bool handle_received = false;
    bool timed_out_waiting = false;

    while (!handle_received) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out_waiting = true;
        break;
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int wait_ms = std::max(1, static_cast<int>(remaining.count()));

      size_t bytes_read = 0;
      auto result = control_pipe->receive(std::span<uint8_t>(control_buffer.data(), control_buffer.size()), bytes_read, wait_ms);

      if (result == PipeResult::Success) {
        if (bytes_read == sizeof(shared_handle_data_t)) {
          shared_handle_data_t handle_data {};
          memcpy(&handle_data, control_buffer.data(), sizeof(handle_data));
          if (setup_shared_texture_from_shared_handle(handle_data.texture_handle, handle_data.width, handle_data.height)) {
            handle_received = true;
          } else {
            break;
          }
        } else if (bytes_read == 1) {
          handle_secure_desktop_message(std::span<const uint8_t>(control_buffer.data(), 1));
        } else if (bytes_read > 0) {
          BOOST_LOG(warning) << "Ignoring unexpected control payload (" << bytes_read << " bytes) while waiting for shared handle";
        }
      } else if (result == PipeResult::Timeout) {
        continue;
      } else if (result == PipeResult::BrokenPipe) {
        BOOST_LOG(warning) << "Broken pipe while waiting for handle data from helper process";
        break;
      } else {
        BOOST_LOG(error) << "Control pipe receive failed while waiting for handle data (state=" << static_cast<int>(result) << ')';
        break;
      }
    }

    if (!handle_received) {
      if (timed_out_waiting) {
        BOOST_LOG(error) << "Timed out waiting for handle data from helper process (3s)";
      }
      BOOST_LOG(error) << "Failed to receive handle data from helper process! Helper is likely deadlocked!";
      _process_helper->terminate();
      return;
    }

    auto cleanup_on_failure = util::fail_guard([this]() {
      if (_pipe) {
        _pipe->stop();
        _pipe.reset();
      }
      if (_frame_queue_pipe) {
        _frame_queue_pipe->disconnect();
        _frame_queue_pipe.reset();
      }
      _shared_texture = nullptr;
      _keyed_mutex = nullptr;
      if (_process_helper) {
        _process_helper->terminate();
      }
    });

    _pipe = std::make_unique<AsyncNamedPipe>(std::move(control_pipe));
    _frame_queue_pipe = std::move(frame_queue_pipe);

    if (!_pipe->start(on_message, on_error, on_broken_pipe)) {
      BOOST_LOG(error) << "Failed to start AsyncNamedPipe for helper communication";
      return;
    }

    cleanup_on_failure.disable();
    _initialized = true;
  }

  bool ipc_session_t::wait_for_frame(std::chrono::milliseconds timeout) {
    frame_ready_msg_t frame_msg {};
    size_t bytesRead = 0;
    // Use a span over the frame_msg buffer
    auto result = _frame_queue_pipe->receive_latest(std::span<uint8_t>(reinterpret_cast<uint8_t *>(&frame_msg), sizeof(frame_msg)), bytesRead, static_cast<int>(timeout.count()));
    if (result == PipeResult::Success && bytesRead == sizeof(frame_ready_msg_t)) {
      if (frame_msg.message_type == FRAME_READY_MSG) {
        _frame_qpc = frame_msg.frame_qpc;
        _frame_ready = true;
        return true;
      }
    }
    return false;
  }

  bool ipc_session_t::try_get_adapter_luid(LUID &luid_out) {
    luid_out = {};

    if (!_device) {
      BOOST_LOG(warning) << "_device was null; default adapter will be used";
      return false;
    }

    winrt::com_ptr<IDXGIDevice> dxgi_device = _device.try_as<IDXGIDevice>();
    if (!dxgi_device) {
      BOOST_LOG(warning) << "try_as<IDXGIDevice>() failed; default adapter will be used";
      return false;
    }

    winrt::com_ptr<IDXGIAdapter> adapter;
    HRESULT hr = dxgi_device->GetAdapter(adapter.put());
    if (FAILED(hr) || !adapter) {
      BOOST_LOG(warning) << "GetAdapter() failed; default adapter will be used";
      return false;
    }

    DXGI_ADAPTER_DESC desc {};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "GetDesc() failed; default adapter will be used";
      return false;
    }

    luid_out = desc.AdapterLuid;
    return true;
  }

  capture_e ipc_session_t::acquire(std::chrono::milliseconds timeout, winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out) {
    if (!wait_for_frame(timeout)) {
      return capture_e::timeout;
    }

    // Additional validation: ensure required resources are available
    if (!_shared_texture || !_keyed_mutex) {
      return capture_e::error;
    }

    HRESULT hr = _keyed_mutex->AcquireSync(0, 3000);

    if (hr == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Helper process abandoned the keyed mutex, implying it may have crashed or was forcefully terminated.";
      _should_swap_to_dxgi = false;  // Don't swap to DXGI, just reinit
      _force_reinit = true;
      return capture_e::reinit;
    } else if (hr != S_OK || hr == WAIT_TIMEOUT) {
      return capture_e::error;
    }

    // Set output parameters
    gpu_tex_out = _shared_texture;
    frame_qpc_out = _frame_qpc;

    return capture_e::ok;
  }

  void ipc_session_t::release() {
    if (_keyed_mutex) {
      _keyed_mutex->ReleaseSync(0);
    }
  }

  bool ipc_session_t::setup_shared_texture_from_shared_handle(HANDLE shared_handle, UINT width, UINT height) {
    if (!_device) {
      BOOST_LOG(error) << "No D3D11 device available for setup_shared_texture_from_shared_handle";
      return false;
    }

    if (!shared_handle || shared_handle == INVALID_HANDLE_VALUE) {
      BOOST_LOG(error) << "Invalid shared handle provided";
      return false;
    }

    // Get the helper process handle to duplicate from
    HANDLE helper_process_handle = _process_helper->get_process_handle();
    if (!helper_process_handle) {
      BOOST_LOG(error) << "Failed to get helper process handle for duplication";
      return false;
    }

    // Duplicate the handle from the helper process into this process
    // We copy from the helper process because it runs at a lower integrity level
    HANDLE duplicated_handle = nullptr;
    BOOL dup_result = DuplicateHandle(
      helper_process_handle,  // Source process (helper process)
      shared_handle,  // Source handle
      GetCurrentProcess(),  // Target process (this process)
      &duplicated_handle,  // Target handle
      0,  // Desired access (0 = same as source)
      FALSE,  // Don't inherit
      DUPLICATE_SAME_ACCESS  // Same access rights
    );

    if (!dup_result) {
      BOOST_LOG(error) << "Failed to duplicate handle from helper process: " << GetLastError();
      return false;
    }

    auto fg = util::fail_guard([&]() {
      if (duplicated_handle) {
        CloseHandle(duplicated_handle);
      }
    });

    auto device1 = _device.as<ID3D11Device1>();
    if (!device1) {
      BOOST_LOG(error) << "Failed to get ID3D11Device1 interface for duplicated handle";
      return false;
    }

    winrt::com_ptr<IUnknown> unknown;
    HRESULT hr = device1->OpenSharedResource1(duplicated_handle, __uuidof(IUnknown), winrt::put_abi(unknown));
    auto texture = unknown.as<ID3D11Texture2D>();
    if (FAILED(hr) || !texture) {
      BOOST_LOG(error) << "Failed to open shared texture from duplicated handle: 0x" << std::hex << hr << " (decimal: " << std::dec << (int32_t) hr << ")";
      return false;
    }

    // Verify texture properties
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    _shared_texture = texture;
    _width = width;
    _height = height;

    // Get keyed mutex interface for synchronization
    _keyed_mutex = _shared_texture.as<IDXGIKeyedMutex>();
    if (!_keyed_mutex) {
      BOOST_LOG(error) << "Failed to get keyed mutex interface from shared texture";
      _shared_texture = nullptr;
      return false;
    }
    return true;
  }

  void ipc_session_t::stop_helper_process() {
    if (!_process_helper) {
      return;
    }

    DWORD exit_code = 0;
    _process_helper->terminate();  // best effort
    _process_helper->wait(exit_code);
    _last_helper_stop = std::chrono::steady_clock::now();
  }

}  // namespace platf::dxgi
