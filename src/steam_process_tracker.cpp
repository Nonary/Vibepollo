#include "steam_process_tracker.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
  #include <csignal>
  #include <dirent.h>
  #include <unistd.h>
#endif

namespace platf::steam::lifecycle {
namespace {

  std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return value;
  }

  std::filesystem::path normalize(const std::filesystem::path &value) {
    if (value.empty()) {
      return {};
    }
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(value, error);
    if (error) {
      error.clear();
      result = std::filesystem::absolute(value, error);
      if (error) {
        result = value;
      }
    }
    result = result.lexically_normal();
#if defined(_WIN32)
    // Windows paths are case-insensitive.  The returned path is only used for
    // comparison; preserve the original process paths in the public result.
    auto text = lower(result.generic_string());
    return std::filesystem::path(text);
#else
    return result;
#endif
  }

  std::string basename(const std::filesystem::path &value) {
    return lower(value.filename().string());
  }

  bool looks_like_steam(const std::string &value) {
    const auto name = lower(std::filesystem::path(value).filename().string());
    return name == "steam" || name == "steam.exe" || name == "steamwebhelper" ||
           name == "steamwebhelper.exe" || name == "steamservice" ||
           name == "steamservice.exe" || name == "steam-runtime" ||
           name == "steam-runtime-sniper" || name == "steam-launch-wrapper";
  }

  bool looks_like_compatibility_runtime(const process_info &process) {
    const auto exe = basename(process.executable);
    if (exe.find("proton") != std::string::npos || exe.find("wine") != std::string::npos ||
        exe == "pressure-vessel" || exe == "pressure-vessel-wrap" ||
        exe == "gamescope") {
      return true;
    }
    for (const auto &argument : process.command_line) {
      const auto value = lower(std::filesystem::path(argument).filename().string());
      if (value.find("proton") != std::string::npos || value.find("wine") != std::string::npos ||
          value == "pressure-vessel" || value == "pressure-vessel-wrap") {
        return true;
      }
    }
    return false;
  }

  bool command_line_contains_install(const process_info &process,
                                     const std::filesystem::path &install_dir) {
    for (const auto &argument : process.command_line) {
      // A Wine/Proton command line normally contains the Windows executable
      // as one argument.  Test both the argument itself and the path before
      // the first quote/space so wrapper flags do not accidentally match.
      if (path_is_within(std::filesystem::path(argument), install_dir)) {
        return true;
      }
      const auto slash_argument = std::filesystem::path(argument).generic_string();
      if (slash_argument.find('/') != std::string::npos &&
          path_is_within(std::filesystem::path(slash_argument), install_dir)) {
        return true;
      }
    }
    return false;
  }

  bool direct_install_evidence(const process_info &process,
                               const std::filesystem::path &install_dir) {
    return path_is_within(process.executable, install_dir) ||
           path_is_within(process.cwd, install_dir) ||
           command_line_contains_install(process, install_dir);
  }

  class procfs_provider final : public process_snapshot_provider {
  public:
    std::optional<process_snapshot> snapshot() override { return snapshot_processes(); }
  };

#if defined(__linux__)
  std::optional<process_id_t> parse_pid(const char *name) {
    if (!name || !*name) {
      return std::nullopt;
    }
    process_id_t value = 0;
    for (const auto *cursor = name; *cursor; ++cursor) {
      if (*cursor < '0' || *cursor > '9') {
        return std::nullopt;
      }
      value = value * 10 + static_cast<process_id_t>(*cursor - '0');
    }
    return value == 0 ? std::nullopt : std::optional<process_id_t>(value);
  }

  struct proc_stat_identity {
    process_id_t parent_pid = 0;
    std::uint64_t start_time_ticks = 0;
  };

  std::optional<proc_stat_identity> read_proc_stat(process_id_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (line.empty()) {
      return std::nullopt;
    }
    // comm may contain spaces and ')' characters.  The final ')' before the
    // state field is the reliable delimiter for /proc/<pid>/stat.
    const auto close = line.rfind(')');
    if (close == std::string::npos || close + 2 >= line.size()) {
      return std::nullopt;
    }
    std::string fields = line.substr(close + 2);  // state, ppid, ..., starttime
    std::istringstream stream(fields);
    char state = 0;
    if (!(stream >> state)) {
      return std::nullopt;
    }
    proc_stat_identity identity;
    if (!(stream >> identity.parent_pid)) {
      return std::nullopt;
    }
    // Fields after the command name start at state=3.  We consumed state and
    // ppid, so field 22 (starttime) follows fields 5 through 21.
    std::string ignored;
    for (int field = 5; field <= 21; ++field) {
      if (!(stream >> ignored)) {
        return std::nullopt;
      }
    }
    if (!(stream >> identity.start_time_ticks)) {
      return std::nullopt;
    }
    return identity;
  }

  std::filesystem::path read_link(const std::string &path) {
    std::error_code error;
    return std::filesystem::read_symlink(path, error);
  }

  std::vector<std::string> read_command_line(process_id_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t position = 0; position <= bytes.size(); ++position) {
      if (position == bytes.size() || bytes[position] == '\0') {
        if (position > start) {
          result.emplace_back(bytes.substr(start, position - start));
        }
        start = position + 1;
      }
    }
    return result;
  }
#endif

  class native_controller final : public process_controller {
  public:
    bool signal(process_id_t pid, signal_kind kind) override {
#if defined(__linux__)
      const int value = kind == signal_kind::terminate ? SIGTERM : SIGKILL;
      return ::kill(static_cast<pid_t>(pid), value) == 0;
#else
      (void) pid;
      (void) kind;
      return false;
#endif
    }

    bool alive(process_id_t pid) override {
#if defined(__linux__)
      if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
      }
      return errno == EPERM;
#else
      (void) pid;
      return false;
#endif
    }

    bool identity_matches(process_id_t pid, const process_info &expected) override {
#if defined(__linux__)
      if (expected.start_time_ticks == 0) {
        return true;
      }
      const auto current = read_proc_stat(pid);
      return current && current->start_time_ticks == expected.start_time_ticks;
#else
      (void) pid;
      (void) expected;
      return true;
#endif
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      std::this_thread::sleep_for(duration);
    }
  };

}  // namespace

bool path_is_within(const std::filesystem::path &path,
                    const std::filesystem::path &directory) {
  if (path.empty() || directory.empty()) {
    return false;
  }
  const auto candidate = normalize(path);
  const auto root = normalize(directory);
  if (candidate.empty() || root.empty()) {
    return false;
  }
  auto candidate_it = candidate.begin();
  auto root_it = root.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end()) {
      return false;
    }
#if defined(_WIN32)
    if (lower(candidate_it->generic_string()) != lower(root_it->generic_string())) {
#else
    if (candidate_it->generic_string() != root_it->generic_string()) {
#endif
      return false;
    }
  }
  return true;
}

bool is_protected_steam_process(const process_info &process,
                                const association_options &options) {
  if (std::find(options.protected_steam_roots.begin(), options.protected_steam_roots.end(), process.pid) !=
      options.protected_steam_roots.end()) {
    return true;
  }
  if (looks_like_steam(process.executable.string())) {
    return true;
  }
  if (!process.command_line.empty() && looks_like_steam(process.command_line.front())) {
    return true;
  }
  return false;
}

association_result associate(const process_snapshot &baseline,
                             const process_snapshot &after,
                             const std::filesystem::path &install_dir,
                             const association_options &options) {
  association_result result;
  result.tree.install_dir = normalize(install_dir);
  if (result.tree.install_dir.empty()) {
    result.reason = "process snapshot or install directory is unavailable";
    return result;
  }

  std::map<process_id_t, process_info> fresh;
  for (const auto &[pid, process] : after.processes) {
    // PID reuse is intentionally conservative: a PID present in the baseline
    // is never attributed to this launch.
    if (!baseline.processes.contains(pid)) {
      fresh.emplace(pid, process);
    }
  }
  if (fresh.empty()) {
    result.outcome = association_outcome::baseline_only;
    result.reason = "no process appeared after the launch baseline";
    return result;
  }

  std::set<process_id_t> selected;
  for (const auto &[pid, process] : fresh) {
    if (!is_protected_steam_process(process, options) &&
        (direct_install_evidence(process, result.tree.install_dir) ||
         (looks_like_compatibility_runtime(process) &&
          command_line_contains_install(process, result.tree.install_dir)))) {
      selected.insert(pid);
    }
  }

  // Once a game/compatibility-root is found, retain all of its new children.
  // This handles launchers, Wine wineserver processes, and games that fork a
  // renderer outside the install directory.  Steam processes themselves are
  // excluded but do not stop expansion: a game may briefly launch a Steam
  // helper while it is starting.
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (const auto &[pid, process] : fresh) {
      if (selected.contains(pid) || is_protected_steam_process(process, options)) {
        continue;
      }
      if (selected.contains(process.parent_pid)) {
        selected.insert(pid);
        expanded = true;
      }
    }
  }

  if (selected.empty()) {
    result.reason = "new processes had no install-directory or compatibility-runtime evidence";
    result.outcome = association_outcome::untrackable;
    return result;
  }

  result.outcome = association_outcome::associated;
  for (const auto pid : selected) {
    result.tree.processes.emplace(pid, tracked_process {fresh.at(pid), false});
  }
  // The lowest selected process whose parent is not selected is the launch
  // root.  Determinism matters when multiple game binaries start together.
  for (const auto &[pid, process] : result.tree.processes) {
    if (!result.tree.processes.contains(process.info.parent_pid)) {
      result.tree.root_pid = pid;
      break;
    }
  }
  result.reason = "associated new game process tree";
  return result;
}

std::optional<process_snapshot> snapshot_processes() {
  process_snapshot result;
#if defined(__linux__)
  DIR *directory = ::opendir("/proc");
  if (!directory) {
    return std::nullopt;
  }
  while (const auto *entry = ::readdir(directory)) {
    const auto pid = parse_pid(entry->d_name);
    if (!pid) {
      continue;
    }
    const auto parent = read_proc_stat(*pid);
    if (!parent) {
      result.complete = false;
      continue;
    }
    process_info info;
    info.pid = *pid;
    info.parent_pid = parent->parent_pid;
    info.start_time_ticks = parent->start_time_ticks;
    info.executable = read_link("/proc/" + std::to_string(*pid) + "/exe");
    info.cwd = read_link("/proc/" + std::to_string(*pid) + "/cwd");
    info.command_line = read_command_line(*pid);
    result.processes.emplace(*pid, std::move(info));
  }
  ::closedir(directory);
#else
  // Keep the provider usable on Windows/macOS while native enumeration is
  // added at their platform seams.  No fabricated process is ever returned.
#endif
  return result;
}

tracker::tracker(std::shared_ptr<process_snapshot_provider> provider):
  provider_(std::move(provider)) {
  if (!provider_) {
    provider_ = std::make_shared<procfs_provider>();
  }
}

bool tracker::begin(const std::filesystem::path &install_dir,
                   const association_options &options) {
  clear();
  const auto baseline = provider_->snapshot();
  if (!baseline) {
    return false;
  }
  install_dir_ = install_dir;
  options_ = options;
  baseline_ = *baseline;
  begun_ = true;
  return true;
}

association_result tracker::finish() {
  if (!begun_) {
    association_result result;
    result.reason = "tracker was not started";
    return result;
  }
  const auto after = provider_->snapshot();
  if (!after) {
    association_result result;
    result.reason = "post-launch process snapshot is unavailable";
    return result;
  }
  return finish(*after);
}

association_result tracker::finish(const process_snapshot &after) {
  if (!begun_) {
    association_result result;
    result.reason = "tracker was not started";
    return result;
  }
  auto result = associate(baseline_, after, install_dir_, options_);
  if (result.associated()) {
    if (!tree_.empty()) {
      // Preserve already-associated processes and add descendants discovered
      // on later polls. This matters when the original game binary is a
      // short-lived launcher and its real renderer appears afterwards.
      for (const auto &[pid, process] : result.tree.processes) {
        const auto existing = tree_.processes.find(pid);
        if (existing == tree_.processes.end() ||
            existing->second.info.start_time_ticks == 0 || process.info.start_time_ticks == 0 ||
            existing->second.info.start_time_ticks == process.info.start_time_ticks) {
          tree_.processes.emplace(pid, process);
        } else {
          // PID reuse is a new identity. Replace the stale record so future
          // stop operations validate the new process identity.
          existing->second = process;
        }
      }
      for (const auto &[pid, process] : after.processes) {
        if (tree_.processes.contains(pid) || is_protected_steam_process(process, options_)) {
          continue;
        }
        if (tree_.processes.contains(process.parent_pid)) {
          tree_.processes.emplace(pid, tracked_process {process, false});
        }
      }
      result.tree = tree_;
    } else {
      tree_ = result.tree;
    }
  } else if (!tree_.empty()) {
    const bool retained = std::any_of(tree_.processes.begin(), tree_.processes.end(),
                                      [&](const auto &entry) {
                                        const auto found = after.processes.find(entry.first);
                                        if (found == after.processes.end()) {
                                          return false;
                                        }
                                        return entry.second.info.start_time_ticks == 0 ||
                                               found->second.start_time_ticks == 0 ||
                                               entry.second.info.start_time_ticks == found->second.start_time_ticks;
                                      });
    if (retained || !after.complete) {
      result.outcome = association_outcome::associated;
      result.tree = tree_;
      result.reason = "retained previously associated game process tree";
    } else {
      tree_ = {};
    }
  }
  return result;
}

bool tracker::has_live_processes(const process_snapshot &current) const {
  if (tree_.empty()) {
    return false;
  }
  for (const auto &[pid, process] : tree_.processes) {
    const auto found = current.processes.find(pid);
    if (found == current.processes.end()) {
      continue;
    }
    if (process.info.start_time_ticks == 0 || found->second.start_time_ticks == 0 ||
        process.info.start_time_ticks == found->second.start_time_ticks) {
      return true;
    }
  }
  return !current.complete;
}

void tracker::clear() {
  baseline_ = {};
  install_dir_.clear();
  options_ = {};
  tree_ = {};
  begun_ = false;
}

stop_result stop_tree(const tracked_tree &tree,
                      process_controller &controller,
                      const stop_options &options) {
  stop_result result;
  if (tree.empty()) {
    return result;
  }

  std::vector<process_id_t> order;
  order.reserve(tree.processes.size());
  for (const auto &[pid, process] : tree.processes) {
    if (!process.protected_process) {
      order.push_back(pid);
    } else {
      ++result.skipped;
    }
  }
  // A stable descendant-first order prevents a parent from reaping or
  // detaching a child before the child receives the graceful signal.
  auto depth = [&](process_id_t pid) {
    std::size_t value = 0;
    std::set<process_id_t> visited;
    auto current = pid;
    while (tree.processes.contains(current) && visited.insert(current).second) {
      const auto parent = tree.processes.at(current).info.parent_pid;
      if (!tree.processes.contains(parent)) {
        break;
      }
      current = parent;
      ++value;
    }
    return value;
  };
  std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
    const auto left_depth = depth(left);
    const auto right_depth = depth(right);
    return left_depth != right_depth ? left_depth > right_depth : left > right;
  });

  std::vector<process_id_t> signalled;
  for (const auto pid : order) {
    const auto &tracked = tree.processes.at(pid);
    if (controller.identity_matches(pid, tracked.info) && controller.alive(pid) &&
        controller.signal(pid, signal_kind::terminate)) {
      signalled.push_back(pid);
      ++result.terminate_sent;
    } else {
      ++result.skipped;
    }
  }

  auto remaining = [&]() {
    return std::any_of(signalled.begin(), signalled.end(), [&](auto pid) {
      const auto &tracked = tree.processes.at(pid);
      return controller.identity_matches(pid, tracked.info) && controller.alive(pid);
    });
  };
  auto elapsed = std::chrono::milliseconds(0);
  const auto poll_period = options.poll_period > std::chrono::milliseconds(0) ?
      options.poll_period : std::chrono::milliseconds(1);
  while (remaining() && elapsed < options.grace_period) {
    controller.sleep_for(poll_period);
    elapsed += poll_period;
  }
  for (const auto pid : signalled) {
    const auto &tracked = tree.processes.at(pid);
    if (controller.identity_matches(pid, tracked.info) && controller.alive(pid) &&
        controller.signal(pid, signal_kind::kill)) {
      ++result.kill_sent;
    }
  }
  result.complete = !remaining();
  return result;
}

std::shared_ptr<process_controller> native_process_controller() {
  return std::make_shared<native_controller>();
}

}  // namespace platf::steam::lifecycle
