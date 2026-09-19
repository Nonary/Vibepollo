/**
 * @file src/host_stats.h
 * @brief Singleton sampler for host system performance counters
 *        (CPU/GPU/RAM/VRAM/temperatures).
 */
#pragma once

#include "host_stats_types.h"
#include "platform/common.h"

#include <memory>

namespace host_stats {

  /**
   * @brief Start the host stats sampler thread.
   *
   * Spawns one background thread that sleeps until a stream or Web UI
   * consumer needs dynamic metrics. Static host information is cached during
   * startup.
   *
   * @return RAII guard that stops the sampler when destroyed.
   */
  std::unique_ptr<platf::deinit_t>
    start();

  /**
   * @brief Return the most recent stats snapshot.
   *
   * Thread-safe. If the sampler is not running, returns a default
   * (sentinel-filled) @ref platf::host_stats_t.
   */
  platf::host_stats_t
    latest();

  /**
   * @brief Renew the Web UI consumer lease and return a fresh snapshot.
   */
  platf::host_stats_t
    latest_for_consumer();

  /**
   * @brief Return the cached static host info.
   *
   * Sampled once on @ref start; subsequent calls are O(1) and lock-free.
   */
  const platf::host_info_t &
    info();

  void
    rtsp_session_started();

  void
    rtsp_session_ended();

  void
    webrtc_session_started();

  void
    webrtc_session_ended();

  void
    configuration_changed();

#ifdef SUNSHINE_TESTS
  bool
    is_running_for_tests();
#endif

}  // namespace host_stats
