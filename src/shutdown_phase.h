#pragma once

#include <atomic>
#include <string_view>

namespace shutdown_phase {
  enum class phase_e : unsigned char {
    early_exit_cleanup,
    pyrowave_host_shutdown,
    waiting_for_shutdown_event,
    linux_private_display_preserve,
    termination_signal_monitor_stop,
    session_monitor_window_future,
    session_monitor_thread_id_future,
    session_monitor_join_wait,
    session_monitor_join,
    session_monitor_force_quit_wait,
    session_monitor_forced_join,
    deferred_virtual_output_shutdown,
    orphan_recovery_monitor_shutdown,
    virtual_display_recovery_shutdown,
    deferred_virtual_output_worker_join,
    virtual_display_recovery_monitor_join,
    http_thread_join,
    config_thread_join,
    rtsp_thread_join,
    lutris_autosync_stop,
    steam_autosync_stop,
    display_helper_watchdog_stop,
    retained_display_cleanup,
    virtual_display_device_close,
    task_pool_stop,
    task_pool_join,
    system_tray_stop,
    restore_nvprefs,
    session_history_shutdown,
    main_cleanup_return,
  };

  static_assert(std::atomic<phase_e>::is_always_lock_free, "shutdown phase must be lock-free in the watchdog");

  constexpr std::string_view name(phase_e phase) noexcept {
    switch (phase) {
      case phase_e::early_exit_cleanup:
        return "early-exit-cleanup";
      case phase_e::pyrowave_host_shutdown:
        return "pyrowave-host-shutdown";
      case phase_e::waiting_for_shutdown_event:
        return "waiting-for-shutdown-event";
      case phase_e::linux_private_display_preserve:
        return "linux-private-display-preserve";
      case phase_e::termination_signal_monitor_stop:
        return "termination-signal-monitor-stop";
      case phase_e::session_monitor_window_future:
        return "session-monitor-window-future";
      case phase_e::session_monitor_thread_id_future:
        return "session-monitor-thread-id-future";
      case phase_e::session_monitor_join_wait:
        return "session-monitor-join-wait";
      case phase_e::session_monitor_join:
        return "session-monitor-join";
      case phase_e::session_monitor_force_quit_wait:
        return "session-monitor-force-quit-wait";
      case phase_e::session_monitor_forced_join:
        return "session-monitor-forced-join";
      case phase_e::deferred_virtual_output_shutdown:
        return "deferred-virtual-output-shutdown";
      case phase_e::orphan_recovery_monitor_shutdown:
        return "orphan-recovery-monitor-shutdown";
      case phase_e::virtual_display_recovery_shutdown:
        return "virtual-display-recovery-shutdown";
      case phase_e::deferred_virtual_output_worker_join:
        return "deferred-virtual-output-worker-join";
      case phase_e::virtual_display_recovery_monitor_join:
        return "virtual-display-recovery-monitor-join";
      case phase_e::http_thread_join:
        return "http-thread-join";
      case phase_e::config_thread_join:
        return "config-thread-join";
      case phase_e::rtsp_thread_join:
        return "rtsp-thread-join";
      case phase_e::lutris_autosync_stop:
        return "lutris-autosync-stop";
      case phase_e::steam_autosync_stop:
        return "steam-autosync-stop";
      case phase_e::display_helper_watchdog_stop:
        return "display-helper-watchdog-stop";
      case phase_e::retained_display_cleanup:
        return "retained-display-cleanup";
      case phase_e::virtual_display_device_close:
        return "virtual-display-device-close";
      case phase_e::task_pool_stop:
        return "task-pool-stop";
      case phase_e::task_pool_join:
        return "task-pool-join";
      case phase_e::system_tray_stop:
        return "system-tray-stop";
      case phase_e::restore_nvprefs:
        return "restore-nvprefs";
      case phase_e::session_history_shutdown:
        return "session-history-shutdown";
      case phase_e::main_cleanup_return:
        return "main-cleanup-return";
    }
    return "unknown";
  }

  class tracker_t {
  public:
    void set(phase_e phase) noexcept {
      phase_.store(phase, std::memory_order_relaxed);
    }

    phase_e current() const noexcept {
      return phase_.load(std::memory_order_relaxed);
    }

    std::string_view current_name() const noexcept {
      return name(current());
    }

  private:
    std::atomic<phase_e> phase_ {phase_e::early_exit_cleanup};
  };
}  // namespace shutdown_phase
