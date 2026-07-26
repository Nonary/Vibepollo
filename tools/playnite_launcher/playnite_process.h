#pragma once

#include <optional>
#include <string>
#include <winsock2.h>
#include <windows.h>

namespace playnite_launcher::playnite {

  // Holds a per-game cleanup lease while a launcher is responsible for the
  // game. A watchdog only cleans up after its parent exits when no successor
  // launcher has claimed the same install directory.
  class cleanup_lease_t {
  public:
    cleanup_lease_t() = default;
    cleanup_lease_t(const cleanup_lease_t &) = delete;
    cleanup_lease_t &operator=(const cleanup_lease_t &) = delete;
    cleanup_lease_t(cleanup_lease_t &&other) noexcept;
    cleanup_lease_t &operator=(cleanup_lease_t &&other) noexcept;
    ~cleanup_lease_t();

    static std::optional<cleanup_lease_t> acquire(const std::string &install_dir_utf8);
    static bool held_by_another_launcher(const std::string &install_dir_utf8);

    explicit operator bool() const {
      return handle_ != nullptr;
    }

  private:
    explicit cleanup_lease_t(HANDLE handle) : handle_(handle) {}

    HANDLE handle_ = nullptr;
  };

  bool is_playnite_running();
  bool launch_uri_detached_parented(const std::wstring &uri);
  bool launch_executable_detached_parented_with_args(const std::wstring &exe_full_path, const std::wstring &args);
  std::wstring query_playnite_executable_from_assoc();
  bool launch_executable_detached_parented(const std::wstring &exe_full_path);
  bool spawn_cleanup_watchdog_process(const std::wstring &self_path, const std::string &install_dir_utf8, int exit_timeout_secs, bool fullscreen_flag, std::optional<DWORD> wait_for_pid);

}  // namespace playnite_launcher::playnite
