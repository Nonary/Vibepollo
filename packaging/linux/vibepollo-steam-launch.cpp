/**
 * @file packaging/linux/vibepollo-steam-launch.cpp
 * @brief Unprivileged desktop-session Steam direct-launch resolver.
 *
 * The session broker enters the selected desktop UID before executing this
 * helper. User-owned Steam metadata is therefore never parsed by the
 * capability-bearing machine host or broker.
 */
#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include "src/platform/linux/mangohud_policy.h"
#include "src/platform/linux/mangohud_state.h"
#include "src/steam_integration.h"
#include "src/steam_big_picture_policy.h"

#include <nlohmann/json.hpp>

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
#include <set>
#include <pwd.h>
#include <string>
#include <string_view>

#include <sys/prctl.h>
#include <sys/file.h>
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
    if (argc < 2) return std::nullopt;
    const bool global = std::string_view(argv[1]) == "--global";
    if ((global && argc != 12) ||
        (!global && (argc != 13 || !parse_u32(argv[1], app_id) || app_id == 0))) {
      return std::nullopt;
    }
    if (global) app_id = 1;
    platf::steam::session_launch_policy_t policy;
    policy.provider = argv[2];
    if (!parse_u32(argv[3], policy.limit_millihz)) {
      return std::nullopt;
    }
    policy.preset = argv[4];
    if ((std::string_view(argv[5]) != "0" && std::string_view(argv[5]) != "1") ||
        (std::string_view(argv[7]) != "0" && std::string_view(argv[7]) != "1") ||
        (std::string_view(argv[8]) != "0" && std::string_view(argv[8]) != "1") ||
        (!global && std::string_view(argv[9]) != "0" && std::string_view(argv[9]) != "1") ||
        (global && std::string_view(argv[9]) != "sdr" &&
                   std::string_view(argv[9]) != "sdr10" &&
                   std::string_view(argv[9]) != "hdr") ||
        (std::string_view(argv[10]) != "0" && std::string_view(argv[10]) != "1") ||
        (std::string_view(argv[11]) != "0" && std::string_view(argv[11]) != "1") ||
        (!global && std::string_view(argv[12]) != "0" && std::string_view(argv[12]) != "1")) {
      return std::nullopt;
    }
    policy.always_show_graph = std::string_view(argv[5]) == "1";
    policy.limiter_method = argv[6];
    policy.smooth_motion = std::string_view(argv[7]) == "1";
    policy.smooth_motion_graphics_queue = std::string_view(argv[8]) == "1";
    policy.hdr = global ? std::string_view(argv[9]) == "hdr" : std::string_view(argv[9]) == "1";
    policy.wayland_hdr_compatibility = std::string_view(argv[10]) == "1";
    policy.proton_dualsense_compatibility = std::string_view(argv[11]) == "1";
    policy.playstation_controller_attached = !global && std::string_view(argv[12]) == "1";
    if (policy.playstation_controller_attached && !policy.proton_dualsense_compatibility) {
      return std::nullopt;
    }
    if (policy.wayland_hdr_compatibility && !policy.hdr) {
      return std::nullopt;
    }
    auto validation_policy = policy;
    if (global) {
      // A global SDR policy still needs the hook even when no limiter is
      // active. Use HDR solely to exercise the direct policy validator's
      // feature-presence requirement; the helper receives argv[9] verbatim.
      validation_policy.hdr = true;
    }
    if (platf::steam::session_launch_command(app_id, validation_policy).empty()) {
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

  bool virtual_dualsense_ready() {
    std::error_code error;
    const fs::path usb_devices = "/sys/bus/usb/devices";
    for (fs::directory_iterator entry(usb_devices, error), end;
         !error && entry != end;
         entry.increment(error)) {
      const auto canonical = fs::weakly_canonical(entry->path(), error);
      if (error) {
        error.clear();
        continue;
      }
      if (canonical.string().find("/devices/platform/vibeshine_ds5_hcd.") == std::string::npos) {
        continue;
      }
      std::ifstream vendor(entry->path() / "idVendor");
      std::ifstream product(entry->path() / "idProduct");
      std::string vendor_id;
      std::string product_id;
      if (vendor >> vendor_id && product >> product_id &&
          vendor_id == "054c" && product_id == "0ce6") {
        const auto interface_prefix = entry->path().filename().string() + ":";
        std::error_code interface_error;
        for (fs::directory_iterator interface(usb_devices, interface_error), interface_end;
             !interface_error && interface != interface_end;
             interface.increment(interface_error)) {
          if (!interface->path().filename().string().starts_with(interface_prefix)) {
            continue;
          }
          const auto sound_root = interface->path() / "sound";
          std::error_code card_error;
          for (fs::directory_iterator card(sound_root, card_error), card_end;
               !card_error && card != card_end;
               card.increment(card_error)) {
            std::error_code pcm_error;
            for (fs::directory_iterator pcm(card->path(), pcm_error), pcm_end;
                 !pcm_error && pcm != pcm_end;
                 pcm.increment(pcm_error)) {
              const auto name = pcm->path().filename().string();
              if (name.starts_with("pcm") && name.ends_with("p")) {
                return true;
              }
            }
          }
        }
      }
    }
    return false;
  }

  void wait_for_virtual_dualsense() {
    if (virtual_dualsense_ready()) {
      return;
    }
    report(LOG_INFO, "PlayStation controller attached; waiting for the virtual DualSense before Proton launch");
    constexpr int ready_timeout_ms = 2000;
    constexpr int poll_ms = 25;
    for (int waited = 0; waited < ready_timeout_ms; waited += poll_ms) {
      sleep_milliseconds(poll_ms);
      if (virtual_dualsense_ready()) {
        report(LOG_INFO, "Virtual DualSense ready after " + std::to_string(waited + poll_ms) + " ms");
        return;
      }
    }
    report(LOG_WARNING, "Virtual DualSense did not enumerate within 2 s; launching anyway");
  }

  int global_limiter(char **argv) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        !install_clean_environment()) return 126;
    std::set<std::string> roots;
    try {
      scoped_metadata_limits limits;
      if (!limits.armed()) return 126;
      const auto libraries = platf::steam::default_library_roots();
      for (const auto &root : libraries) roots.insert(root.string());
      for (const auto &game : platf::steam::discover(libraries)) {
        if (!game.library_path.empty()) roots.insert(game.library_path.string());
        if (!game.proton_path.empty()) roots.insert(game.proton_path.string());
      }
    } catch (...) {
      report(LOG_ERR, "could not discover Proton installations for global limiting");
      return 1;
    }
    std::vector<std::string> arguments {
      "/usr/bin/python3", "-I",
      "/usr/libexec/vibeshine/vibepollo-global-limiter.py",
      argv[2], argv[3], argv[4], argv[5], argv[6], argv[9], argv[10], argv[11]
    };
    arguments.insert(arguments.end(), roots.begin(), roots.end());
    std::vector<char *> pointers;
    for (auto &argument : arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    execv(pointers.front(), pointers.data());
    report(LOG_ERR, "could not execute the global Proton limiter helper");
    return 126;
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

    const bool proton_wayland_hdr_compatibility =
      policy.wayland_hdr_compatibility && game->launch_os == "windows";
    if (setenv("NVPRESENT_ENABLE_SMOOTH_MOTION", policy.smooth_motion ? "1" : "", 1) != 0 ||
        setenv("NVPRESENT_QUEUE_FAMILY",
               policy.smooth_motion_graphics_queue ? "1" : "", 1) != 0 ||
        setenv("PROTON_ENABLE_HDR", policy.hdr ? "1" : "0", 1) != 0 ||
        setenv("DXVK_HDR", policy.hdr ? "1" : "0", 1) != 0 ||
        (proton_wayland_hdr_compatibility &&
         (setenv("ENABLE_HDR_WSI", "1", 1) != 0 ||
          setenv("PROTON_ENABLE_WAYLAND", "1", 1) != 0))) {
      report(LOG_ERR, "could not install launch policy");
      return 1;
    }
    if (policy.proton_dualsense_compatibility && game->launch_os == "windows") {
      if (setenv("PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE", "1", 0) != 0 ||
          setenv("PROTON_SONY_WINDOWS_DEVICE_NAMES", "1", 0) != 0) {
        report(LOG_ERR, "could not install DualSense compatibility policy");
        return 1;
      }
    }
    if (proton_wayland_hdr_compatibility) {
      report(LOG_INFO, "Wayland HDR compatibility enabled for direct Proton launch");
    }

    ensure_steam_client();
    if (policy.proton_dualsense_compatibility &&
        policy.playstation_controller_attached && game->launch_os == "windows") {
      wait_for_virtual_dualsense();
    }

    // Steam Launch Options are user-authored shell expressions. They are
    // interpreted only here, after irreversible transition to that same UID.
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    report(LOG_ERR, std::string("exec failed: ") + std::strerror(errno));
    return errno == ENOENT ? 127 : 126;
  }
}  // namespace

namespace {
  std::optional<platf::steam::lifecycle::process_snapshot> big_picture_snapshot() {
    auto result = platf::steam::lifecycle::snapshot_processes();
    if (!result) return result;
    for (auto &[pid, process] : result->processes) {
      std::ifstream environment("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      // Bound reads and retain only the numeric game ID, never other values.
      std::string bytes(128 * 1024, '\0');
      environment.read(bytes.data(), bytes.size());
      bytes.resize(environment.gcount());
      process.steam_app_id = platf::steam::lifecycle::big_picture_app_id(bytes);
    }
    return result;
  }

  std::uint64_t current_boot_ticks() {
    const long ticks_per_sec = sysconf(_SC_CLK_TCK);
    if (ticks_per_sec <= 0) return 0;
    timespec boot {};
    if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0) return 0;
    return static_cast<std::uint64_t>(boot.tv_sec) * static_cast<std::uint64_t>(ticks_per_sec) +
           static_cast<std::uint64_t>(boot.tv_nsec) *
             static_cast<std::uint64_t>(ticks_per_sec) / 1000000000ULL;
  }

  int lock_big_picture_state(const char *path) {
    const int state = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (state < 0) return -1;
    struct stat attributes {};
    if (fchmod(state, 0600) != 0 || fstat(state, &attributes) != 0 ||
        !S_ISREG(attributes.st_mode) || attributes.st_uid != getuid() ||
        (attributes.st_mode & 0777) != 0600 || attributes.st_nlink != 1 ||
        flock(state, LOCK_EX | LOCK_NB) != 0) {
      close(state);
      return -1;
    }
    return state;
  }

  // Runs as the selected desktop user. The machine host cannot inspect or
  // signal that user's games, and must never parse this user-owned state.
  int big_picture(std::string_view uri) {
    const bool env_ok = install_clean_environment();
    if (!env_ok) {
      syslog(LOG_WARNING, "Big Picture environment sanitization failed; handing off to Steam without cleanup state");
    }
    const bool opening = uri == "steam://open/bigpicture";
    // The URI is delivered to an already-running client. Start that client in
    // its own user unit first, or a cold host never reaches Big Picture.
    if (opening) ensure_steam_client();

    int state = -1;
    struct stat attributes {};
    if (env_ok) {
      if (const char *runtime = std::getenv("XDG_RUNTIME_DIR")) {
        const auto state_path = fs::path(runtime) / "vibepollo-big-picture.json";
        state = lock_big_picture_state(state_path.c_str());
      }
      if (state >= 0) {
        if (fstat(state, &attributes) != 0) {
          close(state);
          state = -1;
        }
      } else {
        syslog(LOG_WARNING, "Big Picture game cleanup unavailable: cannot lock session state");
      }
    }

    try {
      if (state >= 0 && opening) {
        // Invalidate a previous session even if configuration or sampling fails.
        if (ftruncate(state, 0) != 0) throw std::runtime_error("cannot reset baseline");
        bool close_games = true;
        if (const char *config_home = std::getenv("XDG_CONFIG_HOME")) {
          std::ifstream settings(fs::path(config_home) / "vibepollo/steam-big-picture.json");
          if (settings) {
            const auto config = nlohmann::json::parse(settings);
            close_games = config.value("close-games", true);
          }
        }
        if (close_games) {
          const auto started_after_ticks = current_boot_ticks();
          const auto baseline = big_picture_snapshot();
          if (!baseline) throw std::runtime_error("baseline unavailable");
          if (!started_after_ticks && !baseline->complete) {
            throw std::runtime_error("complete baseline unavailable");
          }
          nlohmann::json saved = {
            {"version", 2},
            {"started_after_ticks", started_after_ticks},
            {"pids", nlohmann::json::array()}
          };
          for (const auto &[pid, process] : baseline->processes) {
            saved["pids"].push_back({pid, process.steam_app_id});
          }
          const auto bytes = saved.dump();
          if (bytes.size() > 4 * 1024 * 1024 ||
              write(state, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
            throw std::runtime_error("cannot write baseline");
          }
        }
      } else if (state >= 0) {
        // Consume once before signalling; repeated Quit requests cannot reuse
        // a baseline to claim a later game. Missing state means close UI only.
        std::string bytes;
        if (attributes.st_size > 0 && attributes.st_size <= 4 * 1024 * 1024) {
          bytes.resize(attributes.st_size);
          if (read(state, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) bytes.clear();
        }
        if (ftruncate(state, 0) != 0) throw std::runtime_error("cannot consume baseline");
        if (!bytes.empty()) {
          const auto saved = nlohmann::json::parse(bytes);
          const auto version = saved.at("version").get<int>();
          if (version != 1 && version != 2) throw std::runtime_error("unknown baseline version");
          platf::steam::lifecycle::process_snapshot baseline;
          for (const auto &value : saved.at("pids")) {
            const auto pid = value.at(0).get<platf::steam::lifecycle::process_id_t>();
            baseline.processes[pid].pid = pid;
            baseline.processes[pid].steam_app_id = value.at(1).get<std::uint32_t>();
          }
          const auto started_after_ticks = version == 2 ?
            saved.at("started_after_ticks").get<std::uint64_t>() : 0;
          if (const auto current = big_picture_snapshot()) {
            const auto tree = platf::steam::lifecycle::big_picture_tree(
              baseline, *current, started_after_ticks);
            auto controller = platf::steam::lifecycle::native_process_controller();
            platf::steam::lifecycle::stop_options options;
            options.grace_period = std::chrono::seconds(5);
            const auto stopped = platf::steam::lifecycle::stop_tree(tree, *controller, options);
            syslog(LOG_INFO, "Big Picture game cleanup: TERM=%zu KILL=%zu skipped=%zu complete=%s",
                   stopped.terminate_sent, stopped.kill_sent, stopped.skipped, stopped.complete ? "yes" : "no");
          }
        }
      }
    } catch (const std::exception &error) {
      if (state >= 0) (void) ftruncate(state, 0);
      syslog(LOG_WARNING, "Big Picture game cleanup unavailable: %s", error.what());
    }
    // Keep the lock until the fixed Steam handoff replaces this process.
    // Ordinary stream disconnects never execute the app's undo command.
    const std::string target(uri);
    execl("/usr/bin/steam", "/usr/bin/steam", target.c_str(), nullptr);
    if (state >= 0) {
      (void) ftruncate(state, 0);
      close(state);
    }
    return 126;
  }
}

int main(int argc, char **argv) {
  openlog("vibepollo-steam-launch", LOG_PID, LOG_USER);
  if (argc == 3 && std::string_view(argv[1]) == "--big-picture" &&
      (std::string_view(argv[2]) == "steam://open/bigpicture" ||
       std::string_view(argv[2]) == "steam://close/bigpicture")) {
    return big_picture(argv[2]);
  }
  std::uint32_t app_id = 0;
  const auto policy = parse_policy(argc, argv, app_id);
  if (!policy) {
    std::cerr << "usage: vibepollo-steam-launch APPID PROVIDER LIMIT_MILLIHZ "
                 "PRESET GRAPH METHOD SMOOTH QUEUE HDR WAYLAND_HDR_COMPATIBILITY DUALSENSE_COMPATIBILITY\n"
                 "       vibepollo-steam-launch --global PROVIDER LIMIT_MILLIHZ "
                 "PRESET GRAPH METHOD 0 0 COLOR_MODE WAYLAND_HDR_COMPATIBILITY DUALSENSE_COMPATIBILITY\n";
    return 2;
  }
  if (std::string_view(argv[1]) == "--global") return global_limiter(argv);
  return launch(app_id, *policy);
}
