#include "display_output_monitor.h"
#include "src/logging.h"
#include "src/capture_color_state.h"
#include "src/platform/windows/ipc/display_settings_client.h"
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
                                                     int offset_x, int offset_y, int env_width, int env_height, std::uint64_t mutation_revision):
      adapter_(adapter), output_(output), hdr_valid_(hdr_valid), hdr_(hdr),
      offset_x_(offset_x), offset_y_(offset_y), env_width_(env_width), env_height_(env_height),
      mutation_revision_(mutation_revision), worker_([this](std::stop_token stop) { run(stop); }) {}

  display_output_monitor_t::~display_output_monitor_t() {
    worker_.request_stop();
    notifications_->cv.notify_all();
    worker_.join();
  }

  bool display_output_monitor_t::mutation_current() const {
    return mutation_revision_ == platf::display_helper_client::capture_mutation_revision();
  }

  bool display_output_monitor_t::wait_for_initial_validation(std::chrono::milliseconds timeout) {
    std::unique_lock lock(notifications_->mutex);
    notifications_->cv.wait_for(lock, timeout, [this] {
      return initial_validated_.load(std::memory_order_acquire) || reinit_requested() || failed();
    });
    return initial_validated_.load(std::memory_order_acquire) && mutation_current() && !hold_frames() && !reinit_requested() && !failed();
  }

  bool display_output_monitor_t::hold_frames() const {
    const int expected = notifications_->expected_hdr.load(std::memory_order_acquire);
    return hold_.load(std::memory_order_acquire) ||
      (expected >= 0 && (expected != 0) != hdr_) ||
      (expected < 0 && notifications_->notified.load(std::memory_order_acquire));
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
          if (controller.DispatcherQueue().TryEnqueue([subscription, notifications, monitor, registration, messaging] {
                try {
                  constexpr GUID iid {0x7449121c, 0x382b, 0x4705, {0x8d, 0xa7, 0xa7, 0x95, 0xba, 0x48, 0x20, 0x13}};
                  auto factory = winrt::get_activation_factory<winrt::Windows::Graphics::Display::DisplayInformation>();
                  winrt::com_ptr<display_information_interop_t> interop;
                  winrt::check_hresult(factory.as<IInspectable>()->QueryInterface(iid, interop.put_void()));
                  winrt::check_hresult(interop->GetForMonitor(monitor, winrt::guid_of<winrt::Windows::Graphics::Display::DisplayInformation>(), winrt::put_abi(subscription->info)));
                  subscription->token = subscription->info.AdvancedColorInfoChanged([notifications](auto const &sender, auto const &) {
                    try {
                      notifications->expected_hdr.store(sender.GetAdvancedColorInfo().CurrentAdvancedColorKind() == winrt::Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange ? 1 : 0, std::memory_order_release);
                    } catch (...) { notifications->expected_hdr.store(-1, std::memory_order_release); }
                    notifications->notified.store(true, std::memory_order_release);
                    notifications->dirty.store(true, std::memory_order_release);
                    notifications->cv.notify_all();
                  });
                  subscription->registered = true;
                  notifications->expected_hdr.store(subscription->info.GetAdvancedColorInfo().CurrentAdvancedColorKind() == winrt::Windows::Graphics::Display::AdvancedColorKind::HighDynamicRange ? 1 : 0, std::memory_order_release);
                  registration->set_value(true);
                } catch (...) { registration->set_value(false); }
              }) && ready.wait_for(2s) == std::future_status::ready) subscribed = ready.get();
        }
      }
    } catch (...) { subscribed = false; }
#endif
    BOOST_LOG(debug) << "Display color monitoring: " << (subscribed ? "AdvancedColorInfoChanged events" : "background polling fallback");
    auto next_poll = std::chrono::steady_clock::time_point::min();
    video::capture_color::transition_t transition(hdr_);
    std::optional<output_sample_t> last_sample;
    const auto publish = [this](video::capture_color::action_e action) {
      hold_.store(action == video::capture_color::action_e::hold, std::memory_order_release);
      if (action == video::capture_color::action_e::retain) initial_validated_.store(true, std::memory_order_release);
      if (action == video::capture_color::action_e::reinit) reinit_.store(true, std::memory_order_release);
      if (action == video::capture_color::action_e::error) failed_.store(true, std::memory_order_release);
      notifications_->cv.notify_all();
    };
    const std::string output_name(output_.DeviceName, output_.DeviceName + std::wcslen(output_.DeviceName));
    while (!stop.stop_requested() && !reinit_requested()) {
      // This is the background response-reader fallback when no command waiter
      // owns the pipe. It also acknowledges mutation admission before a helper
      // worker can change the source, so capture sees the hold atomics first.
      platf::display_helper_client::pump_mutation_notifications();
      const auto mutation = platf::display_helper_client::capture_mutation_state(output_name);
      const auto mutation_action = transition.mutation(mutation.revision, mutation.active, mutation.failed);
      if (mutation_action != video::capture_color::action_e::retain) {
        publish(mutation_action);
        if (mutation_action != video::capture_color::action_e::hold) break;
      } else {
        const auto now = std::chrono::steady_clock::now();
        const bool notified = notifications_->dirty.exchange(false, std::memory_order_acq_rel);
        if (notified) {
          const int expected = notifications_->expected_hdr.load(std::memory_order_acquire);
          transition.notified(now, expected < 0 ? std::nullopt : std::optional<bool>(expected != 0));
        }
        const bool retrying = transition.validation_pending();
        const bool stale = last_sample && !last_sample->factory->IsCurrent();
        const auto input_change = wgc_policy::assess_input_geometry(
          {offset_x_, offset_y_, env_width_, env_height_}, output_.DesktopCoordinates.left, output_.DesktopCoordinates.top,
          {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)});
        if (input_change == wgc_policy::input_geometry_change_e::changed) {
          reinit_.store(true, std::memory_order_release);
          break;
        }
        if (notified || stale || (retrying && now >= next_poll) || (!subscribed && now >= next_poll)) {
          next_poll = now + (retrying ? 50ms : 1s);
          auto sample = sample_output(adapter_, output_.DeviceName);
          if (!sample) {
            publish(transition.observed(std::nullopt, now));
          } else {
            const auto &a = output_.DesktopCoordinates;
            const auto &b = sample->desc.DesktopCoordinates;
            const bool structural = !sample->desc.AttachedToDesktop || sample->desc.Monitor != output_.Monitor ||
              sample->desc.Rotation != output_.Rotation || a.left != b.left || a.top != b.top || a.right != b.right || a.bottom != b.bottom || sample->hdr_valid != hdr_valid_;
            if (structural) { reinit_.store(true, std::memory_order_release); break; }
            publish(transition.observed(sample->hdr, now));
            notifications_->notified.store(false, std::memory_order_release);
            last_sample = std::move(sample);
          }
        }
      }
      std::unique_lock lock(notifications_->mutex);
      notifications_->cv.wait_for(lock, stop, 25ms, [this] { return notifications_->dirty.load(std::memory_order_acquire); });
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
