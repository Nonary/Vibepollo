/**
 * @file src/host_stats.h
 * @brief Singleton sampler for host system performance counters
 *        (CPU/GPU/RAM/VRAM/temperatures).
 */
#pragma once

#include "platform/common.h"

#include <memory>

namespace host_stats {

  /**
   * @brief Start the host stats sampler thread.
   *
   * Spawns one background thread that polls the platform provider every
   * 2 seconds and caches the latest snapshot. The first sample is taken
   * synchronously so @ref latest never returns sentinel values once
   * @c start has returned.
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
   * @brief Return the cached static host info.
   *
   * Sampled once on @ref start; subsequent calls are O(1) and lock-free.
   */
  const platf::host_info_t &
    info();

}  // namespace host_stats
