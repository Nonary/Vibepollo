// SPDX-License-Identifier: GPL-3.0-or-later
#include "dualsense_haptics.h"

#include "misc.h"

#include <mutex>
#include <sddl.h>
#include <shellapi.h>

namespace platf::dualsense_audio {
  namespace {
    std::mutex mutex;
    std::weak_ptr<session> current;
    std::array<bool, dualsense_haptics::slot_count> slots {};
    std::array<std::uint32_t, dualsense_haptics::slot_count> next_sequences {};
  }  // namespace

  session::~session() {
    if (state) {
      InterlockedExchange(&state->alive, 0);
      UnmapViewOfFile(state);
    }
    if (mapping) {
      CloseHandle(mapping);
    }
  }

  std::shared_ptr<session> start() {
    std::lock_guard lock(mutex);
    auto result = std::make_shared<session>();
    GUID guid {};
    if (FAILED(CoCreateGuid(&guid))) {
      return {};
    }
    wchar_t text[40] {};
    StringFromGUID2(guid, text, 40);
    const auto name = std::wstring(L"Local\\Vibepollo.Haptics.") + text;
    // Grant only the game-launch user and SYSTEM access to this random session name.
    HANDLE token = nullptr;
    if (is_running_as_system()) {
      token = retrieve_users_token(false);
    } else {
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    }
    if (!token) {
      return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const auto token_ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!token_ok) {
      return {};
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) {
      return {};
    }
    const auto sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      return {};
    }
    SECURITY_ATTRIBUTES security {sizeof(security), descriptor, FALSE};
    result->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(dualsense_haptics::shared_state), name.c_str());
    const auto existed = GetLastError() == ERROR_ALREADY_EXISTS;
    LocalFree(descriptor);
    if (!result->mapping || existed) {
      return {};
    }
    result->state = static_cast<dualsense_haptics::shared_state *>(MapViewOfFile(result->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(dualsense_haptics::shared_state)));
    if (!result->state) {
      return {};
    }
    *result->state = {};
    result->state->signature = dualsense_haptics::magic;
    result->state->alive = 1;
    for (unsigned i = 0; i < slots.size(); ++i) {
      result->state->slots[i].active = slots[i];
      result->state->slots[i].sequence = static_cast<LONG>(next_sequences[i]);
    }
    result->mapping_name = utf_utils::to_utf8(name);
    current = result;
    return result;
  }

  void set_slot(unsigned index, bool active) {
    if (index >= slots.size()) {
      return;
    }
    std::lock_guard lock(mutex);
    slots[index] = active;
    if (auto s = current.lock()) {
      auto &slot = s->state->slots[index];
      InterlockedExchange(&slot.active, 0);
      InterlockedIncrement(&slot.generation);
      InterlockedExchange(&slot.active, active);
      // Drain resets at allocation/free prevent samples reaching a new slot owner.
      if (dualsense_haptics::try_lock(slot)) {
        slot.read = slot.write;
        dualsense_haptics::unlock(slot);
      }
    }
  }

  bool enabled() {
    std::lock_guard lock(mutex);
    return !current.expired();
  }

  std::vector<dualsense_haptics::packet> drain(unsigned index) {
    std::lock_guard lock(mutex);
    std::vector<dualsense_haptics::packet> result;
    auto s = current.lock();
    if (!s || index >= slots.size()) {
      return result;
    }
    auto &slot = s->state->slots[index];
    if (!dualsense_haptics::try_lock(slot)) {
      return result;
    }
    // Treat a corrupt or stalled producer as a discontinuity. Bound every read.
    if (slot.write - slot.read > dualsense_haptics::queue_size) {
      slot.read = slot.write;
    }
    while (slot.read != slot.write) {
      auto &packet = slot.packets[slot.read++ % dualsense_haptics::queue_size];
      if (packet.generation == slot.generation && GetTickCount() - packet.timestamp <= 30) {
        result.push_back(packet);
        next_sequences[index] = packet.sequence + 1;
      }
    }
    dualsense_haptics::unlock(slot);
    return result;
  }

  std::string hook_library_path() {
    wchar_t path[32768] {};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) {
      return {};
    }
    std::wstring dll(path);
    dll = dll.substr(0, dll.find_last_of(L"\\") + 1) + L"tools\\vibepollo_dualsense_audio.dll";
    return GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string {} : utf_utils::to_utf8(dll);
  }

  std::string wrap_command(const std::string &command, bool playnite) {
    int argc = 0;
    const auto args = CommandLineToArgvW(utf_utils::from_utf8(command).c_str(), &argc);
    if (!args || !argc) {
      if (args) {
        LocalFree(args);
      }
      return {};
    }
    std::wstring executable = args[0];
    LocalFree(args);
    const auto name = executable.substr(executable.find_last_of(L"/\\") + 1);
    if (playnite ? _wcsicmp(name.c_str(), L"playnite-launcher.exe") != 0 :
        (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".exe") != 0 || _wcsicmp(name.c_str(), L"cmd.exe") == 0 || _wcsicmp(name.c_str(), L"steam.exe") == 0 || _wcsicmp(name.c_str(), L"powershell.exe") == 0 || _wcsicmp(name.c_str(), L"pwsh.exe") == 0 || _wcsicmp(name.c_str(), L"explorer.exe") == 0 || name.find(L"Playnite") != std::wstring::npos)) {
      return {};
    }
    wchar_t path[32768] {};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) {
      return {};
    }
    std::wstring helper(path);
    helper = helper.substr(0, helper.find_last_of(L"\\") + 1) + L"tools\\vibepollo_dualsense_haptics.exe";
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return {};
    }
    // Encode the complete original command as one argv value (Windows CRT rules).
    std::string argument = "\"";
    unsigned slashes = 0;
    for (char ch : command) {
      if (ch == '\\') {
        ++slashes;
        continue;
      }
      argument.append(ch == '"' ? slashes * 2 + 1 : slashes, '\\');
      slashes = 0;
      argument += ch;
    }
    argument.append(slashes * 2, '\\');
    argument += '"';
    return "\"" + utf_utils::to_utf8(helper) + "\" " + (playnite ? "--playnite " : "") + argument;
  }
}  // namespace platf::dualsense_audio
