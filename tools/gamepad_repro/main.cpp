// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Manual host-only lifecycle probe. Sends neutral input to its own controllers.
#include "libvirtualgamepad/client.h"

#include <hidsdi.h>
#include <setupapi.h>
#include <Xinput.h>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::atomic<bool> interrupted {false};
ULONGLONG started;

BOOL WINAPI interrupt(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
  interrupted.store(true);
  return TRUE;
}

void log(const char *message) {
  std::printf("[%8llu ms] %s\n", GetTickCount64() - started, message);
  std::fflush(stdout);
}

void require(DWORD status, const char *operation) {
  if (status != ERROR_SUCCESS) {
    throw std::runtime_error(std::string(operation) + ": Win32 error " + std::to_string(status));
  }
}

struct options {
  std::string profile = "both";
  std::string cleanup = "alternate";
  unsigned cycles = 20;
  unsigned connected_ms = 5000;
  unsigned disconnected_ms = 5000;
};

void usage() {
  std::puts("vhf-gamepad-repro [--profile xbox|dualsense|both|ds4|switch]\n"
            "  [--cycles 20] [--connected-ms 5000] [--disconnected-ms 5000]\n"
            "  [--cleanup destroy|close|alternate]\n"
            "Run with streaming sessions disconnected. Keep Steam open to observe it.\n"
            "Creates slots 0 (and 1 for both); sends only neutral input. Ctrl+C stops.\n"
            "IOCTL success alone does not prove that Windows or Steam removed a device.");
}

unsigned number(std::string_view value, unsigned maximum) {
  unsigned result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() ||
      result == 0 || result > maximum) {
    throw std::runtime_error("Invalid positive numeric argument: " + std::string(value));
  }
  return result;
}

options parse(int argc, char **argv) {
  options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (++i == argc) throw std::runtime_error("Missing value for " + std::string(key));
    const std::string_view value = argv[i];
    if (key == "--profile") result.profile = value;
    else if (key == "--cleanup") result.cleanup = value;
    else if (key == "--cycles") result.cycles = number(value, 10000);
    else if (key == "--connected-ms") result.connected_ms = number(value, 3600000);
    else if (key == "--disconnected-ms") result.disconnected_ms = number(value, 3600000);
    else throw std::runtime_error("Unknown argument: " + std::string(key));
  }
  if (result.cleanup != "destroy" && result.cleanup != "close" && result.cleanup != "alternate") {
    throw std::runtime_error("Cleanup must be destroy, close, or alternate");
  }
  return result;
}

std::vector<lvg::profile> profiles(const std::string &name) {
  if (name == "both") return {lvg::profile::xbox_series, lvg::profile::dualsense};
  if (name == "xbox") return {lvg::profile::xbox_series};
  if (name == "dualsense") return {lvg::profile::dualsense};
  if (name == "ds4") return {lvg::profile::dualshock_4};
  if (name == "switch") return {lvg::profile::switch_pro};
  throw std::runtime_error("Unknown profile: " + name);
}

bool relevant(const HIDD_ATTRIBUTES &a) {
  return (a.VendorID == 0x054c && (a.ProductID == 0x09cc || a.ProductID == 0x0ce6)) ||
         (a.VendorID == 0x045e && (a.ProductID == 0x02ea || a.ProductID == 0x0b12)) ||
         (a.VendorID == 0x057e && a.ProductID == 0x2009);
}

// Fresh OS inventory each time. These are HID interfaces (not logical controller
// counts), including physical devices with these IDs; compare with BASELINE.
void inventory(const char *phase) {
  log(phase);
  GUID hid {};
  HidD_GetHidGuid(&hid);
  const auto devices = SetupDiGetClassDevsW(&hid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices == INVALID_HANDLE_VALUE) {
    std::printf("  HID inventory unavailable: %lu\n", GetLastError());
  } else {
    unsigned matching = 0, unreadable = 0;
    bool complete = true;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA item {};
      item.cbSize = sizeof(item);
      if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hid, index, &item)) {
        if (GetLastError() != ERROR_NO_MORE_ITEMS) complete = false;
        break;
      }
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(devices, &item, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        ++unreadable;
        continue;
      }
      std::vector<unsigned char> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, size, nullptr, nullptr)) {
        ++unreadable;
        continue;
      }
      const HANDLE handle = CreateFileW(detail->DevicePath, 0,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        ++unreadable;
        continue;
      }
      HIDD_ATTRIBUTES attributes {};
      attributes.Size = sizeof(attributes);
      if (!HidD_GetAttributes(handle, &attributes)) ++unreadable;
      else if (relevant(attributes)) {
        ++matching;
        std::printf("  HID %04x:%04x %ls\n", static_cast<unsigned>(attributes.VendorID),
                    static_cast<unsigned>(attributes.ProductID), detail->DevicePath);
      }
      CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(devices);
    std::printf("  Matching present HID interfaces: %u; unreadable HID paths: %u; enumeration: %s\n",
                matching, unreadable, complete ? "complete" : "incomplete");
  }

  const auto library = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (library) {
    const auto capabilities = reinterpret_cast<decltype(&XInputGetCapabilities)>(
        GetProcAddress(library, "XInputGetCapabilities"));
    if (capabilities) {
      unsigned connected = 0;
      for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_CAPABILITIES value {};
        const auto status = capabilities(slot, 0, &value);
        if (status == ERROR_SUCCESS) {
          ++connected;
          std::printf("  OS XInput slot %lu connected\n", slot);
        } else if (status != ERROR_DEVICE_NOT_CONNECTED) {
          std::printf("  OS XInput slot %lu query error: %lu\n", slot, status);
        }
      }
      std::printf("  OS XInput connected slots: %u (includes other physical/virtual controllers)\n", connected);
    } else std::puts("  XInputGetCapabilities unavailable");
    FreeLibrary(library);
  } else std::puts("  XInput library unavailable");
  std::fflush(stdout);
}

void wait(unsigned milliseconds, lvg::client *client = nullptr, unsigned count = 0) {
  const auto end = GetTickCount64() + milliseconds;
  do {
    if (interrupted.load()) break;
    if (client) {
      for (unsigned slot = 0; slot < count; ++slot) {
        lvg::input_state_request state {};
        state.header.size = sizeof(state);
        state.header.version = lvg::k_protocol_version;
        state.controller_id = slot;
        require(client->submit_input_state(state), "submit neutral state");
      }
    }
    Sleep(10);
  } while (GetTickCount64() < end);
}
}  // namespace

int main(int argc, char **argv) {
  started = GetTickCount64();
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    usage();
    return 0;
  }
  try {
    const auto config = parse(argc, argv);
    const auto requested = profiles(config.profile);
    require(SetConsoleCtrlHandler(interrupt, TRUE) ? ERROR_SUCCESS : GetLastError(), "install Ctrl+C handler");
    std::printf("PID %lu; protocol %u; profile %s; cleanup %s; cycles %u\n",
                GetCurrentProcessId(), static_cast<unsigned>(lvg::k_protocol_version),
                config.profile.c_str(), config.cleanup.c_str(), config.cycles);
    inventory("BASELINE: no controllers created by this process");
    for (unsigned cycle = 1; cycle <= config.cycles && !interrupted.load(); ++cycle) {
      lvg::client client;
      // Open/create failures stop the probe; the driver rejects occupied slots.
      require(client.connect(), "connect (end streaming sessions first; driver/client protocols must match)");
      for (unsigned slot = 0; slot < requested.size(); ++slot) {
        require(client.create_controller(slot, requested[slot]), "create controller");
      }
      const auto label = "Cycle " + std::to_string(cycle) + ": created " +
                         std::to_string(requested.size()) + " controller(s)";
      log(label.c_str());
      wait(config.connected_ms, &client, static_cast<unsigned>(requested.size()));
      inventory("CONNECTED: compare Windows inventory with Steam");
      const bool close_only = config.cleanup == "close" ||
                              (config.cleanup == "alternate" && cycle % 2 == 0);
      if (close_only) {
        client.close();
        log("Closed control handle without destroy IOCTLs");
      } else {
        for (unsigned slot = 0; slot < requested.size(); ++slot) {
          require(client.destroy_controller(slot), "destroy controller");
        }
        log("Destroy IOCTLs succeeded; retaining control handle during removal observation");
      }
      wait(config.disconnected_ms);
      inventory("DISCONNECTED: expected to return to BASELINE after settling");
      client.close();
    }
    if (interrupted.load()) log("Stopped by Ctrl+C; cleanup issued (settling wait was skipped)");
    else log("Lifecycle operations completed. Inspect inventories and Steam; this is not a removal assertion.");
    return 0;
  } catch (const std::exception &error) {
    log(error.what());
    log("Stopped; any owned control handle was closed during unwinding");
    return 1;
  }
}
