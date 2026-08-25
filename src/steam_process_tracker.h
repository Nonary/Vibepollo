/** @file src/steam_process_tracker.h
 *  @brief Process ownership and cleanup policy for Steam launches.
 *
 * Steam does not expose a portable "process tree for this launch" API.  This
 * small boundary deliberately owns that missing behaviour.  The policy is
 * usable with synthetic snapshots, which keeps the safety decisions testable
 * without starting a game or depending on a particular Steam runtime.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf::steam::lifecycle {

  using process_id_t = std::uint64_t;

  struct process_info {
    process_id_t pid = 0;
    process_id_t parent_pid = 0;
    std::filesystem::path executable;
    std::filesystem::path cwd;
    std::vector<std::string> command_line;
    // Linux /proc start-time ticks provide PID identity across PID reuse.
    // Zero means the platform provider has no identity value.
    std::uint64_t start_time_ticks = 0;
  };

  struct process_snapshot {
    // A snapshot may be incomplete when /proc entries disappear while they
    // are being read. Available records remain useful for association; the
    // caller can use complete to decide whether absence proves termination.
    bool complete = true;
    std::map<process_id_t, process_info> processes;
  };

  enum class association_outcome {
    associated,
    baseline_only,
    untrackable,
  };

  struct tracked_process {
    process_info info;
    bool protected_process = false;
  };

  struct tracked_tree {
    process_id_t root_pid = 0;
    std::filesystem::path install_dir;
    std::map<process_id_t, tracked_process> processes;

    bool empty() const { return processes.empty(); }
  };

  struct association_result {
    association_outcome outcome = association_outcome::untrackable;
    tracked_tree tree;
    std::string reason;

    bool associated() const { return outcome == association_outcome::associated; }
  };

  // A process id in this list is never selected as a game process.  Steam
  // itself is also recognized by executable/cmdline below; explicit roots are
  // useful when a launcher uses a wrapper with a non-Steam executable name.
  struct association_options {
    std::vector<process_id_t> protected_steam_roots;
  };

  // Pure policy: compare the post-launch snapshot with the pre-launch
  // baseline and associate only new processes belonging to install_dir.
  association_result associate(const process_snapshot &baseline,
                               const process_snapshot &after,
                               const std::filesystem::path &install_dir,
                               const association_options &options = {});

  bool path_is_within(const std::filesystem::path &path,
                      const std::filesystem::path &directory);
  bool is_protected_steam_process(const process_info &process,
                                  const association_options &options = {});

  class process_snapshot_provider {
  public:
    virtual ~process_snapshot_provider() = default;
    virtual std::optional<process_snapshot> snapshot() = 0;
  };

  // Linux implementation reads /proc.  Other platforms provide an empty,
  // successful snapshot so this boundary remains buildable until a native
  // provider is added there.
  std::optional<process_snapshot> snapshot_processes();

  class tracker {
  public:
    explicit tracker(std::shared_ptr<process_snapshot_provider> provider = {});

    // The baseline must be captured immediately before Steam is launched.
    bool begin(const std::filesystem::path &install_dir,
              const association_options &options = {});
    association_result finish();
    association_result finish(const process_snapshot &after);
    // Whether any retained PID is present in a current snapshot. Incomplete
    // snapshots conservatively retain the prior running state.
    bool has_live_processes(const process_snapshot &current) const;
    const tracked_tree &tree() const { return tree_; }
    void clear();

  private:
    std::shared_ptr<process_snapshot_provider> provider_;
    process_snapshot baseline_;
    std::filesystem::path install_dir_;
    association_options options_;
    tracked_tree tree_;
    bool begun_ = false;
  };

  enum class signal_kind { terminate, kill };

  class process_controller {
  public:
    virtual ~process_controller() = default;
    virtual bool signal(process_id_t pid, signal_kind signal) = 0;
    virtual bool alive(process_id_t pid) = 0;
    virtual bool identity_matches(process_id_t pid, const process_info &expected) {
      (void) pid;
      (void) expected;
      return true;
    }
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
  };

  struct stop_options {
    std::chrono::milliseconds grace_period {2000};
    std::chrono::milliseconds poll_period {25};
  };

  struct stop_result {
    std::size_t terminate_sent = 0;
    std::size_t kill_sent = 0;
    std::size_t skipped = 0;
    bool complete = true;
  };

  // Stops only processes retained in tree.  Children are signalled before
  // parents and every PID is checked as alive by the injectable controller;
  // callers can therefore reject PID reuse or a changed executable.  The
  // implementation never sends a process-group signal.
  stop_result stop_tree(const tracked_tree &tree,
                        process_controller &controller,
                        const stop_options &options = {});

  // Native controller used by production integration.  It is deliberately a
  // no-op on Windows until a Job Object based controller is wired in.
  std::shared_ptr<process_controller> native_process_controller();

}  // namespace platf::steam::lifecycle
