#include "src/shutdown_phase.h"

#include <array>
#include <gtest/gtest.h>
#include <utility>

TEST(ShutdownPhase, EveryPublishedPhaseHasAStableDiagnosticName) {
  using shutdown_phase::phase_e;

  const std::array phases {
    std::pair {phase_e::early_exit_cleanup, "early-exit-cleanup"},
    std::pair {phase_e::pyrowave_host_shutdown, "pyrowave-host-shutdown"},
    std::pair {phase_e::waiting_for_shutdown_event, "waiting-for-shutdown-event"},
    std::pair {phase_e::linux_private_display_preserve, "linux-private-display-preserve"},
    std::pair {phase_e::termination_signal_monitor_stop, "termination-signal-monitor-stop"},
    std::pair {phase_e::session_monitor_window_future, "session-monitor-window-future"},
    std::pair {phase_e::session_monitor_thread_id_future, "session-monitor-thread-id-future"},
    std::pair {phase_e::session_monitor_join_wait, "session-monitor-join-wait"},
    std::pair {phase_e::session_monitor_join, "session-monitor-join"},
    std::pair {phase_e::session_monitor_force_quit_wait, "session-monitor-force-quit-wait"},
    std::pair {phase_e::session_monitor_forced_join, "session-monitor-forced-join"},
    std::pair {phase_e::deferred_virtual_output_shutdown, "deferred-virtual-output-shutdown"},
    std::pair {phase_e::orphan_recovery_monitor_shutdown, "orphan-recovery-monitor-shutdown"},
    std::pair {phase_e::virtual_display_recovery_shutdown, "virtual-display-recovery-shutdown"},
    std::pair {phase_e::deferred_virtual_output_worker_join, "deferred-virtual-output-worker-join"},
    std::pair {phase_e::virtual_display_recovery_monitor_join, "virtual-display-recovery-monitor-join"},
    std::pair {phase_e::http_thread_join, "http-thread-join"},
    std::pair {phase_e::config_thread_join, "config-thread-join"},
    std::pair {phase_e::rtsp_thread_join, "rtsp-thread-join"},
    std::pair {phase_e::lutris_autosync_stop, "lutris-autosync-stop"},
    std::pair {phase_e::steam_autosync_stop, "steam-autosync-stop"},
    std::pair {phase_e::display_helper_watchdog_stop, "display-helper-watchdog-stop"},
    std::pair {phase_e::retained_display_cleanup, "retained-display-cleanup"},
    std::pair {phase_e::virtual_display_device_close, "virtual-display-device-close"},
    std::pair {phase_e::task_pool_stop, "task-pool-stop"},
    std::pair {phase_e::task_pool_join, "task-pool-join"},
    std::pair {phase_e::system_tray_stop, "system-tray-stop"},
    std::pair {phase_e::restore_nvprefs, "restore-nvprefs"},
    std::pair {phase_e::session_history_shutdown, "session-history-shutdown"},
    std::pair {phase_e::main_cleanup_return, "main-cleanup-return"},
  };

  for (const auto &[phase, expected_name] : phases) {
    EXPECT_EQ(shutdown_phase::name(phase), expected_name);
  }
  EXPECT_EQ(shutdown_phase::name(static_cast<phase_e>(255)), "unknown");
}

TEST(ShutdownPhase, WatchdogReadsTheMostRecentlyPublishedMainThreadPhase) {
  shutdown_phase::tracker_t phase;
  EXPECT_EQ(phase.current_name(), "early-exit-cleanup");

  phase.set(shutdown_phase::phase_e::http_thread_join);
  EXPECT_EQ(phase.current(), shutdown_phase::phase_e::http_thread_join);
  EXPECT_EQ(phase.current_name(), "http-thread-join");

  phase.set(shutdown_phase::phase_e::session_monitor_forced_join);
  EXPECT_EQ(phase.current_name(), "session-monitor-forced-join");
}
