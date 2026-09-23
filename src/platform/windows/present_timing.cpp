/**
 * @file src/platform/windows/present_timing.cpp
 * @brief Stamps captured frames at the foreground game's present cadence.
 */
#include "present_timing.h"

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

// platform includes
#include <dxgi.h>
#include <evntrace.h>
#include <evntcons.h>

// local includes
#include "src/logging.h"

#ifndef INVALID_PROCESSTRACE_HANDLE
  #define INVALID_PROCESSTRACE_HANDLE ((TRACEHANDLE) INVALID_HANDLE_VALUE)
#endif
#ifndef EVENT_TRACE_USE_MS_FLUSH_TIMER
  #define EVENT_TRACE_USE_MS_FLUSH_TIMER 0x00000010
#endif
#ifndef PROCESS_TRACE_MODE_REAL_TIME
  #define PROCESS_TRACE_MODE_REAL_TIME 0x00000100
#endif
#ifndef PROCESS_TRACE_MODE_RAW_TIMESTAMP
  #define PROCESS_TRACE_MODE_RAW_TIMESTAMP 0x00001000
#endif
#ifndef PROCESS_TRACE_MODE_EVENT_RECORD
  #define PROCESS_TRACE_MODE_EVENT_RECORD 0x10000000
#endif
#ifndef EVENT_HEADER_FLAG_32_BIT_HEADER
  #define EVENT_HEADER_FLAG_32_BIT_HEADER 0x0020
#endif

namespace platf::dxgi::present_timing {
  namespace {
    // Microsoft-Windows-DXGI
    constexpr GUID dxgi_provider {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
    constexpr USHORT present_start_event = 42;
    constexpr USHORT present_multiplane_overlay_start_event = 55;
    constexpr GUID session_guid {0x6E2E1F0C, 0x6A5B, 0x4F5B, {0x9C, 0x3D, 0x3B, 0x0E, 0x9A, 0x7C, 0x1D, 0x42}};
    constexpr wchar_t session_name[] = L"VibeshinePresentTiming";
    // Stamping happens as each frame is captured; a present is only useful if
    // its event has already been delivered, so flush the session every 2 ms.
    constexpr ULONG flush_timer_ms = 2;
    constexpr std::size_t maximum_events = 16384;

    struct properties_t {
      EVENT_TRACE_PROPERTIES header;
      wchar_t logger_name[64];
    };

    properties_t make_properties(const bool millisecond_flush) {
      properties_t properties {};
      properties.header.Wnode.BufferSize = sizeof(properties_t);
      properties.header.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
      properties.header.Wnode.ClientContext = 1;  // Raw QPC event timestamps.
      properties.header.Wnode.Guid = session_guid;
      properties.header.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | (millisecond_flush ? EVENT_TRACE_USE_MS_FLUSH_TIMER : 0);
      properties.header.FlushTimer = millisecond_flush ? flush_timer_ms : 1;
      properties.header.BufferSize = 16;  // KB
      properties.header.MinimumBuffers = 4;
      properties.header.MaximumBuffers = 32;
      properties.header.LoggerNameOffset = offsetof(properties_t, logger_name);
      properties.header.LogFileNameOffset = 0;
      return properties;
    }

    std::int64_t query_frequency() {
      LARGE_INTEGER frequency {};
      QueryPerformanceFrequency(&frequency);
      return frequency.QuadPart;
    }
  }  // namespace

  /**
   * @brief Real-time ETW session collecting DXGI present times of every process.
   */
  class tracker_t {
  public:
    tracker_t() = default;
    tracker_t(const tracker_t &) = delete;
    tracker_t &operator=(const tracker_t &) = delete;

    ~tracker_t() {
      stop();
    }

    bool start() {
      const auto frequency = query_frequency();
      if (frequency <= 0) {
        return false;
      }
      retention_ = frequency * 2;

      bool millisecond_flush = true;
      auto status = start_session(millisecond_flush);
      if (status == ERROR_ALREADY_EXISTS) {
        // A previous instance did not stop its session (crash or kill).
        auto properties = make_properties(false);
        ControlTraceW(0, session_name, &properties.header, EVENT_TRACE_CONTROL_STOP);
        status = start_session(millisecond_flush);
      }
      if (status == ERROR_INVALID_PARAMETER) {
        millisecond_flush = false;
        status = start_session(millisecond_flush);
      }
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(info) << "Present-time frame stamping unavailable: StartTrace failed (" << status << ')';
        return false;
      }

      status = EnableTraceEx2(
        session_,
        &dxgi_provider,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        ~0ULL,
        0,
        0,
        nullptr
      );
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(info) << "Present-time frame stamping unavailable: enabling the DXGI provider failed (" << status << ')';
        stop();
        return false;
      }

      EVENT_TRACE_LOGFILEW logfile {};
      logfile.LoggerName = const_cast<LPWSTR>(session_name);
      logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
      logfile.EventRecordCallback = &tracker_t::on_event_record;
      logfile.Context = this;
      consumer_ = OpenTraceW(&logfile);
      if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
        BOOST_LOG(info) << "Present-time frame stamping unavailable: OpenTrace failed (" << GetLastError() << ')';
        stop();
        return false;
      }

      thread_ = std::thread([handle = consumer_]() mutable {
        ProcessTrace(&handle, 1, nullptr, nullptr);
      });
      if (!millisecond_flush) {
        BOOST_LOG(info) << "Present-time frame stamping: millisecond ETW flush unsupported; most frames will keep their composition time";
      }
      return true;
    }

    void stop() {
      if (session_ != 0) {
        auto properties = make_properties(false);
        ControlTraceW(session_, nullptr, &properties.header, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
      }
      if (consumer_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(consumer_);
        consumer_ = INVALID_PROCESSTRACE_HANDLE;
      }
      if (thread_.joinable()) {
        thread_.join();
      }
    }

    /**
     * @brief Collects presents of the dominant swap chain of `pid`.
     * @param pid Process whose presents are wanted.
     * @param since Presents at or before this time are omitted from `out`.
     * @param until Latest present time of interest (the composition time).
     * @param window How far back swap-chain activity is counted.
     * @param preferred Swap chain to keep while it is still presenting.
     * @param out Receives the present times in (since, until], ascending.
     * @return The selected swap chain, or 0 when `pid` has not presented recently.
     */
    std::uint64_t presents_for(
      const DWORD pid,
      const std::int64_t since,
      const std::int64_t until,
      const std::int64_t window,
      const std::uint64_t preferred,
      std::vector<std::int64_t> &out
    ) {
      out.clear();
      // Local scratch: concurrent streams may share this tracker.
      std::vector<event_t> matching;
      std::vector<std::pair<std::uint64_t, unsigned>> counts;
      {
        std::lock_guard lock(mutex_);
        for (const auto &event : events_) {
          if (event.pid != pid || event.qpc > until || event.qpc <= until - window) {
            continue;
          }
          matching.push_back(event);
        }
      }
      if (matching.empty()) {
        return 0;
      }

      // Keep the current swap chain while it still presents; otherwise follow
      // the busiest one (a game window rather than its launcher).
      const auto recent = until - window / 4;
      std::uint64_t selected = 0;
      for (const auto &event : matching) {
        if (event.swapchain == preferred && preferred != 0 && event.qpc > recent) {
          selected = preferred;
          break;
        }
      }
      if (selected == 0) {
        for (const auto &event : matching) {
          auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &entry) {
            return entry.first == event.swapchain;
          });
          if (it == counts.end()) {
            counts.emplace_back(event.swapchain, 1u);
          } else {
            ++it->second;
          }
        }
        selected = std::max_element(counts.begin(), counts.end(), [](const auto &a, const auto &b) {
                     return a.second < b.second;
                   })->first;
      }

      for (const auto &event : matching) {
        if (event.swapchain == selected && event.qpc > since) {
          out.push_back(event.qpc);
        }
      }
      // Per-processor ETW buffers do not deliver events in time order.
      std::sort(out.begin(), out.end());
      return selected;
    }

  private:
    struct event_t {
      std::int64_t qpc;
      std::uint64_t swapchain;
      DWORD pid;
    };

    ULONG start_session(const bool millisecond_flush) {
      auto properties = make_properties(millisecond_flush);
      TRACEHANDLE handle = 0;
      const auto status = StartTraceW(&handle, session_name, &properties.header);
      if (status == ERROR_SUCCESS) {
        session_ = handle;
      }
      return status;
    }

    static void WINAPI on_event_record(PEVENT_RECORD record) {
      auto *self = static_cast<tracker_t *>(record->UserContext);
      const auto &header = record->EventHeader;
      if (self == nullptr || !IsEqualGUID(header.ProviderId, dxgi_provider)) {
        return;
      }
      const auto id = header.EventDescriptor.Id;
      if (id != present_start_event && id != present_multiplane_overlay_start_event) {
        return;
      }

      // Payload: pIDXGISwapChain (pointer), Flags (UInt32), SyncInterval (Int32).
      const std::size_t pointer_size = (header.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
      if (record->UserData == nullptr || record->UserDataLength < pointer_size + sizeof(std::uint32_t)) {
        return;
      }
      const auto *data = static_cast<const std::byte *>(record->UserData);
      std::uint64_t swapchain = 0;
      std::memcpy(&swapchain, data, pointer_size);
      std::uint32_t flags = 0;
      std::memcpy(&flags, data + pointer_size, sizeof(flags));
      if (flags & DXGI_PRESENT_TEST) {
        return;
      }

      self->record_present(header.TimeStamp.QuadPart, header.ProcessId, swapchain);
    }

    void record_present(const std::int64_t qpc, const DWORD pid, const std::uint64_t swapchain) {
      std::lock_guard lock(mutex_);
      events_.push_back({qpc, swapchain, pid});
      while (!events_.empty() &&
             (events_.size() > maximum_events || events_.front().qpc < qpc - retention_)) {
        events_.pop_front();
      }
    }

    TRACEHANDLE session_ {0};
    TRACEHANDLE consumer_ {INVALID_PROCESSTRACE_HANDLE};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<event_t> events_;
    std::int64_t retention_ {0};
  };

  namespace {
    std::shared_ptr<tracker_t> acquire_tracker() {
      static std::mutex mutex;
      static std::weak_ptr<tracker_t> shared;
      std::lock_guard lock(mutex);
      if (auto existing = shared.lock()) {
        return existing;
      }
      auto tracker = std::make_shared<tracker_t>();
      if (!tracker->start()) {
        return nullptr;
      }
      shared = tracker;
      return tracker;
    }
  }  // namespace

  capture_stamper_t::capture_stamper_t(const wchar_t *gdi_device_name) {
    frequency_ = query_frequency();
    if (frequency_ <= 0) {
      return;
    }
    // One 90 kHz RTP tick, so consecutive frames never share a timestamp.
    minimum_step_ = frequency_ / 90000 + 1;
    stale_ = frequency_ / 10;

    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    DWORD refresh_hz = 0;
    if (gdi_device_name != nullptr && gdi_device_name[0] != L'\0' &&
        EnumDisplaySettingsW(gdi_device_name, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
      refresh_hz = mode.dmDisplayFrequency;
      grid_ = frequency_ / refresh_hz;
    }
    if (grid_ > 0) {
      tracker_ = acquire_tracker();
    }
    BOOST_LOG(info) << "Present-time frame stamping: display refresh " << refresh_hz
                    << " Hz, DXGI present tracking " << (tracker_ ? "active" : "unavailable");
  }

  capture_stamper_t::~capture_stamper_t() = default;

  void capture_stamper_t::refresh_foreground(const std::int64_t now_qpc) {
    if (now_qpc < next_foreground_poll_) {
      return;
    }
    next_foreground_poll_ = now_qpc + frequency_ / 4;
    DWORD pid = 0;
    if (HWND window = GetForegroundWindow()) {
      GetWindowThreadProcessId(window, &pid);
    }
    foreground_pid_ = pid;
  }

  std::int64_t capture_stamper_t::stamp(const std::int64_t composition_qpc) {
    if (composition_qpc <= 0 || frequency_ <= 0) {
      return composition_qpc;
    }

    // Without present tracking keep the composition time exactly as before.
    const std::int64_t grid = tracker_ ? grid_ : 0;
    presents_.clear();
    if (grid > 0) {
      refresh_foreground(composition_qpc);
      std::uint64_t source = 0;
      if (foreground_pid_ != 0) {
        source = tracker_->presents_for(foreground_pid_, composition_qpc - stale_, composition_qpc, frequency_, source_, presents_);
      }
      if (source != source_) {
        refiner_.reset_source();
        source_ = source;
      }
    }

    const auto result = refiner_.refine(composition_qpc, grid, presents_, minimum_step_, stale_);
    ++frames_;
    matched_ += result.matched;
    refined_ += result.refined;
    if (refiner_.grid_disabled() && !grid_disabled_logged_) {
      grid_disabled_logged_ = true;
      BOOST_LOG(info) << "Present-time frame stamping disabled: compositions arrive faster than the display refresh";
    }
    if (grid > 0 && frames_ % 1200 == 0) {
      BOOST_LOG(debug) << "Present-time frame stamping: matched=" << matched_ * 100 / frames_
                       << "% refined=" << refined_ * 100 / frames_
                       << "% present_to_composition_us=" << refiner_.latency() * 1000000 / frequency_
                       << " grid_us=" << grid * 1000000 / frequency_;
    }
    return result.stamp;
  }

}  // namespace platf::dxgi::present_timing
