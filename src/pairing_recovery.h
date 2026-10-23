#pragma once

#include <iosfwd>
#include <optional>

namespace pairing_recovery {
  // Offline maintenance: explicit input snapshots, no configured/live state access.
  int run(int argc, const char *const argv[], std::ostream &out, std::ostream &error);

  // UTF-8 argument boundary used by component tests and the native adapter. Unrelated commands
  // remain with the normal CLI; only explicit --pairing-recovery or
  // --auxiliary-recovery first flags enter offline maintenance.
  std::optional<int> dispatch(int argc, const char *const argv[], std::ostream &out, std::ostream &error);
  // Process entry boundary. On Windows read recovery arguments from UTF-16,
  // never from potentially lossy ACP-encoded CRT argv. Other CLI flags fall through.
  std::optional<int> dispatch_native(int argc, const char *const argv[], std::ostream &out, std::ostream &error);

#ifdef _WIN32
  // Shared production parser/converter, exposed for Windows boundary tests.
  std::optional<int> dispatch_windows_command_line(const wchar_t *command_line, std::ostream &out, std::ostream &error);
#endif
}  // namespace pairing_recovery
