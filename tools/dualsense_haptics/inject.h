// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "bootstrap.h"

#include <cstring>
#include <string>
#include <vector>
#include <winternl.h>

namespace dualsense_haptics {
  inline bool same_architecture(HANDLE process) {
    USHORT ours = 0, ours_native = 0, theirs = 0, theirs_native = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &ours, &ours_native) || !IsWow64Process2(process, &theirs, &theirs_native)) {
      return false;
    }
    return (ours ? ours : ours_native) == (theirs ? theirs : theirs_native);
  }

  inline bool read(HANDLE process, std::uintptr_t address, void *out, SIZE_T size) {
    SIZE_T bytes = 0;
    return ReadProcessMemory(process, reinterpret_cast<void *>(address), out, size, &bytes) && bytes == size;
  }

  // Toolhelp's module list is not initialized in a newly suspended process.
  // Locate the already-mapped ntdll image without letting the game run.
  inline std::uintptr_t ntdll_base(HANDLE process) {
    std::uintptr_t address = 0;
    MEMORY_BASIC_INFORMATION region {};
    while (VirtualQueryEx(process, reinterpret_cast<void *>(address), &region, sizeof(region))) {
      const auto allocation = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
      if (region.Type == MEM_IMAGE && address == allocation) {
        IMAGE_DOS_HEADER dos {};
        IMAGE_NT_HEADERS headers {};
        if (read(process, allocation, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1024 * 1024 && read(process, allocation + dos.e_lfanew, &headers, sizeof(headers)) && headers.Signature == IMAGE_NT_SIGNATURE) {
          const auto rva = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
          IMAGE_EXPORT_DIRECTORY exports {};
          char name[16] {};
          if (rva && read(process, allocation + rva, &exports, sizeof(exports)) && read(process, allocation + exports.Name, name, sizeof(name)) && _stricmp(name, "ntdll.dll") == 0) {
            return allocation;
          }
        }
      }
      const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
      if (next <= address) {
        break;
      }
      address = next;
    }
    return 0;
  }

  // Stage an entry-point bootstrap while the main thread is suspended. The
  // Windows loader runs normally; no debugger or remote thread is attached.
  // At entry, the bootstrap loads our audio DLL, restores the original entry
  // bytes, signals readiness, and waits for this caller to release its gate.
  inline bool inject(PROCESS_INFORMATION &child, const std::wstring &dll, const std::wstring &mapping_name, bool keep_suspended = false) {
#ifndef __x86_64__
    return false;
#else
    if (!same_architecture(child.hProcess) || mapping_name.size() >= 128 || dll.size() > 32766) {
      return false;
    }
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto remote_ntdll = ntdll_base(child.hProcess);
    if (!remote_ntdll) {
      return false;
    }
    const auto remote_function = [&](const char *name) {
      const auto function = GetProcAddress(ntdll, name);
      return function ? remote_ntdll + reinterpret_cast<std::uintptr_t>(function) - reinterpret_cast<std::uintptr_t>(ntdll) : 0;
    };
    const auto load = remote_function("LdrLoadDll");
    const auto delay = remote_function("NtDelayExecution");
    const auto exit = remote_function("RtlExitUserProcess");
    const auto query = reinterpret_cast<NTSTATUS(NTAPI *)(HANDLE, PROCESSINFOCLASS, void *, ULONG, ULONG *)>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    PROCESS_BASIC_INFORMATION info {};
    if (!load || !delay || !exit || !query || query(child.hProcess, ProcessBasicInformation, &info, sizeof(info), nullptr) < 0) {
      return false;
    }
    std::uintptr_t image = 0;
    // Native x64 PEB.ImageBaseAddress precedes the loader data pointer.
    if (!read(child.hProcess, reinterpret_cast<std::uintptr_t>(info.PebBaseAddress) + 0x10, &image, sizeof(image))) {
      return false;
    }
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS headers {};
    if (!read(child.hProcess, image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024 || !read(child.hProcess, image + dos.e_lfanew, &headers, sizeof(headers)) || headers.Signature != IMAGE_NT_SIGNATURE || !headers.OptionalHeader.AddressOfEntryPoint || headers.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
      return false;
    }
    const auto entry = image + headers.OptionalHeader.AddressOfEntryPoint;
    const auto module = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
      return false;
    }
    const auto bootstrap = GetProcAddress(module, "VibepolloHapticsBootstrap");
    const auto bootstrap_offset = reinterpret_cast<std::uintptr_t>(bootstrap) - reinterpret_cast<std::uintptr_t>(module);
    FreeLibrary(module);
    if (!bootstrap) {
      return false;
    }

    struct payload {
      bootstrap_config config;
      UNICODE_STRING dll_name;
      std::uint64_t module;
      std::int64_t delay;
    } data {};

    wcscpy(data.config.mapping_name, mapping_name.c_str());
    data.config.entry = entry;
    data.delay = -10000;  // 1 ms readiness gate, not a busy wait.
    if (!read(child.hProcess, entry, data.config.original.data(), data.config.original.size())) {
      return false;
    }
    const auto size = sizeof(data) + (dll.size() + 1) * sizeof(wchar_t);
    const auto allocation = VirtualAllocEx(child.hProcess, nullptr, 4096 + size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocation) {
      return false;
    }
    const auto code_address = reinterpret_cast<std::uintptr_t>(allocation);
    const auto data_address = code_address + 4096;
    data.dll_name.Length = static_cast<USHORT>(dll.size() * sizeof(wchar_t));
    data.dll_name.MaximumLength = data.dll_name.Length + sizeof(wchar_t);
    data.dll_name.Buffer = reinterpret_cast<wchar_t *>(data_address + sizeof(data));
    const auto write = [&](std::uintptr_t address, const void *buffer, SIZE_T bytes) {
      SIZE_T written = 0;
      return WriteProcessMemory(child.hProcess, reinterpret_cast<void *>(address), buffer, bytes, &written) && written == bytes;
    };
    if (!write(data_address, &data, sizeof(data)) || !write(data_address + sizeof(data), dll.c_str(), (dll.size() + 1) * sizeof(wchar_t))) {
      return false;
    }

    std::vector<BYTE> code;
    const auto emit = [&](std::initializer_list<BYTE> bytes) {
      code.insert(code.end(), bytes);
    };
    const auto imm64 = [&](std::uint64_t value) {
      const auto p = reinterpret_cast<const BYTE *>(&value);
      code.insert(code.end(), p, p + 8);
    };
    const auto branch = [&](BYTE condition) {
      emit({0x0f, condition, 0, 0, 0, 0});
      return code.size() - 4;
    };
    const auto fix_branch = [&](std::size_t offset, std::size_t target) {
      const auto relative = static_cast<std::int32_t>(target - offset - 4);
      memcpy(code.data() + offset, &relative, 4);
    };
    // Save volatile integer registers and flags. Keep Windows x64 shadow space
    // and 16-byte stack alignment for every call into ntdll and the audio DLL.
    emit({0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x83, 0xec, 0x28});
    emit({0x31, 0xc9, 0x31, 0xd2, 0x49, 0xb8});
    imm64(data_address + offsetof(payload, dll_name));
    emit({0x49, 0xb9});
    imm64(data_address + offsetof(payload, module));
    emit({0x48, 0xb8});
    imm64(load);
    emit({0xff, 0xd0, 0x85, 0xc0});
    const auto load_failed = branch(0x88);  // JS: failing NTSTATUS.
    emit({0x48, 0xb8});
    imm64(data_address + offsetof(payload, module));
    emit({0x48, 0x8b, 0x00});
    emit({0x48, 0xba});
    imm64(bootstrap_offset);
    emit({0x48, 0x01, 0xd0, 0x48, 0xb9});
    imm64(data_address);
    emit({0xff, 0xd0, 0x85, 0xc0});
    const auto init_failed = branch(0x84);  // JZ.
    const auto gate_loop = code.size();
    emit({0x48, 0xb8});
    imm64(data_address + offsetof(payload, config) + offsetof(bootstrap_config, gate));
    emit({0x83, 0x38, 0});
    const auto gate_open = branch(0x85);  // JNZ.
    emit({0x31, 0xc9, 0x48, 0xba});
    imm64(data_address + offsetof(payload, delay));
    emit({0x48, 0xb8});
    imm64(delay);
    emit({0xff, 0xd0, 0xe9, 0, 0, 0, 0});
    fix_branch(code.size() - 4, gate_loop);
    fix_branch(gate_open, code.size());
    emit({0x48, 0x83, 0xc4, 0x28, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d});
    emit({0xff, 0x25, 0, 0, 0, 0});
    imm64(entry);
    fix_branch(load_failed, code.size());
    fix_branch(init_failed, code.size());
    emit({0xb9, 125, 0, 0, 0, 0x48, 0xb8});
    imm64(exit);
    emit({0xff, 0xd0, 0xcc});
    DWORD protection = 0;
    if (code.size() > 4096 || !write(code_address, code.data(), code.size()) || !VirtualProtectEx(child.hProcess, allocation, 4096, PAGE_EXECUTE_READ, &protection)) {
      return false;
    }
    std::array<BYTE, 14> jump {0xff, 0x25, 0, 0, 0, 0};
    memcpy(jump.data() + 6, &code_address, 8);
    if (!VirtualProtectEx(child.hProcess, reinterpret_cast<void *>(entry), jump.size(), PAGE_EXECUTE_READWRITE, &protection) || !write(entry, jump.data(), jump.size())) {
      return false;
    }
    DWORD ignored = 0;
    VirtualProtectEx(child.hProcess, reinterpret_cast<void *>(entry), jump.size(), protection, &ignored);
    FlushInstructionCache(child.hProcess, nullptr, 0);
    if (ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
      return false;
    }
    const auto ready_address = data_address + offsetof(payload, config) + offsetof(bootstrap_config, ready);
    const auto deadline = GetTickCount64() + 10000;
    LONG ready = 0;
    while (GetTickCount64() < deadline) {
      if (!read(child.hProcess, ready_address, &ready, sizeof(ready))) {
        return false;
      }
      if (ready) {
        break;
      }
      if (WaitForSingleObject(child.hProcess, 1) != WAIT_TIMEOUT) {
        return false;
      }
    }
    if (!ready || (keep_suspended && SuspendThread(child.hThread) == static_cast<DWORD>(-1))) {
      return false;
    }
    const LONG gate = 1;
    return write(data_address + offsetof(payload, config) + offsetof(bootstrap_config, gate), &gate, sizeof(gate));
#endif
  }
}  // namespace dualsense_haptics
