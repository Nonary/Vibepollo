/**
 * @file packaging/linux/vibepollo-steam-launch.cpp
 * @brief Unprivileged desktop-session Steam direct-launch resolver.
 *
 * The session broker enters the selected desktop UID before executing this
 * helper. User-owned Steam metadata is therefore never parsed by the
 * capability-bearing machine host or broker.
 */
#include "src/platform/linux/mangohud_policy.h"
#include "src/platform/linux/mangohud_state.h"
#include "src/steam_integration.h"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;

  constexpr std::string_view fixed_path = "/usr/local/bin:/usr/bin:/bin";

  class scoped_metadata_limits final {
  public:
    scoped_metadata_limits() {
      if (getrlimit(RLIMIT_AS, &original_) != 0) {
        return;
      }
      auto limited = original_;
      constexpr rlim_t maximum_address_space = 512ULL * 1024ULL * 1024ULL;
      limited.rlim_cur = original_.rlim_cur == RLIM_INFINITY ?
                           maximum_address_space :
                           std::min(original_.rlim_cur, maximum_address_space);
      if (setrlimit(RLIMIT_AS, &limited) != 0) {
        return;
      }
      armed_ = true;
      alarm(10);
    }

    ~scoped_metadata_limits() {
      if (armed_) {
        alarm(0);
        (void) setrlimit(RLIMIT_AS, &original_);
      }
    }

    [[nodiscard]] bool armed() const { return armed_; }

  private:
    struct rlimit original_ {};
    bool armed_ = false;
  };

  bool parse_u32(std::string_view token, std::uint32_t &value) {
    if (token.empty() || token.front() == '+' || token.front() == '-') {
      return false;
    }
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc {} && parsed.ptr == token.data() + token.size();
  }

  bool safe_environment_value(const char *value, std::size_t maximum) {
    if (!value) {
      return false;
    }
    const auto length = strnlen(value, maximum + 1);
    if (length > maximum) {
      return false;
    }
    return std::none_of(value, value + length, [](unsigned char character) {
      return character < 0x20 || character == 0x7f;
    });
  }

  bool set_environment(std::string_view name, const std::string &value) {
    return setenv(std::string(name).c_str(), value.c_str(), 1) == 0;
  }

  bool install_clean_environment() {
    const uid_t uid = getuid();
    if (uid == 0 || geteuid() != uid || getgid() == 0 || getegid() != getgid()) {
      return false;
    }
    const auto *account = getpwuid(uid);
    if (!account || !account->pw_name || !account->pw_dir ||
        account->pw_dir[0] != '/' || !safe_environment_value(account->pw_name, 64) ||
        !safe_environment_value(account->pw_dir, 4095)) {
      return false;
    }

    const std::string runtime = "/run/user/" + std::to_string(uid);
    struct stat attributes {};
    if (lstat(runtime.c_str(), &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
        attributes.st_uid != uid || (attributes.st_mode & 0777) != 0700) {
      return false;
    }

    const char *wayland_value = std::getenv("WAYLAND_DISPLAY");
    const char *display_value = std::getenv("DISPLAY");
    const char *xauthority_value = std::getenv("XAUTHORITY");
    if (!safe_environment_value(wayland_value, 79) ||
        (display_value && !safe_environment_value(display_value, 79)) ||
        (xauthority_value && (!safe_environment_value(xauthority_value, 4095) ||
                              xauthority_value[0] != '/'))) {
      return false;
    }
    const std::string wayland {wayland_value};
    const std::optional<std::string> display = display_value ?
      std::optional<std::string> {display_value} : std::nullopt;
    const std::optional<std::string> xauthority = xauthority_value ?
      std::optional<std::string> {xauthority_value} : std::nullopt;
    const std::string home {account->pw_dir};
    const std::string user {account->pw_name};

    if (clearenv() != 0) {
      return false;
    }
    return set_environment("HOME", home) &&
           set_environment("USER", user) &&
           set_environment("LOGNAME", user) &&
           set_environment("PATH", std::string(fixed_path)) &&
           set_environment("LANG", "C.UTF-8") &&
           set_environment("SHELL", "/bin/sh") &&
           set_environment("XDG_CONFIG_HOME", home + "/.config") &&
           set_environment("XDG_DATA_HOME", home + "/.local/share") &&
           set_environment("XDG_RUNTIME_DIR", runtime) &&
           set_environment("PIPEWIRE_RUNTIME_DIR", runtime) &&
           set_environment("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + runtime + "/bus") &&
           set_environment("XDG_SESSION_TYPE", "wayland") &&
           set_environment("WAYLAND_DISPLAY", wayland) &&
           (!display || set_environment("DISPLAY", *display)) &&
           (!xauthority || set_environment("XAUTHORITY", *xauthority));
  }

  std::optional<platf::steam::session_launch_policy_t> parse_policy(
    int argc,
    char **argv,
    std::uint32_t &app_id
  ) {
    if (argc != 9 || !parse_u32(argv[1], app_id) || app_id == 0) {
      return std::nullopt;
    }
    platf::steam::session_launch_policy_t policy;
    policy.provider = argv[2];
    if (!parse_u32(argv[3], policy.limit_millihz)) {
      return std::nullopt;
    }
    policy.preset = argv[4];
    if ((std::string_view(argv[5]) != "0" && std::string_view(argv[5]) != "1") ||
        (std::string_view(argv[7]) != "0" && std::string_view(argv[7]) != "1") ||
        (std::string_view(argv[8]) != "0" && std::string_view(argv[8]) != "1")) {
      return std::nullopt;
    }
    policy.always_show_graph = std::string_view(argv[5]) == "1";
    policy.limiter_method = argv[6];
    policy.smooth_motion = std::string_view(argv[7]) == "1";
    policy.smooth_motion_graphics_queue = std::string_view(argv[8]) == "1";
    if (platf::steam::session_launch_command(app_id, policy).empty()) {
      return std::nullopt;
    }
    return policy;
  }

  int launch(std::uint32_t app_id,
             const platf::steam::session_launch_policy_t &policy) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        !install_clean_environment()) {
      std::cerr << "vibepollo-steam-launch: unsafe desktop execution context\n";
      return 126;
    }

    std::vector<platf::steam::game_t> games;
    try {
      scoped_metadata_limits limits;
      if (!limits.armed()) {
        std::cerr << "vibepollo-steam-launch: could not bound metadata parsing\n";
        return 126;
      }
      const auto roots = platf::steam::default_library_roots();
      if (roots.empty()) {
        std::cerr << "vibepollo-steam-launch: Steam installation is unavailable\n";
        return 1;
      }
      games = platf::steam::discover(roots);
    } catch (...) {
      std::cerr << "vibepollo-steam-launch: Steam metadata parsing failed\n";
      return 1;
    }
    const auto game = std::find_if(games.begin(), games.end(), [app_id](const auto &candidate) {
      return candidate.app_id == app_id && candidate.installed;
    });
    if (game == games.end()) {
      std::cerr << "vibepollo-steam-launch: requested AppID is not installed\n";
      return 1;
    }

    const auto command = platf::steam::launch_command(*game);
    if (command.empty() || command == platf::steam::launch_command(app_id)) {
      std::cerr << "vibepollo-steam-launch: direct launch metadata is incomplete\n";
      return 1;
    }

    if (policy.provider == "disabled") {
      platf::mangohud::remove_runtime_state(std::to_string(app_id));
    } else {
      const auto state = platf::mangohud::write_runtime_state(
        std::to_string(app_id),
        policy.provider,
        platf::mangohud::format_limit(policy.limit_millihz),
        policy.preset,
        policy.always_show_graph,
        policy.limiter_method
      );
      if (state.empty()) {
        std::cerr << "vibepollo-steam-launch: could not create limiter state\n";
        return 1;
      }
    }

    if (setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", policy.smooth_motion ? "1" : "", 1) != 0 ||
        setenv("NVPRESENT_QUEUE_FAMILY",
               policy.smooth_motion_graphics_queue ? "1" : "", 1) != 0) {
      std::cerr << "vibepollo-steam-launch: could not install launch policy\n";
      return 1;
    }

    // Steam Launch Options are user-authored shell expressions. They are
    // interpreted only here, after irreversible transition to that same UID.
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    std::cerr << "vibepollo-steam-launch: exec failed: " << std::strerror(errno) << '\n';
    return errno == ENOENT ? 127 : 126;
  }
}  // namespace

int main(int argc, char **argv) {
  std::uint32_t app_id = 0;
  const auto policy = parse_policy(argc, argv, app_id);
  if (!policy) {
    std::cerr << "usage: vibepollo-steam-launch APPID PROVIDER LIMIT_MILLIHZ "
                 "PRESET GRAPH METHOD SMOOTH QUEUE\n";
    return 2;
  }
  return launch(app_id, *policy);
}
