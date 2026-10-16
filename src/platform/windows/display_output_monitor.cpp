#include "display_output_monitor.h"
#include "src/logging.h"
#include "src/platform/windows/wgc_capture_policy.h"

#include <chrono>
#include <cwchar>
#include <future>
#include <optional>
#include <wrl/client.h>

// C++/WinRT is already required by the WGC helper. Define only the documented
// desktop interop ABI here so older MinGW SDK headers do not disable supported
// Windows 11 runtimes at compile time.
#if __has_include(<winrt/Windows.Graphics.Display.h>)
  #define VIBESHINE_HAS_DISPLAY_COLOR_EVENTS 1
  #include <winrt/Windows.Foundation.h>
  #include <winrt/Windows.Graphics.Display.h>
  #include <winrt/Windows.System.h>

  struct display_information_interop_t : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND window, REFIID riid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetForMonitor(HMONITOR monitor, REFIID riid, void **result) = 0;
  };
  #ifdef __MINGW32__
  __CRT_UUID_DECL(display_information_interop_t, 0x7449121c, 0x382b, 0x4705, 0x8d, 0xa7, 0xa7, 0x95, 0xba, 0x48, 0x20, 0x13)
  #endif
#endif

namespace platf::dxgi {
  using namespace std::chrono_literals;
  namespace {
    std::atomic_uint64_t next_monitor_id {1};
    template<class T> using com_ptr = Microsoft::WRL::ComPtr<T>;
    struct output_sample_t {
      com_ptr<IDXGIFactory1> factory;
      DXGI_OUTPUT_DESC desc {};
      bool hdr_valid = false;
      bool hdr = false;
    };
    std::optional<output_sample_t> sample_output(LUID luid, const wchar_t *name) {
      output_sample_t result;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&result.factory)))) return std::nullopt;
      for (UINT a = 0;; ++a) {
        com_ptr<IDXGIAdapter1> adapter;
        if (result.factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) return std::nullopt;
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc))) return std::nullopt;
        if (desc.AdapterLuid.HighPart != luid.HighPart || desc.AdapterLuid.LowPart != luid.LowPart) continue;
        for (UINT o = 0;; ++o) {
          com_ptr<IDXGIOutput> output;
          if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
          if (!output) return std::nullopt;
          if (FAILED(output->GetDesc(&result.desc))) return std::nullopt;
          if (std::wcscmp(name, result.desc.DeviceName) != 0) continue;
          com_ptr<IDXGIOutput6> output6;
          if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 color {};
            if (FAILED(output6->GetDesc1(&color))) return std::nullopt;
            result.hdr_valid = true;
            result.hdr = color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
          }
          return result;
        }
      }
      return std::nullopt;
    }
  }

  display_output_monitor_t::display_output_monitor_t(LUID adapter, DXGI_OUTPUT_DESC output, bool hdr_valid, bool hdr,
                                                     int offset_x, int offset_y, int env_width, int env_height):
      adapter_(adapter), output_(output), hdr_valid_(hdr_valid), hdr_(hdr),
      offset_x_(offset_x), offset_y_(offset_y), env_width_(env_width), env_height_(env_height),
      diagnostic_id_(next_monitor_id.fetch_add(1, std::memory_order_relaxed)),
      worker_([this](std::stop_token stop) { run(stop); }) {}

  display_output_monitor_t::~display_output_monitor_t() {
    worker_.request_stop();
    notifications_->cv.notify_all();
    worker_.join();
  }

  void display_output_monitor_t::run(std::stop_token stop) {
    bool subscribed = false;
#ifdef VIBESHINE_HAS_DISPLAY_COLOR_EVENTS
    bool apartment_initialized = false;
    winrt::Windows::System::DispatcherQueueController controller {nullptr};
    struct subscription_t {
      winrt::Windows::Graphics::Display::DisplayInformation info {nullptr};
      winrt::event_token token {};
      bool registered = false;
    };
    auto subscription = std::make_shared<subscription_t>();
    auto messaging = std::shared_ptr<std::remove_pointer_t<HMODULE>>(LoadLibraryW(L"CoreMessaging.dll"), [](HMODULE module) { if (module) FreeLibrary(module); });
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      apartment_initialized = true;
      if (messaging) {
        // Exact ABI of CreateDispatcherQueueController; avoid mixing generated
        // C++/WinRT projections with MinGW's older windows.system ABI header.
        struct queue_options_t { DWORD size; int thread_type; int apartment_type; };
        using create_queue_t = HRESULT (WINAPI *)(queue_options_t, void **);
        const auto create_queue = reinterpret_cast<create_queue_t>(GetProcAddress(messaging.get(), "CreateDispatcherQueueController"));
        if (create_queue) {
          queue_options_t options {sizeof(options), 1, 2}; // dedicated thread, COM STA
          winrt::check_hresult(create_queue(options, winrt::put_abi(controller)));
          auto registration = std::make_shared<std::promise<bool>>();
          auto ready = registration->get_future();
          const auto notifications = notifications_;
          const auto monitor = output_.Monitor;
          const bool captured_hdr = hdr_;
          const auto observation_id = diagnostic_id_;
          if (controller.DispatcherQueue().TryEnqueue([subscription, notifications, monitor, captured_hdr, observation_id, registration, messaging] {
                try {
                  constexpr GUID iid {0x7449121c, 0x382b, 0x4705, {0x8d, 0xa7, 0xa7, 0x95, 0xba, 0x48, 0x20, 0x13}};
                  auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Display::DisplayInformation>();
                  winrt::com_ptr<display_information_interop_t> interop;
                  winrt::check_hresult(factory.as<IInspectable>()->QueryInterface(iid, interop.put_void()));
                  winrt::check_hresult(interop->GetForMonitor(monitor, winrt::guid_of<winrt::Windows::Graphics::Display::DisplayInformation>(), winrt::put_abi(subscription->info)));
                  // Older runtimes can expose DisplayInformation without the
                  // color API. Leave those runtimes on the DXGI polling fallback.
                  const auto initial_kind = subscription->info.GetAdvancedColorInfo().CurrentAdvancedColorKind();
                  const bool initial_hdr = initial_kind == winrt::Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange;
                  const auto color_changed = [notifications, captured_hdr, observation_id](auto const &sender) {
                    try {
                      const auto kind = sender.GetAdvancedColorInfo().CurrentAdvancedColorKind();
                      const bool hdr = kind == winrt::Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange;
                      if (hdr != captured_hdr && !notifications->reinit.exchange(true, std::memory_order_release)) {
                        BOOST_LOG(debug) << "Capture restart request: source=output-monitor reason=winrt-color-event captured_hdr="
                                         << captured_hdr << " observed_hdr=" << hdr << " observed_kind=" << static_cast<int>(kind)
                                         << " monitor_id=" << observation_id;
                      }
                    } catch (...) {
                      // Unknown color is not SDR; refresh the DXGI observation.
                    }
                    notifications->dirty.store(true, std::memory_order_release);
                    notifications->cv.notify_all();
                  };
                  subscription->token = subscription->info.AdvancedColorInfoChanged(
                    [color_changed](auto const &sender, auto const &) { color_changed(sender); });
                  subscription->registered = true;
                  if (initial_hdr != captured_hdr && !notifications->reinit.exchange(true, std::memory_order_release)) {
                    BOOST_LOG(debug) << "Capture restart request: source=output-monitor reason=winrt-initial-color captured_hdr="
                                     << captured_hdr << " observed_hdr=" << initial_hdr << " observed_kind=" << static_cast<int>(initial_kind)
                                     << " monitor_id=" << observation_id;
                  }
                  registration->set_value(true);
                } catch (...) { registration->set_value(false); }
              }) && ready.wait_for(2s) == std::future_status::ready) subscribed = ready.get();
        }
      }
    } catch (...) { subscribed = false; }
#endif
    BOOST_LOG(debug) << "Display color monitoring: " << (subscribed ? "AdvancedColorInfoChanged events" : "background polling fallback")
                     << " monitor_id=" << diagnostic_id_;
    auto next_poll = std::chrono::steady_clock::time_point::min();
    std::optional<std::chrono::steady_clock::time_point> sample_failure_since;
    constexpr auto sample_retry_interval = 100ms;
    constexpr auto sample_failure_timeout = 2s;
    std::optional<output_sample_t> last_sample;
    while (!stop.stop_requested() && !reinit_requested()) {
      const auto now = std::chrono::steady_clock::now();
      const bool notified = notifications_->dirty.exchange(false, std::memory_order_acq_rel);
      const bool stale = last_sample && !last_sample->factory->IsCurrent();
      const wgc_policy::desktop_bounds_t current_bounds {
        GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
      const auto input_change = wgc_policy::assess_input_geometry(
        {offset_x_, offset_y_, env_width_, env_height_}, output_.DesktopCoordinates.left, output_.DesktopCoordinates.top,
        current_bounds);
      if (input_change == wgc_policy::input_geometry_change_e::changed) {
        if (!notifications_->reinit.exchange(true, std::memory_order_release)) {
          BOOST_LOG(debug) << "Capture restart request: source=output-monitor reason=input-geometry captured="
                           << offset_x_ << ',' << offset_y_ << ',' << env_width_ << ',' << env_height_
                           << " observed_desktop=" << current_bounds.origin_x << ',' << current_bounds.origin_y
                           << ',' << current_bounds.width << ',' << current_bounds.height << " monitor_id=" << diagnostic_id_;
        }
        break;
      }
      if (notified || stale || ((!subscribed || sample_failure_since) && now >= next_poll)) {
        next_poll = now + 1s;
        auto sample = sample_output(adapter_, output_.DeviceName);
        const auto &a = output_.DesktopCoordinates;
        if (!sample) {
          // DXGI enumeration can be temporarily unavailable while a color or
          // display notification settles, even while WGC still produces frames.
          // Keep consuming capture until an observation proves a change or the
          // bounded retry window expires; an unknown sample is not a new state.
          const auto failed_at = std::chrono::steady_clock::now();
          if (!sample_failure_since) {
            sample_failure_since = failed_at;
            BOOST_LOG(debug) << "Display output observation unavailable; retrying without capture reinitialization.";
          }
          if (failed_at - *sample_failure_since >= sample_failure_timeout) {
            BOOST_LOG(warning) << "Display output observation remained unavailable; requesting capture reinitialization.";
            if (!notifications_->reinit.exchange(true, std::memory_order_release)) {
              BOOST_LOG(debug) << "Capture restart request: source=output-monitor reason=observation-timeout monitor_id=" << diagnostic_id_;
            }
            break;
          }
          next_poll = failed_at + sample_retry_interval;
        } else {
          if (sample_failure_since) {
            BOOST_LOG(debug) << "Display output observation recovered after a transient failure.";
          }
          sample_failure_since.reset();
          if (!sample->desc.AttachedToDesktop || sample->desc.Monitor != output_.Monitor ||
              sample->desc.Rotation != output_.Rotation ||
              a.left != sample->desc.DesktopCoordinates.left || a.top != sample->desc.DesktopCoordinates.top ||
              a.right != sample->desc.DesktopCoordinates.right || a.bottom != sample->desc.DesktopCoordinates.bottom ||
              sample->hdr_valid != hdr_valid_ || sample->hdr != hdr_) {
            if (!notifications_->reinit.exchange(true, std::memory_order_release)) {
              const auto &b = sample->desc.DesktopCoordinates;
              BOOST_LOG(debug) << "Capture restart request: source=output-monitor reason=output-state attached="
                               << sample->desc.AttachedToDesktop << " monitor_changed=" << (sample->desc.Monitor != output_.Monitor)
                               << " rotation=" << static_cast<int>(output_.Rotation) << "->" << static_cast<int>(sample->desc.Rotation)
                               << " rect=" << a.left << ',' << a.top << ',' << a.right << ',' << a.bottom
                               << "->" << b.left << ',' << b.top << ',' << b.right << ',' << b.bottom
                               << " hdr_valid=" << hdr_valid_ << "->" << sample->hdr_valid << " hdr=" << hdr_ << "->" << sample->hdr
                               << " monitor_id=" << diagnostic_id_;
            }
            break;
          }
          last_sample = std::move(sample);
        }
      }
      std::unique_lock lock(notifications_->mutex);
      notifications_->cv.wait_for(lock, stop, 100ms, [this] {
        return notifications_->dirty.load(std::memory_order_acquire);
      });
    }
#ifdef VIBESHINE_HAS_DISPLAY_COLOR_EVENTS
    if (controller) {
      auto revoked = std::make_shared<std::promise<void>>();
      auto ready = revoked->get_future();
      if (controller.DispatcherQueue().TryEnqueue([subscription, revoked, messaging] {
            try { if (subscription->info && subscription->registered) subscription->info.AdvancedColorInfoChanged(subscription->token); } catch (...) {}
            subscription->info = nullptr;
            revoked->set_value();
          })) ready.wait_for(2s);
      try {
        auto stopped = std::make_shared<std::promise<void>>();
        auto done = stopped->get_future();
        auto shutdown = controller.ShutdownQueueAsync();
        shutdown.Completed([stopped, messaging](auto const &, auto) { stopped->set_value(); });
        if (done.wait_for(2s) != std::future_status::ready) {
          BOOST_LOG(warning) << "Display color event dispatcher shutdown remains pending; callbacks retain their own state";
        }
      } catch (...) {}
      controller = nullptr;
    }
    if (apartment_initialized) winrt::uninit_apartment();
#endif
  }
}
