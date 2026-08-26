/** @file src/steam_integration.cpp */
#include "steam_integration.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
  #include <shellapi.h>
  #include <windows.h>
#elif defined(__APPLE__)
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#else
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
  using platf::steam::vdf_node;

  struct lexer {
    const std::string &s;
    size_t pos = 0;

    void whitespace() {
      while (pos < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[pos]))) {
          ++pos;
        } else if (s[pos] == '/' && pos + 1 < s.size() && s[pos + 1] == '/') {
          pos += 2;
          while (pos < s.size() && s[pos] != '\n') {
            ++pos;
          }
        } else {
          break;
        }
      }
    }

    std::optional<std::string> token() {
      whitespace();
      if (pos >= s.size() || s[pos] == '{' || s[pos] == '}') {
        return std::nullopt;
      }
      if (s[pos] == '"') {
        ++pos;
        std::string out;
        while (pos < s.size()) {
          const char c = s[pos++];
          if (c == '"') {
            break;
          }
          if (c == '\\' && pos < s.size()) {
            const char escaped = s[pos++];
            switch (escaped) {
              case 'n':
                out += '\n';
                break;
              case 't':
                out += '\t';
                break;
              case 'r':
                out += '\r';
                break;
              default:
                out += escaped;
                break;
            }
          } else {
            out += c;
          }
        }
        return out;
      }
      const auto begin = pos;
      while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos])) && s[pos] != '{' && s[pos] != '}') {
        ++pos;
      }
      return s.substr(begin, pos - begin);
    }
  };

  void parse_body(lexer &lex, vdf_node &out, bool stop_at_brace) {
    while (true) {
      lex.whitespace();
      if (lex.pos >= lex.s.size()) {
        return;
      }
      if (stop_at_brace && lex.s[lex.pos] == '}') {
        ++lex.pos;
        return;
      }
      auto key = lex.token();
      if (!key) {
        // An unexpected opening brace cannot form a valid pair.
        if (lex.pos < lex.s.size() && lex.s[lex.pos] == '{') {
          ++lex.pos;
        }
        continue;
      }
      lex.whitespace();
      if (lex.pos < lex.s.size() && lex.s[lex.pos] == '{') {
        ++lex.pos;
        vdf_node child;
        parse_body(lex, child, true);
        out.children.emplace_back(*key, std::move(child));
      } else if (auto value = lex.token()) {
        out.children.emplace_back(*key, vdf_node {*value, {}});
      } else {
        out.children.emplace_back(*key, vdf_node {});
      }
    }
  }

  std::optional<std::uint64_t> number(const vdf_node *node) {
    if (!node) {
      return std::nullopt;
    }
    try {
      size_t used = 0;
      const auto n = std::stoull(node->value, &used, 10);
      return used == node->value.size() ? std::optional<std::uint64_t>(n) : std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string env(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
  }

  fs::path steamapps_for(fs::path root) {
    std::error_code ec;
    if (root.filename() == "steamapps" && fs::is_directory(root, ec)) {
      return root;
    }
    if (fs::is_regular_file(root / "libraryfolders.vdf", ec)) {
      return root;
    }
    if (fs::is_directory(root / "steamapps", ec)) {
      return root / "steamapps";
    }
    return {};
  }

  const vdf_node *library_path_node(const vdf_node &node) {
    if (const auto *path = node.find("path")) {
      return path;
    }
    // Legacy libraryfolders.vdf may use a direct path value.
    return node.value.empty() ? nullptr : &node;
  }

  void add_root(std::vector<fs::path> &roots, fs::path root) {
    if (root.empty()) {
      return;
    }
    std::error_code ec;
    root = fs::weakly_canonical(root, ec);
    if (ec || !fs::exists(root, ec)) {
      return;
    }
    if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
      roots.push_back(std::move(root));
    }
  }

  bool regular(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
  }

  fs::path first_existing(const std::vector<fs::path> &dirs, std::uint32_t app_id,
                         std::initializer_list<const char *> suffixes) {
    const auto id = std::to_string(app_id);
    for (const auto &dir : dirs) {
      for (const auto *suffix : suffixes) {
        const auto path = dir / (id + suffix);
        if (regular(path)) {
          return path;
        }
      }
    }
    return {};
  }

  fs::path first_nested_existing(const std::vector<fs::path> &dirs, std::uint32_t app_id,
                                 std::initializer_list<const char *> names) {
    const auto id = std::to_string(app_id);
    for (const auto &dir : dirs) {
      for (const auto *name : names) {
        const auto path = dir / id / name;
        if (regular(path)) return path;
      }
    }
    return {};
  }

  struct artwork_t {
    fs::path portrait;
    fs::path header;
    fs::path icon;
    std::string format;
  };

  artwork_t artwork_for(std::uint32_t app_id, const fs::path &library,
                        const std::vector<fs::path> &steam_roots) {
    std::vector<fs::path> cache_dirs;
    std::vector<fs::path> grid_dirs;
    auto add_root_dirs = [&](const fs::path &root) {
      if (root.empty()) return;
      cache_dirs.push_back(root / "appcache/librarycache");
      cache_dirs.push_back(root / "librarycache");
      std::error_code ec;
      const auto userdata = root / "userdata";
      if (fs::is_directory(userdata, ec)) {
        for (const auto &user : fs::directory_iterator(userdata, ec)) {
          if (!ec && user.is_directory(ec)) {
            grid_dirs.push_back(user.path() / "config/grid");
          }
        }
      }
    };
    add_root_dirs(library);
    for (const auto &root : steam_roots) add_root_dirs(root);

    // New librarycache artwork is preferred because it is Steam's canonical
    // portrait cover. Steam's _2x asset is the only local variant that is
    // normally 600x900; check every cache directory for it before considering
    // the older 300x450 files. User grid art is a useful fallback for custom
    // artwork.
    artwork_t out;
    out.portrait = first_existing(cache_dirs, app_id,
      {"_library_600x900_2x.jpg", "_library_600x900_2x.png", "_library_600x900_2x.webp"});
    if (out.portrait.empty()) {
      out.portrait = first_nested_existing(cache_dirs, app_id,
        {"library_600x900_2x.jpg", "library_600x900_2x.png", "library_600x900_2x.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_existing(cache_dirs, app_id,
        {"_library_600x900.jpg", "_library_600x900.png", "_library_600x900.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_nested_existing(cache_dirs, app_id,
        {"library_600x900.jpg", "library_600x900.png", "library_600x900.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_existing(grid_dirs, app_id, {"p.png", "p.jpg", "_p.png", "_p.jpg"});
    }
    out.header = first_existing(cache_dirs, app_id, {"_header.jpg", "_header.png", "_header.webp"});
    if (out.header.empty()) out.header = first_nested_existing(cache_dirs, app_id, {"header.jpg", "header.png", "header.webp"});
    if (out.header.empty()) out.header = first_existing(grid_dirs, app_id, {"_hero.png", "_hero.jpg"});
    out.icon = first_existing(cache_dirs, app_id, {"_icon.jpg", "_icon.png", "_icon.webp"});
    if (out.icon.empty()) out.icon = first_nested_existing(cache_dirs, app_id, {"icon.jpg", "icon.png", "icon.webp"});
    if (out.icon.empty()) out.icon = first_existing(grid_dirs, app_id, {"_icon.png", "_icon.jpg", ".png"});
    out.format = !out.portrait.empty() ? out.portrait.extension().string() :
                 (!out.header.empty() ? out.header.extension().string() :
                  (!out.icon.empty() ? out.icon.extension().string() : std::string {}));
    if (!out.format.empty() && out.format.front() == '.') out.format.erase(0, 1);
    return out;
  }
}  // namespace

namespace platf::steam {
  const vdf_node *vdf_node::find(const std::string &key) const {
    for (const auto &[name, node] : children) {
      if (name == key) {
        return &node;
      }
    }
    return nullptr;
  }

  vdf_node parse_vdf(const std::string &contents) {
    lexer lex {contents};
    vdf_node root;
    parse_body(lex, root, false);
    return root;
  }

  std::vector<fs::path> default_library_roots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    DWORD steam_path_size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, nullptr, &steam_path_size) == ERROR_SUCCESS && steam_path_size > sizeof(wchar_t)) {
      std::vector<wchar_t> steam_path(steam_path_size / sizeof(wchar_t));
      if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                       nullptr, steam_path.data(), &steam_path_size) == ERROR_SUCCESS) {
        add_root(roots, fs::path(steam_path.data()));
      }
    }
    if (!env("PROGRAMFILES(X86)").empty()) {
      add_root(roots, fs::path(env("PROGRAMFILES(X86)")) / "Steam");
    }
    if (!env("PROGRAMFILES").empty()) {
      add_root(roots, fs::path(env("PROGRAMFILES")) / "Steam");
    }
    if (!env("LOCALAPPDATA").empty()) {
      add_root(roots, fs::path(env("LOCALAPPDATA")) / "Steam");
    }
#elif defined(__APPLE__)
    const auto home = env("HOME");
    if (!home.empty()) {
      add_root(roots, fs::path(home) / "Library/Application Support/Steam");
    }
#else
    const auto home = env("HOME");
    const auto xdg = env("XDG_DATA_HOME");
    if (!home.empty()) {
      add_root(roots, fs::path(home) / ".local/share/Steam");
      add_root(roots, fs::path(home) / ".steam/steam");
      add_root(roots, fs::path(home) / ".steam/root");
      add_root(roots, fs::path(home) / ".steam/debian-installation");
      // Flatpak Steam keeps its native library metadata in the sandbox data
      // directory. Libraries added on other mounts are then picked up from
      // its libraryfolders.vdf just like a native installation.
      add_root(roots, fs::path(home) / ".var/app/com.valvesoftware.Steam/data/Steam");
      add_root(roots, fs::path(home) / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
      add_root(roots, fs::path(home) / "snap/steam/common/.local/share/Steam");
    }
    if (!xdg.empty()) {
      add_root(roots, fs::path(xdg) / "Steam");
    }
#endif
    return roots;
  }

  std::vector<game_t> discover(const std::vector<fs::path> &requested_roots) {
    auto roots = requested_roots;
    if (roots.empty()) {
      roots = default_library_roots();
    }
    std::map<std::uint32_t, game_t> found;
    std::unordered_set<std::string> parsed_libraries;

    auto scan_library = [&](const fs::path &library) {
      const auto apps = steamapps_for(library);
      if (apps.empty()) {
        return;
      }
      std::error_code ec;
      const auto key = fs::weakly_canonical(apps, ec).generic_string();
      if (!parsed_libraries.insert(key).second) {
        return;
      }
      for (const auto &entry : fs::directory_iterator(apps, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
          continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename.rfind("appmanifest_", 0) != 0 || entry.path().extension() != ".acf") {
          continue;
        }
        std::ifstream input(entry.path());
        if (!input) {
          continue;
        }
        std::stringstream buffer;
        buffer << input.rdbuf();
        const auto doc = parse_vdf(buffer.str());
        const auto *app = doc.find("AppState");
        if (!app) {
          app = &doc;
        }
        const auto id = number(app->find("appid"));
        if (!id || *id == 0 || *id > UINT32_MAX) {
          continue;
        }
        game_t game;
        game.app_id = static_cast<std::uint32_t>(*id);
        game.stable_id = "steam:" + std::to_string(game.app_id);
        if (const auto *name = app->find("name")) {
          game.name = name->value;
        }
        if (const auto *type = app->find("type")) {
          game.app_type = type->value;
        }
        if (const auto *dir = app->find("installdir")) {
          game.install_dir = apps / "common" / dir->value;
        }
        game.library_path = apps.parent_path();
        if (const auto flags = number(app->find("StateFlags"))) {
          game.state_flags = static_cast<std::uint32_t>(*flags);
        }
        if (const auto updated = number(app->find("LastUpdated"))) {
          game.last_updated = *updated;
        }
        // A manifest can linger while an uninstall/move is in progress. Do
        // not advertise or launch it unless Steam's declared install folder
        // currently exists. StateFlags alone is not reliable during updates.
        if (game.install_dir.empty() || !fs::is_directory(game.install_dir, ec)) {
          ec.clear();
          continue;
        }
        const auto art = artwork_for(game.app_id, game.library_path, roots);
        game.portrait_path = art.portrait;
        game.boxart_path = art.portrait;
        game.artwork_path = !art.portrait.empty() ? art.portrait : (!art.header.empty() ? art.header : art.icon);
        game.artwork_format = art.format;
        game.icon_path = art.icon;
        game.header_path = art.header;
        auto [existing, inserted] = found.emplace(game.app_id, game);
        if (!inserted) {
          // A duplicate manifest can be reached through multiple roots. Keep
          // the first deterministic record but fill artwork/type gaps from
          // the later record.
          auto &candidate = existing->second;
          const auto &other = game;
          if (candidate.portrait_path.empty()) candidate.portrait_path = other.portrait_path;
          if (candidate.boxart_path.empty()) candidate.boxart_path = other.boxart_path;
          if (candidate.artwork_path.empty()) candidate.artwork_path = other.artwork_path;
          if (candidate.artwork_format.empty()) candidate.artwork_format = other.artwork_format;
          if (candidate.header_path.empty()) candidate.header_path = other.header_path;
          if (candidate.icon_path.empty()) candidate.icon_path = other.icon_path;
          if (candidate.app_type.empty()) candidate.app_type = other.app_type;
        }
      }
    };

    for (const auto &root : roots) {
      const auto apps = steamapps_for(root);
      if (!apps.empty()) {
        scan_library(apps);
      }
      const auto file = apps.empty() ? root / "steamapps/libraryfolders.vdf" : apps / "libraryfolders.vdf";
      std::ifstream input(file);
      if (!input) {
        continue;
      }
      std::stringstream buffer;
      buffer << input.rdbuf();
      const auto doc = parse_vdf(buffer.str());
      const auto *libraries = doc.find("libraryfolders");
      if (!libraries) {
        libraries = &doc;
      }
      for (const auto &[index, node] : libraries->children) {
        (void) index;
        if (const auto *path = library_path_node(node)) {
          scan_library(fs::path(path->value));
        }
      }
    }
    std::vector<game_t> result;
    result.reserve(found.size());
    for (auto &[id, game] : found) {
      result.push_back(std::move(game));
    }
    return result;
  }

  std::string launch_uri(std::uint32_t app_id) {
    return app_id == 0 ? std::string {} : "steam://rungameid/" + std::to_string(app_id);
  }

  std::string launch_command(std::uint32_t app_id) {
    if (app_id == 0) {
      return {};
    }
#ifdef _WIN32
    return "cmd /c start \"\" " + launch_uri(app_id);
#elif defined(__APPLE__)
    return "open " + launch_uri(app_id);
#else
    // Send the request directly to Steam. Desktop URI openers can exit
    // successfully even when KDE drops the handoff during an output switch.
    return "steam -applaunch " + std::to_string(app_id);
#endif
  }

  bool launch(std::uint32_t app_id) {
    const auto uri = launch_uri(app_id);
    if (uri.empty()) {
      return false;
    }
#ifdef _WIN32
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", std::filesystem::path(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    // The opener must be detached, but the caller still needs to know whether
    // it was actually executed. A close-on-exec pipe reports only pre-exec
    // failures: successful exec closes the write end and produces EOF.
    int exec_error_pipe[2] {-1, -1};
    if (pipe(exec_error_pipe) != 0 ||
        fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
      if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
      if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
      return false;
    }
    const auto pid = fork();
    if (pid < 0) {
      close(exec_error_pipe[0]);
      close(exec_error_pipe[1]);
      return false;
    }
    if (pid == 0) {
      // Detach the desktop opener so a long-lived xdg-open process cannot
      // become a Sunshine child; the intermediate child is reaped below.
      const auto grandchild = fork();
      if (grandchild > 0) {
        close(exec_error_pipe[0]);
        close(exec_error_pipe[1]);
        _exit(0);
      }
      if (grandchild < 0) {
        const int error = errno;
        close(exec_error_pipe[0]);
        (void) write(exec_error_pipe[1], &error, sizeof(error));
        close(exec_error_pipe[1]);
        _exit(127);
      }
      close(exec_error_pipe[0]);
  #ifdef __APPLE__
      execlp("open", "open", uri.c_str(), static_cast<char *>(nullptr));
  #else
      const auto app_id_string = std::to_string(app_id);
      execlp("steam", "steam", "-applaunch", app_id_string.c_str(), static_cast<char *>(nullptr));
  #endif
      const int error = errno;
      (void) write(exec_error_pipe[1], &error, sizeof(error));
      close(exec_error_pipe[1]);
      _exit(127);
    }
    close(exec_error_pipe[1]);
    int child_status = 0;
    while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
    }
    int exec_error = 0;
    ssize_t received = 0;
    do {
      received = read(exec_error_pipe[0], &exec_error, sizeof(exec_error));
    } while (received < 0 && errno == EINTR);
    close(exec_error_pipe[0]);
    return received == 0 && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
#endif
  }
}  // namespace platf::steam
