#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace platf::linux_private_display {
  // Dormant managed outputs and explicitly selected connected physical/dummy
  // outputs are reservable. Missing names and duplicates cannot create slots.
  template<class ConnectedOutput>
  inline std::size_t configured_client_output_capacity(
    std::vector<std::string> configured,
    const std::vector<std::string> &managed,
    ConnectedOutput &&connected_output
  ) {
    std::sort(configured.begin(), configured.end());
    configured.erase(std::unique(configured.begin(), configured.end()), configured.end());
    return std::count_if(configured.begin(), configured.end(), [&](const auto &name) {
      return std::find(managed.begin(), managed.end(), name) != managed.end() || connected_output(name);
    });
  }
}  // namespace platf::linux_private_display
