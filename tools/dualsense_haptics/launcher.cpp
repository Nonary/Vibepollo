// SPDX-License-Identifier: GPL-3.0-or-later
#include "inject.h"
#include "ipc.h"

#include <cstdio>
#include <devpropdef.h>
#include <setupapi.h>
#include <vector>

namespace {
  bool controller_enumerated(unsigned slot) {
    const GUID hid_class {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0, 0x11, 0x11, 0, 0, 0x30}};
    const DEVPROPKEY container_key {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
    const auto devices = SetupDiGetClassDevsW(&hid_class, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
      return false;
    }
    SP_DEVINFO_DATA device {sizeof(device)};
    bool found = false;
    for (DWORD n = 0; SetupDiEnumDeviceInfo(devices, n, &device); ++n) {
      DEVPROPTYPE type = 0;
      GUID container {};
      if (SetupDiGetDevicePropertyW(devices, &device, &container_key, &type, reinterpret_cast<BYTE *>(&container), sizeof(container), nullptr, 0) && type == DEVPROP_TYPE_GUID && container == dualsense_haptics::container_id(slot)) {
        found = true;
        break;
      }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
  }
}  // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    return 125;
  }
  wchar_t name[128] {};
  if (!GetEnvironmentVariableW(dualsense_haptics::environment_key, name, 128)) {
    return 125;
  }
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!mapping) {
    return 125;
  }
  auto state = static_cast<dualsense_haptics::shared_state *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(dualsense_haptics::shared_state)));
  if (!state || state->signature != dualsense_haptics::magic) {
    return 125;
  }
  // The controller arrives over the stream after the application launch request.
  // Keep the helper alive while RTSP connects, but do not start the game yet.
  const auto deadline = GetTickCount64() + 15000;
  bool ready = false;
  while (state->alive && GetTickCount64() < deadline) {
    for (unsigned i = 0; i < dualsense_haptics::slot_count; ++i) {
      if (state->slots[i].active && controller_enumerated(i)) {
        ready = true;
      }
    }
    if (ready) {
      break;
    }
    Sleep(10);
  }
  UnmapViewOfFile(state);
  CloseHandle(mapping);
  if (!ready) {
    std::fwprintf(stderr, L"DualSense haptics: no streamed VHF DualSense became ready.\n");
    return 125;
  }
  wchar_t path[32768] {};
  const auto length = GetModuleFileNameW(nullptr, path, 32768);
  if (!length || length >= 32768) {
    return 125;
  }
  std::wstring dll(path);
  dll = dll.substr(0, dll.find_last_of(L"\\") + 1) + L"vibepollo_dualsense_audio.dll";
  std::vector<wchar_t> command(argv[1], argv[1] + wcslen(argv[1]) + 1);
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION child {};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &child)) {
    return 125;
  }
  bool ok = dualsense_haptics::inject(child, dll, name);
  if (!ok) {
    std::fwprintf(stderr, L"DualSense haptics: audio hook initialization failed; game was not resumed.\n");
    TerminateProcess(child.hProcess, 125);
  }
  WaitForSingleObject(child.hProcess, INFINITE);
  DWORD code = 125;
  GetExitCodeProcess(child.hProcess, &code);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return static_cast<int>(code);
}
