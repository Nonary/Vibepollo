/** @file src/steam_auto_sync_policy.h */
#pragma once

#include "steam_integration.h"

#include <cstdint>
#include <vector>

namespace platf::steam::autosync {
  // A stable fingerprint of the locally discoverable Steam catalog and its
  // selected artwork sources. It is intentionally independent of apps.json,
  // so a failed write can be retried without treating catalog writes as input.
  std::uint64_t source_fingerprint(const std::vector<game_t> &games);
}
