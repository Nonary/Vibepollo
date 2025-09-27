#include "tools/playnite_launcher/arguments.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>

namespace playnite_launcher {
  namespace {

    bool parse_arg(std::span<char *> args, std::string_view name, std::string &out) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (name == args[i]) {
          out = args[i + 1];
          return true;
        }
        std::string_view current(args[i]);
        std::string_view prefix = std::string(name) + "=";
        if (current.rfind(prefix, 0) == 0) {
          out.assign(args[i] + name.size() + 1);
          return true;
        }
      }
      return false;
    }

    bool parse_flag(std::span<char *> args, std::string_view name) {
      for (size_t i = 0; i < args.size(); ++i) {
        std::string_view current(args[i]);
        if (current == name) {
          return true;
        }
        if (current == std::string(name) + "=true") {
          return true;
        }
      }
      return false;
    }

    struct RawArgs {
      std::string game_id;
      std::string public_guid;
      std::string timeout;
      std::string focus_attempts;
      std::string focus_timeout;
      std::string exit_timeout;
      std::string install_dir;
      std::string wait_for_pid;
      bool fullscreen = false;
      bool cleanup = false;
      bool focus_exit_on_first = false;
    };

    RawArgs collect_raw_arguments(std::span<char *> args) {
      RawArgs raw;
      parse_arg(args, "--game-id", raw.game_id);
      parse_arg(args, "--public-guid", raw.public_guid);
      parse_arg(args, "--timeout", raw.timeout);
      parse_arg(args, "--focus-attempts", raw.focus_attempts);
      parse_arg(args, "--focus-timeout", raw.focus_timeout);
      parse_arg(args, "--exit-timeout", raw.exit_timeout);
      parse_arg(args, "--install-dir", raw.install_dir);
      parse_arg(args, "--wait-for-pid", raw.wait_for_pid);
      raw.focus_exit_on_first = parse_flag(args, "--focus-exit-on-first");
      raw.fullscreen = parse_flag(args, "--fullscreen");
      raw.cleanup = parse_flag(args, "--do-cleanup");
      return raw;
    }

    void apply_strings(const RawArgs &raw, LauncherConfig &config) {
      config.game_id = raw.game_id;
      config.public_guid = raw.public_guid;
      config.install_dir = raw.install_dir;
      config.wait_for_pid = raw.wait_for_pid;
    }

    void apply_numeric_option(const std::string &value, int min_value, int &target) {
      if (value.empty()) {
        return;
      }
      try {
        int parsed = std::stoi(value);
        target = std::max(min_value, parsed);
      } catch (...) {
      }
    }

    void apply_numeric_fields(const RawArgs &raw, LauncherConfig &config) {
      apply_numeric_option(raw.timeout, 1, config.timeout_sec);
      apply_numeric_option(raw.focus_attempts, 0, config.focus_attempts);
      apply_numeric_option(raw.focus_timeout, 0, config.focus_timeout_secs);
      apply_numeric_option(raw.exit_timeout, 0, config.exit_timeout_secs);
    }

    bool validate_modes(const LauncherConfig &config) {
      if (config.fullscreen || config.cleanup) {
        return true;
      }
      return !config.game_id.empty();
    }

  }  // namespace

  ParseResult parse_arguments(int argc, char **argv) {
    ParseResult result;
    std::span<char *> args(argv, static_cast<size_t>(argc));
    RawArgs raw = collect_raw_arguments(args);
    apply_strings(raw, result.config);
    result.config.focus_exit_on_first = raw.focus_exit_on_first;
    result.config.fullscreen = raw.fullscreen;
    result.config.cleanup = raw.cleanup;
    apply_numeric_fields(raw, result.config);
    if (!validate_modes(result.config)) {
      std::fprintf(stderr, "playnite-launcher: missing --game-id <GUID> or --fullscreen\n");
      result.success = false;
      result.exit_code = 2;
      return result;
    }
    result.success = true;
    result.exit_code = 0;
    return result;
  }

}  // namespace playnite_launcher
