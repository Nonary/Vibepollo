/**
 * @file src/app_order_policy.cpp
 * @brief Pure ordering rules for the application list sent to clients.
 */
#include "app_order_policy.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace proc::app_order {
  namespace {
    std::string lower(std::string_view text) {
      // ponytail: ASCII folding only, matching how Moonlight compares names.
      std::string result {text};
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return result;
    }

    std::string_view trim(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
      return text;
    }

    sort_e parse_sort(std::string_view text) {
      const auto value = lower(trim(text));
      if (value == "recent") return sort_e::recent;
      if (value == "playtime") return sort_e::playtime;
      return sort_e::name;
    }
  }  // namespace

  group_e group_of(std::string_view playnite_id, std::string_view steam_id, std::string_view lutris_id) {
    if (!trim(playnite_id).empty()) return group_e::playnite;
    if (!trim(steam_id).empty()) return group_e::steam;
    if (!trim(lutris_id).empty()) return group_e::lutris;
    return group_e::custom;
  }

  settings_t parse(std::string_view groups, std::string_view steam, std::string_view playnite, std::string_view lutris) {
    settings_t result;
    result.steam = parse_sort(steam);
    result.playnite = parse_sort(playnite);
    result.lutris = parse_sort(lutris);
    while (!groups.empty()) {
      const auto comma = groups.find(',');
      const auto token = lower(trim(groups.substr(0, comma)));
      groups = comma == std::string_view::npos ? std::string_view {} : groups.substr(comma + 1);
      group_e group;
      if (token == "custom") group = group_e::custom;
      else if (token == "steam") group = group_e::steam;
      else if (token == "playnite") group = group_e::playnite;
      else if (token == "lutris") group = group_e::lutris;
      else continue;
      if (std::find(result.groups.begin(), result.groups.end(), group) == result.groups.end()) {
        result.groups.push_back(group);
      }
    }
    return result;
  }

  std::vector<std::size_t> order(const std::vector<entry_t> &entries, const settings_t &settings) {
    std::vector<std::size_t> result(entries.size());
    std::iota(result.begin(), result.end(), std::size_t {0});
    if (settings.groups.empty()) {
      return result;
    }

    const auto unlisted = settings.groups.size();
    const auto rank = [&](group_e group) {
      return static_cast<std::size_t>(std::find(settings.groups.begin(), settings.groups.end(), group) - settings.groups.begin());
    };
    const auto sort_for = [&](group_e group) {
      switch (group) {
        case group_e::steam:
          return settings.steam;
        case group_e::playnite:
          return settings.playnite;
        case group_e::lutris:
          return settings.lutris;
        case group_e::custom:
          break;
      }
      return sort_e::name;
    };

    std::vector<std::string> names(entries.size());
    std::transform(entries.begin(), entries.end(), names.begin(), [](const auto &entry) {
      return lower(entry.name);
    });

    std::stable_sort(result.begin(), result.end(), [&](std::size_t left, std::size_t right) {
      const auto &a = entries[left];
      const auto &b = entries[right];
      const auto rank_a = rank(a.group);
      const auto rank_b = rank(b.group);
      if (rank_a != rank_b) return rank_a < rank_b;
      // Custom apps are ordered by hand and unlisted groups stay untouched.
      if (rank_a == unlisted || a.group == group_e::custom) return false;
      switch (sort_for(a.group)) {
        case sort_e::recent:
          if (a.last_played != b.last_played) return a.last_played > b.last_played;
          break;
        case sort_e::playtime:
          if (a.playtime_minutes != b.playtime_minutes) return a.playtime_minutes > b.playtime_minutes;
          break;
        case sort_e::name:
          break;
      }
      return names[left] < names[right];
    });
    return result;
  }
}  // namespace proc::app_order
