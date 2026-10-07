/**
 * @file src/platform/windows/advanced_color_watcher.cpp
 * @brief Definitions for the advanced-color change watcher.
 */
// standard includes
#include <future>
#include <utility>

// platform includes
#include <winsock2.h>
#include <windows.h>
#include <dispatcherqueue.h>
#include <inspectable.h>
#include <roapi.h>

// lib includes
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>

// local includes
#include "advanced_color_watcher.h"
#include "src/logging.h"
#include "src/utility.h"

// From windows.graphics.display.interop.h (Windows 11 SDK 10.0.22621), which
// MinGW does not ship.
struct IDisplayInformationStaticsInterop: public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND window, REFIID riid, void **display_info) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetForMonitor(HMONITOR monitor, REFIID riid, void **display_info) = 0;
};
__CRT_UUID_DECL(IDisplayInformationStaticsInterop, 0x7449121c, 0x382b, 0x4705, 0x8d, 0xa7, 0xa7, 0x95, 0xba, 0x48, 0x20, 0x13)

namespace platf::dxgi {
  namespace {
    using winrt::Windows::Graphics::Display::DisplayInformation;
    using winrt::Windows::System::DispatcherQueueController;

    DisplayInformation display_information_for(HMONITOR monitor) {
      // Activate directly rather than through winrt::get_activation_factory:
      // its process-wide cache would outlive this thread's apartment.
      const winrt::hstring class_name {winrt::name_of<DisplayInformation>()};
      winrt::com_ptr<IDisplayInformationStaticsInterop> interop;
      winrt::check_hresult(RoGetActivationFactory(
        static_cast<HSTRING>(winrt::get_abi(class_name)),
        winrt::guid_of<IDisplayInformationStaticsInterop>(),
        interop.put_void()
      ));

      DisplayInformation info {nullptr};
      winrt::check_hresult(interop->GetForMonitor(monitor, winrt::guid_of<DisplayInformation>(), winrt::put_abi(info)));
      return info;
    }

    // Dispatch this thread's messages until `handle` is signaled. Returns
    // false if the wait times out or fails first.
    bool pump_until(HANDLE handle, DWORD timeout_ms) {
      const auto deadline = GetTickCount64() + timeout_ms;
      for (;;) {
        DWORD remaining = INFINITE;
        if (timeout_ms != INFINITE) {
          const auto now = GetTickCount64();
          remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        }
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &handle, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
          return true;
        }
        if (wait != WAIT_OBJECT_0 + 1) {
          return false;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
      }
    }

    // A queue bound to this thread has to be shut down while the thread still
    // dispatches messages.
    void shut_down_queue(DispatcherQueueController &controller) noexcept {
      if (!controller) {
        return;
      }

      try {
        winrt::handle done {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (done) {
          auto shutdown = controller.ShutdownQueueAsync();
          shutdown.Completed([event = done.get()](const auto &, const auto &) {
            SetEvent(event);
          });
          if (!pump_until(done.get(), 2000)) {
            BOOST_LOG(warning) << "Display color notification queue did not shut down within 2 s";
          }
        }
      } catch (const winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Failed to shut down the display color notification queue [0x" << util::hex(e.code().value).to_string_view() << ']';
      }
      controller = nullptr;
    }

    void watch(HMONITOR monitor, std::function<void()> on_change, HANDLE stop, std::promise<bool> &subscribed) {
      try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
      } catch (const winrt::hresult_error &e) {
        BOOST_LOG(debug) << "Display color notifications unavailable: COM apartment [0x" << util::hex(e.code().value).to_string_view() << ']';
        subscribed.set_value(false);
        return;
      }

      DispatcherQueueController controller {nullptr};
      DisplayInformation info {nullptr};
      winrt::event_token token {};
      try {
        const DispatcherQueueOptions options {sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(
          options,
          reinterpret_cast<ABI::Windows::System::IDispatcherQueueController **>(winrt::put_abi(controller))
        ));
        info = display_information_for(monitor);
        token = info.AdvancedColorInfoChanged([on_change = std::move(on_change)](const auto &, const auto &) {
          on_change();
        });
      } catch (const winrt::hresult_error &e) {
        BOOST_LOG(debug) << "Display color notifications unavailable [0x" << util::hex(e.code().value).to_string_view() << "]: " << winrt::to_string(e.message());
        info = nullptr;
        shut_down_queue(controller);
        winrt::uninit_apartment();
        subscribed.set_value(false);
        return;
      }

      // `subscribed` belongs to start(), which returns once it is set.
      subscribed.set_value(true);
      (void) pump_until(stop, INFINITE);

      try {
        info.AdvancedColorInfoChanged(token);
      } catch (const winrt::hresult_error &) {
        // The subscription ends with the object either way.
      }
      info = nullptr;
      shut_down_queue(controller);
      winrt::uninit_apartment();
    }
  }  // namespace

  std::unique_ptr<advanced_color_watcher_t> advanced_color_watcher_t::start(HMONITOR monitor, std::function<void()> on_change) {
    if (!monitor || !on_change) {
      return nullptr;
    }

    std::unique_ptr<advanced_color_watcher_t> watcher {new advanced_color_watcher_t()};
    watcher->_stop.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!watcher->_stop) {
      return nullptr;
    }

    std::promise<bool> subscribed;
    auto result = subscribed.get_future();
    watcher->_thread = std::thread([monitor, on_change = std::move(on_change), stop = watcher->_stop.get(), &subscribed]() mutable {
      watch(monitor, std::move(on_change), stop, subscribed);
    });
    if (!result.get()) {
      watcher->_thread.join();
      return nullptr;
    }
    return watcher;
  }

  advanced_color_watcher_t::~advanced_color_watcher_t() {
    if (_thread.joinable()) {
      SetEvent(_stop.get());
      _thread.join();
    }
  }

}  // namespace platf::dxgi
