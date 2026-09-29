#include "managed_app_focus.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace managed_app_focus {
  policy settings;

  std::string launcher_arguments(policy options) {
    return " --focus-attempts " + std::to_string(options.attempts) +
           " --focus-timeout " + std::to_string(options.timeout_secs) +
           (options.exit_on_first ? " --focus-exit-on-first" : "");
  }

  policy parse_policy(std::unordered_map<std::string, std::string> &vars) {
    policy result;
    const auto take = [&vars](const std::string &key) -> std::string {
      const auto it = vars.find(key);
      if (it == vars.end()) {
        return {};
      }
      auto value = std::move(it->second);
      vars.erase(it);
      return value;
    };
    const auto integer = [&](const char *key, int &value, int maximum) {
      const auto raw = take(key);
      int parsed = 0;
      const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
      if (converted.ec == std::errc {} && converted.ptr == raw.data() + raw.size()) {
        value = std::clamp(parsed, 0, maximum);
      }
    };
    const auto boolean = [&](const char *key) {
      auto raw = take(key);
      if (raw.empty()) {
        return;
      }
      std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return std::tolower(c);
      });
      result.exit_on_first = raw == "true" || raw == "yes" || raw == "enable" || raw == "enabled" || raw == "on" || raw == "1";
    };
    integer("playnite_focus_attempts", result.attempts, 100);
    integer("playnite_focus_timeout_secs", result.timeout_secs, 300);
    boolean("playnite_focus_exit_on_first");
    integer("app_focus_attempts", result.attempts, 100);
    integer("app_focus_timeout_secs", result.timeout_secs, 300);
    boolean("app_focus_exit_on_first");
    return result;
  }
}  // namespace managed_app_focus
