#pragma once

#include <memory>

namespace platf::display_power {
  // Carried from a pending launch into its active capture. The last owner
  // closes the broker connection and releases the session inhibitor.
  // A null result means that a machine-host request could not become ready.
  std::shared_ptr<void> acquire();
}  // namespace platf::display_power
