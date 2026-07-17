/**
 * @file src/platform/windows/ipc/ipc_session.cpp
 * @brief Implements the IPC session logic for Windows WGC capture integration.
 * Handles control IPC, shared texture setup, and event-driven frame synchronization
 * between the main process and the WGC capture helper process.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <thread>

// local includes
#include "config.h"
#include "ipc_session.h"
#include "misc_utils.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

// platform includes
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

namespace platf::dxgi {
  namespace {
    constexpr auto kRecentDesktopSwitchGrace = std::chrono::seconds(3);
    constexpr auto kHelperHandleWaitTimeout = std::chrono::seconds(15);
    constexpr auto kHelperHandleProgressInterval = std::chrono::seconds(3);
    constexpr std::int64_t kWgcMinUpdateInterval100ns = 10000;  // 1 ms
    constexpr uint32_t kWgcLowLatencyInitialBufferSize = 1;
    constexpr uint32_t kWgcAdaptiveMaxBufferSize = 2;
    constexpr uint32_t kWgcAdaptiveFramePoolFlags =
      WGC_IPC_FLAG_DRAIN_TO_LATEST |
      WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE;
    std::atomic<std::int64_t> g_last_wgc_desktop_switch_us {0};

    std::int64_t now_steady_us() {
      using namespace std::chrono;
      return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    }

    void record_recent_wgc_desktop_switch() {
      g_last_wgc_desktop_switch_us.store(now_steady_us(), std::memory_order_relaxed);
    }

    int wgc_target_fps(const ::video::config_t &config) {
      if (config.framerate > 0) {
        return config.framerate;
      }

      if (config.framerateX100 > 0) {
        return std::max(1, (config.framerateX100 + 50) / 100);
      }

      return 60;
    }

    std::int64_t wgc_min_update_interval_100ns(const ::video::config_t & /*config*/) {
      // Keep WGC's producer cadence latency-first. The stream-aware half-frame
      // interval used in 2efebf12f reduced callback pressure, but the helper then
      // delivered visibly uneven frame cadence on real streams. Requesting the
      // previous 1ms minimum lets WGC surface every compositor update and leaves
      // pacing/drop decisions to Sunshine's existing capture loop.
      return kWgcMinUpdateInterval100ns;
    }

    uint32_t wgc_ipc_flags(const ::video::config_t &config) {
      // Keep WGC latency-first when the helper is keeping up. If producer
      // backpressure appears, the helper can grow to a second frame, drain stale
      // queued WGC frames, then shrink back after a quiet period.
      auto flags = kWgcAdaptiveFramePoolFlags;
      if (config.rtx_hdr_active) {
        // TrueHDR generates the HDR frame itself. Keep the stream HDR, but ask WGC
        // for an SDR-compatible UNORM frame pool so the NGX TrueHDR input accepts it.
        flags |= WGC_IPC_FLAG_FORCE_SDR_CAPTURE_FORMAT;
      }
      return flags;
    }

    uint32_t wgc_initial_frame_buffer_size() {
      return kWgcLowLatencyInitialBufferSize;
    }

    uint32_t wgc_max_frame_buffer_size() {
      return kWgcAdaptiveMaxBufferSize;
    }

    struct frame_metadata_snapshot_t {
      LONG64 frame_id = 0;
      LONG64 frame_qpc = 0;
      LONG64 texture_slot = 0;
    };

    struct slot_frame_metadata_snapshot_t {
      LONG state = static_cast<LONG>(wgc_texture_slot_state_e::free);
      LONG64 frame_id = 0;
      LONG64 frame_qpc = 0;
    };

    bool read_frame_metadata_snapshot(const frame_metadata_t *metadata, frame_metadata_snapshot_t &snapshot) {
      if (!metadata) {
        return false;
      }

      constexpr int max_attempts = 64;
      for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const auto sequence_start = metadata->sequence;
        if ((sequence_start & 1) != 0) {
          std::this_thread::yield();
          continue;
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        const auto frame_id = metadata->frame_id;
        const auto frame_qpc = metadata->frame_qpc;
        const auto texture_slot = metadata->texture_slot;
        std::atomic_thread_fence(std::memory_order_acquire);

        const auto sequence_end = metadata->sequence;
        if (sequence_start == sequence_end && (sequence_end & 1) == 0) {
          snapshot.frame_id = frame_id;
          snapshot.frame_qpc = frame_qpc;
          snapshot.texture_slot = texture_slot;
          return true;
        }

        std::this_thread::yield();
      }

      return false;
    }

    bool read_slot_frame_metadata_snapshot(const frame_metadata_t *metadata, size_t slot, slot_frame_metadata_snapshot_t &snapshot) {
      if (!metadata || slot >= WGC_IPC_TEXTURE_SLOT_COUNT) {
        return false;
      }

      auto &slot_metadata = metadata->slots[slot];
      snapshot.state = InterlockedCompareExchange(const_cast<volatile LONG *>(&slot_metadata.state), 0, 0);
      std::atomic_thread_fence(std::memory_order_acquire);
      snapshot.frame_id = InterlockedCompareExchange64(const_cast<volatile LONG64 *>(&slot_metadata.frame_id), 0, 0);
      snapshot.frame_qpc = InterlockedCompareExchange64(const_cast<volatile LONG64 *>(&slot_metadata.frame_qpc), 0, 0);
      return true;
    }
  }  // namespace

  void note_wgc_desktop_switch() {
    record_recent_wgc_desktop_switch();
  }

  bool recent_wgc_desktop_switch_grace_active() {
    const auto last_switch_us = g_last_wgc_desktop_switch_us.load(std::memory_order_relaxed);
    if (last_switch_us == 0) {
      return false;
    }

    return (now_steady_us() - last_switch_us) <
           std::chrono::duration_cast<std::chrono::microseconds>(kRecentDesktopSwitchGrace).count();
  }

  ipc_session_t::~ipc_session_t() {
    // Best-effort shutdown. Avoid throwing from a destructor.
    try {
      _initialized = false;
      _force_reinit = true;

      // Flush any pending work on the capture device before tearing down shared resources.
      if (_device) {
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        _device->GetImmediateContext(ctx.put());
        if (ctx) {
          ctx->Flush();
        }
      }

      if (_pipe) {
        _pipe->stop();
        _pipe.reset();
      }

      if (_frame_metadata) {
        UnmapViewOfFile(_frame_metadata);
        _frame_metadata = nullptr;
      }

      _shared_textures.fill(nullptr);
      _keyed_mutexes.fill(nullptr);
      _shared_texture_handles.fill(nullptr);
      _frame_ready_event.close();
      _frame_metadata_mapping.close();

      stop_helper_process();
    } catch (...) {
      // Intentionally swallow all exceptions.
    }
  }

  void ipc_session_t::handle_desktop_switch_message(std::span<const uint8_t> msg) {
    if (msg.size() == 1 && msg[0] == SECURE_DESKTOP_MSG) {
      record_recent_wgc_desktop_switch();
      BOOST_LOG(info) << "WGC helper reported a desktop switch; forcing capture reinit and preferring DXGI fallback";
      _should_swap_to_dxgi = true;
    }
  }

  int ipc_session_t::init(const ::video::config_t &config, std::string_view display_name, ID3D11Device *device, bool advanced_color_capture) {
    _process_helper = std::make_unique<ProcessHandler>();
    _config = config;
    _display_name = display_name;
    _device.copy_from(device);
    _advanced_color_capture = advanced_color_capture;
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
    if (_frame_metadata) {
      UnmapViewOfFile(_frame_metadata);
      _frame_metadata = nullptr;
    }
    _frame_ready_event.close();
    _frame_metadata_mapping.close();
    _shared_textures.fill(nullptr);
    _keyed_mutexes.fill(nullptr);
    _shared_texture_handles.fill(nullptr);
    _locked_texture_slot = WGC_IPC_TEXTURE_SLOT_COUNT;
    _locked_frame_id = 0;
    _last_frame_id = 0;
    _frame_qpc = 0;
    _force_reinit = false;
    _should_swap_to_dxgi = false;

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

    auto on_message = [this](std::span<const uint8_t> msg) {
      if (msg.size() == 1) {
        handle_desktop_switch_message(msg);
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
    if (!control_pipe) {
      BOOST_LOG(error) << "IPC pipe setup failed for WGC session; aborting";
      return;
    }

    std::filesystem::path exe_path = mainExeDir / L"tools" / L"sunshine_wgc_capture.exe";
    std::wstring arguments = platf::from_utf8(pipe_guid);

    if (!_process_helper->start(exe_path.wstring(), arguments)) {
      auto err = GetLastError();
      BOOST_LOG(error) << "Failed to start sunshine_wgc_capture executable at: " << exe_path.wstring()
                       << " (error code: " << err << ")";
      return;
    }

    control_pipe->wait_for_client_connection(5000);

    if (!control_pipe->is_connected()) {
      BOOST_LOG(error) << "Helper failed to connect to control pipe within timeout";
      _process_helper->terminate();
      return;
    }

    // Send config data to helper process
    config_data_t config_data = {};
    config_data.dynamic_range = _config.dynamicRange;
    config_data.advanced_color_capture = _advanced_color_capture ? 1u : 0u;
    config_data.log_level = config::sunshine.min_log_level;
    config_data.min_update_interval_100ns = wgc_min_update_interval_100ns(_config);
    config_data.target_fps = wgc_target_fps(_config);
    config_data.flags = wgc_ipc_flags(_config);
    config_data.initial_frame_buffer_size = wgc_initial_frame_buffer_size();
    config_data.max_frame_buffer_size = wgc_max_frame_buffer_size();

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

    const auto handle_wait_start = std::chrono::steady_clock::now();
    auto deadline = handle_wait_start + kHelperHandleWaitTimeout;
    auto next_progress_log = handle_wait_start + kHelperHandleProgressInterval;
    std::array<uint8_t, sizeof(shared_handle_data_t)> control_buffer {};
    bool handle_received = false;
    bool timed_out_waiting = false;
    bool helper_exited = false;
    DWORD helper_exit_code = 0;

    while (!handle_received) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out_waiting = true;
        break;
      }

      if (HANDLE helper_process = _process_helper ? _process_helper->get_process_handle() : nullptr) {
        const DWORD wait_result = WaitForSingleObject(helper_process, 0);
        if (wait_result == WAIT_OBJECT_0) {
          helper_exited = true;
          GetExitCodeProcess(helper_process, &helper_exit_code);
          break;
        }
      }

      if (now >= next_progress_log) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - handle_wait_start).count();
        BOOST_LOG(warning) << "Still waiting for WGC helper shared handles after " << elapsed_ms
                           << "ms; helper startup may be delayed by system load.";
        next_progress_log = now + kHelperHandleProgressInterval;
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int wait_ms = std::max(1, static_cast<int>(remaining.count()));

      size_t bytes_read = 0;
      auto result = control_pipe->receive(
        std::span<uint8_t>(control_buffer.data(), control_buffer.size()),
        bytes_read,
        std::min(wait_ms, 250)
      );

      if (result == PipeResult::Success) {
        if (bytes_read == sizeof(shared_handle_data_t)) {
          shared_handle_data_t handle_data {};
          memcpy(&handle_data, control_buffer.data(), sizeof(handle_data));
          if (setup_shared_resources_from_shared_handles(handle_data)) {
            handle_received = true;
          } else {
            break;
          }
        } else if (bytes_read == 1) {
          handle_desktop_switch_message(std::span<const uint8_t>(control_buffer.data(), 1));
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
      if (helper_exited) {
        BOOST_LOG(error) << "WGC helper exited before sending shared handle data (exit_code=" << helper_exit_code << ')';
      }
      if (timed_out_waiting) {
        BOOST_LOG(error) << "Timed out waiting for handle data from helper process ("
                         << std::chrono::duration_cast<std::chrono::seconds>(kHelperHandleWaitTimeout).count() << "s)";
      }
      BOOST_LOG(error) << "Failed to receive handle data from WGC helper process.";
      _process_helper->terminate();
      return;
    }

    auto cleanup_on_failure = util::fail_guard([this]() {
      if (_pipe) {
        _pipe->stop();
        _pipe.reset();
      }
      if (_frame_metadata) {
        UnmapViewOfFile(_frame_metadata);
        _frame_metadata = nullptr;
      }
      _shared_textures.fill(nullptr);
      _keyed_mutexes.fill(nullptr);
      _shared_texture_handles.fill(nullptr);
      _frame_ready_event.close();
      _frame_metadata_mapping.close();
      if (_process_helper) {
        _process_helper->terminate();
      }
    });

    _pipe = std::make_unique<AsyncNamedPipe>(std::move(control_pipe));

    if (!_pipe->start(on_message, on_error, on_broken_pipe)) {
      BOOST_LOG(error) << "Failed to start AsyncNamedPipe for helper communication";
      return;
    }

    cleanup_on_failure.disable();
    _initialized = true;
  }

  capture_e ipc_session_t::wait_for_frame(std::chrono::milliseconds timeout) {
    if (!_frame_ready_event || !_frame_metadata) {
      return capture_e::error;
    }

    auto has_new_frame = [&]() -> bool {
      frame_metadata_snapshot_t snapshot;
      return read_frame_metadata_snapshot(_frame_metadata, snapshot) && snapshot.frame_id > _last_frame_id;
    };

    // The auto-reset event is a wakeup hint, not the frame predicate. With split wait/lock,
    // Sunshine can wait for an encoder image after the event fires while the helper publishes
    // a newer frame. In that case, lock_frame() consumes the newer metadata, and the older
    // event can become stale. Always check metadata before and after waiting.
    if (has_new_frame()) {
      return capture_e::ok;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      DWORD wait_ms = 0;
      if (timeout.count() > 0) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds::zero()) {
          return has_new_frame() ? capture_e::ok : capture_e::timeout;
        }
        wait_ms = static_cast<DWORD>(std::min<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count(),
          INFINITE - 1
        ));
        wait_ms = std::max<DWORD>(1, wait_ms);
      }

      const DWORD wait_result = WaitForSingleObject(_frame_ready_event.get(), wait_ms);
      if (wait_result == WAIT_TIMEOUT) {
        return has_new_frame() ? capture_e::ok : capture_e::timeout;
      }

      if (wait_result != WAIT_OBJECT_0) {
        BOOST_LOG(warning) << "Frame-ready event wait failed (" << GetLastError() << "); forcing re-init";
        _force_reinit = true;
        _initialized = false;
        return capture_e::reinit;
      }

      if (has_new_frame()) {
        return capture_e::ok;
      }

      // Consumed a stale auto-reset signal. With a poll-style wait, report timeout.
      if (timeout.count() <= 0) {
        return capture_e::timeout;
      }
    }
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
    set_last_wgc_adapter_luid(luid_out);
    return true;
  }

  capture_e ipc_session_t::acquire(std::chrono::milliseconds timeout, winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out) {
    const auto wait_start = std::chrono::steady_clock::now();
    auto wait_status = wait_for_frame(timeout);
    const auto event_wait = std::chrono::steady_clock::now() - wait_start;
    if (wait_status != capture_e::ok) {
      return wait_status;
    }

    auto status = lock_frame(gpu_tex_out, frame_qpc_out);
    if (status != capture_e::ok) {
      return status;
    }

    const auto frame_count = _frames_acquired.load(std::memory_order_relaxed);
    const auto event_wait_ms = std::chrono::duration<double, std::milli>(event_wait).count();
    const bool slow_event_wait = event_wait_ms > 5.0 && timeout.count() == 0;
    if (slow_event_wait) {
      _slow_event_waits.fetch_add(1, std::memory_order_relaxed);
    }
    if (frame_count == 1 || frame_count % 600 == 0 || slow_event_wait) {
      BOOST_LOG(debug) << "WGC IPC acquire timing: frame=" << frame_count
                       << " event_wait_ms=" << event_wait_ms
                       << " frame_id=" << _last_frame_id
                       << " slow_event_waits=" << _slow_event_waits.load(std::memory_order_relaxed)
                       << " slow_mutex_waits=" << _slow_mutex_waits.load(std::memory_order_relaxed);
    }

    return capture_e::ok;
  }

  capture_e ipc_session_t::lock_frame(winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out) {
    if (!_shared_textures[0] || !_keyed_mutexes[0] || !_frame_metadata) {
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    std::chrono::steady_clock::duration mutex_wait {};
    for (size_t attempt = 0; attempt < WGC_IPC_TEXTURE_SLOT_COUNT * 2; ++attempt) {
      frame_metadata_snapshot_t snapshot;
      if (!read_frame_metadata_snapshot(_frame_metadata, snapshot) ||
          snapshot.frame_id <= _last_frame_id ||
          snapshot.texture_slot < 0 ||
          snapshot.texture_slot >= static_cast<LONG64>(WGC_IPC_TEXTURE_SLOT_COUNT)) {
        return capture_e::timeout;
      }

      const auto slot = static_cast<size_t>(snapshot.texture_slot);
      auto &slot_metadata = _frame_metadata->slots[slot];
      if (!transition_wgc_texture_slot(slot_metadata, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased)) {
        continue;
      }

      slot_frame_metadata_snapshot_t slot_snapshot;
      read_slot_frame_metadata_snapshot(_frame_metadata, slot, slot_snapshot);
      if (slot_snapshot.frame_id <= _last_frame_id) {
        release_frame_slot(slot, slot_snapshot.frame_id);
        continue;
      }

      const auto mutex_wait_start = std::chrono::steady_clock::now();
      const HRESULT hr = _keyed_mutexes[slot]->AcquireSync(0, 3000);
      mutex_wait += std::chrono::steady_clock::now() - mutex_wait_start;
      if (hr == WAIT_ABANDONED) {
        BOOST_LOG(error) << "Helper process abandoned a WGC IPC slot mutex, implying it may have crashed or was forcefully terminated.";
        (void) _keyed_mutexes[slot]->ReleaseSync(0);
        release_frame_slot(slot, slot_snapshot.frame_id);
        _should_swap_to_dxgi = false;
        _force_reinit = true;
        _initialized = false;
        return capture_e::reinit;
      }
      if (hr != S_OK) {
        BOOST_LOG(error) << "Failed to acquire WGC IPC slot mutex [0x"sv << util::hex(hr).to_string_view() << "]; forcing re-init";
        release_frame_slot(slot, slot_snapshot.frame_id);
        _should_swap_to_dxgi = false;
        _force_reinit = true;
        _initialized = false;
        return capture_e::reinit;
      }

      _locked_texture_slot = slot;
      _locked_frame_id = slot_snapshot.frame_id;
      _last_frame_id = slot_snapshot.frame_id;
      _frame_qpc = static_cast<uint64_t>(slot_snapshot.frame_qpc);
      gpu_tex_out = _shared_textures[slot];
      frame_qpc_out = _frame_qpc;

      const auto frame_count = _frames_acquired.fetch_add(1, std::memory_order_relaxed) + 1;
      const auto mutex_wait_ms = std::chrono::duration<double, std::milli>(mutex_wait).count();
      if (mutex_wait_ms > 1.0) {
        _slow_mutex_waits.fetch_add(1, std::memory_order_relaxed);
      }
      if (frame_count == 1 || frame_count % 600 == 0 || mutex_wait_ms > 1.0) {
        BOOST_LOG(debug) << "WGC IPC lock timing: frame=" << frame_count
                         << " mutex_wait_ms=" << mutex_wait_ms
                         << " frame_id=" << _last_frame_id
                         << " slow_mutex_waits=" << _slow_mutex_waits.load(std::memory_order_relaxed);
      }

      return capture_e::ok;
    }

    return capture_e::timeout;
  }

  capture_e ipc_session_t::claim_frame(shared_frame_t &frame_out) {
    frame_out = {};
    if (!_shared_textures[0] || !_keyed_mutexes[0] || !_frame_metadata) {
      _force_reinit = true;
      _initialized = false;
      return capture_e::reinit;
    }

    for (size_t attempt = 0; attempt < WGC_IPC_TEXTURE_SLOT_COUNT * 2; ++attempt) {
      frame_metadata_snapshot_t snapshot;
      if (!read_frame_metadata_snapshot(_frame_metadata, snapshot) ||
          snapshot.frame_id <= _last_frame_id ||
          snapshot.texture_slot < 0 ||
          snapshot.texture_slot >= static_cast<LONG64>(WGC_IPC_TEXTURE_SLOT_COUNT)) {
        return capture_e::timeout;
      }

      const auto slot = static_cast<size_t>(snapshot.texture_slot);
      auto &slot_metadata = _frame_metadata->slots[slot];
      if (!transition_wgc_texture_slot(slot_metadata, wgc_texture_slot_state_e::ready, wgc_texture_slot_state_e::leased)) {
        continue;
      }

      slot_frame_metadata_snapshot_t slot_snapshot;
      read_slot_frame_metadata_snapshot(_frame_metadata, slot, slot_snapshot);
      if (slot_snapshot.frame_id <= _last_frame_id) {
        release_frame_slot(slot, slot_snapshot.frame_id);
        continue;
      }
      if (!_shared_texture_handles[slot]) {
        BOOST_LOG(error) << "Missing encoder handle for WGC shared texture-ring slot " << slot;
        release_frame_slot(slot, slot_snapshot.frame_id);
        _force_reinit = true;
        _initialized = false;
        return capture_e::reinit;
      }

      auto session = shared_from_this();
      std::shared_ptr<wgc_texture_slot_release_sink_t> release_sink {
        std::move(session),
        static_cast<wgc_texture_slot_release_sink_t *>(this)
      };

      frame_out.texture = _shared_textures[slot];
      frame_out.keyed_mutex = _keyed_mutexes[slot];
      frame_out.encoder_texture_handle = _shared_texture_handles[slot];
      frame_out.lease = wgc_texture_slot_lease_t {std::move(release_sink), slot, slot_snapshot.frame_id};
      frame_out.texture_slot = slot;
      frame_out.frame_id = static_cast<uint64_t>(slot_snapshot.frame_id);
      frame_out.frame_qpc = static_cast<uint64_t>(slot_snapshot.frame_qpc);

      _last_frame_id = slot_snapshot.frame_id;
      _frame_qpc = frame_out.frame_qpc;
      _frames_acquired.fetch_add(1, std::memory_order_relaxed);
      return capture_e::ok;
    }

    return capture_e::timeout;
  }

  void ipc_session_t::release() {
    if (_locked_texture_slot < WGC_IPC_TEXTURE_SLOT_COUNT) {
      const HRESULT hr = _keyed_mutexes[_locked_texture_slot]->ReleaseSync(0);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to release WGC IPC slot mutex [0x"sv << util::hex(hr).to_string_view() << ']';
        _force_reinit = true;
      } else {
        release_frame_slot(_locked_texture_slot, _locked_frame_id);
      }
      _locked_texture_slot = WGC_IPC_TEXTURE_SLOT_COUNT;
      _locked_frame_id = 0;
    }
  }

  void ipc_session_t::release_frame_slot(size_t slot, LONG64 frame_id) {
    if (!_frame_metadata || slot >= WGC_IPC_TEXTURE_SLOT_COUNT) {
      return;
    }

    auto &slot_metadata = _frame_metadata->slots[slot];
    if (InterlockedCompareExchange64(&slot_metadata.frame_id, 0, 0) != frame_id) {
      return;
    }

    (void) platf::dxgi::release_wgc_texture_slot(slot_metadata, frame_id);
  }

  bool ipc_session_t::setup_shared_resources_from_shared_handles(const shared_handle_data_t &handle_data) {
    if (!_device) {
      BOOST_LOG(error) << "No D3D11 device available for WGC shared-resource setup";
      return false;
    }

    if (!handle_data.frame_event_handle || handle_data.frame_event_handle == INVALID_HANDLE_VALUE ||
        !handle_data.frame_metadata_handle || handle_data.frame_metadata_handle == INVALID_HANDLE_VALUE) {
      BOOST_LOG(error) << "Invalid WGC shared handle data provided";
      return false;
    }
    for (HANDLE texture_handle : handle_data.texture_handles) {
      if (!texture_handle || texture_handle == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "Invalid WGC shared texture-ring handle data provided";
        return false;
      }
    }

    // Get the helper process handle to duplicate from
    HANDLE helper_process_handle = _process_helper->get_process_handle();
    if (!helper_process_handle) {
      BOOST_LOG(error) << "Failed to get helper process handle for duplication";
      return false;
    }

    // Duplicate handles from the helper process into this process. We copy from
    // the helper because it runs at a lower integrity level.
    auto duplicate_helper_handle = [&](HANDLE source, const char *name) -> winrt::handle {
      HANDLE duplicated = nullptr;
      if (!DuplicateHandle(
            helper_process_handle,
            source,
            GetCurrentProcess(),
            &duplicated,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
          )) {
        BOOST_LOG(error) << "Failed to duplicate WGC " << name << " handle from helper process: " << GetLastError();
        return {};
      }

      return winrt::handle {duplicated};
    };

    std::array<std::shared_ptr<winrt::handle>, WGC_IPC_TEXTURE_SLOT_COUNT> duplicated_texture_handles;
    for (size_t slot = 0; slot < WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      auto handle = duplicate_helper_handle(handle_data.texture_handles[slot], "texture-ring slot");
      if (handle) {
        duplicated_texture_handles[slot] = std::make_shared<winrt::handle>(std::move(handle));
      }
    }
    auto duplicated_event_handle = duplicate_helper_handle(handle_data.frame_event_handle, "frame event");
    auto duplicated_metadata_handle = duplicate_helper_handle(handle_data.frame_metadata_handle, "frame metadata");
    if (!duplicated_event_handle || !duplicated_metadata_handle) {
      return false;
    }
    for (const auto &texture_handle : duplicated_texture_handles) {
      if (!texture_handle) {
        return false;
      }
    }

    auto device1 = _device.try_as<ID3D11Device1>();
    if (!device1) {
      BOOST_LOG(error) << "Failed to get ID3D11Device1 interface for duplicated handle";
      return false;
    }

    std::array<winrt::com_ptr<ID3D11Texture2D>, WGC_IPC_TEXTURE_SLOT_COUNT> textures;
    std::array<winrt::com_ptr<IDXGIKeyedMutex>, WGC_IPC_TEXTURE_SLOT_COUNT> keyed_mutexes;
    for (size_t slot = 0; slot < WGC_IPC_TEXTURE_SLOT_COUNT; ++slot) {
      winrt::com_ptr<IUnknown> unknown;
      HRESULT hr = device1->OpenSharedResource1(duplicated_texture_handles[slot]->get(), __uuidof(IUnknown), winrt::put_abi(unknown));
      if (FAILED(hr) || !unknown) {
        BOOST_LOG(error) << "Failed to open WGC shared texture-ring slot " << slot << ": 0x" << std::hex << hr;
        return false;
      }
      hr = unknown->QueryInterface(__uuidof(ID3D11Texture2D), textures[slot].put_void());
      if (FAILED(hr) || !textures[slot]) {
        BOOST_LOG(error) << "Failed to query WGC shared texture-ring slot " << slot << ": 0x" << std::hex << hr;
        return false;
      }
      D3D11_TEXTURE2D_DESC desc;
      textures[slot]->GetDesc(&desc);
      if (desc.Width != handle_data.width || desc.Height != handle_data.height) {
        BOOST_LOG(warning) << "WGC shared texture-ring slot " << slot << " size mismatch (expected "
                           << handle_data.width << "x" << handle_data.height << ", got " << desc.Width << "x" << desc.Height << ")";
      }
      keyed_mutexes[slot] = textures[slot].try_as<IDXGIKeyedMutex>();
      if (!keyed_mutexes[slot]) {
        BOOST_LOG(error) << "Failed to get keyed mutex interface from WGC shared texture-ring slot " << slot;
        return false;
      }
    }

    auto *metadata = static_cast<frame_metadata_t *>(MapViewOfFile(
      duplicated_metadata_handle.get(),
      FILE_MAP_READ | FILE_MAP_WRITE,
      0,
      0,
      sizeof(frame_metadata_t)
    ));
    if (!metadata) {
      BOOST_LOG(error) << "Failed to map WGC frame metadata view: " << GetLastError();
      return false;
    }

    auto metadata_guard = util::fail_guard([&]() {
      UnmapViewOfFile(metadata);
    });

    _shared_textures = std::move(textures);
    _keyed_mutexes = std::move(keyed_mutexes);
    _shared_texture_handles = std::move(duplicated_texture_handles);
    _width = handle_data.width;
    _height = handle_data.height;

    _frame_ready_event = std::move(duplicated_event_handle);
    _frame_metadata_mapping = std::move(duplicated_metadata_handle);
    _frame_metadata = metadata;
    frame_metadata_snapshot_t snapshot;
    _last_frame_id = read_frame_metadata_snapshot(_frame_metadata, snapshot) ? snapshot.frame_id : 0;
    metadata_guard.disable();
    return true;
  }

  void ipc_session_t::stop_helper_process() {
    if (!_process_helper) {
      return;
    }

    if (!_process_helper->get_process_handle()) {
      return;
    }

    DWORD exit_code = 0;
    _process_helper->terminate();  // best effort
    if (!_process_helper->wait_for(exit_code, 3000)) {
      BOOST_LOG(warning) << "WGC helper did not exit within 3000ms after termination request; continuing teardown.";
      _process_helper = std::make_unique<ProcessHandler>();
    }
    _last_helper_stop = std::chrono::steady_clock::now();
  }

}  // namespace platf::dxgi
