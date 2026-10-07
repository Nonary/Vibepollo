/**
 * @file src/platform/linux/render_device.h
 * @brief Resolve a renderer from cached DRM topology without opening a GPU.
 */
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace platf::drm_topology {
  inline bool numbered_node(std::string_view name, std::string_view prefix) {
    return name.starts_with(prefix) && name.size() > prefix.size() &&
           std::ranges::all_of(name.substr(prefix.size()), [](char c) {
             return c >= '0' && c <= '9';
           });
  }

  inline std::string find_render_node_with_display(
    const std::filesystem::path &drm_class = "/sys/class/drm",
    const std::filesystem::path &dri = "/dev/dri"
  ) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(drm_class, ec), end; !ec && it != end; it.increment(ec)) {
      entries.push_back(it->path());
    }
    if (ec) {
      return {};
    }
    std::ranges::sort(entries);
    for (const auto &card : entries) {
      const auto name = card.filename().string();
      if (!numbered_node(name, "card")) {
        continue;
      }
      const auto connector_prefix = name + '-';
      const bool connected = std::ranges::any_of(entries, [&](const auto &connector) {
        if (!connector.filename().string().starts_with(connector_prefix)) {
          return false;
        }
        std::ifstream status(connector / "status");
        std::string value;
        return (status >> value) && value == "connected";
      });
      if (!connected) {
        continue;
      }

      // A virtual display has no render node. Keep looking for a physical
      // GPU instead of opening its primary node and forcing connector probes.
      std::vector<fs::path> renderers;
      ec.clear();
      for (fs::directory_iterator it(card / "device/drm", ec), end; !ec && it != end; it.increment(ec)) {
        const auto render_name = it->path().filename().string();
        if (numbered_node(render_name, "renderD")) {
          renderers.push_back(dri / render_name);
        }
      }
      if (ec) {
        continue;
      }
      std::ranges::sort(renderers);
      for (const auto &renderer : renderers) {
        if (fs::exists(renderer, ec) && !ec) {
          return renderer.string();
        }
      }
    }
    return {};
  }
}  // namespace platf::drm_topology
