#include "lutris_integration.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string_view>

#if defined(__linux__)
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace platf::lutris {
  namespace {
    struct database_deleter {
      void operator()(sqlite3 *database) const {
        if (database) sqlite3_close(database);
      }
    };
    struct statement_deleter {
      void operator()(sqlite3_stmt *statement) const {
        if (statement) sqlite3_finalize(statement);
      }
    };
    using database_ptr = std::unique_ptr<sqlite3, database_deleter>;
    using statement_ptr = std::unique_ptr<sqlite3_stmt, statement_deleter>;

    std::string text(sqlite3_stmt *statement, int column) {
      const auto *value = sqlite3_column_text(statement, column);
      return value ? reinterpret_cast<const char *>(value) : std::string {};
    }

    std::string lower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
      return value;
    }

    std::filesystem::path first_existing(const std::filesystem::path &base,
                                         const std::string &slug,
                                         std::initializer_list<std::string_view> extensions) {
      for (const auto extension : extensions) {
        auto candidate = base / (slug + std::string(extension));
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
      }
      return {};
    }

    std::filesystem::path discover_image(const std::filesystem::path &lutris_data,
                                         const std::string &slug) {
      if (slug.empty()) return {};
      // Sunshine's catalog image contract is PNG. Prefer Lutris' generated
      // application icon; JPEG cover art remains visible through the API but
      // is not passed to clients that require a PNG signature.
      auto data_home = lutris_data.parent_path();
      for (const auto size : {"256x256", "128x128", "64x64", "48x48"}) {
        auto image = first_existing(data_home / "icons/hicolor" / size / "apps",
                                    "lutris_" + slug, {".png"});
        if (!image.empty()) return image;
      }
      return first_existing(lutris_data / "coverart", slug, {".png"});
    }

    void hash_bytes(std::uint64_t &hash, std::string_view value) {
      constexpr std::uint64_t prime = 1099511628211ULL;
      for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= prime;
      }
      hash ^= 0xff;
      hash *= prime;
    }

#if defined(__linux__)
    std::filesystem::path find_lutris_executable() {
      const auto *path_value = std::getenv("PATH");
      if (!path_value || !*path_value) return {};
      std::string_view paths {path_value};
      while (true) {
        const auto separator = paths.find(':');
        const auto directory = paths.substr(0, separator);
        const auto candidate = (directory.empty() ? std::filesystem::current_path() : std::filesystem::path(directory)) / "lutris";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (separator == std::string_view::npos) break;
        paths.remove_prefix(separator + 1);
      }
      return {};
    }
#endif
  }  // namespace

  bool game_t::steam_backed() const {
    return lower(runner) == "steam" || lower(service) == "steam";
  }

  std::filesystem::path default_database_path() {
#if defined(__linux__)
    if (const auto *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
      const auto candidate = std::filesystem::path(xdg) / "lutris/pga.db";
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    if (const auto *home = std::getenv("HOME"); home && *home) {
      for (const auto &relative : {
             std::filesystem::path(".local/share/lutris/pga.db"),
             std::filesystem::path(".var/app/net.lutris.Lutris/data/lutris/pga.db")}) {
        const auto candidate = std::filesystem::path(home) / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
      }
    }
#endif
    return {};
  }

  std::vector<game_t> discover(const std::filesystem::path &requested_database) {
    const auto database_path = requested_database.empty() ? default_database_path() : requested_database;
    if (database_path.empty()) return {};

    sqlite3 *raw_database = nullptr;
    if (sqlite3_open_v2(database_path.c_str(), &raw_database,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
      if (raw_database) sqlite3_close(raw_database);
      return {};
    }
    database_ptr database {raw_database};
    sqlite3_busy_timeout(database.get(), 1000);

    constexpr const char *query =
      "SELECT id, name, slug, runner, platform, directory, configpath, "
      "service, service_id, lastplayed, playtime "
      "FROM games WHERE installed = 1 AND configpath IS NOT NULL AND configpath <> '' ORDER BY id";
    sqlite3_stmt *raw_statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), query, -1, &raw_statement, nullptr) != SQLITE_OK) return {};
    statement_ptr statement {raw_statement};

    std::vector<game_t> games;
    const auto lutris_data = database_path.parent_path();
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
      game_t game;
      game.id = sqlite3_column_int64(statement.get(), 0);
      if (game.id <= 0) continue;
      game.name = text(statement.get(), 1);
      game.slug = text(statement.get(), 2);
      game.runner = text(statement.get(), 3);
      game.platform = text(statement.get(), 4);
      game.directory = text(statement.get(), 5);
      game.config_path = text(statement.get(), 6);
      game.service = text(statement.get(), 7);
      game.service_id = text(statement.get(), 8);
      game.last_played = sqlite3_column_int64(statement.get(), 9);
      game.playtime_seconds = sqlite3_column_double(statement.get(), 10);
      game.image_path = discover_image(lutris_data, game.slug);
      game.stable_id = game.steam_backed() && !game.service_id.empty() ?
                         "steam:" + game.service_id :
                         "lutris:" + std::to_string(game.id);
      games.push_back(std::move(game));
    }
    return games;
  }

  std::uint64_t source_fingerprint(const std::vector<game_t> &games) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &game : games) {
      hash_bytes(hash, std::to_string(game.id));
      hash_bytes(hash, game.name);
      hash_bytes(hash, game.runner);
      hash_bytes(hash, game.service_id);
      hash_bytes(hash, game.directory.generic_string());
      hash_bytes(hash, game.config_path);
    }
    return hash;
  }

  std::string launch_uri(std::int64_t id) {
    return id > 0 ? "lutris:rungameid/" + std::to_string(id) : std::string {};
  }

  bool executable_available() {
#if defined(__linux__)
    return !find_lutris_executable().empty();
#else
    return false;
#endif
  }

  bool launch(std::int64_t id) {
#if defined(__linux__)
    const auto executable = find_lutris_executable();
    const auto uri = launch_uri(id);
    if (executable.empty() || uri.empty()) return false;

    int error_pipe[2] {-1, -1};
    if (pipe(error_pipe) != 0 || fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
      if (error_pipe[0] >= 0) close(error_pipe[0]);
      if (error_pipe[1] >= 0) close(error_pipe[1]);
      return false;
    }
    const auto child = fork();
    if (child < 0) {
      close(error_pipe[0]);
      close(error_pipe[1]);
      return false;
    }
    if (child == 0) {
      close(error_pipe[0]);
      const auto grandchild = fork();
      if (grandchild > 0) _exit(0);
      if (grandchild < 0) {
        const int error = errno;
        (void) write(error_pipe[1], &error, sizeof(error));
        _exit(127);
      }
      setsid();
      execl(executable.c_str(), executable.c_str(), uri.c_str(), static_cast<char *>(nullptr));
      const int error = errno;
      (void) write(error_pipe[1], &error, sizeof(error));
      _exit(127);
    }
    close(error_pipe[1]);
    (void) waitpid(child, nullptr, 0);
    int error = 0;
    const auto bytes = read(error_pipe[0], &error, sizeof(error));
    close(error_pipe[0]);
    return bytes == 0;
#else
    (void) id;
    return false;
#endif
  }
}  // namespace platf::lutris
