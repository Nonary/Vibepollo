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
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <vector>

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

  // The session broker discards this helper's stderr, so mirror every
  // diagnostic into the journal where `journalctl -t vibepollo-steam-launch`
  // can find it.
  void report(int priority, const std::string &message) {
    std::cerr << "vibepollo-steam-launch: " << message << '\n';
    syslog(priority, "%s", message.c_str());
  }

  fs::path steam_state_directory() {
    const char *home = std::getenv("HOME");
    return home && *home ? fs::path(home) / ".steam" : fs::path {};
  }

  // Steam records its client PID in ~/.steam/steam.pid and opens
  // ~/.steam/steam.pipe once it accepts commands. A stale pid file is the
  // normal state after the client crashed or the machine slept.
  bool steam_client_running(const fs::path &state) {
    if (state.empty()) {
      return false;
    }
    std::ifstream input(state / "steam.pid");
    std::string token;
    if (!(input >> token)) {
      return false;
    }
    std::uint32_t pid = 0;
    if (!parse_u32(token, pid) || pid == 0 || kill(static_cast<pid_t>(pid), 0) != 0) {
      return false;
    }
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline");
    std::string command((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    return command.find("steam") != std::string::npos;
  }

  // Run a small helper with a bounded lifetime and report whether its stdout
  // contained `needle`. Used for D-Bus name probes; never for anything the
  // user controls.
  bool helper_output_contains(const char *path, char *const argv[], std::string_view needle) {
    int output_pipe[2] = {-1, -1};
    if (pipe2(output_pipe, O_CLOEXEC) != 0) {
      return false;
    }
    const pid_t child = fork();
    if (child < 0) {
      close(output_pipe[0]);
      close(output_pipe[1]);
      return false;
    }
    if (child == 0) {
      const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
      if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
          dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) {
        _exit(126);
      }
      alarm(10);
      execv(path, argv);
      _exit(127);
    }
    close(output_pipe[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
      const ssize_t received = read(output_pipe[0], buffer, sizeof(buffer));
      if (received <= 0) {
        break;
      }
      if (output.size() < 1 << 20) {
        output.append(buffer, static_cast<std::size_t>(received));
      }
    }
    close(output_pipe[0]);
    int status = 0;
    (void) waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && output.find(needle) != std::string::npos;
  }

  // Proton runs games "alongside Steam" through this session-bus service,
  // which the client registers only once it has finished bootstrapping. A
  // stale steam.pipe left by a crashed client must not count as ready.
  bool steam_client_ready(const fs::path &state) {
    if (!steam_client_running(state)) {
      return false;
    }
    char *const argv[] = {
      const_cast<char *>("busctl"), const_cast<char *>("--user"), const_cast<char *>("--no-pager"),
      const_cast<char *>("--no-legend"), const_cast<char *>("list"), nullptr
    };
    return helper_output_contains("/usr/bin/busctl", argv, "com.steampowered.PressureVessel.LaunchAlongsideSteam");
  }

  // Start the client in its own transient user unit. Launching it from this
  // process would place it in the game's unit, and systemd kills that whole
  // control group the moment the game exits, taking Steam down with it.
  bool start_steam_client() {
    const std::string unit = "vibepollo-steam-client-" + std::to_string(::time(nullptr));
    std::vector<std::string> arguments = {
      "systemd-run", "--user", "--quiet", "--collect",
      "--unit=" + unit,
      "--description=Steam client started by Vibepollo",
      "--property=KillMode=process"
    };
    for (const char *name : {"HOME", "USER", "LOGNAME", "PATH", "LANG", "XDG_RUNTIME_DIR",
                             "XDG_CONFIG_HOME", "XDG_DATA_HOME", "DBUS_SESSION_BUS_ADDRESS",
                             "XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"}) {
      if (const char *value = std::getenv(name)) {
        arguments.push_back(std::string("--setenv=") + name + "=" + value);
      }
    }
    arguments.emplace_back("/usr/bin/steam");
    arguments.emplace_back("-silent");
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
      return false;
    }
    if (child == 0) {
      const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
      if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 ||
          dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) {
        _exit(126);
      }
      alarm(15);
      execv("/usr/bin/systemd-run", argv.data());
      _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
      return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  void sleep_milliseconds(long milliseconds) {
    timespec interval {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
  }

  // Proton titles need a live Steam client for Steamworks; without one the
  // game exits within seconds and nothing explains why. Bring the client up
  // first and give it a bounded time to become ready.
  void ensure_steam_client() {
    const auto state = steam_state_directory();
    if (state.empty()) {
      return;
    }
    if (steam_client_ready(state)) {
      return;
    }
    if (steam_client_running(state)) {
      report(LOG_INFO, "Steam client is starting; waiting for it to accept commands");
    } else {
      report(LOG_WARNING, "Steam client is not running; starting it before the direct launch");
      if (!start_steam_client()) {
        report(LOG_ERR, "could not start the Steam client; launching anyway");
        return;
      }
    }
    constexpr int ready_timeout_ms = 90000;
    constexpr int poll_ms = 500;
    for (int waited = 0; waited < ready_timeout_ms; waited += poll_ms) {
      sleep_milliseconds(poll_ms);
      if (steam_client_ready(state)) {
        report(LOG_INFO, "Steam client is ready after " + std::to_string(waited + poll_ms) + " ms");
        sleep_milliseconds(2000);
        return;
      }
    }
    report(LOG_WARNING, "Steam client did not signal readiness within " +
                          std::to_string(ready_timeout_ms / 1000) + " s; launching anyway");
  }

  int launch(std::uint32_t app_id,
             const platf::steam::session_launch_policy_t &policy) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        !install_clean_environment()) {
      report(LOG_ERR, "unsafe desktop execution context");
      return 126;
    }

    std::vector<platf::steam::game_t> games;
    try {
      scoped_metadata_limits limits;
      if (!limits.armed()) {
        report(LOG_ERR, "could not bound metadata parsing");
        return 126;
      }
      const auto roots = platf::steam::default_library_roots();
      if (roots.empty()) {
        report(LOG_ERR, "Steam installation is unavailable");
        return 1;
      }
      games = platf::steam::discover(roots);
    } catch (...) {
      report(LOG_ERR, "Steam metadata parsing failed");
      return 1;
    }
    const auto game = std::find_if(games.begin(), games.end(), [app_id](const auto &candidate) {
      return candidate.app_id == app_id && candidate.installed;
    });
    if (game == games.end()) {
      report(LOG_ERR, "requested AppID is not installed");
      return 1;
    }

    const auto command = platf::steam::launch_command(*game);
    if (command.empty() || command == platf::steam::launch_command(app_id)) {
      report(LOG_ERR, "direct launch metadata is incomplete");
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
        report(LOG_ERR, "could not create limiter state");
        return 1;
      }
    }

    if (setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", policy.smooth_motion ? "1" : "", 1) != 0 ||
        setenv("NVPRESENT_QUEUE_FAMILY",
               policy.smooth_motion_graphics_queue ? "1" : "", 1) != 0) {
      report(LOG_ERR, "could not install launch policy");
      return 1;
    }

    ensure_steam_client();

    // Steam Launch Options are user-authored shell expressions. They are
    // interpreted only here, after irreversible transition to that same UID.
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    report(LOG_ERR, std::string("exec failed: ") + std::strerror(errno));
    return errno == ENOENT ? 127 : 126;
  }
}  // namespace

int main(int argc, char **argv) {
  openlog("vibepollo-steam-launch", LOG_PID, LOG_USER);
  std::uint32_t app_id = 0;
  const auto policy = parse_policy(argc, argv, app_id);
  if (!policy) {
    std::cerr << "usage: vibepollo-steam-launch APPID PROVIDER LIMIT_MILLIHZ "
                 "PRESET GRAPH METHOD SMOOTH QUEUE\n";
    return 2;
  }
  return launch(app_id, *policy);
}
