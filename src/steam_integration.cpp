/** @file src/steam_integration.cpp */
#include "steam_integration.h"

#if defined(__linux__)
  #include "provider_scan_protocol.h"
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
  // clang-format off
  // windows.h must precede shellapi.h: the latter relies on its type macros.
  #include <windows.h>
  #include <shellapi.h>
  // clang-format on
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
        // A stray brace cannot form a valid pair. Always consume it so a
        // malformed localconfig cannot stall Steam discovery indefinitely.
        if (lex.pos < lex.s.size() && (lex.s[lex.pos] == '{' || lex.s[lex.pos] == '}')) {
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

#if defined(__linux__)
  bool machine_host_mode() {
    const auto *value = std::getenv("VIBEPOLLO_MACHINE_HOST");
    return value && *value;
  }

  std::shared_ptr<const platf::provider_scan::steam_catalog_t> machine_steam_catalog() {
    static std::mutex mutex;
    static std::shared_ptr<const platf::provider_scan::steam_catalog_t> cached;
    static std::chrono::steady_clock::time_point refreshed_at {};
    static bool initialized = false;
    constexpr auto cache_lifetime = std::chrono::seconds {2};
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock {mutex};
    if (!initialized || now - refreshed_at >= cache_lifetime) {
      auto scanned = platf::provider_scan::scan_steam_session();
      cached = scanned ? std::make_shared<const platf::provider_scan::steam_catalog_t>(std::move(*scanned)) : nullptr;
      refreshed_at = now;
      initialized = true;
    }
    return cached;
  }
#endif

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

  fs::path first_existing(const std::vector<fs::path> &dirs, std::uint32_t app_id, std::initializer_list<const char *> suffixes) {
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

  fs::path first_nested_existing(const std::vector<fs::path> &dirs, std::uint32_t app_id, std::initializer_list<const char *> names) {
    const auto id = std::to_string(app_id);
    for (const auto &dir : dirs) {
      for (const auto *name : names) {
        const auto path = dir / id / name;
        if (regular(path)) {
          return path;
        }
      }
    }
    return {};
  }

  fs::path first_hashed_nested_existing(const std::vector<fs::path> &dirs, std::uint32_t app_id, std::initializer_list<const char *> names) {
    const auto id = std::to_string(app_id);
    for (const auto &dir : dirs) {
      const auto app_dir = dir / id;
      std::error_code ec;
      if (!fs::is_directory(app_dir, ec)) {
        continue;
      }
      std::vector<fs::path> asset_dirs;
      for (const auto &entry : fs::directory_iterator(app_dir, ec)) {
        if (ec) {
          break;
        }
        if (entry.is_directory(ec)) {
          asset_dirs.push_back(entry.path());
        }
      }
      std::sort(asset_dirs.begin(), asset_dirs.end());
      for (const auto &asset_dir : asset_dirs) {
        for (const auto *name : names) {
          const auto path = asset_dir / name;
          if (regular(path)) {
            return path;
          }
        }
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

  artwork_t artwork_for(std::uint32_t app_id, const fs::path &library, const std::vector<fs::path> &steam_roots) {
    std::vector<fs::path> cache_dirs;
    std::vector<fs::path> grid_dirs;
    auto add_root_dirs = [&](const fs::path &root) {
      if (root.empty()) {
        return;
      }
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
    for (const auto &root : steam_roots) {
      add_root_dirs(root);
    }

    // New librarycache artwork is preferred because it is Steam's canonical
    // portrait cover. Steam's _2x asset is the only local variant that is
    // normally 600x900; check every cache directory for it before considering
    // the older 300x450 files. User grid art is a useful fallback for custom
    // artwork.
    artwork_t out;
    out.portrait = first_existing(cache_dirs, app_id, {"_library_600x900_2x.jpg", "_library_600x900_2x.png", "_library_600x900_2x.webp"});
    if (out.portrait.empty()) {
      out.portrait = first_nested_existing(cache_dirs, app_id, {"library_600x900_2x.jpg", "library_600x900_2x.png", "library_600x900_2x.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_existing(cache_dirs, app_id, {"_library_600x900.jpg", "_library_600x900.png", "_library_600x900.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_nested_existing(cache_dirs, app_id, {"library_600x900.jpg", "library_600x900.png", "library_600x900.webp"});
    }
    if (out.portrait.empty()) {
      // Current Steam clients key cached store assets by their content hash:
      // <appid>/<hash>/library_capsule.jpg. The capsule is the same portrait
      // cover as the legacy library_600x900 asset (normally 300x450 locally).
      out.portrait = first_hashed_nested_existing(cache_dirs, app_id, {"library_capsule.jpg", "library_capsule.png", "library_capsule.webp"});
    }
    if (out.portrait.empty()) {
      out.portrait = first_existing(grid_dirs, app_id, {"p.png", "p.jpg", "_p.png", "_p.jpg"});
    }
    out.header = first_existing(cache_dirs, app_id, {"_header.jpg", "_header.png", "_header.webp"});
    if (out.header.empty()) {
      out.header = first_nested_existing(cache_dirs, app_id, {"header.jpg", "header.png", "header.webp"});
    }
    if (out.header.empty()) {
      out.header = first_hashed_nested_existing(cache_dirs, app_id, {"library_header.jpg", "library_header.png", "library_header.webp"});
    }
    if (out.header.empty()) {
      out.header = first_existing(grid_dirs, app_id, {"_hero.png", "_hero.jpg"});
    }
    out.icon = first_existing(cache_dirs, app_id, {"_icon.jpg", "_icon.png", "_icon.webp"});
    if (out.icon.empty()) {
      out.icon = first_nested_existing(cache_dirs, app_id, {"icon.jpg", "icon.png", "icon.webp"});
    }
    if (out.icon.empty()) {
      out.icon = first_existing(grid_dirs, app_id, {"_icon.png", "_icon.jpg", ".png"});
    }
    out.format = !out.portrait.empty() ? out.portrait.extension().string() :
                                         (!out.header.empty() ? out.header.extension().string() :
                                                                (!out.icon.empty() ? out.icon.extension().string() : std::string {}));
    if (!out.format.empty() && out.format.front() == '.') {
      out.format.erase(0, 1);
    }
    return out;
  }

  std::uint32_t read_u32(const std::vector<std::uint8_t> &data, std::size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Steam appinfo u32");
    }
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
  }

  std::uint64_t read_u64(const std::vector<std::uint8_t> &data, std::size_t offset) {
    if (offset + 8 > data.size()) {
      throw std::out_of_range("Steam appinfo u64");
    }
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(data[offset++]) << shift;
    }
    return value;
  }

  std::string read_cstring(const std::vector<std::uint8_t> &data, std::size_t &offset, std::size_t end) {
    const auto begin = offset;
    while (offset < end && data[offset] != 0) {
      ++offset;
    }
    if (offset >= end) {
      throw std::out_of_range("Steam appinfo string");
    }
    std::string value(reinterpret_cast<const char *>(data.data() + begin), offset - begin);
    ++offset;
    return value;
  }

  vdf_node parse_binary_vdf(
    const std::vector<std::uint8_t> &data,
    std::size_t &offset,
    std::size_t end,
    const std::vector<std::string> &keys
  ) {
    vdf_node result;
    while (offset < end) {
      const auto type = data[offset++];
      if (type == 0x08) {
        return result;
      }
      const auto key_index = read_u32(data, offset);
      offset += 4;
      if (key_index >= keys.size()) {
        throw std::out_of_range("Steam appinfo key");
      }
      vdf_node value;
      switch (type) {
        case 0x00:
          value = parse_binary_vdf(data, offset, end, keys);
          break;
        case 0x01:
          value.value = read_cstring(data, offset, end);
          break;
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x06:
          value.value = std::to_string(read_u32(data, offset));
          offset += 4;
          break;
        case 0x05:
          {
            const auto begin = offset;
            while (offset + 1 < end && (data[offset] != 0 || data[offset + 1] != 0)) {
              offset += 2;
            }
            if (offset + 1 >= end) {
              throw std::out_of_range("Steam appinfo wide string");
            }
            // Launch metadata is UTF-8. Preserve a readable ASCII subset for
            // the uncommon wide-string values without adding a codec dependency.
            for (auto cursor = begin; cursor < offset; cursor += 2) {
              value.value.push_back(data[cursor] < 0x80 && data[cursor + 1] == 0 ? static_cast<char>(data[cursor]) : '?');
            }
            offset += 2;
            break;
          }
        case 0x07:
        case 0x0a:
          value.value = std::to_string(read_u64(data, offset));
          offset += 8;
          break;
        default:
          throw std::runtime_error("Unsupported Steam appinfo value type");
      }
      result.children.emplace_back(keys[key_index], std::move(value));
    }
    throw std::out_of_range("Unterminated Steam appinfo object");
  }

  std::string normalized_os(const vdf_node &launch) {
    const auto *config = launch.find("config");
    const auto *os = config ? config->find("oslist") : nullptr;
    auto value = os ? os->value : std::string {};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (value.find("linux") != std::string::npos) {
      return "linux";
    }
    if (value.empty() || value.find("windows") != std::string::npos) {
      return "windows";
    }
    return {};
  }

  void apply_launch_node(platf::steam::game_t &game, const vdf_node &appinfo) {
    const auto *config = appinfo.find("config");
    const auto *launches = config ? config->find("launch") : nullptr;
    if (!launches) {
      return;
    }

    const vdf_node *selected = nullptr;
    int selected_score = -1;
    for (const auto &[index, launch] : launches->children) {
      const auto *executable = launch.find("executable");
      const auto os = normalized_os(launch);
      if (!executable || executable->value.empty() || os.empty()) {
        continue;
      }
      int score = os == "linux" ? 200 : 100;
      if (index == "0") {
        score += 20;
      }
      if (const auto *type = launch.find("type"); type && (type->value == "default" || type->value == "none")) {
        score += 10;
      }
      if (score > selected_score) {
        selected = &launch;
        selected_score = score;
      }
    }
    if (!selected) {
      return;
    }

    auto relative = selected->find("executable")->value;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    game.launch_executable = game.install_dir / relative;
    game.launch_os = normalized_os(*selected);
    if (const auto *arguments = selected->find("arguments")) {
      game.launch_arguments = arguments->value;
    }
    if (const auto *working = selected->find("workingdir"); working && !working->value.empty()) {
      auto directory = working->value;
      std::replace(directory.begin(), directory.end(), '\\', '/');
      game.launch_working_dir = game.install_dir / directory;
    } else {
      game.launch_working_dir = game.launch_executable.parent_path();
    }
  }

  void apply_common_node(platf::steam::game_t &game, const vdf_node &appinfo) {
    const auto *common = appinfo.find("common");
    if (!common) {
      return;
    }
    if (const auto *name = common->find("name"); name && !name->value.empty()) {
      game.name = name->value;
    }
    if (const auto *type = common->find("type"); type && !type->value.empty()) {
      game.app_type = type->value;
    }
  }

  void enrich_from_appinfo(const fs::path &path, std::map<std::uint32_t, platf::steam::game_t> &games) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), {});
    if (data.size() < 20 || read_u32(data, 0) != 0x07564429) {
      return;
    }
    const auto string_table_offset = read_u64(data, 8);
    if (string_table_offset > data.size() - 4) {
      return;
    }

    std::size_t strings_offset = static_cast<std::size_t>(string_table_offset);
    const auto string_count = read_u32(data, strings_offset);
    strings_offset += 4;
    if (string_count > data.size() - strings_offset) {
      return;
    }
    std::vector<std::string> keys;
    keys.reserve(string_count);
    for (std::uint32_t i = 0; i < string_count; ++i) {
      keys.push_back(read_cstring(data, strings_offset, data.size()));
    }

    std::size_t offset = 16;
    while (offset + 4 <= string_table_offset) {
      const auto app_id = read_u32(data, offset);
      if (app_id == 0) {
        break;
      }
      if (offset + 68 > string_table_offset) {
        return;
      }
      const auto entry_size = read_u32(data, offset + 4);
      if (entry_size > string_table_offset - offset - 8) {
        return;
      }
      const auto entry_end = offset + 8 + static_cast<std::size_t>(entry_size);
      const auto value_offset = offset + 68;
      if (entry_size < 60 || entry_end > string_table_offset || value_offset > entry_end) {
        return;
      }
      if (const auto found = games.find(app_id); found != games.end()) {
        try {
          auto cursor = value_offset;
          const auto root = parse_binary_vdf(data, cursor, entry_end, keys);
          const auto *appinfo = root.find("appinfo");
          const auto &metadata = appinfo ? *appinfo : root;
          apply_common_node(found->second, metadata);
          apply_launch_node(found->second, metadata);
        } catch (...) {
          // A single malformed/cache-version entry must not suppress the rest
          // of Steam discovery; unresolved games keep the broker fallback.
        }
      }
      offset = entry_end;
    }
  }

  void apply_user_metadata(const fs::path &steam_root, std::map<std::uint32_t, platf::steam::game_t> &games, bool include_history) {
    std::error_code ec;
    const auto userdata = steam_root / "userdata";
    if (!fs::is_directory(userdata, ec)) {
      return;
    }
    for (const auto &user : fs::directory_iterator(userdata, ec)) {
      if (ec || !user.is_directory(ec)) {
        continue;
      }
      std::ifstream input(user.path() / "config/localconfig.vdf");
      if (!input) {
        continue;
      }
      std::stringstream buffer;
      buffer << input.rdbuf();
      const auto doc = platf::steam::parse_vdf(buffer.str());
      const vdf_node *apps = doc.find("UserLocalConfigStore");
      apps = apps ? apps->find("Software") : nullptr;
      apps = apps ? apps->find("Valve") : nullptr;
      apps = apps ? apps->find("Steam") : nullptr;
      apps = apps ? apps->find("apps") : nullptr;
      if (!apps) {
        continue;
      }
      for (const auto &[id_text, app] : apps->children) {
        std::uint64_t id = 0;
        try {
          std::size_t used = 0;
          id = std::stoull(id_text, &used, 10);
          if (used != id_text.size() || id == 0 || id > UINT32_MAX) {
            continue;
          }
        } catch (...) {
          continue;
        }
        const auto last_played = number(app.find("LastPlayed")).value_or(0);
        const auto playtime = number(app.find("Playtime")).value_or(0);
        auto found = games.find(static_cast<std::uint32_t>(id));
        if (found == games.end()) {
          if (!include_history || (last_played == 0 && playtime == 0)) {
            continue;
          }
          platf::steam::game_t game;
          game.app_id = static_cast<std::uint32_t>(id);
          game.stable_id = "steam:" + id_text;
          found = games.emplace(game.app_id, std::move(game)).first;
        }
        auto &game = found->second;
        game.last_played = std::max(game.last_played, last_played);
        game.playtime_minutes = std::max(game.playtime_minutes, playtime);
        if (const auto *options = app.find("LaunchOptions"); options && !options->value.empty()) {
          game.launch_options = options->value;
        }
      }
    }
  }

  [[maybe_unused]] void apply_proton_metadata(platf::steam::game_t &game) {
    if (game.launch_os != "windows") {
      return;
    }
    game.compatdata_path = game.library_path / "steamapps" / "compatdata" / std::to_string(game.app_id);
    std::ifstream input(game.compatdata_path / "config_info");
    std::string version;
    std::string files_path;
    std::string library_path;
    std::string steam_client_path;
    if (!std::getline(input, version) || !std::getline(input, files_path)) {
      return;
    }
    std::getline(input, library_path);
    std::getline(input, steam_client_path);
    const auto marker = files_path.find("/files/");
    if (marker == std::string::npos) {
      return;
    }
    game.proton_path = files_path.substr(0, marker);

    fs::path steamapps;
    std::error_code ec;
    if (!steam_client_path.empty() && fs::is_directory(fs::path(steam_client_path) / "steamapps", ec)) {
      game.steam_client_path = steam_client_path;
      steamapps = game.steam_client_path / "steamapps";
    } else {
      steamapps = game.proton_path;
      while (!steamapps.empty() && steamapps.filename() != "steamapps") {
        const auto parent = steamapps.parent_path();
        if (parent == steamapps) {
          steamapps.clear();
          break;
        }
        steamapps = parent;
      }
      if (steamapps.empty() || steamapps.filename() != "steamapps") {
        return;
      }
      game.steam_client_path = steamapps.parent_path();
    }

    std::ifstream tool_manifest(game.proton_path / "toolmanifest.vdf");
    if (!tool_manifest) {
      return;
    }
    std::stringstream tool_buffer;
    tool_buffer << tool_manifest.rdbuf();
    const auto tool_doc = platf::steam::parse_vdf(tool_buffer.str());
    const auto *manifest = tool_doc.find("manifest");
    const auto *required = manifest ? manifest->find("require_tool_appid") : nullptr;
    if (!required || required->value.empty()) {
      return;
    }

    const auto required_id = number(required);
    if (!required_id || *required_id == 0 || *required_id > UINT32_MAX) {
      return;
    }
    const auto required_text = std::to_string(*required_id);

    // Steam can install the pressure-vessel runtime beside the game even when
    // a compatibility tool records a different client root. Prefer that
    // authoritative game library, then the client library. This is the layout
    // used by current UMU-Proton installs and avoids silently falling back to
    // an already-running Steam process that cannot inherit the stream limit.
    std::vector<fs::path> runtime_roots {
      game.library_path / "steamapps",
      steamapps,
    };
    std::unordered_set<std::string> visited;
    for (const auto &runtime_steamapps : runtime_roots) {
      ec.clear();
      const auto canonical = fs::weakly_canonical(runtime_steamapps, ec).generic_string();
      if (ec || !visited.insert(canonical).second) {
        continue;
      }
      std::ifstream runtime_manifest(
        runtime_steamapps / ("appmanifest_" + required_text + ".acf")
      );
      if (!runtime_manifest) {
        continue;
      }
      std::stringstream runtime_buffer;
      runtime_buffer << runtime_manifest.rdbuf();
      const auto runtime_doc = platf::steam::parse_vdf(runtime_buffer.str());
      const auto *app_state = runtime_doc.find("AppState");
      const auto app_id = app_state ? number(app_state->find("appid")) : std::nullopt;
      const auto *install_dir = app_state ? app_state->find("installdir") : nullptr;
      if (!app_id || *app_id != *required_id || !install_dir ||
          install_dir->value.empty() || install_dir->value == "." ||
          install_dir->value == ".." || install_dir->value.size() > 255 ||
          install_dir->value.find('/') != std::string::npos ||
          install_dir->value.find('\\') != std::string::npos) {
        continue;
      }
      const auto runtime = runtime_steamapps / "common" / install_dir->value;
      ec.clear();
      if (fs::is_regular_file(runtime / "_v2-entry-point", ec)) {
        game.proton_runtime_path = runtime;
        break;
      }
    }
  }

  [[maybe_unused]] std::string shell_quote(std::string_view value) {
    std::string result = "'";
    for (const char ch : value) {
      if (ch == '\'') {
        result += "'\\''";
      } else {
        result.push_back(ch);
      }
    }
    result.push_back('\'');
    return result;
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
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, nullptr, &steam_path_size) == ERROR_SUCCESS && steam_path_size > sizeof(wchar_t)) {
      std::vector<wchar_t> steam_path(steam_path_size / sizeof(wchar_t));
      if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam_path.data(), &steam_path_size) == ERROR_SUCCESS) {
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
    const bool machine_host = machine_host_mode();
    // The CAP_SYS_ADMIN machine daemon must not parse desktop-user-controlled
    // provider data. A session-side scanner can populate the machine catalog;
    // until then, retain the already migrated catalog and expose no home roots.
    if (machine_host) {
      return roots;
    }
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

  bool available() {
#if defined(__linux__)
    if (machine_host_mode()) {
      const auto catalog = machine_steam_catalog();
      return catalog && catalog->available;
    }
#endif
    return !default_library_roots().empty();
  }

  std::vector<game_t> discover(const std::vector<fs::path> &requested_roots) {
#if defined(__linux__)
    if (machine_host_mode()) {
      const auto catalog = machine_steam_catalog();
      if (!catalog || !catalog->available) return {};
      std::vector<game_t> installed;
      std::copy_if(catalog->games.begin(), catalog->games.end(), std::back_inserter(installed), [](const auto &game) {
        return game.installed;
      });
      return installed;
    }
#endif
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
        game.installed = true;
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
          if (candidate.portrait_path.empty()) {
            candidate.portrait_path = other.portrait_path;
          }
          if (candidate.boxart_path.empty()) {
            candidate.boxart_path = other.boxart_path;
          }
          if (candidate.artwork_path.empty()) {
            candidate.artwork_path = other.artwork_path;
          }
          if (candidate.artwork_format.empty()) {
            candidate.artwork_format = other.artwork_format;
          }
          if (candidate.header_path.empty()) {
            candidate.header_path = other.header_path;
          }
          if (candidate.icon_path.empty()) {
            candidate.icon_path = other.icon_path;
          }
          if (candidate.app_type.empty()) {
            candidate.app_type = other.app_type;
          }
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
#ifndef _WIN32
    std::unordered_set<std::string> metadata_roots;
    for (const auto &root : roots) {
      const auto apps = steamapps_for(root);
      const auto steam_root = apps.empty() ? root : apps.parent_path();
      std::error_code ec;
      const auto key = fs::weakly_canonical(steam_root, ec).generic_string();
      if (!ec && metadata_roots.insert(key).second) {
        try {
          enrich_from_appinfo(steam_root / "appcache/appinfo.vdf", found);
          apply_user_metadata(steam_root, found, false);
        } catch (...) {
          // Local launch metadata is an optimization. Steam's broker remains
          // the safe fallback for an unreadable or newly-versioned cache.
        }
      }
    }
    for (auto &[app_id, game] : found) {
      (void) app_id;
      apply_proton_metadata(game);
    }
#endif
    std::vector<game_t> result;
    result.reserve(found.size());
    for (auto &[id, game] : found) {
      result.push_back(std::move(game));
    }
    return result;
  }

  std::vector<game_t> discover_catalog(const std::vector<fs::path> &requested_roots) {
#if defined(__linux__)
    if (machine_host_mode()) {
      const auto catalog = machine_steam_catalog();
      return catalog && catalog->available ? catalog->games : std::vector<game_t> {};
    }
#endif
    auto roots = requested_roots;
    if (roots.empty()) {
      roots = default_library_roots();
    }
    auto installed = discover(roots);
    std::map<std::uint32_t, game_t> found;
    for (auto &game : installed) {
      found.emplace(game.app_id, std::move(game));
    }

    std::unordered_set<std::string> metadata_roots;
    for (const auto &root : roots) {
      const auto apps = steamapps_for(root);
      const auto steam_root = apps.empty() ? root : apps.parent_path();
      std::error_code ec;
      const auto key = fs::weakly_canonical(steam_root, ec).generic_string();
      if (ec || !metadata_roots.insert(key).second) {
        continue;
      }
      try {
        apply_user_metadata(steam_root, found, true);
        enrich_from_appinfo(steam_root / "appcache/appinfo.vdf", found);
      } catch (...) {
        // Preserve installed discovery if Steam is rewriting either cache.
      }
    }

    std::vector<game_t> result;
    for (auto &[id, game] : found) {
      (void) id;
      if (game.name.empty()) {
        continue;
      }
      if (!game.installed) {
        const auto art = artwork_for(game.app_id, {}, roots);
        game.portrait_path = art.portrait;
        game.boxart_path = art.portrait;
        game.artwork_path = !art.portrait.empty() ? art.portrait : (!art.header.empty() ? art.header : art.icon);
        game.artwork_format = art.format;
        game.icon_path = art.icon;
        game.header_path = art.header;
      }
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
  #if defined(__linux__)
    // The machine host has no desktop of its own; the session broker runs
    // `steam -applaunch` inside the selected desktop session for it.
    if (machine_host_mode()) {
      return "/usr/libexec/vibeshine/vibepollo-session-exec steam " + std::to_string(app_id);
    }
  #endif
    // Send the request directly to Steam. Desktop URI openers can exit
    // successfully even when KDE drops the handoff during an output switch.
    return "steam -applaunch " + std::to_string(app_id);
#endif
  }

#ifdef __linux__
  namespace {
    constexpr std::string_view session_exec_path =
      "/usr/libexec/vibeshine/vibepollo-session-exec";

    bool valid_session_launch_policy(const session_launch_policy_t &policy) {
      const bool overlay = policy.provider == "mangohud" ||
                           policy.provider == "mangohud-proton";
      const bool limited = overlay || policy.provider == "proton";
      if ((!limited && policy.provider != "disabled") ||
          (limited && (policy.limit_millihz < 1000 ||
                       policy.limit_millihz > 1000000)) ||
          (!limited && policy.limit_millihz != 0) ||
          (policy.preset != "custom" && policy.preset != "1" &&
           policy.preset != "2" && policy.preset != "3" &&
           policy.preset != "4") ||
          (!overlay && (policy.preset != "custom" ||
                        policy.always_show_graph)) ||
          (policy.limiter_method != "early" &&
           policy.limiter_method != "late") ||
          (policy.provider != "mangohud" &&
           policy.limiter_method != "late") ||
          (!policy.smooth_motion && policy.smooth_motion_graphics_queue)) {
        return false;
      }
      return limited || policy.smooth_motion;
    }

    bool parse_u32_token(std::string_view token, std::uint32_t &value) {
      if (token.empty() || token.front() == '+' || token.front() == '-') {
        return false;
      }
      const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
      return parsed.ec == std::errc {} && parsed.ptr == token.data() + token.size();
    }
  }  // namespace

  std::string session_launch_command(std::uint32_t app_id,
                                     const session_launch_policy_t &policy) {
    if (app_id == 0 || !valid_session_launch_policy(policy)) {
      return {};
    }
    return std::string(session_exec_path) + " steam-direct " +
           std::to_string(app_id) + " " + policy.provider + " " +
           std::to_string(policy.limit_millihz) + " " + policy.preset + " " +
           (policy.always_show_graph ? "1" : "0") + " " +
           policy.limiter_method + " " + (policy.smooth_motion ? "1" : "0") +
           " " + (policy.smooth_motion_graphics_queue ? "1" : "0");
  }

  std::optional<std::vector<std::string>> session_launch_arguments(
    std::string_view command
  ) {
    std::vector<std::string> tokens;
    std::size_t offset = 0;
    while (offset < command.size()) {
      const auto separator = command.find(' ', offset);
      const auto token = command.substr(
        offset,
        separator == std::string_view::npos ? command.size() - offset :
                                               separator - offset
      );
      if (token.empty()) {
        return std::nullopt;
      }
      tokens.emplace_back(token);
      if (separator == std::string_view::npos) {
        break;
      }
      offset = separator + 1;
    }
    // The client fallback is also semantic. Otherwise run_command would
    // wrap this trusted helper invocation in the generic app verb.
    if (tokens.size() == 3 && tokens[0] == session_exec_path && tokens[1] == "steam") {
      std::uint32_t app_id = 0;
      if (!parse_u32_token(tokens[2], app_id) || app_id == 0 ||
          command != std::string(session_exec_path) + " steam " + std::to_string(app_id)) {
        return std::nullopt;
      }
      tokens.erase(tokens.begin());
      return tokens;
    }
    if (tokens.size() != 10 || tokens[0] != session_exec_path ||
        tokens[1] != "steam-direct") {
      return std::nullopt;
    }

    std::uint32_t app_id = 0;
    session_launch_policy_t policy;
    if (!parse_u32_token(tokens[2], app_id) || app_id == 0 ||
        !parse_u32_token(tokens[4], policy.limit_millihz) ||
        (tokens[6] != "0" && tokens[6] != "1") ||
        (tokens[8] != "0" && tokens[8] != "1") ||
        (tokens[9] != "0" && tokens[9] != "1")) {
      return std::nullopt;
    }
    policy.provider = tokens[3];
    policy.preset = tokens[5];
    policy.always_show_graph = tokens[6] == "1";
    policy.limiter_method = tokens[7];
    policy.smooth_motion = tokens[8] == "1";
    policy.smooth_motion_graphics_queue = tokens[9] == "1";
    if (session_launch_command(app_id, policy) != command) {
      return std::nullopt;
    }
    tokens.erase(tokens.begin());
    return tokens;
  }
#endif

  std::string runtime_launch_command(std::string_view app_id, const std::string &configured_command, bool gamescope_session) {
    if (!gamescope_session || app_id.empty()) {
      return configured_command;
    }
    std::uint32_t id = 0;
    const auto parsed = std::from_chars(app_id.data(), app_id.data() + app_id.size(), id);
    if (parsed.ec != std::errc {} || parsed.ptr != app_id.data() + app_id.size() || id == 0) {
      return {};
    }
    return launch_command(id);
  }

  std::string launch_command(const game_t &game) {
#ifdef __linux__
    if (game.app_id == 0 || game.launch_executable.empty()) {
      return launch_command(game.app_id);
    }

    std::string wrapper = "/usr/bin/vibepollo-mangohud";
    // Relocatable installations ship the helper beside the running host.
    // Resolve the executable itself so switching a bundle's "current"
    // symlink cannot send an older host through a different release's helper.
    std::error_code wrapper_error;
    const auto executable = fs::read_symlink("/proc/self/exe", wrapper_error);
    if (!wrapper_error) {
      const auto bundled_wrapper = executable.parent_path() / "vibepollo-mangohud";
      if (bundled_wrapper != wrapper && fs::is_regular_file(bundled_wrapper, wrapper_error) &&
          access(bundled_wrapper.c_str(), X_OK) == 0) {
        wrapper = shell_quote(bundled_wrapper.generic_string());
      }
    }
    std::string command = wrapper + " --appid " + std::to_string(game.app_id) + " -- env ";
    command += "SteamAppId=" + std::to_string(game.app_id) + " ";
    command += "SteamGameId=" + std::to_string(game.app_id) + " ";
    if (game.launch_os == "windows") {
      if (game.compatdata_path.empty() || game.proton_path.empty() || game.proton_runtime_path.empty() || game.steam_client_path.empty()) {
        return launch_command(game.app_id);
      }
      const auto app_id = std::to_string(game.app_id);
      const auto steamapps = game.library_path / "steamapps";
      const auto client_steamapps = game.steam_client_path / "steamapps";
      const auto shader_cache = steamapps / "shadercache" / app_id;
      std::string library_paths = steamapps.generic_string();
      if (client_steamapps != steamapps) {
        library_paths += ":" + client_steamapps.generic_string();
      }
      const auto tool_paths = game.proton_path.generic_string() + ":" + game.proton_runtime_path.generic_string();
      command += "STEAM_COMPAT_APP_ID=" + app_id + " SteamOverlayGameId=" + app_id + " ";
      command += "STEAM_COMPAT_DATA_PATH=" + shell_quote(game.compatdata_path.generic_string()) + " ";
      command += "STEAM_COMPAT_CLIENT_INSTALL_PATH=" + shell_quote(game.steam_client_path.generic_string()) + " ";
      command += "STEAM_COMPAT_INSTALL_PATH=" + shell_quote(game.install_dir.generic_string()) + " ";
      command += "STEAM_COMPAT_LIBRARY_PATHS=" + shell_quote(library_paths) + " ";
      command += "STEAM_COMPAT_TOOL_PATHS=" + shell_quote(tool_paths) + " ";
      command += "STEAM_COMPAT_MOUNTS=" + shell_quote(tool_paths) + " ";
      command += "STEAM_COMPAT_SHADER_PATH=" + shell_quote(shader_cache.generic_string()) + " ";
      command += "STEAM_COMPAT_MEDIA_PATH=" + shell_quote((shader_cache / "fozmediav1").generic_string()) + " ";
      command += "STEAM_COMPAT_TRANSCODED_MEDIA_PATH=" + shell_quote(shader_cache.generic_string()) + " ";
      command += "DXVK_STATE_CACHE_PATH=" + shell_quote((shader_cache / "DXVK_state_cache").generic_string()) + " ";
      command += "__GL_SHADER_DISK_CACHE_PATH=" + shell_quote((shader_cache / "nvidiav1").generic_string()) + " ";
      command += "__GL_SHADER_DISK_CACHE_APP_NAME=steamapp_shader_cache ";
      command += "__GL_SHADER_DISK_CACHE_READ_ONLY_APP_NAME=" + shell_quote("steam_shader_cache;steamapp_merged_shader_cache") + " ";
      command += "__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 ";
      command += shell_quote((game.proton_runtime_path / "_v2-entry-point").generic_string());
      command += " --verb=waitforexitandrun -- ";
      command += shell_quote((game.proton_path / "proton").generic_string());
      command += " waitforexitandrun " + shell_quote(game.launch_executable.generic_string());
    } else {
      command += shell_quote(game.launch_executable.generic_string());
    }
    if (!game.launch_arguments.empty()) {
      command.push_back(' ');
      command += game.launch_arguments;
    }

    std::string shell_command;
    if (game.launch_options.empty()) {
      shell_command = std::move(command);
    } else {
      shell_command = game.launch_options;
      constexpr std::string_view placeholder = "%command%";
      if (const auto position = shell_command.find(placeholder); position != std::string::npos) {
        shell_command.replace(position, placeholder.size(), command);
      } else {
        shell_command = command + " " + shell_command;
      }
    }
    // Steam Launch Options are shell expressions, not argv fragments. They
    // commonly begin with environment assignments and may contain wrapper
    // commands or operators, so preserve those semantics explicitly.
    return "/bin/sh -c " + shell_quote(shell_command);
#else
    return launch_command(game.app_id);
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
    if (pipe(exec_error_pipe) != 0 || fcntl(exec_error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
      if (exec_error_pipe[0] >= 0) {
        close(exec_error_pipe[0]);
      }
      if (exec_error_pipe[1] >= 0) {
        close(exec_error_pipe[1]);
      }
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
        const auto ignored = write(exec_error_pipe[1], &error, sizeof(error));
        (void) ignored;
        close(exec_error_pipe[1]);
        _exit(127);
      }
      close(exec_error_pipe[0]);
  #ifdef __APPLE__
      execlp("open", "open", uri.c_str(), static_cast<char *>(nullptr));
  #else
      const auto app_id_string = std::to_string(app_id);
      if (std::getenv("VIBEPOLLO_MACHINE_HOST")) {
        execl("/usr/libexec/vibeshine/vibepollo-session-exec", "vibepollo-session-exec", "steam", app_id_string.c_str(), static_cast<char *>(nullptr));
      } else {
        execlp("steam", "steam", "-applaunch", app_id_string.c_str(), static_cast<char *>(nullptr));
      }
  #endif
      const int error = errno;
      const auto ignored = write(exec_error_pipe[1], &error, sizeof(error));
      (void) ignored;
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
