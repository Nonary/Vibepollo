#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace version_compare {
  using prerelease_identifier_t = std::variant<int, std::string>;

  struct semver_t {
    int major {0};
    int minor {0};
    int patch {0};
    std::vector<prerelease_identifier_t> prerelease;
  };

  inline std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  inline bool is_numeric_identifier(const std::string &identifier) {
    return !identifier.empty() &&
           std::all_of(identifier.begin(), identifier.end(), [](unsigned char ch) {
             return std::isdigit(ch) != 0;
           });
  }

  inline semver_t parse_semver(std::string_view version) {
    semver_t out;
    if (version.empty()) {
      return out;
    }

    std::string value(version);
    if (!value.empty() && (value[0] == 'v' || value[0] == 'V')) {
      value.erase(0, 1);
    }
    if (auto plus = value.find('+'); plus != std::string::npos) {
      value.resize(plus);
    }

    std::string core = value;
    if (auto dash = value.find('-'); dash != std::string::npos) {
      core = value.substr(0, dash);
      std::stringstream prerelease_stream(value.substr(dash + 1));
      std::string identifier;
      while (std::getline(prerelease_stream, identifier, '.')) {
        if (identifier.empty()) {
          continue;
        }
        if (is_numeric_identifier(identifier)) {
          try {
            out.prerelease.emplace_back(std::stoi(identifier));
            continue;
          } catch (...) {
          }
        }
        out.prerelease.emplace_back(identifier);
      }
    }

    try {
      std::stringstream core_stream(core);
      std::string part;
      if (std::getline(core_stream, part, '.')) {
        out.major = std::stoi(part);
      }
      if (std::getline(core_stream, part, '.')) {
        out.minor = std::stoi(part);
      }
      if (std::getline(core_stream, part, '.')) {
        out.patch = std::stoi(part);
      }
    } catch (...) {
      out = semver_t {};
    }

    return out;
  }

  inline bool is_stable_channel(const semver_t &version) {
    if (version.prerelease.empty() || !std::holds_alternative<std::string>(version.prerelease.front())) {
      return false;
    }
    return ascii_lower(std::get<std::string>(version.prerelease.front())) == "stable";
  }

  inline int compare_identifier_lists(
    const std::vector<prerelease_identifier_t> &lhs,
    const std::vector<prerelease_identifier_t> &rhs,
    size_t start_index = 0
  ) {
    const size_t len = std::max(lhs.size(), rhs.size());
    for (size_t i = start_index; i < len; ++i) {
      if (i >= lhs.size()) {
        return -1;
      }
      if (i >= rhs.size()) {
        return 1;
      }

      const auto &left = lhs[i];
      const auto &right = rhs[i];
      const bool left_is_num = std::holds_alternative<int>(left);
      const bool right_is_num = std::holds_alternative<int>(right);

      if (left_is_num && right_is_num) {
        const int left_value = std::get<int>(left);
        const int right_value = std::get<int>(right);
        if (left_value != right_value) {
          return left_value < right_value ? -1 : 1;
        }
        continue;
      }

      if (left_is_num != right_is_num) {
        return left_is_num ? -1 : 1;
      }

      const auto &left_value = std::get<std::string>(left);
      const auto &right_value = std::get<std::string>(right);
      if (left_value != right_value) {
        return left_value < right_value ? -1 : 1;
      }
    }

    return 0;
  }

  inline int compare_semver(std::string_view lhs, std::string_view rhs) {
    const auto left = parse_semver(lhs);
    const auto right = parse_semver(rhs);

    if (left.major != right.major) {
      return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
      return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
      return left.patch < right.patch ? -1 : 1;
    }

    const bool left_stable = is_stable_channel(left);
    const bool right_stable = is_stable_channel(right);

    // Vibeshine uses `-stable.N` for post-release respins, so those need to sort
    // above the naked release while keeping normal prerelease semantics elsewhere.
    if (left_stable && right_stable) {
      return compare_identifier_lists(left.prerelease, right.prerelease, 1);
    }
    if (left_stable != right_stable) {
      return left_stable ? 1 : -1;
    }

    if (left.prerelease.empty() && right.prerelease.empty()) {
      return 0;
    }
    if (left.prerelease.empty()) {
      return 1;
    }
    if (right.prerelease.empty()) {
      return -1;
    }

    return compare_identifier_lists(left.prerelease, right.prerelease);
  }
}  // namespace version_compare
