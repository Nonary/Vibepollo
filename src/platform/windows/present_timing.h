/**
 * @file src/platform/windows/present_timing.h
 * @brief Stamps captured frames at the foreground game's present cadence.
 */
#pragma once

#include "present_timing_policy.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include <winsock2.h>
#include <windows.h>

namespace platf::dxgi::present_timing {

  class tracker_t;

  /**
   * @brief Per-capture timestamp refinement.
   *
   * WGC reports when DWM composed a frame. That time is quantized to the
   * captured display's refresh, e.g. 2.156 ms for a 116 FPS stream on a 4x
   * virtual display. The stamper listens to Microsoft-Windows-DXGI Present
   * events of the foreground process through a shared real-time ETW session and
   * moves each stamp to where that game's present cadence places it inside the
   * composition refresh. Without ETW access or a matching present the stamp is
   * the middle of the refresh, so it never moves more than one refresh from the
   * composition time.
   *
   * Refinement runs when the encoded frame is packetized, not when it is
   * captured: by then the present that produced it has almost always been
   * delivered by ETW, which batches events. Calls must come from one thread,
   * in frame order.
   */
  class capture_stamper_t {
  public:
    /**
     * @param gdi_device_name GDI name of the captured output, e.g. `\\.\DISPLAY3`.
     */
    explicit capture_stamper_t(const wchar_t *gdi_device_name);
    ~capture_stamper_t();

    capture_stamper_t(const capture_stamper_t &) = delete;
    capture_stamper_t &operator=(const capture_stamper_t &) = delete;

    /**
     * @brief Returns the QPC time to report for a frame composed at `composition_qpc`.
     */
    std::int64_t stamp(std::int64_t composition_qpc);

  private:
    void refresh_foreground(std::int64_t now_qpc);

    std::shared_ptr<tracker_t> tracker_;
    refiner_t refiner_;
    std::int64_t frequency_ {0};
    std::int64_t grid_ {0};
    std::int64_t minimum_step_ {0};
    std::int64_t stale_ {0};
    DWORD foreground_pid_ {0};
    std::int64_t next_foreground_poll_ {0};
    std::uint64_t source_ {0};
    std::vector<std::int64_t> presents_;
    std::uint64_t frames_ {0};
    std::uint64_t matched_ {0};
    std::uint64_t refined_ {0};
    bool grid_disabled_logged_ {false};
  };

  /**
   * @brief Makes `stamper` the one used by refine_send_timestamp().
   */
  void set_active_stamper(std::shared_ptr<capture_stamper_t> stamper);

  /**
   * @brief Stops using `stamper` if it is still the active one.
   */
  void clear_active_stamper(const capture_stamper_t *stamper);

  /**
   * @brief Refines a captured frame's composition time for its RTP timestamp.
   * @return `composition` unchanged when no capture stamper is active.
   */
  std::chrono::steady_clock::time_point refine_send_timestamp(std::chrono::steady_clock::time_point composition);

}  // namespace platf::dxgi::present_timing
