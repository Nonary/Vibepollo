/**
 * @file src/platform/linux/private_display_snapshot_policy.h
 * @brief Persisted desktop topology validation and conservative recovery policy.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>

namespace platf::linux_private_display::snapshot_policy {
  inline bool safe_identifier(const std::string &value) {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](const unsigned char ch) {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
             (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
  }

  template<typename Json>
  bool valid(const Json &snapshot) {
    try {
      if (!snapshot.is_object() || !snapshot.contains("outputs") || !snapshot["outputs"].is_array() || snapshot["outputs"].size() > 64) {
        return false;
      }
      std::set<std::string> names;
      for (const auto &output : snapshot["outputs"]) {
        if (!output.is_object() || !output.contains("enabled") || !output["enabled"].is_boolean() || !output.contains("connected") || !output["connected"].is_boolean()) {
          return false;
        }
        const auto name = output.value("name", std::string {});
        if (!safe_identifier(name) || !names.insert(name).second) {
          return false;
        }
        if (output.contains("hdr") && !output["hdr"].is_boolean()) {
          return false;
        }
        const auto scale = output.value("scale", 1.0);
        if (!std::isfinite(scale) || scale < 0.25 || scale > 5.0) {
          return false;
        }
        const auto rotation = output.value("rotation", 1);
        if (rotation != 1 && rotation != 2 && rotation != 4 && rotation != 8 && rotation != 16 && rotation != 32 && rotation != 64 && rotation != 128) {
          return false;
        }
        const auto priority = output.value("priority", 0);
        if (priority < 0 || priority > 64) {
          return false;
        }
        const auto position = output.value("pos", Json::object());
        for (const auto key : {"x", "y"}) {
          if (position.contains(key) && !position[key].is_number_integer()) {
            return false;
          }
          const auto value = position.value(key, 0);
          if (value < -1000000 || value > 1000000) {
            return false;
          }
        }
        const auto mode_id = output.value("currentModeId", std::string {});
        if (!mode_id.empty() && !safe_identifier(mode_id)) {
          return false;
        }
        const auto modes = output.value("modes", Json::array());
        if (!modes.is_array() || modes.size() > 1024) {
          return false;
        }
        for (const auto &mode : modes) {
          const auto refresh = mode.value("refreshRate", 0.0);
          if (!mode.is_object() || !safe_identifier(mode.value("id", std::string {})) || !std::isfinite(refresh) || refresh < 0.0 || refresh > 10000.0) {
            return false;
          }
          const auto size = mode.value("size", Json::object());
          for (const auto key : {"width", "height"}) {
            if (size.contains(key) && !size[key].is_number_integer()) {
              return false;
            }
            const auto dimension = size.value(key, 0);
            if (dimension < 0 || dimension > 65536) {
              return false;
            }
          }
        }
        const auto size = output.value("size", Json::object());
        for (const auto key : {"width", "height"}) {
          if (size.contains(key) && !size[key].is_number_integer()) {
            return false;
          }
          const auto dimension = size.value(key, 0);
          if (dimension < 0 || dimension > 65536) {
            return false;
          }
        }
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  template<typename Json>
  std::optional<Json> decode(const std::string &contents, const std::string &owner, bool *restore_pending = nullptr) {
    if (contents.size() > 1024 * 1024) {
      return std::nullopt;
    }
    try {
      const auto saved = Json::parse(contents);
      if (saved.value("version", 0) != 1 || saved.value("owner", std::string {}) != owner || !saved.contains("topology") || !valid(saved["topology"])) {
        return std::nullopt;
      }
      // Older records preceded the intent flag and may describe an orphan
      // stream; retain their recovery behavior until an idle refresh replaces them.
      const bool pending = saved.value("restore_pending", true);
      if (restore_pending) {
        *restore_pending = pending;
      }
      return saved["topology"];
    } catch (...) {
      return std::nullopt;
    }
  }

  template<typename Json>
  Json baseline(Json configuration, const std::set<std::string> &private_names) {
    const bool active_physical = std::ranges::any_of(configuration["outputs"], [&](const auto &output) {
      return output.value("connected", false) && output.value("enabled", false) &&
             !private_names.contains(output.value("name", std::string {}));
    });
    for (auto &output : configuration["outputs"]) {
      if (!output.value("connected", false) || (active_physical && private_names.contains(output.value("name", std::string {})))) {
        output["enabled"] = false;
      }
    }
    return configuration;
  }

  template<typename Json>
  bool idle(const Json &configuration, const std::set<std::string> &private_names, const bool reserved) {
    return !reserved && std::ranges::none_of(configuration["outputs"], [&](const auto &output) {
      return output.value("connected", false) && private_names.contains(output.value("name", std::string {}));
    });
  }

  /** Refresh before snapshot gating; an old failed restore must not freeze idle preferences. */
  template<typename Json, typename Persist>
  bool capture(std::optional<Json> &snapshot, const Json &current, const std::set<std::string> &private_names, const bool reserved, Persist persist) {
    if (snapshot && !idle(current, private_names, reserved)) {
      return true;
    }
    if (!valid(current)) {
      return false;
    }
    auto replacement = baseline(current, private_names);
    if (!valid(replacement) || !persist(replacement)) {
      return false;
    }
    snapshot = std::move(replacement);
    return true;
  }

  template<typename Json>
  std::set<std::string> retiring_outputs(const Json &snapshot, const Json &current, const std::set<std::string> &managed_names) {
    std::set<std::string> result;
    for (const auto &present : current["outputs"]) {
      const auto name = present.value("name", std::string {});
      if (!managed_names.contains(name) || !present.value("connected", false)) {
        continue;
      }
      const auto saved = std::ranges::find_if(snapshot["outputs"], [&](const auto &output) {
        return output.value("name", std::string {}) == name;
      });
      if (saved == snapshot["outputs"].end() || !saved->value("enabled", false)) {
        result.insert(name);
      }
    }
    return result;
  }

  /** An idle baseline alone does not authorize a topology mutation. */
  template<typename Json>
  bool restore_needed(const std::optional<Json> &snapshot, const Json &current, const std::set<std::string> &managed_names, const bool reserved) {
    return snapshot.has_value() || reserved || std::ranges::any_of(current["outputs"], [&](const auto &output) {
      return output.value("connected", false) && managed_names.contains(output.value("name", std::string {}));
    });
  }

  /** Missing/unusable snapshots may use live enabled monitors, never guess disabled intent. */
  template<typename Json>
  std::optional<Json> live_fallback(const Json &current, const std::set<std::string> &private_names) {
    if (!valid(current) || std::ranges::none_of(current["outputs"], [&](const auto &output) {
          return output.value("connected", false) && output.value("enabled", false) &&
                 !private_names.contains(output.value("name", std::string {}));
        })) {
      return std::nullopt;
    }
    return baseline(current, private_names);
  }
}  // namespace platf::linux_private_display::snapshot_policy
