// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "inject.h"
#include "launcher_scope.h"

#include <tlhelp32.h>

namespace dualsense_haptics {
  inline std::uintptr_t module_base(DWORD pid, const std::wstring &path) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return 0;
    }
    MODULEENTRY32W module {sizeof(module)};
    std::uintptr_t result = 0;
    if (Module32FirstW(snapshot, &module)) {
      do {
        if (_wcsicmp(module.szExePath, path.c_str()) == 0) {
          result = reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
          break;
        }
      } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return result;
  }

  // Running storefronts need only a scoped process-creation hook. Games are
  // still initialized by inject() while their entry point is suspended.
  inline bool attach_launcher(DWORD pid, const std::wstring &dll, const launcher_config &config) {
    DWORD ours = 0, theirs = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ours) ||
        !ProcessIdToSessionId(pid, &theirs) || ours != theirs) {
      return false;
    }
    const auto process = OpenProcess(SYNCHRONIZE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
      return false;
    }
    const auto close_process = [&]() { CloseHandle(process); };
    if (!same_architecture(process)) {
      close_process();
      return false;
    }
    const auto call = [&](std::uintptr_t function, const void *argument, SIZE_T size, bool require_success = true) {
      const auto memory = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!memory) {
        return false;
      }
      SIZE_T written = 0;
      HANDLE thread = nullptr;
      if (WriteProcessMemory(process, memory, argument, size, &written) && written == size) {
        thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(function), memory, 0, nullptr);
      }
      DWORD result = 0;
      const bool finished = thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
      const bool ok = finished && GetExitCodeThread(thread, &result) && (!require_success || result != 0);
      if (!thread || finished) {
        VirtualFreeEx(process, memory, 0, MEM_RELEASE);
      }
      // A timed-out loader may still consume its argument. Do not free it or
      // terminate a storefront; the session's alive flag disarms late setup.
      if (thread) {
        CloseHandle(thread);
      }
      return ok;
    };
    auto remote_dll = module_base(pid, dll);
    if (!remote_dll) {
      const auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
      HMODULE owner = nullptr;
      wchar_t owner_path[32768] {};
      if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(load), &owner) ||
          !GetModuleFileNameW(owner, owner_path, 32768)) {
        close_process();
        return false;
      }
      auto remote_owner = module_base(pid, owner_path);
      const auto loader_deadline = GetTickCount64() + 2000;
      while (!remote_owner && GetTickCount64() < loader_deadline && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        Sleep(10);
        remote_owner = module_base(pid, owner_path);
      }
      // LoadLibrary returns an HMODULE; its remote-thread exit code contains
      // only the low 32 bits. Confirm the loaded image through module_base().
      if (!remote_owner || !call(remote_owner + reinterpret_cast<std::uintptr_t>(load) - reinterpret_cast<std::uintptr_t>(owner), dll.c_str(), (dll.size() + 1) * sizeof(wchar_t), false)) {
        close_process();
        return false;
      }
      remote_dll = module_base(pid, dll);
    }
    const auto local = GetModuleHandleW(dll.c_str());
    const auto initialize = local ? GetProcAddress(local, "VibepolloHapticsInitializeLauncher") : nullptr;
    const bool ok = remote_dll && initialize && call(remote_dll + reinterpret_cast<std::uintptr_t>(initialize) - reinterpret_cast<std::uintptr_t>(local), &config, sizeof(config));
    close_process();
    return ok;
  }
}  // namespace dualsense_haptics
