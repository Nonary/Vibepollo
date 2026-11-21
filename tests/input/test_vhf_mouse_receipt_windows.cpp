// Exercise the production receipt matcher without installing or injecting input.
#include "src/platform/windows/vhf_mouse_receipt.h"
#include <cassert>

namespace platf::vhf_keyboard_mouse {
  struct mouse_receipt_test {
    static void run() {
      const auto previous_tag = mouse_receipt::injection_sequence.load();
      mouse_receipt::injection_sequence.store(UINT32_MAX - 1);
      assert(mouse_receipt::next_injection_tag() == UINT32_MAX);
      assert(mouse_receipt::next_injection_tag() == 1); // zero skipped at wrap
      mouse_receipt::injection_sequence.store(previous_tag);
      const auto parent = L"VHF\\HID_DEVICE_SYSTEM_VHF\\1&2510ced4&2&VibeshineKeyboardMouse";
      assert(mouse_receipt::collection_identity(L"HID\\HID_DEVICE_SYSTEM_VHF&Col02\\2&123&0&0001", parent) == 2);
      assert(mouse_receipt::collection_identity(L"hid\\hid_device_system_vhf&col03\\2&123&0&0001", parent) == 3);
      assert(mouse_receipt::collection_identity(L"HID\\HID_DEVICE_SYSTEM_VHF&Col01\\2&123&0&0001", parent) == 0);
      assert(mouse_receipt::collection_identity(L"HID\\HID_DEVICE_SYSTEM_VHF&Col02\\2&123&0&0001",
        L"VHF\\HID_DEVICE_SYSTEM_VHF\\1&2510ced4&2&VibeshineGamepad0") == 0);
      mouse_receipt receipt;
      receipt.relative_device = reinterpret_cast<HANDLE>(1);
      receipt.absolute_device = reinterpret_cast<HANDLE>(2);
      mouse_receipt::expectation expected {};
      expected.motion = true;
      expected.x = 10;
      expected.y = -9;
      expected.buttons = RI_MOUSE_LEFT_BUTTON_DOWN;
      expected.wheel = 120;
      receipt.arm(expected);
      RAWINPUT input {};
      input.header.dwType = RIM_TYPEMOUSE;
      input.header.hDevice = reinterpret_cast<HANDLE>(3); // physical/other child
      input.data.mouse.lLastX = 10;
      input.data.mouse.lLastY = -9;
      input.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
      input.data.mouse.usButtonData = 120;
      receipt.observe(input);
      assert(!receipt.pending.empty() && receipt.pending.front().motion);
      input.header.hDevice = receipt.relative_device;
      input.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
      receipt.observe(input);
      assert(!receipt.pending.empty() && receipt.pending.front().motion);
      input.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
      input.data.mouse.usButtonFlags = 0;
      receipt.observe(input);
      assert(!receipt.pending.empty() && !receipt.pending.front().motion && receipt.pending.front().buttons);
      input.data.mouse.lLastX = input.data.mouse.lLastY = 0;
      input.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL;
      input.data.mouse.usButtonData = 240; // wrong detent data cannot acknowledge
      receipt.observe(input);
      assert(!receipt.pending.empty() && receipt.pending.front().wheel == 120);
      input.data.mouse.usButtonData = 120;
      receipt.observe(input);
      assert(receipt.wait());

      expected = {};
      expected.absolute = expected.motion = true;
      expected.x = 65535;
      expected.y = 0;
      receipt.arm(expected);
      input.header.hDevice = receipt.absolute_device;
      input.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
      input.data.mouse.lLastX = 65533; // inbox 15-bit HID scaling rounding
      input.data.mouse.lLastY = 0;
      std::thread delivery([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        receipt.observe(input);
      });
      const auto start = std::chrono::steady_clock::now();
      assert(receipt.wait());
      assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(5));
      delivery.join();

      expected = {};
      expected.buttons = RI_MOUSE_RIGHT_BUTTON_UP;
      receipt.arm(expected);
      assert(!receipt.wait()); // no cursor/key-state-based success
      assert(!receipt.pending.empty()); // timed-out receipts remain uncertain

      receipt.pending.clear();
      asynchronous_stream(receipt);
      coalesced_absolute(receipt);
      observer_scope(receipt);
      observer_restart(receipt);
    }

    static void asynchronous_stream(mouse_receipt &receipt) {
      mouse_receipt::expectation move {};
      move.motion = true;
      move.x = 1;
      const auto before = receipt.stats();
      for (int i = 0; i < 32; ++i) assert(receipt.arm(move));
      const auto submitted = receipt.stats();
      assert(submitted.barriers == before.barriers); // no per-movement wait
      assert(submitted.peak_pending >= 32);
      RAWINPUT input {};
      input.header.dwType = RIM_TYPEMOUSE;
      input.header.hDevice = receipt.relative_device;
      input.data.mouse.lLastX = 1;
      std::thread delivery([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (int i = 0; i < 32; ++i) receipt.observe(input);
      });
      move.absolute = true;
      assert(receipt.arm(move)); // crossing waits for all prior relative receipts
      delivery.join();
      assert(receipt.stats().barriers == before.barriers + 1);
      assert(receipt.pending.size() == 1 && receipt.pending.front().absolute);
      input.header.hDevice = receipt.absolute_device;
      input.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
      receipt.observe(input);
      assert(receipt.wait());

      // Capacity is bounded. Full-queue backpressure times out without admitting
      // an additional event, keeping the uncertain prior state for recovery.
      move.absolute = false;
      for (int i = 0; i < 64; ++i) assert(receipt.arm(move));
      assert(!receipt.arm(move));
      assert(receipt.pending.size() == 64);
      receipt.pending.clear();
    }

    static void coalesced_absolute(mouse_receipt &receipt) {
      mouse_receipt::expectation position {};
      position.absolute = position.motion = true;
      position.x = position.y = 100;
      assert(receipt.arm(position));
      position.x = position.y = 200;
      assert(receipt.arm(position));
      position.x = position.y = 300;
      assert(receipt.arm(position));
      RAWINPUT input {};
      input.header.dwType = RIM_TYPEMOUSE;
      input.header.hDevice = receipt.absolute_device;
      input.data.mouse.usFlags = MOUSE_MOVE_ABSOLUTE;
      input.data.mouse.lLastX = input.data.mouse.lLastY = 300;
      const auto before = receipt.stats();
      receipt.observe(input);
      assert(receipt.wait() && receipt.stats().coalesced == before.coalesced + 2);

      // A/B/A must not let a receipt for the first A retire the final A.
      position.x = position.y = 100;
      assert(receipt.arm(position));
      position.x = position.y = 200;
      assert(receipt.arm(position));
      input.data.mouse.lLastX = input.data.mouse.lLastY = 200;
      std::thread delivery([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        receipt.observe(input);
      });
      position.x = position.y = 100;
      const auto started = std::chrono::steady_clock::now();
      assert(receipt.arm(position));
      assert(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(5));
      delivery.join();
      assert(receipt.pending.size() == 1 && receipt.pending.front().x == 100);
      input.data.mouse.lLastX = input.data.mouse.lLastY = 100;
      receipt.observe(input);
      assert(receipt.wait());
    }

    static void observer_scope(mouse_receipt &receipt) {
      mouse_receipt::expectation expected {};
      expected.injected = 12345;
      receipt.arm(expected);
      mouse_receipt::observer = &receipt;
      MSLLHOOKSTRUCT event {};
      event.dwExtraInfo = 12344;
      event.flags = LLMHF_INJECTED;
      mouse_receipt::mouse_hook(HC_ACTION, WM_MOUSEMOVE, reinterpret_cast<LPARAM>(&event));
      assert(!receipt.pending.empty());
      event.dwExtraInfo = 12345;
      event.flags = 0;
      mouse_receipt::mouse_hook(HC_ACTION, WM_MOUSEMOVE, reinterpret_cast<LPARAM>(&event));
      assert(!receipt.pending.empty());
      event.flags = LLMHF_INJECTED;
      mouse_receipt::mouse_hook(HC_ACTION, WM_MOUSEMOVE, reinterpret_cast<LPARAM>(&event));
      assert(receipt.wait());
      mouse_receipt::observer = nullptr;
    }

    static void observer_restart(mouse_receipt &receipt) {
      // Simulate a retiring observer publishing state on its way out. Stop
      // must join before clearing that state for the replacement observer.
      receipt.worker = std::thread([&] {
        std::lock_guard lock(receipt.mutex);
        receipt.hook = reinterpret_cast<HHOOK>(1);
        receipt.relative_device = reinterpret_cast<HANDLE>(2);
        receipt.absolute_device = reinterpret_cast<HANDLE>(3);
        receipt.pending.push_back({.injected = 42});
        receipt.initialized = true;
        receipt.removed = true;
      });
      receipt.stop();
      assert(!receipt.worker.joinable());
      assert(!receipt.window && !receipt.hook && !receipt.initialized);
      assert(!receipt.relative_device && !receipt.absolute_device && !receipt.removed);
      assert(receipt.wait() && receipt.pending.empty() && receipt.stats().operations == 0);

      // A receipt for the old device cannot satisfy the new observer.
      receipt.relative_device = reinterpret_cast<HANDLE>(4);
      assert(receipt.arm({.motion = true, .x = 1}));
      RAWINPUT input {};
      input.header.dwType = RIM_TYPEMOUSE;
      input.header.hDevice = reinterpret_cast<HANDLE>(2);
      input.data.mouse.lLastX = 1;
      receipt.observe(input);
      assert(receipt.pending.size() == 1);
      input.header.hDevice = receipt.relative_device;
      receipt.observe(input);
      assert(receipt.wait());
    }
  };
}

int main() {
  platf::vhf_keyboard_mouse::mouse_receipt_test::run();
}
