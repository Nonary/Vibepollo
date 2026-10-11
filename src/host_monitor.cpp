/**
 * @file src/host_monitor.cpp
 * @brief Eclipse: physical monitor off while streaming, on request. See host_monitor.h.
 */
#include "host_monitor.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logging.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
#endif

using namespace std::literals;

namespace host_monitor {
#ifdef _WIN32
  namespace {
    bool query(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
      // Retry: the configuration can change between the size query and the read (ERROR_INSUFFICIENT_BUFFER).
      for (int attempt = 0; attempt < 3; attempt++) {
        UINT32 n_paths = 0, n_modes = 0;
        if (GetDisplayConfigBufferSizes(flags, &n_paths, &n_modes) != ERROR_SUCCESS) {
          return false;
        }
        paths.assign(n_paths, DISPLAYCONFIG_PATH_INFO {});
        modes.assign(n_modes, DISPLAYCONFIG_MODE_INFO {});
        const LONG r = QueryDisplayConfig(flags, &n_paths, paths.data(), &n_modes, modes.data(), nullptr);
        if (r == ERROR_SUCCESS) {
          paths.resize(n_paths);
          modes.resize(n_modes);
          return true;
        }
        if (r != ERROR_INSUFFICIENT_BUFFER) {
          return false;
        }
      }
      return false;
    }

    bool target_name(const DISPLAYCONFIG_PATH_INFO &p, DISPLAYCONFIG_TARGET_DEVICE_NAME &tn) {
      tn = {};
      tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      tn.header.size = sizeof(tn);
      tn.header.adapterId = p.targetInfo.adapterId;
      tn.header.id = p.targetInfo.id;
      return DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS;
    }

    std::wstring source_gdi_name(const DISPLAYCONFIG_PATH_INFO &p) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME sn {};
      sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      sn.header.size = sizeof(sn);
      sn.header.adapterId = p.sourceInfo.adapterId;
      sn.header.id = p.sourceInfo.id;
      return DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS ? std::wstring(sn.viewGdiDeviceName) : std::wstring();
    }

    /** Virtual = an indirect output (IddCx drivers, Miracast) or the SudoVDA driver's EDID vendor "SMK" --
     *  SudoVDA reports its output technology as HDMI, so technology alone calls it physical. */
    bool is_virtual(const DISPLAYCONFIG_PATH_INFO &p) {
      DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
      if (!target_name(p, tn)) {
        return false;
      }
      const auto t = tn.outputTechnology;
      if (t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL ||
          t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST) {
        return true;
      }
      if (tn.flags.edidIdsValid) {
        const UINT16 id = _byteswap_ushort(tn.edidManufactureId);
        const char vendor[4] = {char('A' - 1 + ((id >> 10) & 0x1F)), char('A' - 1 + ((id >> 5) & 0x1F)), char('A' - 1 + (id & 0x1F)), 0};
        if (std::string(vendor) == "SMK") {
          return true;
        }
      }
      return false;
    }

    void count_active(int &virtual_n, int &physical_n) {
      virtual_n = physical_n = -1;
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      if (!query(QDC_ONLY_ACTIVE_PATHS, paths, modes)) {
        return;
      }
      virtual_n = physical_n = 0;
      for (auto &p : paths) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
          (is_virtual(p) ? virtual_n : physical_n)++;
        }
      }
    }

    /** Deactivate every physical path, keep the virtual one(s), move what is left to the origin, save.
     *  Returns the SetDisplayConfig result. The caller has checked a virtual display is active. */
    LONG physical_off() {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      if (!query(QDC_ALL_PATHS, paths, modes)) {
        return ERROR_GEN_FAILURE;
      }
      int virtual_n = 0;
      for (auto &p : paths) {
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE)) {
          continue;
        }
        if (is_virtual(p)) {
          virtual_n++;
          continue;
        }
        p.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
        // A deactivated path must not keep pointing at a mode, or SetDisplayConfig rejects the whole config.
        p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      }
      if (virtual_n == 0) {
        return ERROR_INVALID_STATE;  // never write a layout with no display
      }
      // The panel is usually primary at (0,0) with the virtual display beside it; dropping the panel alone
      // leaves no source at the origin, which SetDisplayConfig refuses with 87 having changed nothing.
      LONG min_x = MAXLONG, min_y = MAXLONG;
      for (auto &p : paths) {
        const UINT32 i = p.sourceInfo.modeInfoIdx;
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE) || i >= modes.size() || modes[i].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
          continue;
        }
        min_x = modes[i].sourceMode.position.x < min_x ? modes[i].sourceMode.position.x : min_x;
        min_y = modes[i].sourceMode.position.y < min_y ? modes[i].sourceMode.position.y : min_y;
      }
      if (min_x != MAXLONG && (min_x != 0 || min_y != 0)) {
        for (auto &m : modes) {
          if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            m.sourceMode.position.x -= min_x;
            m.sourceMode.position.y -= min_y;
          }
        }
      }
      // SAVED, or Windows' stored layout for this monitor set (panel active) is re-applied mid-session.
      // Safe: stored layouts are keyed per monitor SET, so this one is only chosen while the virtual
      // display exists -- and it always contains it.
      return SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    }

    /** Save the layout now active, so the stored entry is our last deliberate state (on stays on). */
    void persist_active() {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      if (query(QDC_ONLY_ACTIVE_PATHS, paths, modes)) {
        SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
      }
    }

    /** Re-sync a panel that is active but dark: flip to its smallest other mode and back. */
    void mode_cycle(const std::wstring &dev) {
      DEVMODEW cur {};
      cur.dmSize = sizeof(cur);
      if (!EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &cur)) {
        return;
      }
      DEVMODEW alt {}, m {};
      bool have_alt = false;
      m.dmSize = sizeof(m);
      for (DWORD i = 0; EnumDisplaySettingsW(dev.c_str(), i, &m); i++, m.dmSize = sizeof(m)) {
        if (m.dmBitsPerPel != cur.dmBitsPerPel ||
            (m.dmPelsWidth == cur.dmPelsWidth && m.dmPelsHeight == cur.dmPelsHeight && m.dmDisplayFrequency == cur.dmDisplayFrequency)) {
          continue;
        }
        if (!have_alt || m.dmPelsWidth * m.dmPelsHeight < alt.dmPelsWidth * alt.dmPelsHeight) {
          alt = m;
          have_alt = true;
        }
      }
      if (!have_alt) {
        return;
      }
      alt.dmSize = sizeof(alt);
      alt.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
      ChangeDisplaySettingsExW(dev.c_str(), &alt, nullptr, CDS_UPDATEREGISTRY, nullptr);
      std::this_thread::sleep_for(1200ms);
      ChangeDisplaySettingsExW(dev.c_str(), &cur, nullptr, CDS_UPDATEREGISTRY, nullptr);
    }

    /** Bring the physical monitor(s) back: EXTEND, verified; if still dark, activate each connected panel
     *  (once per monitor, preferring its active path) and re-sync it. Returns true when one is active. */
    bool physical_on() {
      SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
      std::this_thread::sleep_for(600ms);
      int virtual_n, physical_n;
      count_active(virtual_n, physical_n);
      if (physical_n > 0) {
        persist_active();
        return true;
      }
      BOOST_LOG(warning) << "[HOST-MONITOR] still dark after EXTEND - activating the physical display(s)"sv;
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      if (!query(QDC_ALL_PATHS, paths, modes)) {
        return false;
      }
      std::vector<uint64_t> done;
      std::vector<DISPLAYCONFIG_PATH_INFO> chosen;
      for (int pass = 0; pass < 2; pass++) {
        for (auto &p : paths) {
          const bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
          if (active != (pass == 0) || is_virtual(p)) {
            continue;
          }
          DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
          if (!target_name(p, tn) || tn.monitorFriendlyDeviceName[0] == 0) {
            continue;  // not connected
          }
          const uint64_t key = ((uint64_t) (uint32_t) p.targetInfo.adapterId.LowPart << 32) | p.targetInfo.id;
          bool seen = false;
          for (auto k : done) {
            seen = seen || k == key;
          }
          if (seen) {
            continue;
          }
          done.push_back(key);
          chosen.push_back(p);
        }
      }
      // One monitor per apply, as dispwake does: one path Windows rejects must not block the others.
      for (auto &want : chosen) {
        if (!query(QDC_ALL_PATHS, paths, modes)) {
          break;
        }
        for (auto &p : paths) {
          if (p.sourceInfo.id == want.sourceInfo.id && p.targetInfo.id == want.targetInfo.id &&
              p.targetInfo.adapterId.LowPart == want.targetInfo.adapterId.LowPart &&
              p.targetInfo.adapterId.HighPart == want.targetInfo.adapterId.HighPart) {
            p.flags |= DISPLAYCONFIG_PATH_ACTIVE;
          }
        }
        SetDisplayConfig((UINT32) paths.size(), paths.data(), (UINT32) modes.size(), modes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_FORCE_MODE_ENUMERATION);
        std::this_thread::sleep_for(400ms);
        const auto dev = source_gdi_name(want);
        if (!dev.empty()) {
          mode_cycle(dev);
        }
      }
      count_active(virtual_n, physical_n);
      if (physical_n > 0) {
        persist_active();
        return true;
      }
      return false;
    }

    std::mutex lock;
    std::condition_variable cv;
    bool want_off = false;
    bool worker_running = false;

    /** One thread owns every display change here, so a restore and a new session's switch-off never race. */
    void worker() {
      bool turned_off = false;    // this module switched the panel off and has not restored it
      bool refused_logged = false;
      int came_back = 0;
      auto since = std::chrono::steady_clock::now();
      std::unique_lock ul(lock);
      for (;;) {
        const bool off = want_off;
        ul.unlock();
        int virtual_n, physical_n;
        count_active(virtual_n, physical_n);
        if (off) {
          if (physical_n > 0 && virtual_n > 0) {
            if (turned_off) {
              came_back++;
              BOOST_LOG(warning) << "[HOST-MONITOR] physical display came back on during the session (" << came_back << "x) - switching it off again"sv;
            }
            const LONG r = physical_off();
            if (r == ERROR_SUCCESS) {
              BOOST_LOG(info) << "[HOST-MONITOR] physical display(s) off, virtual display kept ("sv << physical_n << " physical, "sv << virtual_n << " virtual)"sv;
              turned_off = true;
            } else {
              BOOST_LOG(warning) << "[HOST-MONITOR] switching the physical display off failed: SetDisplayConfig "sv << r;
            }
          } else if (physical_n > 0 && virtual_n == 0 && !refused_logged && std::chrono::steady_clock::now() - since > 20s) {
            // Never switch off the only display: the stream would show nothing and the host would be dark.
            BOOST_LOG(warning) << "[HOST-MONITOR] monitor off requested but no virtual display is active - physical display left ON"sv;
            refused_logged = true;
          }
        } else {
          if (turned_off) {
            const bool ok = physical_on();
            BOOST_LOG(info) << "[HOST-MONITOR] stream stopped: physical display "sv << (ok ? "restored"sv : "NOT restored (none active after the wake)"sv)
                            << " (came back on by itself "sv << came_back << "x during the session)"sv;
            turned_off = false;
          }
          refused_logged = false;
          came_back = 0;
        }
        ul.lock();
        if (want_off != off) {
          since = std::chrono::steady_clock::now();
          continue;  // changed while we worked
        }
        if (want_off) {
          // Faster until it first succeeds (the host may still be bringing the virtual display up), then a watchdog.
          cv.wait_for(ul, turned_off ? 3s : 1s);
          if (want_off != off) {
            since = std::chrono::steady_clock::now();
          }
        } else {
          cv.wait(ul, [] { return want_off; });
          since = std::chrono::steady_clock::now();
        }
      }
    }

    void set_wanted(bool off) {
      std::lock_guard lg(lock);
      want_off = off;
      if (!worker_running && off) {
        worker_running = true;
        std::thread(worker).detach();
      }
      cv.notify_all();
    }
  }  // namespace

  bool supported() {
    return true;
  }

  void session_started(bool monitor_off) {
    if (monitor_off) {
      BOOST_LOG(info) << "[HOST-MONITOR] client asked for the physical display off (umbraMonitorOff=1)"sv;
    }
    set_wanted(monitor_off);
  }

  void sessions_stopped() {
    set_wanted(false);
  }
#else
  bool supported() {
    return false;
  }

  void session_started(bool) {}

  void sessions_stopped() {}
#endif
}  // namespace host_monitor
