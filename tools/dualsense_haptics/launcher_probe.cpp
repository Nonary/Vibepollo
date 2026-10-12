// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real launcher hooks without Playnite, Steam, or controller hardware.
#include "attach.h"

#include <cstdio>
#include <mmdeviceapi.h>

#define CHECK(expr) \
  do { \
    if (!(expr)) { \
      std::fprintf(stderr, "Failed at line %d: %s (error=%lu)\n", __LINE__, #expr, GetLastError()); \
      return 1; \
    } \
  } while (0)

namespace {
  std::wstring self_path() {
    wchar_t path[32768] {};
    GetModuleFileNameW(nullptr, path, 32768);
    return path;
  }

  bool spawn(const std::wstring &arguments, bool suspended = false) {
    auto command = L"\"" + self_path() + L"\" " + arguments;
    STARTUPINFOW startup {sizeof(startup)};
    PROCESS_INFORMATION child {};
    // An explicit environment omitting the mapping verifies native bootstrap
    // propagation rather than accidental environment inheritance.
    wchar_t environment[] = L"SystemRoot=C:\\Windows\0\0";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | (suspended ? CREATE_SUSPENDED : 0),
                        environment, nullptr, &startup, &child)) {
      return false;
    }
    bool ok = true;
    if (suspended) {
      ok = WaitForSingleObject(child.hProcess, 50) == WAIT_TIMEOUT && ResumeThread(child.hThread) == 1;
    }
    if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0) {
      TerminateProcess(child.hProcess, 1);
      ok = false;
    }
    DWORD code = 1;
    ok = GetExitCodeProcess(child.hProcess, &code) && code == 0 && ok;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return ok;
  }

  bool virtual_endpoint() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      return false;
    }
    IMMDeviceEnumerator *enumerator = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)));
    IMMDevice *device = nullptr;
    ok = ok && SUCCEEDED(enumerator->GetDevice(L"Vibepollo.DualSense.Audio.3", &device));
    if (device) {
      device->Release();
    }
    if (enumerator) {
      enumerator->Release();
    }
    CoUninitialize();
    return ok;
  }
}

int wmain(int argc, wchar_t **argv) {
  using namespace dualsense_haptics;
  if (argc > 1 && wcscmp(argv[1], L"--injected") == 0) {
    CHECK(!IsDebuggerPresent());
    CHECK(virtual_endpoint());
    return 0;
  }
  if (argc > 1 && wcscmp(argv[1], L"--plain") == 0) {
    CHECK(!GetModuleHandleW(L"vibepollo_dualsense_audio.dll"));
    return 0;
  }
  if (argc == 4 && wcscmp(argv[1], L"--storefront") == 0) {
    const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]);
    CHECK(ready && SetEvent(ready));
    CloseHandle(ready);
    const auto event = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    CHECK(event && WaitForSingleObject(event, 20000) == WAIT_OBJECT_0);
    CloseHandle(event);
    CHECK(spawn(L"--injected", true));
    return 0;
  }

  const auto path = self_path();
  const auto directory = path.substr(0, path.find_last_of(L"\\"));
  const auto dll_path = directory + L"\\vibepollo_dualsense_audio.dll";
  const auto dll = LoadLibraryW(dll_path.c_str());
  CHECK(dll);
  const auto initialize = reinterpret_cast<DWORD(WINAPI *)(void *)>(GetProcAddress(dll, "VibepolloHapticsInitializeLauncher"));
  CHECK(initialize);
  launcher_config config;
  swprintf(config.mapping_name, 128, L"Local\\Vibepollo.Launcher.Test.%lu", GetCurrentProcessId());
  const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(shared_state), config.mapping_name);
  CHECK(mapping);
  auto state = static_cast<shared_state *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_state)));
  CHECK(state);
  *state = {};
  state->signature = magic;
  state->alive = 1;
  state->slots[3].active = 1;
  wcscpy(config.directory, (directory + L"\\other-game").c_str());
  CHECK(initialize(&config));
  CHECK(spawn(L"--plain", true));  // outside the selected install directory
  wcscpy(config.directory, directory.c_str());
  CHECK(initialize(&config));
  CHECK(!virtual_endpoint());  // launchers do not get an audio endpoint
  CHECK(spawn(L"--injected", true));
  state->alive = 0;
  CHECK(spawn(L"--plain"));  // ended session leaves subsequent launches alone

  // A second session must replace the dead mapping in a persistent launcher.
  launcher_config second = config;
  wcscat(second.mapping_name, L".second");
  const auto next_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(shared_state), second.mapping_name);
  CHECK(next_mapping);
  auto next = static_cast<shared_state *>(MapViewOfFile(next_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_state)));
  CHECK(next);
  *next = {};
  next->signature = magic;
  next->alive = 1;
  next->slots[3].active = 1;
  CHECK(initialize(&second));
  CHECK(spawn(L"--injected"));

  // Model an already-running Steam: attach only after its process is running,
  // then let it create the game. No real storefront is modified by this test.
  next->alive = 0;
  wchar_t event_name[128] {};
  swprintf(event_name, 128, L"Local\\Vibepollo.Storefront.Test.%lu", GetCurrentProcessId());
  const auto event = CreateEventW(nullptr, TRUE, FALSE, event_name);
  CHECK(event);
  const auto ready_name = std::wstring(event_name) + L".ready";
  const auto ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
  CHECK(ready);
  auto command = L"\"" + path + L"\" --storefront " + event_name + L" " + ready_name;
  STARTUPINFOW startup {sizeof(startup)};
  PROCESS_INFORMATION storefront {};
  CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &storefront));
  CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
  CloseHandle(ready);
  next->alive = 1;
  CHECK(attach_launcher(storefront.dwProcessId, dll_path, second));
  CHECK(SetEvent(event));
  CHECK(WaitForSingleObject(storefront.hProcess, 20000) == WAIT_OBJECT_0);
  DWORD exit = 1;
  CHECK(GetExitCodeProcess(storefront.hProcess, &exit) && exit == 0);
  CloseHandle(storefront.hThread);
  CloseHandle(storefront.hProcess);
  CloseHandle(event);
  next->alive = 0;
  UnmapViewOfFile(next);
  CloseHandle(next_mapping);
  UnmapViewOfFile(state);
  CloseHandle(mapping);
  std::puts("Launcher scope, suspended startup, session replacement, and running storefront checks passed");
  return 0;
}
