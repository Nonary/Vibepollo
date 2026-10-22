#pragma once

#include <iosfwd>
#include <optional>

namespace pairing_recovery {
  // Offline maintenance: explicit input snapshots, no configured/live state access.
  int run(int argc, const char *const argv[], std::ostream &out, std::ostream &error);

  // Shared by the process entry point and component tests. Unrelated commands
  // remain with the normal CLI; only explicit --pairing-recovery or
  // --auxiliary-recovery first flags enter offline maintenance.
  std::optional<int> dispatch(int argc, const char *const argv[], std::ostream &out, std::ostream &error);
}  // namespace pairing_recovery
