/**
 * @file src/provider_scan_protocol.cpp
 * @brief Bounded protocol for unprivileged Linux provider discovery.
 */
#include "provider_scan_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <system_error>

#if defined(__linux__)
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char **environ;
#endif

namespace fs = std::filesystem;

namespace platf::provider_scan {
  namespace {
    using json = nlohmann::json;

    constexpr std::size_t max_name_bytes = 512;
    constexpr std::size_t max_label_bytes = 128;

#if defined(__linux__)
    constexpr auto child_reap_timeout = std::chrono::seconds {1};
    constexpr auto child_reap_poll_interval = std::chrono::milliseconds {10};

    bool reap_child_until(pid_t child, int &status,
                          std::chrono::steady_clock::time_point deadline) {
      while (true) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) return true;
        if (waited < 0 && errno != EINTR) return false;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        if (waited < 0) continue;

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto delay = std::min(remaining, child_reap_poll_interval);
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(delay.count(), 1));
        if (poll(nullptr, 0, wait_ms) < 0 && errno != EINTR) return false;
      }
    }

    bool terminate_and_reap_child(pid_t child, int &status) {
      const auto deadline = std::chrono::steady_clock::now() + child_reap_timeout;
      while (true) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) return true;
        if (waited == 0) break;
        if (errno != EINTR) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
      }

      const bool termination_requested = kill(child, SIGKILL) == 0 || errno == ESRCH;
      // Always make a bounded reap attempt. The child may have exited between
      // waitpid() and kill(), and no cleanup error may turn into a blocking
      // wait on the machine host.
      const bool reaped = reap_child_until(child, status, deadline);
      return termination_requested && reaped;
    }
#endif

    bool safe_text(const std::string &value, std::size_t maximum, bool allow_empty = true) {
      if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
      return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      });
    }

    bool steam_provider(const std::string &runner, const std::string &service) {
      const auto is_steam = [](const std::string &value) {
        return value.size() == 5 &&
               (value[0] == 's' || value[0] == 'S') &&
               (value[1] == 't' || value[1] == 'T') &&
               (value[2] == 'e' || value[2] == 'E') &&
               (value[3] == 'a' || value[3] == 'A') &&
               (value[4] == 'm' || value[4] == 'M');
      };
      return is_steam(runner) || is_steam(service);
    }

    template<typename T>
    bool exact_keys(const json &value, std::initializer_list<T> expected) {
      if (!value.is_object() || value.size() != expected.size()) return false;
      return std::all_of(expected.begin(), expected.end(), [&](const auto *key) {
        return value.contains(key);
      });
    }

    bool unsigned_number(const json &value, std::uint64_t &result) {
      if (!value.is_number_unsigned()) return false;
      result = value.get<std::uint64_t>();
      return true;
    }

    bool signed_number(const json &value, std::int64_t &result) {
      if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return false;
        }
        result = static_cast<std::int64_t>(number);
        return true;
      }
      if (!value.is_number_integer()) return false;
      result = value.get<std::int64_t>();
      return true;
    }

    bool safe_string_field(const json &object, const char *key, std::string &result,
                           std::size_t maximum, bool allow_empty = true) {
      const auto &value = object.at(key);
      if (!value.is_string()) return false;
      result = value.get<std::string>();
      return safe_text(result, maximum, allow_empty);
    }

    bool valid_header(const json &root, std::string_view provider) {
      return root.at("schema").is_string() && root.at("schema").get<std::string>() == schema_name &&
             root.at("version").is_number_unsigned() && root.at("version").get<std::uint64_t>() == schema_version &&
             root.at("provider").is_string() && root.at("provider").get<std::string>() == provider &&
             root.at("games").is_array() && root.at("games").size() <= max_games;
    }

    std::string artwork_revision(std::initializer_list<fs::path> paths) {
      std::uint64_t hash = 1469598103934665603ULL;
      bool found = false;
      for (const auto &path : paths) {
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(path, ec)) continue;
        const auto size = fs::file_size(path, ec);
        if (ec) continue;
        const auto time = fs::last_write_time(path, ec);
        if (ec) continue;
        found = true;
        const auto value = path.generic_string() + ":" + std::to_string(size) + ":" +
                           std::to_string(time.time_since_epoch().count());
        for (const unsigned char byte : value) hash = (hash ^ byte) * 1099511628211ULL;
      }
      return found ? std::to_string(hash) : std::string {};
    }

    bool decode_artwork_revision(const json &entry, std::string &revision) {
      if (!entry.is_object()) return false;
      if (!entry.contains("artwork_revision")) return true;
      if (!safe_string_field(entry, "artwork_revision", revision, 20)) return false;
      return std::all_of(revision.begin(), revision.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
    }

    json steam_game_json(const steam::game_t &game) {
      return {
        {"app_id", game.app_id},
        {"artwork_revision", artwork_revision({game.artwork_path, game.boxart_path, game.icon_path})},
        {"name", game.name},
        {"app_type", game.app_type},
        {"installed", game.installed},
        {"last_played", game.last_played},
        {"playtime_minutes", game.playtime_minutes},
        {"state_flags", game.state_flags},
        {"last_updated", game.last_updated},
      };
    }

    json lutris_game_json(const lutris::game_t &game) {
      return {
        {"id", game.id},
        {"artwork_revision", artwork_revision({game.artwork_path, game.icon_path})},
        {"name", game.name},
        {"slug", game.slug},
        {"runner", game.runner},
        {"platform", game.platform},
        {"service", game.service},
        {"service_id", game.service_id},
        {"last_played", game.last_played},
        {"playtime_seconds", game.playtime_seconds},
      };
    }

    std::optional<json> parse_payload(std::string_view payload) {
      if (payload.empty() || payload.size() > max_payload_bytes) return std::nullopt;
      try {
        return json::parse(payload.begin(), payload.end());
      } catch (const json::exception &) {
        return std::nullopt;
      }
    }

    std::optional<std::string> bounded_dump(const json &root) {
      try {
        auto payload = root.dump();
        if (payload.size() >= max_payload_bytes) return std::nullopt;
        payload.push_back('\n');
        return payload;
      } catch (const json::exception &) {
        return std::nullopt;
      }
    }
  }  // namespace

  std::optional<std::string> encode_steam_catalog(const steam_catalog_t &catalog) {
    if (catalog.games.size() > max_games) return std::nullopt;
    json root {
      {"schema", schema_name},
      {"version", schema_version},
      {"provider", "steam"},
      {"available", catalog.available},
      {"games", json::array()},
    };
    for (const auto &game : catalog.games) root["games"].push_back(steam_game_json(game));
    const auto payload = bounded_dump(root);
    return payload && decode_steam_catalog(*payload) ? payload : std::nullopt;
  }

  std::optional<std::string> encode_lutris_catalog(const lutris_catalog_t &catalog) {
    if (catalog.games.size() > max_games) return std::nullopt;
    json root {
      {"schema", schema_name},
      {"version", schema_version},
      {"provider", "lutris"},
      {"database_available", catalog.database_available},
      {"executable_available", catalog.executable_available},
      {"games", json::array()},
    };
    for (const auto &game : catalog.games) root["games"].push_back(lutris_game_json(game));
    const auto payload = bounded_dump(root);
    return payload && decode_lutris_catalog(*payload) ? payload : std::nullopt;
  }

  std::optional<steam_catalog_t> decode_steam_catalog(std::string_view payload) {
    const auto parsed = parse_payload(payload);
    if (!parsed || !exact_keys(*parsed, {"schema", "version", "provider", "available", "games"}) ||
        !valid_header(*parsed, "steam") || !parsed->at("available").is_boolean()) return std::nullopt;

    steam_catalog_t result;
    result.available = parsed->at("available").get<bool>();
    if (!result.available && !parsed->at("games").empty()) return std::nullopt;
    std::set<std::uint32_t> seen;
    for (auto entry : parsed->at("games")) {
      std::string revision;
      if (!decode_artwork_revision(entry, revision)) return std::nullopt;
      entry.erase("artwork_revision");
      if (!exact_keys(entry, {"app_id", "name", "app_type", "installed",
                              "last_played", "playtime_minutes", "state_flags", "last_updated"})) return std::nullopt;
      steam::game_t game;
      game.session_artwork_revision = revision;
      std::uint64_t number = 0;
      if (!unsigned_number(entry.at("app_id"), number) || number == 0 || number > UINT32_MAX ||
          !seen.insert(static_cast<std::uint32_t>(number)).second) return std::nullopt;
      game.app_id = static_cast<std::uint32_t>(number);
      game.stable_id = "steam:" + std::to_string(game.app_id);
      if (!safe_string_field(entry, "name", game.name, max_name_bytes) ||
          !safe_string_field(entry, "app_type", game.app_type, max_label_bytes) ||
          !entry.at("installed").is_boolean()) return std::nullopt;
      game.installed = entry.at("installed").get<bool>();
      if (!unsigned_number(entry.at("last_played"), game.last_played) ||
          !unsigned_number(entry.at("playtime_minutes"), game.playtime_minutes) ||
          !unsigned_number(entry.at("state_flags"), number) || number > UINT32_MAX) return std::nullopt;
      game.state_flags = static_cast<std::uint32_t>(number);
      if (!unsigned_number(entry.at("last_updated"), game.last_updated)) return std::nullopt;
      // All paths, local artwork, and direct-launch fields remain empty by
      // design. Only the dropped-UID scanner ever reads those user files.
      result.games.push_back(std::move(game));
    }
    return result;
  }

  std::optional<lutris_catalog_t> decode_lutris_catalog(std::string_view payload) {
    const auto parsed = parse_payload(payload);
    if (!parsed || !exact_keys(*parsed, {"schema", "version", "provider", "database_available",
                                        "executable_available", "games"}) ||
        !valid_header(*parsed, "lutris") || !parsed->at("database_available").is_boolean() ||
        !parsed->at("executable_available").is_boolean()) return std::nullopt;

    lutris_catalog_t result;
    result.database_available = parsed->at("database_available").get<bool>();
    result.executable_available = parsed->at("executable_available").get<bool>();
    if (!result.database_available && !parsed->at("games").empty()) return std::nullopt;
    std::set<std::int64_t> seen;
    for (auto entry : parsed->at("games")) {
      std::string revision;
      if (!decode_artwork_revision(entry, revision)) return std::nullopt;
      entry.erase("artwork_revision");
      if (!exact_keys(entry, {"id", "name", "slug", "runner", "platform", "service",
                              "service_id", "last_played", "playtime_seconds"})) return std::nullopt;
      lutris::game_t game;
      game.session_artwork_revision = revision;
      if (!signed_number(entry.at("id"), game.id) || game.id <= 0 || !seen.insert(game.id).second ||
          !safe_string_field(entry, "name", game.name, max_name_bytes) ||
          !safe_string_field(entry, "slug", game.slug, max_label_bytes) ||
          !safe_string_field(entry, "runner", game.runner, max_label_bytes) ||
          !safe_string_field(entry, "platform", game.platform, max_label_bytes) ||
          !safe_string_field(entry, "service", game.service, max_label_bytes) ||
          !safe_string_field(entry, "service_id", game.service_id, max_label_bytes) ||
          !signed_number(entry.at("last_played"), game.last_played) || game.last_played < 0 ||
          !entry.at("playtime_seconds").is_number()) return std::nullopt;
      game.playtime_seconds = entry.at("playtime_seconds").get<double>();
      if (!std::isfinite(game.playtime_seconds) || game.playtime_seconds < 0.0 || game.playtime_seconds > 1.0e12) {
        return std::nullopt;
      }
      if (steam_provider(game.runner, game.service)) {
        if (game.service_id.empty() || !std::all_of(game.service_id.begin(), game.service_id.end(), [](unsigned char c) {
              return c >= '0' && c <= '9';
            })) return std::nullopt;
        game.stable_id = "steam:" + game.service_id;
      } else {
        game.stable_id = "lutris:" + std::to_string(game.id);
      }
      // Every path remains empty: the privileged host has no reason to open
      // it, and launch is delegated by numeric Lutris ID.
      result.games.push_back(std::move(game));
    }
    return result;
  }

#if defined(__linux__)
  namespace detail {
    std::optional<std::string> capture_command(const fs::path &executable, std::string_view verb,
                                               capture_limits_t limits) {
      if (!executable.is_absolute() || verb.empty() || verb.size() > 64 || limits.timeout.count() <= 0 ||
          limits.maximum_bytes == 0 || limits.maximum_bytes > max_payload_bytes) return std::nullopt;
      std::string verb_string {verb};
      if (!safe_text(verb_string, 64, false)) return std::nullopt;

      int output_pipe[2] {-1, -1};
      if (pipe2(output_pipe, O_CLOEXEC) != 0) return std::nullopt;
      const int read_flags = fcntl(output_pipe[0], F_GETFL);
      if (read_flags < 0 || fcntl(output_pipe[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return std::nullopt;
      }

      posix_spawn_file_actions_t actions;
      if (posix_spawn_file_actions_init(&actions) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return std::nullopt;
      }
      bool actions_ok = posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO) == 0 &&
                        posix_spawn_file_actions_addclose(&actions, output_pipe[0]) == 0 &&
                        posix_spawn_file_actions_addclose(&actions, output_pipe[1]) == 0;
      std::string executable_string = executable.string();
      char *arguments[] = {executable_string.data(), verb_string.data(), nullptr};
      pid_t child = -1;
      const int spawn_error = actions_ok ?
        posix_spawn(&child, executable_string.c_str(), &actions, nullptr, arguments, environ) : EINVAL;
      posix_spawn_file_actions_destroy(&actions);
      close(output_pipe[1]);
      if (spawn_error != 0) {
        close(output_pipe[0]);
        return std::nullopt;
      }

      const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
      std::string output;
      output.reserve(std::min<std::size_t>(limits.maximum_bytes, 64U * 1024U));
      bool eof = false;
      bool child_done = false;
      bool failed = false;
      int status = 0;
      while (!failed && !(eof && child_done)) {
        char buffer[8192];
        while (!eof) {
          const auto count = read(output_pipe[0], buffer, sizeof(buffer));
          if (count > 0) {
            if (output.size() + static_cast<std::size_t>(count) > limits.maximum_bytes) {
              failed = true;
              break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
          } else if (count == 0) {
            eof = true;
            break;
          } else if (errno == EINTR) {
            continue;
          } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          } else {
            failed = true;
            break;
          }
        }
        if (!child_done) {
          const auto waited = waitpid(child, &status, WNOHANG);
          if (waited == child) child_done = true;
          else if (waited < 0 && errno != EINTR) failed = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!(eof && child_done) && now >= deadline) {
          failed = true;
          break;
        }
        if (!failed && !(eof && child_done)) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
          const int wait_ms = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 100));
          pollfd descriptor {output_pipe[0], static_cast<short>(eof ? 0 : POLLIN | POLLHUP), 0};
          const auto polled = poll(eof ? nullptr : &descriptor, eof ? 0 : 1, wait_ms);
          if (polled < 0 && errno != EINTR) failed = true;
        }
      }
      close(output_pipe[0]);
      if (failed) {
        if (!child_done) {
          // Failure cleanup has its own short monotonic deadline because the
          // command deadline may already have elapsed. Never let an abnormal
          // child or wait implementation stall the privileged host forever.
          (void) terminate_and_reap_child(child, status);
        }
        return std::nullopt;
      }
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? std::optional<std::string>(std::move(output)) : std::nullopt;
    }
  }  // namespace detail

  std::optional<steam_catalog_t> scan_steam_session() {
    const auto payload = detail::capture_command(
      "/usr/libexec/vibeshine/vibepollo-session-exec", "provider-steam-scan",
      {std::chrono::duration_cast<std::chrono::milliseconds>(command_timeout), max_payload_bytes});
    return payload ? decode_steam_catalog(*payload) : std::nullopt;
  }

  std::optional<lutris_catalog_t> scan_lutris_session() {
    const auto payload = detail::capture_command(
      "/usr/libexec/vibeshine/vibepollo-session-exec", "provider-lutris-scan",
      {std::chrono::duration_cast<std::chrono::milliseconds>(command_timeout), max_payload_bytes});
    return payload ? decode_lutris_catalog(*payload) : std::nullopt;
  }
#endif
}  // namespace platf::provider_scan
