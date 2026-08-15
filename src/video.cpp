/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// local includes
#include "cbs.h"
#include "config.h"
#include "display_device.h"
#include "amf/amf_encoder.h"
#include "amf/amf_lifecycle.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_base.h"
#include "platform/common.h"
#include "process.h"
#include "sync.h"
#include "video.h"
#include "video_encoder_probe_policy.h"
#include "webrtc_stream.h"

#ifdef _WIN32
  #include "src/platform/windows/display.h"
  #include "src/platform/windows/display_helper_integration.h"
  #include "src/platform/windows/display_vram.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/rtx_hdr_runtime.h"
  #include "src/platform/windows/virtual_display.h"
  #include "uuid.h"

  #include <AMF/core/Context.h>
  #include <AMF/core/Debug.h>
  #include <AMF/core/Factory.h>
  #include <AMF/core/Trace.h>

  // These using-declarations must sit between the AMF headers above and
  // hwcontext_amf.h below, and must stay outside the extern "C" block. hwcontext_amf.h
  // refers to the AMF types unqualified, but the AMF headers that define them are C++
  // (namespace amf), so they cannot be pulled in with C linkage. Do not reorder.
  using amf::AMFContext;
  using amf::AMFFactory;
  using amf::AMF_MEMORY_TYPE;
  using amf::AMF_SURFACE_FORMAT;
  #include <libavutil/hwcontext_amf.h>

extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}

namespace proc {
  extern std::atomic<VDISPLAY::DRIVER_STATUS> vDisplayDriverStatus;
  void initVDisplayDriver();
}  // namespace proc
#endif

using namespace std::literals;

namespace video {

  /**
   * @brief Check if we can allow probing for the encoders.
   * @return True if there should be no issues with the probing, false if we should prevent it.
   */
  bool allow_encoder_probing() {
    return true;
  }

  namespace {
#ifdef _WIN32
    void wait_for_recent_display_apply_stability() {
      constexpr auto kFallbackSettleWindow = std::chrono::milliseconds(1500);
      constexpr auto kVerificationPollInterval = std::chrono::milliseconds(25);
      // The helper re-applies HDR at 750/2500/5500ms after APPLY when its synchronous
      // verification does not stick. Cover the 2500ms retry: past that, streaming SDR
      // beats holding a black screen, and part of the point of waiting is to avoid a
      // mid-stream SDR->HDR flip that costs some clients a full display mode-set.
      constexpr auto kHdrReadyWindow = std::chrono::milliseconds(3000);

      auto elapsed = std::chrono::milliseconds(display_helper_integration::ms_since_last_apply());

      // A bounded stream-start APPLY shares one budget with the client's
      // first-video deadline, so this wait has to come out of what is left of it
      // rather than starting a fresh window afterwards. Windows are measured
      // from APPLY completion, so the remaining budget is rebased the same way.
      // Unbounded applies (WebRTC, recovery, non-stream) keep the full windows.
      const auto stream_start_budget = display_helper_integration::remaining_stream_start_budget();
      const auto bounded_window = [&stream_start_budget](std::chrono::milliseconds window, std::chrono::milliseconds since_apply) {
        return stream_start_budget ? std::min(window, since_apply + *stream_start_budget) : window;
      };

      if (display_helper_integration::last_apply_requested_hdr()) {
        const auto output_name = display_device::map_output_name(config::get_active_output_name());
        if (platf::dxgi::is_hdr_active_for_output(output_name)) {
          return;
        }
        if (elapsed >= kHdrReadyWindow) {
          return;
        }
        const auto hdr_ready_window = bounded_window(kHdrReadyWindow, elapsed);
        if (elapsed >= hdr_ready_window) {
          BOOST_LOG(info) << "Stream-start display budget is exhausted; starting capture immediately to stay inside the client deadline.";
          return;
        }

        BOOST_LOG(info) << "Display apply requested HDR; waiting up to "
                        << (hdr_ready_window - elapsed).count()
                        << "ms for the output to report HDR before starting capture";
        while (elapsed < hdr_ready_window) {
          std::this_thread::sleep_for(std::min(kVerificationPollInterval, hdr_ready_window - elapsed));
          if (platf::dxgi::is_hdr_active_for_output(output_name)) {
            BOOST_LOG(debug) << "Display output reported HDR active after "
                             << display_helper_integration::ms_since_last_apply()
                             << "ms; starting capture.";
            return;
          }
          elapsed = std::chrono::milliseconds(display_helper_integration::ms_since_last_apply());
        }

        BOOST_LOG(warning) << "Display apply requested HDR but the output did not report HDR within "
                           << hdr_ready_window.count() << "ms; starting capture in SDR.";
        return;
      }

      if (elapsed >= kFallbackSettleWindow || display_helper_integration::last_apply_is_capture_stable()) {
        return;
      }
      const auto settle_window = bounded_window(kFallbackSettleWindow, elapsed);
      if (elapsed >= settle_window) {
        BOOST_LOG(info) << "Stream-start display budget is exhausted; starting capture immediately to stay inside the client deadline.";
        return;
      }

      BOOST_LOG(info) << "Display topology recently changed; waiting up to "
                      << (settle_window - elapsed).count()
                      << "ms for helper verification or display-settle fallback";
      while (elapsed < settle_window) {
        if (display_helper_integration::last_apply_is_capture_stable()) {
          BOOST_LOG(debug) << "Display topology verification completed; ending settle wait early.";
          return;
        }
        std::this_thread::sleep_for(std::min(kVerificationPollInterval, settle_window - elapsed));
        elapsed = std::chrono::milliseconds(display_helper_integration::ms_since_last_apply());
      }
    }

    bool should_prefer_virtual_display() {
      if (platf::is_lock_screen_active() && VDISPLAY::has_active_physical_display()) {
        return false;
      }

      if (auto runtime_output_name = config::runtime_output_name_override()) {
        return !runtime_output_name->empty() && VDISPLAY::is_virtual_display_output(*runtime_output_name);
      }

      if (!VDISPLAY::isVirtualDisplayDriverInstalled()) {
        return false;
      }

      auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      if (virtual_displays.empty()) {
        return false;
      }

      const auto active_output_name = config::get_active_output_name();
      const bool runtime_targets_virtual =
        !active_output_name.empty() &&
        VDISPLAY::is_virtual_display_output(active_output_name);
      if (runtime_targets_virtual) {
        return true;
      }

      const bool explicit_virtual = (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::per_client || config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::shared);
      const bool auto_activate = config::video.dd.activate_virtual_display;
      if (explicit_virtual || auto_activate) {
        return true;
      }

      const bool any_active = std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::VirtualDisplayInfo &info) {
          return info.is_active;
        }
      );

      if (!any_active) {
        return false;
      }

      if (!VDISPLAY::has_active_physical_display()) {
        return true;
      }

      return false;
    }

#ifdef _WIN32
    bool is_d3d_capture_image(const std::shared_ptr<platf::img_t> &img) {
      return dynamic_cast<platf::dxgi::img_d3d_t *>(img.get()) != nullptr;
    }
#endif

    // Serializes destruction of encode sessions (and, on Windows, the async release of shared
    // D3D capture surfaces). When two clients share one capture target, a capture reinit makes
    // both video threads tear down their NVENC session + D3D11 device at the same instant. Both
    // devices have the same shared capture textures open, and the NVIDIA UMD's cross-device
    // shared-resource dependency cleanup (DestroyDriverInstance) is not safe against a concurrent
    // teardown of the other device: it faults walking freed dependency entries. Funnel all
    // encoder teardown through this mutex so only one device is ever mid-destruction.
    std::mutex encode_session_teardown_mutex;
    // A host restart is the recovery boundary after a vendor call times out.
    // Detached watchdog workers can outlive ordinary static destruction during
    // process shutdown. Deliberately give the runtime fence process lifetime.
    auto &native_amf_lifecycle_gate = *new amf::lifecycle::native_runtime_gate_t();

#ifdef _WIN32
    void release_d3d_capture_images_async(std::vector<std::shared_ptr<platf::img_t>> images) {
      if (images.empty()) {
        return;
      }

      try {
        std::thread {[images = std::move(images)]() mutable {
          platf::set_thread_name("video::d3dRelease");
          std::lock_guard lg {encode_session_teardown_mutex};
          images.clear();
        }}.detach();
      } catch (const std::system_error &err) {
        BOOST_LOG(warning) << "Failed to start async D3D image release thread: " << err.what();
      }
    }
#endif

    std::optional<std::string> active_virtual_display_dxgi_name() {
      auto virtual_displays = VDISPLAY::enumerateVirtualDisplays();
      auto is_dxgi_display_name = [](const std::string &name) {
        static const std::string prefix = "\\\\.\\DISPLAY";
        if (name.size() < prefix.size()) {
          return false;
        }

        for (size_t i = 0; i < prefix.size(); ++i) {
          if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
          }
        }
        return true;
      };
      auto map_to_dxgi_name = [](const std::wstring &name) -> std::optional<std::string> {
        if (name.empty()) {
          return std::nullopt;
        }

        const auto mapped = display_device::map_output_name(platf::to_utf8(name));
        if (mapped.empty()) {
          return std::nullopt;
        }
        return mapped;
      };

      for (const auto &info : virtual_displays) {
        if (info.is_active) {
          if (auto mapped = map_to_dxgi_name(info.device_name)) {
            if (is_dxgi_display_name(*mapped)) {
              return mapped;
            }
          }
        }
      }

      for (const auto &info : virtual_displays) {
        if (auto mapped = map_to_dxgi_name(info.device_name)) {
          if (is_dxgi_display_name(*mapped)) {
            return mapped;
          }
        }
      }

      return std::nullopt;
    }
#endif

    bool ensure_virtual_display_ready(std::vector<std::string> &display_names, int &display_index) {
#ifdef _WIN32
      static thread_local std::chrono::steady_clock::time_point wait_start {};
      static thread_local std::string pending_virtual_name;

      if (display_names.empty()) {
        display_index = 0;
        wait_start = {};
        pending_virtual_name.clear();
        return false;
      }

      display_index = std::clamp(display_index, 0, static_cast<int>(display_names.size()) - 1);

      if (!should_prefer_virtual_display()) {
        wait_start = {};
        pending_virtual_name.clear();
        return true;
      }

      if (auto desired_name = active_virtual_display_dxgi_name()) {
        for (int i = 0; i < static_cast<int>(display_names.size()); ++i) {
          if (boost::iequals(display_names[i], *desired_name)) {
            display_index = i;
            wait_start = {};
            pending_virtual_name.clear();
            return true;
          }
        }
        pending_virtual_name = *desired_name;
      } else {
        pending_virtual_name.clear();
      }

      const auto now = std::chrono::steady_clock::now();
      if (wait_start == std::chrono::steady_clock::time_point {}) {
        wait_start = now;
        std::ostringstream available;
        for (size_t i = 0; i < display_names.size(); ++i) {
          if (i) {
            available << ", ";
          }
          available << display_names[i];
        }
        BOOST_LOG(debug) << "Capture waiting for virtual display to become ready. desired='"
                         << (pending_virtual_name.empty() ? std::string("(unresolved)") : pending_virtual_name)
                         << "' active_output='" << config::get_active_output_name()
                         << "' available=[" << available.str() << "]";
      }

      constexpr auto max_wait = std::chrono::seconds(3);
      if (now - wait_start >= max_wait) {
        BOOST_LOG(debug) << "Capture virtual display wait timed out after "
                         << std::chrono::duration_cast<std::chrono::milliseconds>(max_wait).count()
                         << "ms. desired='" << (pending_virtual_name.empty() ? std::string("(unresolved)") : pending_virtual_name)
                         << "' active_output='" << config::get_active_output_name() << "'.";
        wait_start = {};
        pending_virtual_name.clear();
        return true;
      }

      return false;
#else
      if (display_names.empty()) {
        display_index = 0;
        return false;
      }

      display_index = std::clamp(display_index, 0, static_cast<int>(display_names.size()) - 1);
      return true;
#endif
    }

    bool is_placeholder_capture_image(const platf::img_t &img) {
#ifdef _WIN32
      if (auto d3d_img = dynamic_cast<const platf::dxgi::img_d3d_t *>(&img)) {
        return d3d_img->dummy;
      }
#endif

      return false;
    }

    struct encode_bootstrap_state_t {
      bool allow_placeholder_before_first_real = false;
      bool placeholder_encoded = false;
      bool real_frame_seen = false;
      bool current_input_placeholder = true;

      bool should_encode_placeholder() const {
        return !real_frame_seen && current_input_placeholder &&
               allow_placeholder_before_first_real && !placeholder_encoded;
      }
    };

    struct probe_cache_key_t {
      std::string encoder_configuration;
      std::string adapter_identity;
      std::string adapter_identity_source;
      bool adapter_identity_resolved = false;

      bool operator==(const probe_cache_key_t &other) const {
        return encoder_configuration == other.encoder_configuration &&
               adapter_identity == other.adapter_identity;
      }
    };

    struct probe_adapter_identity_t {
      std::string identity;
      std::string source;
      bool resolved = false;
    };

    struct probe_target_t {
      std::string display_name;
      std::optional<platf::adapter_id_t> required_adapter;
      probe_adapter_identity_t adapter_identity;
    };

    struct EncoderProbeCacheState {
      std::mutex mutex;
      std::optional<probe_cache_key_t> cache_key;
      std::optional<probe_cache_key_t> attempted_cache_key;
      bool valid = false;
      bool hdr_supported = false;
      bool hevc_passed = false;
      bool hevc_hdr_supported = false;
      bool av1_passed = false;
      bool av1_hdr_supported = false;
      advertised_encoder_capabilities_t advertised_capabilities;
#ifdef _WIN32
      std::optional<LUID> pending_virtual_display_adapter_hint;
      std::uint64_t pending_virtual_display_adapter_hint_lease = 0;
      std::uint64_t next_pending_virtual_display_adapter_hint_lease = 0;
      bool pending_virtual_display_adapter_hint_ready_for_verification = false;
#endif

      // Track failed probe attempts per cache key for diagnostics.
      std::optional<probe_cache_key_t> failure_cache_key;
      int failure_count = 0;
    };

    EncoderProbeCacheState &encoder_probe_cache_state() {
      static EncoderProbeCacheState state;
      return state;
    }

#ifdef _WIN32
    platf::adapter_id_t adapter_id_from_luid(const LUID &luid) {
      return platf::adapter_id_t {
        .high_part = luid.HighPart,
        .low_part = luid.LowPart,
      };
    }

    std::string adapter_cache_identity(const platf::adapter_id_t &adapter_id) {
      std::ostringstream oss;
      oss << "luid=" << adapter_id.high_part << ':' << adapter_id.low_part;
      return oss.str();
    }

    std::string luid_cache_identity(const LUID &luid) {
      return adapter_cache_identity(adapter_id_from_luid(luid));
    }

#endif

    probe_target_t resolve_probe_target() {
#ifdef _WIN32
      const auto active_output = config::get_active_output_name();
      const auto mapped_output = display_device::map_output_name(active_output);
      const bool mapped_output_is_active =
        !mapped_output.empty() && display_device::output_is_active(mapped_output);
      const auto current_wgc_identity = [&]() -> std::optional<platf::dxgi::wgc_adapter_identity_t> {
        const auto identity = platf::dxgi::get_last_wgc_adapter_identity();
        if (!identity ||
            !mapped_output_is_active ||
            !boost::iequals(identity->output_name, mapped_output)) {
          return std::nullopt;
        }
        return identity;
      }();
      const auto mapped_output_matches_adapter = [&](const LUID &adapter_luid) {
        if (!mapped_output_is_active) {
          return false;
        }
        const auto output_adapter = platf::resolve_output_adapter(mapped_output);
        return output_adapter && platf::adapter_luid_equal(*output_adapter.luid, adapter_luid);
      };

      std::optional<LUID> pending_virtual_display_adapter_hint;
      bool pending_virtual_display_adapter_hint_ready_for_verification = false;
      {
        auto &state = encoder_probe_cache_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        pending_virtual_display_adapter_hint =
          state.pending_virtual_display_adapter_hint;
        pending_virtual_display_adapter_hint_ready_for_verification =
          state.pending_virtual_display_adapter_hint_ready_for_verification;
      }
      if (pending_virtual_display_adapter_hint) {
        const auto pending_adapter = adapter_id_from_luid(*pending_virtual_display_adapter_hint);
        std::optional<LUID> observed_adapter;
        std::string_view observed_source;
        if (current_wgc_identity) {
          observed_adapter = current_wgc_identity->luid;
          observed_source = "wgc";
        } else {
          const auto output_adapter = platf::resolve_output_adapter(mapped_output);
          if (output_adapter) {
            observed_adapter = *output_adapter.luid;
            observed_source = "dxgi";
          }
        }

        // A pending virtual-display adapter is a lookup expectation. It may
        // reuse only a positive entry that was previously produced by an
        // actual probe on that adapter. The observed output remains diagnostic
        // here because it may still be the physical display being replaced.
        if (observed_adapter) {
          const bool matches_pending = platf::adapter_luid_equal(
            *pending_virtual_display_adapter_hint,
            *observed_adapter
          );
          const bool reuse_observed_output =
            matches_pending && mapped_output_matches_adapter(*pending_virtual_display_adapter_hint);
          std::string probe_output;
          if (reuse_observed_output) {
            probe_output = mapped_output;
          } else if (const auto scoped_output = platf::dxgi::resolve_automatic_capture_output(
                       platf::mem_type_e::dxgi,
                       pending_adapter
                     )) {
            probe_output = scoped_output->output_name;
          }
          return probe_target_t {
            .display_name = std::move(probe_output),
            .required_adapter = pending_adapter,
            .adapter_identity = probe_adapter_identity_t {
              .identity = luid_cache_identity(*pending_virtual_display_adapter_hint),
              .source =
                "pending-virtual-display-lookup-" + std::string(observed_source) +
                (reuse_observed_output ? "-active-match" : "-automatic-output"),
              .resolved = true,
            },
          };
        }

        // This identity is an expectation for cache lookup only. Successful
        // producers replace it with the adapter returned by the initialized
        // probe display before update_probe_cache() is called.
        std::string probe_output;
        if (const auto scoped_output = platf::dxgi::resolve_automatic_capture_output(
              platf::mem_type_e::dxgi,
              pending_adapter
            )) {
          probe_output = scoped_output->output_name;
        }
        return probe_target_t {
          .display_name = std::move(probe_output),
          .required_adapter = pending_adapter,
          .adapter_identity = probe_adapter_identity_t {
            .identity = luid_cache_identity(*pending_virtual_display_adapter_hint),
            .source = pending_virtual_display_adapter_hint_ready_for_verification ?
                        "pending-virtual-display-adapter-awaiting-observation" :
                        "pending-virtual-display-adapter-before-publication",
            .resolved = true,
          },
        };
      }

      const bool adapter_is_configured =
        !config::video.adapter_name.empty() ||
        !config::video.adapter_pnp_id.empty();
      if (adapter_is_configured) {
        const auto configured_adapter = platf::resolve_adapter(
          config::video.adapter_name,
          config::video.adapter_pnp_id
        );
        if (configured_adapter) {
          const auto configured_adapter_id = adapter_id_from_luid(*configured_adapter.luid);
          const bool preferred_output_matches_adapter =
            mapped_output_matches_adapter(*configured_adapter.luid);
          const bool wgc_mismatches_configured_adapter =
            current_wgc_identity &&
            !platf::adapter_luid_equal(*configured_adapter.luid, current_wgc_identity->luid);
          std::string probe_output = preferred_output_matches_adapter ? mapped_output : std::string {};
          if (probe_output.empty()) {
            if (const auto scoped_output = platf::dxgi::resolve_automatic_capture_output(
                  platf::mem_type_e::dxgi,
                  configured_adapter_id
                )) {
              probe_output = scoped_output->output_name;
            }
          }
          return probe_target_t {
            .display_name = std::move(probe_output),
            .required_adapter = configured_adapter_id,
            .adapter_identity = probe_adapter_identity_t {
              .identity = luid_cache_identity(*configured_adapter.luid),
              .source = preferred_output_matches_adapter ?
                          (wgc_mismatches_configured_adapter ?
                             "configured-adapter-active-output-wgc-mismatch" :
                             "configured-adapter-active-output") :
                          (wgc_mismatches_configured_adapter ?
                             "configured-adapter-automatic-output-wgc-mismatch" :
                             "configured-adapter-automatic-output"),
              .resolved = true,
            },
          };
        }
        return probe_target_t {
          .display_name = mapped_output,
          .adapter_identity = probe_adapter_identity_t {
            .identity = "unresolved-configured-adapter=" +
                        std::string(platf::adapter_resolution_status_name(configured_adapter.status)),
            .source = "configured-adapter-unresolved",
            .resolved = false,
          },
        };
      }

      if (current_wgc_identity) {
        return probe_target_t {
          .display_name = mapped_output,
          .required_adapter = adapter_id_from_luid(current_wgc_identity->luid),
          .adapter_identity = probe_adapter_identity_t {
            .identity = luid_cache_identity(current_wgc_identity->luid),
            .source = "current-output-wgc",
            .resolved = true,
          },
        };
      }

      if (!mapped_output.empty()) {
        const auto output_adapter = platf::resolve_output_adapter(mapped_output);
        if (mapped_output_is_active && output_adapter) {
          return probe_target_t {
            .display_name = mapped_output,
            .required_adapter = adapter_id_from_luid(*output_adapter.luid),
            .adapter_identity = probe_adapter_identity_t {
              .identity = luid_cache_identity(*output_adapter.luid),
              .source = "current-output-dxgi",
              .resolved = true,
            },
          };
        }
      }

      // Materialize Automatic using the same ordered, compatibility-tested
      // output enumeration used by normal capture. Passing both values into
      // the probe prevents WGC and DDX from choosing different monitors.
      const auto automatic_output = platf::dxgi::resolve_automatic_capture_output(platf::mem_type_e::dxgi);
      if (automatic_output) {
        return probe_target_t {
          .display_name = automatic_output->output_name,
          .required_adapter = automatic_output->adapter_id,
          .adapter_identity = probe_adapter_identity_t {
            .identity = adapter_cache_identity(automatic_output->adapter_id),
            .source = "automatic-output-dxgi",
            .resolved = true,
          },
        };
      }

      return probe_target_t {
        .adapter_identity = probe_adapter_identity_t {
          .identity = "unresolved-automatic-adapter=not-found|output=",
          .source = "automatic-adapter-unresolved",
          .resolved = false,
        },
      };
#else
      return probe_target_t {
        .adapter_identity = probe_adapter_identity_t {
          .identity = "platform-default",
          .source = "platform-default",
          .resolved = true,
        },
      };
#endif
    }

    probe_cache_key_t build_probe_cache_key(const probe_target_t *probe_target = nullptr) {
      std::ostringstream oss;
      // Keep encoder configuration separate from the exact selected adapter.
      // Output identifiers and the whole-machine GPU inventory are deliberately
      // excluded: neither changes the selected GPU's codec capability.
      auto append_optional = [&](std::string_view name, const std::optional<int> &value) {
        oss << '|' << name << '=';
        if (value) {
          oss << *value;
        } else {
          oss << "auto";
        }
      };
      oss << "encoder=" << config::video.encoder
          << "|hevc=" << config::video.hevc_mode
          << "|av1=" << config::video.av1_mode
          << "|amd_coder=" << config::video.amd.amd_coder
          << "|amd_ltr=" << config::video.amd.amd_ltr_frames
          << "|amd_queue=" << config::video.amd.amd_input_queue_size;
      append_optional("amd_usage_h264", config::video.amd.amd_usage_h264);
      append_optional("amd_usage_hevc", config::video.amd.amd_usage_hevc);
      append_optional("amd_usage_av1", config::video.amd.amd_usage_av1);
      append_optional("amd_rc_h264", config::video.amd.amd_rc_h264);
      append_optional("amd_rc_hevc", config::video.amd.amd_rc_hevc);
      append_optional("amd_rc_av1", config::video.amd.amd_rc_av1);
      append_optional("amd_quality_h264", config::video.amd.amd_quality_h264);
      append_optional("amd_quality_hevc", config::video.amd.amd_quality_hevc);
      append_optional("amd_quality_av1", config::video.amd.amd_quality_av1);
      append_optional("amd_qvbr_quality", config::video.amd.amd_qvbr_quality_level);
      append_optional("amd_vbaq", config::video.amd.amd_vbaq);
      append_optional("amd_preanalysis", config::video.amd.amd_preanalysis);
      append_optional("amd_enforce_hrd", config::video.amd.amd_enforce_hrd);
      append_optional("amd_lowlatency", config::video.amd.amd_lowlatency_mode);
      append_optional("amd_motion_boost", config::video.amd.amd_high_motion_quality_boost);
      append_optional("amd_sav", config::video.amd.amd_smart_access_video);
      append_optional("amd_av1_screen", config::video.amd.amd_av1_screen_content);
      append_optional("amd_av1_latency", config::video.amd.amd_av1_latency_mode);
      // Quarantine changes encoder selection, so it belongs in the key. Once the
      // gate latches, `amdvce` can no longer build a session; a cached success
      // from before the quarantine would keep handing back `chosen_encoder =
      // &amdvce` and every later stream would end before its first packet, even
      // though the software encoder would have validated. Re-key so the next
      // probe re-runs and selection can degrade to software as designed.
      oss << "|amf_quarantined=" << (native_amf_lifecycle_gate.is_quarantined() ? 1 : 0);
      const auto adapter_identity = probe_target ?
                                      probe_target->adapter_identity :
                                      resolve_probe_target().adapter_identity;
      return probe_cache_key_t {
        .encoder_configuration = oss.str(),
        .adapter_identity = adapter_identity.identity,
        .adapter_identity_source = adapter_identity.source,
        .adapter_identity_resolved = adapter_identity.resolved,
      };
    }

    bool probe_cache_matches(const probe_cache_key_t &key, bool want_hdr, bool want_hevc, bool want_hevc_hdr, bool want_av1, bool want_av1_hdr) {
      auto &state = encoder_probe_cache_state();
      std::lock_guard<std::mutex> lock(state.mutex);

      const auto policy_key = encoder_probe_policy::cache_key_t {
        .encoder_configuration = key.encoder_configuration,
        .adapter_identity = key.adapter_identity,
        .adapter_identity_resolved = key.adapter_identity_resolved,
      };
      const auto policy_cached_key = state.cache_key ?
                                       std::optional<encoder_probe_policy::cache_key_t> {
                                         encoder_probe_policy::cache_key_t {
                                           .encoder_configuration = state.cache_key->encoder_configuration,
                                           .adapter_identity = state.cache_key->adapter_identity,
                                           .adapter_identity_resolved = state.cache_key->adapter_identity_resolved,
                                         }
                                       } :
                                       std::nullopt;

      // Check if we have a valid cached success
      if (state.valid &&
          encoder_probe_policy::cache_key_matches(policy_key, policy_cached_key) &&
          (!want_hdr || state.hdr_supported)) {
        const bool hevc_supported = state.hevc_passed && (!want_hevc_hdr || state.hevc_hdr_supported);
        const bool av1_supported = state.av1_passed && (!want_av1_hdr || state.av1_hdr_supported);

        if ((want_hevc && !hevc_supported) || (want_av1 && !av1_supported)) {
          // Never trust a cached negative codec result; force a fresh probe.
          return false;
        }

        return true;
      }

      return false;
    }

    void update_probe_cache(
      const probe_cache_key_t &key,
      const bool success,
      const bool hdr_supported,
      const bool hevc_passed,
      const bool hevc_hdr_supported,
      const bool av1_passed,
      const bool av1_hdr_supported,
      const advertised_encoder_capabilities_t &advertised_capabilities = {}
    ) {
      auto &state = encoder_probe_cache_state();
      std::lock_guard<std::mutex> lock(state.mutex);
      if (success) {
        if (!key.adapter_identity_resolved) {
          state.valid = false;
          state.cache_key.reset();
          state.hdr_supported = false;
          state.hevc_passed = false;
          state.hevc_hdr_supported = false;
          state.av1_passed = false;
          state.av1_hdr_supported = false;
          state.advertised_capabilities = {};
          BOOST_LOG(warning)
            << "Encoder probe succeeded but its effective adapter identity is unresolved; "
               "refusing to cache positive capabilities (identity='"
            << key.adapter_identity << "', source="
            << key.adapter_identity_source << ").";
          return;
        }
        state.cache_key = key;
        state.valid = true;
        state.hdr_supported = hdr_supported;
        state.hevc_passed = hevc_passed;
        state.hevc_hdr_supported = hevc_hdr_supported;
        state.av1_passed = av1_passed;
        state.av1_hdr_supported = av1_hdr_supported;
        state.advertised_capabilities = advertised_capabilities;
        // Clear failure tracking on success
        state.failure_cache_key.reset();
        state.failure_count = 0;
      } else {
        state.valid = false;
        state.cache_key.reset();
        state.hdr_supported = false;
        state.hevc_passed = false;
        state.hevc_hdr_supported = false;
        state.av1_passed = false;
        state.av1_hdr_supported = false;
        state.advertised_capabilities = {};

        // Track failures, but never permanently lock out future probes.
        if (state.failure_cache_key && *state.failure_cache_key == key) {
          state.failure_count++;
        } else {
          state.failure_cache_key = key;
          state.failure_count = 1;
        }
        BOOST_LOG(warning) << "Encoder probe failed (attempt " << state.failure_count
                           << " for this configuration), will retry on next attempt";
      }
    }

    void mark_probe_attempted(const probe_cache_key_t &key) {
      auto &state = encoder_probe_cache_state();
      std::lock_guard<std::mutex> lock(state.mutex);
      state.attempted_cache_key = key;
    }

    void log_probe_cache_miss(const probe_cache_key_t &current_key) {
      auto &state = encoder_probe_cache_state();
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.valid || !state.cache_key) {
        BOOST_LOG(debug)
          << "Encoder capability cache miss: no successful probe is cached; current_adapter='"
          << current_key.adapter_identity << "' source="
          << current_key.adapter_identity_source << '.';
        return;
      }

      BOOST_LOG(debug)
        << "Encoder capability cache miss: configuration_changed="
        << (state.cache_key->encoder_configuration != current_key.encoder_configuration)
        << ", adapter_changed="
        << (state.cache_key->adapter_identity != current_key.adapter_identity)
        << ", cached_adapter='" << state.cache_key->adapter_identity
        << "', current_adapter='" << current_key.adapter_identity
        << "', current_source=" << current_key.adapter_identity_source << '.';
    }
  }  // namespace

  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  void free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);
  }

  namespace nv {

    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
#ifdef SUNSHINE_BUILD_VULKAN
  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
#endif

  class avcodec_software_encode_device_t: public platf::avcodec_encode_device_t {
  public:
    int convert(platf::img_t &img) override {
      // If we need to add aspect ratio padding, we need to scale into an intermediate output buffer
      bool requires_padding = (sw_frame->width != sws_output_frame->width || sw_frame->height != sws_output_frame->height);

      // Setup the input frame using the caller's img_t
      sws_input_frame->data[0] = img.data;
      sws_input_frame->linesize[0] = img.row_pitch;

      // Perform color conversion and scaling to the final size
      auto status = sws_scale_frame(sws.get(), requires_padding ? sws_output_frame.get() : sw_frame.get(), sws_input_frame.get());
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Couldn't scale frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      // If we require aspect ratio padding, copy the output frame into the final padded frame
      if (requires_padding) {
        auto fmt_desc = av_pix_fmt_desc_get((AVPixelFormat) sws_output_frame->format);
        auto planes = av_pix_fmt_count_planes((AVPixelFormat) sws_output_frame->format);
        for (int plane = 0; plane < planes; plane++) {
          auto shift_h = plane == 0 ? 0 : fmt_desc->log2_chroma_h;
          auto shift_w = plane == 0 ? 0 : fmt_desc->log2_chroma_w;
          auto offset = ((offsetW >> shift_w) * fmt_desc->comp[plane].step) + (offsetH >> shift_h) * sw_frame->linesize[plane];

          // Copy line-by-line to preserve leading padding for each row
          for (int line = 0; line < sws_output_frame->height >> shift_h; line++) {
            memcpy(sw_frame->data[plane] + offset + (line * sw_frame->linesize[plane]), sws_output_frame->data[plane] + (line * sws_output_frame->linesize[plane]), (size_t) (sws_output_frame->width >> shift_w) * fmt_desc->comp[plane].step);
          }
        }
      }

      // If frame is not a software frame, it means we still need to transfer from main memory
      // to vram memory
      if (frame->hw_frames_ctx) {
        auto status = av_hwframe_transfer_data(frame, sw_frame.get(), 0);
        if (status < 0) {
          char string[AV_ERROR_MAX_STRING_SIZE];
          BOOST_LOG(error) << "Failed to transfer image data to hardware frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
          return -1;
        }
      }

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->frame = frame;

      // If it's a hwframe, allocate buffers for hardware
      if (hw_frames_ctx) {
        hw_frame.reset(frame);

        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0)) {
          return -1;
        }
      } else {
        sw_frame.reset(frame);
      }

      return 0;
    }

    void apply_colorspace() override {
      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);
      sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 0, sws_getCoefficients(avcodec_colorspace.software_format), avcodec_colorspace.range - 1, 0, 1 << 16, 1 << 16);
    }

    /**
     * When preserving aspect ratio, ensure that padding is black
     */
    void prefill() {
      auto frame = sw_frame ? sw_frame.get() : this->frame;
      av_frame_get_buffer(frame, 0);
      av_frame_make_writable(frame);
      ptrdiff_t linesize[4] = {frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3]};
      av_image_fill_black(frame->data, linesize, (AVPixelFormat) frame->format, frame->color_range, frame->width, frame->height);
    }

    int init(int in_width, int in_height, AVFrame *frame, AVPixelFormat format, bool hardware) {
      // If the device used is hardware, yet the image resides on main memory
      if (hardware) {
        sw_frame.reset(av_frame_alloc());

        sw_frame->width = frame->width;
        sw_frame->height = frame->height;
        sw_frame->format = format;
      } else {
        this->frame = frame;
      }

      // Fill aspect ratio padding in the destination frame
      prefill();

      auto out_width = frame->width;
      auto out_height = frame->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf((float) out_width / in_width, (float) out_height / in_height);
      out_width = in_width * scalar;
      out_height = in_height * scalar;

      sws_input_frame.reset(av_frame_alloc());
      sws_input_frame->width = in_width;
      sws_input_frame->height = in_height;
      sws_input_frame->format = AV_PIX_FMT_BGR0;

      sws_output_frame.reset(av_frame_alloc());
      sws_output_frame->width = out_width;
      sws_output_frame->height = out_height;
      sws_output_frame->format = format;

      // Result is always positive
      offsetW = (frame->width - out_width) / 2;
      offsetH = (frame->height - out_height) / 2;

      sws.reset(sws_alloc_context());
      if (!sws) {
        return -1;
      }

      AVDictionary *options {nullptr};
      av_dict_set_int(&options, "srcw", sws_input_frame->width, 0);
      av_dict_set_int(&options, "srch", sws_input_frame->height, 0);
      av_dict_set_int(&options, "src_format", sws_input_frame->format, 0);
      av_dict_set_int(&options, "dstw", sws_output_frame->width, 0);
      av_dict_set_int(&options, "dsth", sws_output_frame->height, 0);
      av_dict_set_int(&options, "dst_format", sws_output_frame->format, 0);
      av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
      av_dict_set_int(&options, "threads", config::video.min_threads, 0);

      auto status = av_opt_set_dict(sws.get(), &options);
      av_dict_free(&options);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to set SWS options: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      status = sws_init_context(sws.get(), nullptr, nullptr);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to initialize SWS: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      return 0;
    }

    // Store ownership when frame is hw_frame
    avcodec_frame_t hw_frame;

    avcodec_frame_t sw_frame;
    avcodec_frame_t sws_input_frame;
    avcodec_frame_t sws_output_frame;
    sws_t sws;

    // Offset of input image to output frame in pixels
    int offsetW;
    int offsetH;
  };

  enum flag_e : uint32_t {
    DEFAULT = 0,  ///< Default flags
    PARALLEL_ENCODING = 1 << 1,  ///< Capture and encoding can run concurrently on separate threads
    H264_ONLY = 1 << 2,  ///< When HEVC is too heavy
    LIMITED_GOP_SIZE = 1 << 3,  ///< Some encoders don't like it when you have an infinite GOP_SIZE. e.g. VAAPI
    SINGLE_SLICE_ONLY = 1 << 4,  ///< Never use multiple slices. Older intel iGPU's ruin it for everyone else
    CBR_WITH_VBR = 1 << 5,  ///< Use a VBR rate control mode to simulate CBR
    RELAXED_COMPLIANCE = 1 << 6,  ///< Use FF_COMPLIANCE_UNOFFICIAL compliance mode
    NO_RC_BUF_LIMIT = 1 << 7,  ///< Don't set rc_buffer_size
    REF_FRAMES_INVALIDATION = 1 << 8,  ///< Support reference frames invalidation
    ALWAYS_REPROBE = 1 << 9,  ///< This is an encoder of last resort and we want to aggressively probe for a better one
    YUV444_SUPPORT = 1 << 10,  ///< Encoder may support 4:4:4 chroma sampling depending on hardware
    ASYNC_TEARDOWN = 1 << 11,  ///< Encoder supports async teardown on a different thread
    FIXED_GOP_SIZE = 1 << 12,  ///< Use fixed small GOP size (encoder doesn't support on-demand IDR frames)
  };

  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;

    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject):
        avcodec_ctx {std::move(avcodec_ctx)},
        device {std::move(encode_device)},
        inject {inject} {
    }

    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;

    ~avcodec_encode_session_t() {
      // Don't drain the encoder before freeing it. The drained packets are discarded anyway,
      // and FFmpeg's AMF backend waits on the driver without a deadline while draining — a
      // wedged AMF runtime turns that into a permanent hang on the session teardown path
      // (vibeshine#187). avcodec_free_context() is documented to be safe without a drain.

      // Order matters here because the context relies on the hwdevice still being valid
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    avcodec_encode_session_t &operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      sps = std::move(other.sps);
      vps = std::move(other.vps);

      inject = other.inject;

      return *this;
    }

    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    void request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    void request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    void set_hdr_metadata(const SS_HDR_METADATA &metadata) override {
      if (!device || !device->frame) {
        return;
      }

      auto *frame = device->frame;
      auto *mdm_side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
      auto *mdm = mdm_side_data ?
                    reinterpret_cast<AVMasteringDisplayMetadata *>(mdm_side_data->data) :
                    av_mastering_display_metadata_create_side_data(frame);
      if (mdm) {
        for (int primary = 0; primary < 3; ++primary) {
          mdm->display_primaries[primary][0] = av_make_q(metadata.displayPrimaries[primary].x, 50000);
          mdm->display_primaries[primary][1] = av_make_q(metadata.displayPrimaries[primary].y, 50000);
        }
        mdm->white_point[0] = av_make_q(metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(metadata.whitePoint.y, 50000);
        mdm->min_luminance = av_make_q(metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(metadata.maxDisplayLuminance, 1);
        mdm->has_luminance = metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = metadata.displayPrimaries[0].x != 0 ? 1 : 0;
      }

      auto *clm_side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
      auto *clm = clm_side_data ?
                    reinterpret_cast<AVContentLightMetadata *>(clm_side_data->data) :
                    av_content_light_metadata_create_side_data(frame);
      if (clm) {
        clm->MaxCLL = metadata.maxContentLightLevel;
        clm->MaxFALL = metadata.maxFrameAverageLightLevel;
      }
    }

    void restore_display_lease_after_initialization(std::shared_ptr<platf::display_t> display) {
      if (device) device->restore_display_lease_after_initialization(std::move(display));
    }

    std::shared_ptr<platf::display_t> release_display_lease_for_driver_work() {
      return device ? device->release_display_lease_for_initialization() : nullptr;
    }

    avcodec_ctx_t avcodec_ctx;
    std::unique_ptr<platf::avcodec_encode_device_t> device;

    std::vector<packet_raw_t::replace_t> replacements;

    cbs::nal_t sps;
    cbs::nal_t vps;

    // inject sps/vps data into idr pictures
    int inject;
  };

  class nvenc_encode_session_t: public encode_session_t {
  public:
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    void request_idr_frame() override {
      force_idr = true;
    }

    void request_normal_frame() override {
      force_idr = false;
    }

    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) {
        return;
      }

      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    bool set_bitrate(int bitrate_kbps) override {
      if (!device || !device->nvenc) {
        return false;
      }
      return device->nvenc->set_bitrate(bitrate_kbps);
    }

    void set_hdr_metadata(const SS_HDR_METADATA &metadata) override {
      if (!device || !device->nvenc) {
        return;
      }
      device->hdr_metadata = metadata;
      device->hdr_metadata_valid = true;
      device->nvenc->set_hdr_metadata(metadata);
    }

    nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) {
        return {};
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    bool force_idr = false;
  };

  class amf_encode_session_t: public encode_session_t {
  public:
    amf_encode_session_t(std::unique_ptr<platf::amf_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      const auto result = device->convert(img);
      if (result == 0) {
        fresh_conversion_pending = true;
      }
      return result;
    }

    void request_idr_frame() override {
      force_idr = true;
    }

    void request_normal_frame() override {
      // encode_frames() clears force_idr only after AMF accepts the input. The
      // generic loops call this after every nonfatal attempt, including
      // backpressure drops, so clearing it here would lose recovery IDRs.
    }

    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->amf) {
        return;
      }

      if (!device->amf->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    bool set_bitrate(int bitrate_kbps) override {
      return device && device->amf && device->amf->set_bitrate(bitrate_kbps);
    }

    // Without this the live RTX-HDR refresh path is a no-op on native AMF: the
    // control-channel SS_HDR_METADATA and the forced IDR still go out, but the
    // bitstream keeps carrying the session-start mastering-display values, so the
    // client tone-maps against a peak the stream never declares. The legacy
    // FFmpeg AMD path honoured this, and INPUT_HDR_METADATA is a per-submit
    // component property, so it is writable after Init.
    void set_hdr_metadata(const SS_HDR_METADATA &metadata) override {
      if (!device || !device->amf) {
        return;
      }

      device->hdr_metadata = metadata;
      device->hdr_metadata_valid = true;

      amf::amf_hdr_metadata amf_metadata;
      for (int i = 0; i < 3; i++) {
        amf_metadata.displayPrimaries[i].x = metadata.displayPrimaries[i].x;
        amf_metadata.displayPrimaries[i].y = metadata.displayPrimaries[i].y;
      }
      amf_metadata.whitePoint.x = metadata.whitePoint.x;
      amf_metadata.whitePoint.y = metadata.whitePoint.y;
      amf_metadata.maxDisplayLuminance = metadata.maxDisplayLuminance;
      amf_metadata.minDisplayLuminance = metadata.minDisplayLuminance;
      amf_metadata.maxContentLightLevel = metadata.maxContentLightLevel;
      amf_metadata.maxFrameAverageLightLevel = metadata.maxFrameAverageLightLevel;

      if (!device->amf->set_hdr_metadata(std::optional<amf::amf_hdr_metadata> {amf_metadata})) {
        BOOST_LOG(warning) << "AMF: failed to update live HDR mastering metadata; the bitstream keeps the previous values"sv;
      }
    }

    amf::amf_encode_result encode_frames(uint64_t frame_index) {
      if (!device || !device->amf) {
        return {};
      }

      const bool meaningful_new_input = fresh_conversion_pending || force_idr;
      auto result = device->amf->encode_frame(frame_index, force_idr);
      last_input_accepted = result.input_accepted;
      if (result.input_accepted) {
        last_input_accepted_at = result.input_accepted_at;
        force_idr = false;
        // A duplicate submitted solely to release PA's retained final frame must
        // not arm another flush just because PA now retains that duplicate. A
        // newly converted capture frame or requested IDR starts a new cycle.
        suppress_tail_flush_until_fresh_conversion = !meaningful_new_input;
        fresh_conversion_pending = false;
      }
      return result;
    }

    bool was_last_input_accepted() const {
      return last_input_accepted;
    }

    std::optional<std::chrono::steady_clock::time_point> input_accepted_at() const {
      return last_input_accepted_at;
    }

    amf::amf_encode_result drain_frames(std::chrono::milliseconds timeout) {
      if (!device || !device->amf) {
        return {};
      }
      return device->amf->drain_output(timeout);
    }

    bool has_output_due() {
      return device && device->amf && device->amf->has_output_due();
    }

    bool has_completed_output() {
      return device && device->amf && device->amf->has_completed_output();
    }

    bool has_retained_preanalysis_tail() {
      const bool retained = device && device->amf && device->amf->has_retained_preanalysis_tail();
      return amf::lifecycle::preanalysis_tail_flush_is_due(
        retained,
        suppress_tail_flush_until_fresh_conversion);
    }

    bool begin_drain() {
      return device && device->amf && device->amf->begin_drain();
    }

    std::shared_ptr<platf::display_t> release_display_lease_for_driver_work() {
      return device ? device->release_display_lease_for_initialization() : nullptr;
    }

    // The native AMF encoder is pipelined. A catch-up batch can contain output from
    // earlier submissions, but emitted indices remain strictly increasing (in order,
    // no duplicates), so only flag a genuine regression (out-of-order / duplicate).
    // Forward gaps are also normal (a dropped frame is logged separately at submit time).
    bool note_emitted_index(uint64_t emitted) {
      const bool monotonic = last_emitted_index < 0 || (int64_t) emitted > last_emitted_index;
      last_emitted_index = (int64_t) emitted;
      return monotonic;
    }

    bool has_emitted_frame(uint64_t frame_index) const {
      return last_emitted_index >= 0 && static_cast<uint64_t>(last_emitted_index) >= frame_index;
    }

    bool has_emitted_any_frame() const {
      return last_emitted_index >= 0;
    }

    // Per-frame timestamps captured at submit time. Because the encoder emits an
    // earlier frame than the one just submitted, each packet must be stamped with the
    // timestamps of the frame it actually carries, not the newest submitted frame -
    // otherwise runtime latency stats are skewed by the pipeline depth.
    struct frame_timestamps_t {
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      std::optional<std::chrono::steady_clock::time_point> capture_timestamp;
      std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp;
    };

    void store_frame_timestamps(uint64_t frame_index, const frame_timestamps_t &ts) {
      // Fixed ring indexed by frame number: no per-frame heap traffic on the
      // submit path. The capacity sits well above the real pipeline depth, and a
      // collision only ever overwrites an entry 256 frames stale — one the old
      // bounded map would have evicted anyway.
      auto &slot = pending_timestamps[frame_index & (pending_timestamps.size() - 1)];
      slot.frame_index = frame_index;
      slot.valid = true;
      slot.ts = ts;
    }

    frame_timestamps_t take_frame_timestamps(uint64_t frame_index) {
      auto &slot = pending_timestamps[frame_index & (pending_timestamps.size() - 1)];
      if (!slot.valid || slot.frame_index != frame_index) {
        return {};
      }
      slot.valid = false;
      return slot.ts;
    }

    void discard_frame_timestamps(uint64_t frame_index) {
      auto &slot = pending_timestamps[frame_index & (pending_timestamps.size() - 1)];
      if (slot.valid && slot.frame_index == frame_index) {
        slot.valid = false;
      }
    }

  private:
    std::unique_ptr<platf::amf_encode_device_t> device;
    bool force_idr = false;
    bool last_input_accepted = false;
    std::optional<std::chrono::steady_clock::time_point> last_input_accepted_at;
    bool fresh_conversion_pending = false;
    bool suppress_tail_flush_until_fresh_conversion = false;
    int64_t last_emitted_index = -1;

    struct pending_timestamp_slot_t {
      uint64_t frame_index = 0;
      bool valid = false;
      frame_timestamps_t ts;
    };

    // Power-of-two size; indexed with frame_index & (size - 1).
    std::array<pending_timestamp_slot_t, 256> pending_timestamps {};
  };

  // Sticky per-session HDR state, persists across capture reinits so a transient SDR
  // display reading cannot downgrade an HDR stream's colorspace or poison its metadata.
  struct hdr_latch_t {
    // Set once the session has actually established HDR.
    bool latched = false;
    // Metadata captured while the display genuinely read HDR, reused during reinits
    // where the display transiently reads SDR.
    bool metadata_valid = false;
    SS_HDR_METADATA metadata {};
  };

  struct rtx_hdr_metadata_refresh_state_t {
    std::uint32_t observed_generation {0};
    int pending_peak_nits {0};
    std::chrono::steady_clock::time_point stable_since {};
  };

  struct sync_session_ctx_t {
    safe::signal_t *join_event;
    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::mail_raw_t::queue_t<packet_t> packets;
    safe::mail_raw_t::event_t<bool> idr_events;
    safe::mail_raw_t::event_t<hdr_info_t> hdr_events;
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;
    safe::mail_raw_t::event_t<int> bitrate_events;

    config_t config;
    int frame_nr;
    void *channel_data;
    hdr_latch_t hdr_latch;
    // Last HDR info raised to this session's client, used to suppress duplicates on reinit.
    std::optional<hdr_info_raw_t> last_hdr_info;
    rtx_hdr_metadata_refresh_state_t rtx_hdr_metadata_refresh;
  };

  struct sync_session_t {
    sync_session_ctx_t *ctx;
    std::unique_ptr<encode_session_t> session;
    encode_bootstrap_state_t bootstrap;
  };

  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  using encode_e = platf::capture_e;

  struct capture_ctx_t {
    img_event_t images;
    config_t config;
  };

  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;
    std::thread capture_thread;

    safe::signal_t reinit_event;
    const encoder_t *encoder_p;
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;
  };

  struct capture_thread_sync_ctx_t {
    encode_session_ctx_queue_t encode_session_ctx_queue {30};
  };

  int start_capture_sync(capture_thread_sync_ctx_t &ctx);
  void end_capture_sync(capture_thread_sync_ctx_t &ctx);
  int start_capture_async(capture_thread_async_ctx_t &ctx);
  void end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);
  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);

#ifdef _WIN32
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    ),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
    ),
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) nv::profile_hevc_e::main_10},
      },
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"coder"s, &config::video.nv_legacy.h264_coder},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, (int) nv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING
  };
#endif

#ifdef _WIN32
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::main},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_av1_e::high},
      },
      {},  // Fallback options
      "av1_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"pic_timing_sei"s, 0},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main},
      },
      {
        // HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::main_10},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, (int) qsv::profile_hevc_e::rext},
      },
      {
        // Fallback options
        {"low_power"s, []() {
           return config::video.qsv.qsv_slow_hevc ? 0 : 1;
         }},
      },
      "hevc_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"cavlc"s, &config::video.qsv.qsv_cavlc},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"vcm"s, 1},
        {"pic_timing_sei"s, 0},
        {"max_dec_frame_buffering"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high},
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        {"profile"s, (int) qsv::profile_h264_e::high_444p},
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  // Native AMD AMF encoder (src/amf/amf_d3d11.cpp). Bypasses the FFmpeg AMF
  // wrapper for direct AMF SDK access: D3D11 zero-copy input, reference-frame
  // invalidation and HDR metadata. Selecting amdvce is a strict native-AMF
  // contract: feature, initialization, or runtime failures are reported instead
  // of silently changing encoder implementations. amdvce_legacy remains an
  // explicit user-selected rollback below.
  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_amf>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::unknown,
      platf::pix_fmt_e::unknown
    ),
    {
      {},  // Common options (configured directly via AMF, not FFmpeg AVOptions)
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      {},
      {},
      {},
      {},
      {},
      {},
      "hevc_amf"s,
    },
    {
      {},
      {},
      {},
      {},
      {},
      {},
      "h264_amf"s,
    },
    // ASYNC_TEARDOWN: healthy session ends and runtime bitrate rebuilds detach
    // the bounded destruction worker instead of stalling the encode thread for
    // 100-300 ms; the lifecycle gate serializes it against the next session's
    // initialization. Shutdown, display reinit, and runtime fatals still force
    // an ordered synchronous teardown via the fail-guard's sync conditions.
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | ASYNC_TEARDOWN  // flags
  };

  // Legacy FFmpeg-based AMF encoder. This is an explicit rollback target only;
  // native feature, initialization, and runtime failures never select it.
  encoder_t amdvce_legacy {
    "amdvce_legacy"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, "lowest_latency"s},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preanalysis"s, []() {
           return amf::lifecycle::resolve_preanalysis(
                    config::video.amd.amd_rc_av1,
                    config::video.amd.amd_preanalysis)
             .enabled ? 1 : 0;
         }},
        {"quality"s, &config::video.amd.amd_quality_av1},
        {"rc"s, &config::video.amd.amd_rc_av1},
        {"aq_mode"s, encoder_t::option_t::optional_int_function_t {[]() -> std::optional<int> {
           if (!amf::lifecycle::rate_control_supports_adaptive_quantization(config::video.amd.amd_rc_av1)) {
             return 0;
           }
           if (!config::video.amd.amd_vbaq) return std::nullopt;
           return *config::video.amd.amd_vbaq ? 1 : 0;  // AMF AV1 CAQ / none
         }}},
        {"qvbr_quality_level"s, &config::video.amd.amd_qvbr_quality_level},
        {"usage"s, &config::video.amd.amd_usage_av1},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"gops_per_idr"s, 1},
        {"header_insertion_mode"s, "idr"s},
        {"preanalysis"s, []() {
           return amf::lifecycle::resolve_preanalysis(
                    config::video.amd.amd_rc_hevc,
                    config::video.amd.amd_preanalysis)
             .enabled ? 1 : 0;
         }},
        {"quality"s, &config::video.amd.amd_quality_hevc},
        {"rc"s, &config::video.amd.amd_rc_hevc},
        {"qvbr_quality_level"s, &config::video.amd.amd_qvbr_quality_level},
        {"usage"s, &config::video.amd.amd_usage_hevc},
        {"vbaq"s, encoder_t::option_t::optional_int_function_t {[]() -> std::optional<int> {
           if (!amf::lifecycle::rate_control_supports_adaptive_quantization(config::video.amd.amd_rc_hevc)) {
             return 0;
           }
           return config::video.amd.amd_vbaq;
         }}},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"level"s, [](const config_t &cfg) {
           auto size = cfg.width * cfg.height;
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             } else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         }},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"frame_skipping"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preanalysis"s, []() {
           return amf::lifecycle::resolve_preanalysis(
                    config::video.amd.amd_rc_h264,
                    config::video.amd.amd_preanalysis)
             .enabled ? 1 : 0;
         }},
        {"quality"s, &config::video.amd.amd_quality_h264},
        {"rc"s, &config::video.amd.amd_rc_h264},
        {"qvbr_quality_level"s, &config::video.amd.amd_qvbr_quality_level},
        {"usage"s, &config::video.amd.amd_usage_h264},
        {"vbaq"s, encoder_t::option_t::optional_int_function_t {[]() -> std::optional<int> {
           if (!amf::lifecycle::rate_control_supports_adaptive_quantization(config::video.amd.amd_rc_h264)) {
             return 0;
           }
           return config::video.amd.amd_vbaq;
         }}},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */},  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
    },
    // ASYNC_TEARDOWN: like NVENC, a hung AMF session must not stall the encoder thread on
    // mid-stream reinit; the bounded sync teardown in encode_run covers shutdown/reinit.
    PARALLEL_ENCODING | ASYNC_TEARDOWN
  };

  encoder_t mediafoundation {
    "mediafoundation"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,  // SDR 4:2:0 8-bit (only format Qualcomm supports)
      AV_PIX_FMT_NONE,  // No HDR - Qualcomm MF only supports 8-bit
      AV_PIX_FMT_NONE,  // No YUV444 SDR
      AV_PIX_FMT_NONE,  // No YUV444 HDR
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options for AV1 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_mf"s,
    },
    {
      // Common options for HEVC - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_mf"s,
    },
    {
      // Common options for H.264 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_mf"s,
    },
    PARALLEL_ENCODING | FIXED_GOP_SIZE  // MF encoder doesn't support on-demand IDR frames
  };
#endif

  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P,
      AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      nullptr
    ),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        {"svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s},
        {"preset"s, &config::video.sw.svtav1_preset},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        {"forced-idr"s, 1},
        {"x265-params"s, "info=0:keyint=-1"s},
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"low_power"s, 1},
        {"async_depth"s, 1},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Not all VAAPI drivers expose LP entrypoints
      },
      "av1_vaapi"s,
    },
    {
      // Common options
      {
        {"low_power"s, 1},
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Not all VAAPI drivers expose LP entrypoints
      },
      "hevc_vaapi"s,
    },
    {
      // Common options
      {
        {"low_power"s, 1},
        {"async_depth"s, 1},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Not all VAAPI drivers expose LP entrypoints
      },
      "h264_vaapi"s,
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };
#endif

#ifdef __APPLE__
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VIDEOTOOLBOX,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vt_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"flags"s, "-low_delay"},
      },
      "h264_videotoolbox"s,
    },
    DEFAULT
  };
#endif

#ifdef SUNSHINE_BUILD_VULKAN
  encoder_t vulkan {
    "vulkan"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VULKAN,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VULKAN,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vulkan_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vulkan"s,
    },
    {
      // Common options
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vulkan"s,
    },
    {
      // Common options
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vulkan"s,
    },
    LIMITED_GOP_SIZE | PARALLEL_ENCODING
  };
#endif

  static const std::vector<encoder_t *> encoders {
#ifndef __APPLE__
    &nvenc,
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
    &amdvce_legacy,
    &mediafoundation,
#endif
#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  #ifdef SUNSHINE_BUILD_VULKAN
    &vulkan,
  #endif
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  int active_hevc_mode;
  int active_av1_mode;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};
  std::atomic<std::int64_t> last_negative_hdr_advertisement_probe_ns {0};
  std::mutex encoder_probe_mutex;

  namespace {
    struct encoder_probe_status_t {
      bool attempted = false;
      bool successful = false;
      advertised_encoder_capabilities_t advertised_capabilities;
    };

    encoder_probe_status_t encoder_probe_status_for_key(const probe_cache_key_t &key) {
      auto &state = encoder_probe_cache_state();
      std::lock_guard<std::mutex> lock(state.mutex);
      const bool successful = state.valid && state.cache_key && *state.cache_key == key;
      return encoder_probe_status_t {
        .attempted = state.attempted_cache_key && *state.attempted_cache_key == key,
        .successful = successful,
        .advertised_capabilities = successful ?
                                     state.advertised_capabilities :
                                     advertised_encoder_capabilities_t {},
      };
    }
  }  // namespace

  bool has_attempted_encoder_probe() {
    const auto current_key = build_probe_cache_key();
    return encoder_probe_status_for_key(current_key).attempted;
  }

  bool has_successful_encoder_probe() {
    const auto current_key = build_probe_cache_key();
    return encoder_probe_status_for_key(current_key).successful;
  }

  bool last_encoder_probe_failed() {
    auto &state = encoder_probe_cache_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.failure_cache_key.has_value() && state.failure_count > 0;
  }

#ifdef _WIN32
  encoder_probe_adapter_hint_lease_t set_pending_virtual_display_adapter_hint(const LUID &adapter_luid) {
    auto &state = encoder_probe_cache_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto lease = ++state.next_pending_virtual_display_adapter_hint_lease;
    state.pending_virtual_display_adapter_hint = adapter_luid;
    state.pending_virtual_display_adapter_hint_lease = lease;
    state.pending_virtual_display_adapter_hint_ready_for_verification = false;
    BOOST_LOG(debug)
      << "Published pending virtual-display adapter identity "
      << luid_cache_identity(adapter_luid)
      << " for encoder capability matching (lease=" << lease << ").";
    return lease;
  }

  bool mark_pending_virtual_display_adapter_hint_ready_for_verification(
    const encoder_probe_adapter_hint_lease_t lease
  ) {
    auto &state = encoder_probe_cache_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (lease == 0 || state.pending_virtual_display_adapter_hint_lease != lease) {
      return false;
    }
    state.pending_virtual_display_adapter_hint_ready_for_verification = true;
    BOOST_LOG(debug)
      << "Pending virtual-display adapter identity is ready for WGC verification (lease="
      << lease << ").";
    return true;
  }

  bool clear_pending_virtual_display_adapter_hint(const encoder_probe_adapter_hint_lease_t lease) {
    auto &state = encoder_probe_cache_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (lease == 0 || state.pending_virtual_display_adapter_hint_lease != lease) {
      return false;
    }
    state.pending_virtual_display_adapter_hint.reset();
    state.pending_virtual_display_adapter_hint_lease = 0;
    state.pending_virtual_display_adapter_hint_ready_for_verification = false;
    BOOST_LOG(debug)
      << "Cleared pending virtual-display adapter identity after display publication (lease="
      << lease << ").";
    return true;
  }
#endif

  advertised_encoder_capabilities_t advertised_encoder_capabilities(
    const bool probe_before_negative,
    bool *probe_complete
  ) {
    auto current_key = build_probe_cache_key();
    auto probe_status = encoder_probe_status_for_key(current_key);
    const auto refresh_probe_status = [&]() {
      current_key = build_probe_cache_key();
      probe_status = encoder_probe_status_for_key(current_key);
    };

    if (probe_before_negative && !probe_status.successful && !probe_status.attempted) {
      BOOST_LOG(info) << "Encoder capabilities are unprobed for the current adapter identity; probing encoders now.";
      if (probe_encoders()) {
        BOOST_LOG(warning) << "Encoder probe failed before HTTP capability advertisement; reporting current encoder capabilities.";
      }
      refresh_probe_status();
    } else if (probe_before_negative && !probe_status.successful && probe_status.attempted) {
      const auto now = std::chrono::steady_clock::now();
      const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      const auto last_ns = last_negative_hdr_advertisement_probe_ns.load(std::memory_order_acquire);
      const auto retry_interval = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(10)).count();
      if (last_ns <= 0 || now_ns - last_ns >= retry_interval) {
        last_negative_hdr_advertisement_probe_ns.store(now_ns, std::memory_order_release);
        BOOST_LOG(info) << "Encoder capabilities lack a successful probe for the current adapter identity; re-probing before advertisement.";
        if (probe_encoders()) {
          BOOST_LOG(warning) << "Encoder re-probe failed before HTTP capability advertisement; reporting current encoder capabilities.";
        }
        refresh_probe_status();
      }
    }

    // Capability globals describe the last successful probe, which may belong
    // to a different adapter key. Never publish those stale positive values
    // when the current identity has not produced a successful probe.
    if (probe_complete) {
      *probe_complete = probe_status.successful;
    }
    if (!probe_status.successful) {
      log_probe_cache_miss(current_key);
      return {};
    }
    return probe_status.advertised_capabilities;
  }

  void reset_display(
    std::shared_ptr<platf::display_t> &disp,
    const platf::mem_type_e &type,
    const std::string &display_name,
    const config_t &config,
    const std::optional<platf::adapter_id_t> &required_adapter = std::nullopt
  ) {
    // After a recent display-helper APPLY (topology change), the display subsystem
    // may need time to settle. Use more retries with progressive delays.
    int max_attempts = 2;
    std::chrono::milliseconds base_delay = 200ms;
#ifdef _WIN32
    // The extended ladder only exists for the window right after an APPLY, and a
    // bounded stream start has to spend that window inside the same budget the
    // client's first-video deadline uses. Outside the window nothing changes.
    std::optional<std::chrono::steady_clock::time_point> settle_deadline;
    const auto ms_since_apply = display_helper_integration::ms_since_last_apply();
    if (ms_since_apply < 5000) {
      const auto stream_start_budget = display_helper_integration::remaining_stream_start_budget();
      if (!stream_start_budget || *stream_start_budget > 0ms) {
        max_attempts = 5;
        base_delay = 300ms;
      }
      if (stream_start_budget) {
        settle_deadline = std::chrono::steady_clock::now() + *stream_start_budget;
      }
    }
#endif

    for (int x = 0; x < max_attempts; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config, required_adapter);
      if (disp) {
        break;
      }

      // The capture code depends on us to sleep between failures.
      // Use progressive delays for topology changes to give the display time to settle.
      auto delay = base_delay + std::chrono::milliseconds(x * 100);
#ifdef _WIN32
      if (settle_deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*settle_deadline - std::chrono::steady_clock::now());
        delay = std::max(0ms, std::min(delay, remaining));
      }
#endif
      std::this_thread::sleep_for(delay);
    }
  }

  /**
   * @brief Update the list of display names before or during a stream.
   * @details This will attempt to keep `current_display_index` pointing at the same display.
   * @param dev_type The encoder device type used for display lookup.
   * @param display_names The list of display names to repopulate.
   * @param current_display_index The current display index or -1 if not yet known.
   */
  void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index, std::string &preferred_display_name) {
    // It is possible that the output name may be empty even if it wasn't before (device disconnected) or vice-versa
    const auto runtime_output_override = config::runtime_output_name_override();
    const bool has_runtime_output_override = runtime_output_override.has_value();
    const auto output_name = display_device::map_output_name(config::get_active_output_name());
    std::string current_display_name;
    auto names_match = [](const std::string &lhs, const std::string &rhs) {
      return boost::iequals(lhs, rhs);
    };

    // If we have a current display index, let's start with that
    if (current_display_name.empty() && current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
#ifdef _WIN32
      // During a topology change (e.g. display helper just applied ensure_only_display),
      // DXGI may temporarily report no displays. Don't fall back to stale names that
      // include now-disabled physical displays — this causes the reinit loop to waste
      // time trying to init on unavailable outputs. Instead, use the configured output
      // name so the reinit loop targets the correct display.
      const auto ms_since_apply = display_helper_integration::ms_since_last_apply();
      if (ms_since_apply < 5000 && !output_name.empty()) {
        BOOST_LOG(info) << "No displays found after reenumeration during topology change; "
                        << "using configured output ["sv << output_name << "] instead of stale list"sv;
        display_names.clear();
        display_names.emplace_back(output_name);
      } else {
        BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
        display_names = std::move(old_display_names);
        return;
      }
#else
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
#endif
    } else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    if (has_runtime_output_override && !output_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (names_match(display_names[x], output_name)) {
          current_display_index = x;
          return;
        }
      }

      BOOST_LOG(warning) << "Runtime display override [" << *runtime_output_override
                         << "] mapped to [" << output_name
                         << "] but was not found in the capture display list";
    }

    if (current_display_name.empty()) {
      current_display_name = display_device::map_output_name(config::video.output_name);
    }

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (names_match(display_names[x], current_display_name)) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;

      // If the previous display disappeared, prefer moving back to configured output before
      // defaulting to index 0 (often primary physical display during transient display churn).
      if (!output_name.empty()) {
        for (int x = 0; x < display_names.size(); ++x) {
          if (names_match(display_names[x], output_name)) {
            current_display_index = x;
            return;
          }
        }
      }
    } else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (names_match(display_names[x], output_name)) {
          current_display_index = x;
          return;
        }
      }
    }
  }

  void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index) {
    static std::string empty_str = "";
    refresh_displays(dev_type, display_names, current_display_index, empty_str);
  }

  void captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder
  ) {
    std::vector<capture_ctx_t> capture_ctxs;

    auto fg = util::fail_guard([&]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    std::vector<std::string> display_names;
    int display_p = -1;
    std::shared_ptr<platf::display_t> disp;

    while (capture_ctx_queue->running()) {
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      if (!ensure_virtual_display_ready(display_names, display_p)) {
        std::this_thread::sleep_for(50ms);
        continue;
      }

      disp = platf::display(encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
      if (disp) {
        break;
      }

      std::this_thread::sleep_for(50ms);
    }

    if (!disp) {
      return;
    }

    display_wp = disp;

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);
    uint64_t image_pool_wait_count = 0;

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    auto release_image_pool = [&]() {
#ifdef _WIN32
      std::vector<std::shared_ptr<platf::img_t>> d3d_images;
#endif

      for (auto &img : imgs) {
        if (!img) {
          continue;
        }

#ifdef _WIN32
        if (is_d3d_capture_image(img)) {
          d3d_images.emplace_back(std::move(img));
          continue;
        }
#endif

        img.reset();
      }

#ifdef _WIN32
      release_d3d_capture_images_async(std::move(d3d_images));
#endif
      imgs_used_timestamps.clear();
    };
    auto release_image_pool_guard = util::fail_guard([&]() {
      release_image_pool();
    });

    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&]() {
#ifdef _WIN32
      if (std::any_of(std::begin(imgs), std::end(imgs), [](const auto &img) {
            return img && is_d3d_capture_image(img);
          })) {
        return;
      }
#endif

      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      std::optional<std::chrono::steady_clock::time_point> wait_start;
      uint32_t wait_iterations = 0;
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          if (wait_start) {
            const auto wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *wait_start).count();
            ++image_pool_wait_count;
            if (image_pool_wait_count <= 5 || wait_ms > 1.5 || image_pool_wait_count % 120 == 0) {
              BOOST_LOG(debug) << "Capture image pool waited " << wait_ms
                               << "ms for a free image"
                               << " iterations=" << wait_iterations
                               << " count=" << image_pool_wait_count;
            }
          }

          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          img_out->capture_pacing_timestamp.reset();
          return true;
        } else {
          if (!wait_start) {
            wait_start = std::chrono::steady_clock::now();
          }
          ++wait_iterations;
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // Capture takes place on this thread
    platf::set_thread_name("video::capture");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            capture_ctx->images->raise(img);
          }

          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            reinit_event.raise(true);
            disp->prepare_for_reinit();

            // Some classes of images contain references to the display --> display won't delete unless img is deleted.
            //
            // D3D capture images additionally own cross-device SHARED keyed-mutex surfaces
            // that the per-client encoder devices still have open. Freeing them now (the old
            // behavior, on a detached thread) races the encoders' teardown and the GPU's
            // deferred eviction during the mode change, which can bugcheck the kernel video
            // memory manager (VIDEO_MEMORY_MANAGEMENT_INTERNAL 0x10e) when a second client is
            // connected. These surfaces hold no display reference, so deferring them does not
            // block the use_count() wait below; hold them until every encoder has dropped its
            // display reference, then free them once no device still has them open.
#ifdef _WIN32
            std::vector<std::shared_ptr<platf::img_t>> deferred_d3d_images;
            for (auto &img : imgs) {
              if (img && is_d3d_capture_image(img)) {
                deferred_d3d_images.emplace_back(std::move(img));
              }
            }
#endif
            release_image_pool();

            // display_wp is modified in this thread only
            // Wait for the other shared_ptr's of display to be destroyed.
            // New displays will only be created in this thread.
            while (display_wp->use_count() != 1) {
              // If capture is being torn down (stream/session ending), stop waiting on
              // encoder threads that may still hold a display reference while blocked in a
              // slow or hung driver call. Spinning here until those threads are force-joined
              // is what lets a wedged reinit escalate into the 10s teardown watchdog crash.
              if (!capture_ctx_queue->running()) {
#ifdef _WIN32
                // Don't block this bail-out on a synchronous D3D teardown; hand the deferred
                // surfaces to the async releaser as the old code path did.
                release_d3d_capture_images_async(std::move(deferred_d3d_images));
#endif
                return;
              }

              // Free images that weren't consumed by the encoders. These can reference the display and prevent
              // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
              // conditions where the encoding loop might free a good frame after reinitializing if we capture
              // a new frame here before the encoder has finished reinitializing.
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->peek()) {
                  capture_ctx->images->pop();
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

#ifdef _WIN32
            // Every encoder device has now released the shared capture surfaces (their
            // display references reached zero above), so it is safe to free them. Done
            // synchronously on this thread rather than on a detached thread, so no surface
            // is freed while another device still references it or while it is still queued
            // for GPU eviction during the mode change. Take the teardown mutex so this free
            // cannot overlap an async d3dRelease thread still draining a previous generation.
            {
              std::lock_guard lg {encode_session_teardown_mutex};
              deferred_d3d_images.clear();
            }
#endif

            while (capture_ctx_queue->running()) {
              // Release the display before reenumerating displays, since some capture backends
              // only support a single display session per device/application.
              disp.reset();

#ifdef _WIN32
              wait_for_recent_display_apply_stability();
#endif

              // Refresh display names since a display removal might have caused the reinitialization
              refresh_displays(encoder.platform_formats->dev_type, display_names, display_p, proc::proc.display_name);

              if (!ensure_virtual_display_ready(display_names, display_p)) {
                std::this_thread::sleep_for(50ms);
                continue;
              }

              // Process any pending display switch with the new list of displays.
              // Negative values mean "reinit only; keep display selection logic intact".
              if (switch_display_event->peek()) {
                const int requested = *switch_display_event->pop();
                if (requested >= 0) {
                  display_p = std::clamp(requested, 0, (int) display_names.size() - 1);
                }
              }

              // reset_display() will sleep between retries
              reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
              if (disp) {
                proc::proc.display_name = display_names[display_p];
                break;
              }
            }
            if (!disp) {
              return;
            }

            display_wp = disp;

            reinit_event.reset();
            continue;
          }
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return;
      }
    }
  }

  int encode_avcodec(
    int64_t frame_nr,
    avcodec_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<std::chrono::steady_clock::time_point> capture_timestamp,
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp
  ) {
    auto &frame = session.device->frame;
    frame->pts = frame_nr;

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;
    auto &vps = session.vps;

    // send the frame to the encoder
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      } else if (ret < 0) {
        return ret;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        if (session.inject == 1) {
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        } else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view((char *) std::begin(vps.old), vps.old.size()),
            std::string_view((char *) std::begin(vps._new), vps._new.size())
          );
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view((char *) std::begin(sps.old), sps.old.size()),
          std::string_view((char *) std::begin(sps._new), sps._new.size())
        );
      }

      if (av_packet && av_packet->pts == frame_nr) {
        packet->frame_timestamp = frame_timestamp;
        packet->capture_timestamp = capture_timestamp ? capture_timestamp : frame_timestamp;
        packet->host_processing_timestamp = host_processing_timestamp;
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      if (webrtc_stream::has_active_sessions()) {
        webrtc_stream::submit_video_packet(*packet);
      }
      packet->packet_enqueue_timestamp = std::chrono::steady_clock::now();
      packets->raise(std::move(packet));
    }

    return 0;
  }

  int encode_nvenc(
    int64_t frame_nr,
    nvenc_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<std::chrono::steady_clock::time_point> capture_timestamp,
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp
  ) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packet->capture_timestamp = capture_timestamp ? capture_timestamp : frame_timestamp;
    packet->host_processing_timestamp = host_processing_timestamp;
    if (webrtc_stream::has_active_sessions()) {
      webrtc_stream::submit_video_packet(*packet);
    }
    packet->packet_enqueue_timestamp = std::chrono::steady_clock::now();
    packets->raise(std::move(packet));

    return 0;
  }

  void deliver_amf_frames(
    int64_t submitted_frame_nr,
    amf_encode_session_t &session,
    std::vector<amf::amf_encoded_frame> &encoded_frames,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data
  ) {
    for (auto &encoded_frame : encoded_frames) {
      if (encoded_frame.data.empty()) {
        continue;
      }

      // frame_nr != frame_index is expected when this batch is catching up after a
      // transient delay. Only a non-monotonic emitted index is a real desync.
      if (!session.note_emitted_index(encoded_frame.frame_index)) {
        BOOST_LOG(warning) << "AMF emitted frame index regression: " << encoded_frame.frame_index
                           << " (submitted " << submitted_frame_nr << ")";
      }

      // Stamp every packet with the timestamps of the frame it actually carries,
      // including earlier frames drained in the same catch-up batch.
      const auto ts = session.take_frame_timestamps(encoded_frame.frame_index);

      auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
      packet->channel_data = channel_data;
      packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
      packet->frame_timestamp = ts.frame_timestamp;
      packet->capture_timestamp = ts.capture_timestamp ? ts.capture_timestamp : ts.frame_timestamp;
      packet->host_processing_timestamp = ts.host_processing_timestamp;
      if (webrtc_stream::has_active_sessions()) {
        webrtc_stream::submit_video_packet(*packet);
      }
      packet->packet_enqueue_timestamp = std::chrono::steady_clock::now();
      packets->raise(std::move(packet));
    }
  }

  int encode_amf(
    int64_t frame_nr,
    amf_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<std::chrono::steady_clock::time_point> capture_timestamp,
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp
  ) {
    // Stash this frame's timestamps before submitting; the encoder is pipelined and
    // emits an earlier frame, so the packet is stamped from this map by emitted index.
    session.store_frame_timestamps((uint64_t) frame_nr, {frame_timestamp, capture_timestamp, host_processing_timestamp});

    auto encode_result = session.encode_frames(frame_nr);
    auto &encoded_frames = encode_result.frames;
    if (!encode_result.input_accepted) {
      session.discard_frame_timestamps((uint64_t) frame_nr);
    }
    if (encode_result.fatal || std::any_of(encoded_frames.begin(), encoded_frames.end(), [](const auto &frame) { return frame.fatal; })) {
      BOOST_LOG(error) << "AMF encoder entered an unrecoverable state, requesting reinit";
      return -1;
    }
    if (encoded_frames.empty()) {
      // No output this call (pipeline still filling or transient stall); not fatal.
      // The stashed timestamps stay until this frame is actually emitted.
      return 0;
    }

    deliver_amf_frames(frame_nr, session, encoded_frames, packets, channel_data);

    return 0;
  }

  int encode(
    int64_t frame_nr,
    encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<std::chrono::steady_clock::time_point> capture_timestamp,
    std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp
  ) {
    thread_local logging::min_max_avg_periodic_logger<double> encode_duration_logger(debug, "Video encode call duration", "ms");
    const auto encode_start = std::chrono::steady_clock::now();
    int result = -1;
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      result = encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp, capture_timestamp, host_processing_timestamp);
    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      result = encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp, capture_timestamp, host_processing_timestamp);
    } else if (auto amf_session = dynamic_cast<amf_encode_session_t *>(&session)) {
      result = encode_amf(frame_nr, *amf_session, packets, channel_data, frame_timestamp, capture_timestamp, host_processing_timestamp);
    }

    encode_duration_logger.collect_and_log(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encode_start).count());
    return result;
  }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  /**
   * @brief Synthesize Rec.2020/D65 HDR10 metadata for an RTX HDR (SDR->HDR) stream.
   *
   * RTX HDR captures an SDR source and post-processes it to HDR, so the capture display's own
   * luminance metadata is irrelevant -- the client (and the encoded SEI) must advertise the HDR
   * volume the TrueHDR conversion targets, not whatever the panel reports. Shared by every
   * HDR-metadata producer so the bitstream SEI and the control-channel metadata stay consistent.
   */
  SS_HDR_METADATA synthesize_rtx_hdr_metadata(const int peak_nits) {
    const auto safe_peak_nits = static_cast<uint16_t>(std::clamp(peak_nits, 400, 2000));
    SS_HDR_METADATA m {};
    m.displayPrimaries[0] = {35400, 14600};  // R (Rec.2020, normalized to 50000)
    m.displayPrimaries[1] = {8500, 39850};  // G
    m.displayPrimaries[2] = {6550, 2300};  // B
    m.whitePoint = {15635, 16450};  // D65
    m.maxDisplayLuminance = safe_peak_nits;  // nits
    m.minDisplayLuminance = 1;  // 1/10000th nit (~0)
    m.maxContentLightLevel = safe_peak_nits;  // nits
    m.maxFrameAverageLightLevel = safe_peak_nits / 4;  // nits
    return m;
  }
#endif

  // Raise hdr_info unless it matches what this session last raised. Some clients
  // (moonlight-xbox) perform a full HDMI display mode-set for every HDR mode message,
  // so a redundant one on reinit costs seconds of black screen mid-stream.
  void raise_hdr_info_if_changed(safe::mail_raw_t::event_t<hdr_info_t> &event, std::optional<hdr_info_raw_t> &last_hdr_info, hdr_info_t hdr_info) {
    if (last_hdr_info && last_hdr_info->enabled == hdr_info->enabled &&
        std::memcmp(&last_hdr_info->metadata, &hdr_info->metadata, sizeof(hdr_info->metadata)) == 0) {
      return;
    }

    last_hdr_info = *hdr_info;
    event->raise(std::move(hdr_info));
  }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  bool refresh_rtx_hdr_metadata_if_needed(
    config_t &config,
    encode_session_t &encode_session,
    safe::mail_raw_t::event_t<hdr_info_t> &hdr_event,
    std::optional<hdr_info_raw_t> &last_hdr_info,
    rtx_hdr_metadata_refresh_state_t &refresh_state
  ) {
    using namespace std::chrono_literals;

    if (!config.rtx_hdr_active) {
      return false;
    }

    const auto live_state = platf::rtx_hdr::live_output_metadata_state();
    if (live_state.generation == 0) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (live_state.generation != refresh_state.observed_generation) {
      refresh_state.observed_generation = live_state.generation;
      const int new_peak_nits = std::clamp(live_state.peak_brightness, 400, 2000);
      if (refresh_state.pending_peak_nits != new_peak_nits) {
        refresh_state.pending_peak_nits = new_peak_nits;
        refresh_state.stable_since = now;
      }
    }

    // Live sliders may publish several intermediate values while being dragged. Wait
    // for the peak to settle so clients that mode-set on HDR messages see one update.
    if (refresh_state.pending_peak_nits == 0 || now - refresh_state.stable_since < 250ms) {
      return false;
    }

    const int peak_nits = refresh_state.pending_peak_nits;
    refresh_state.pending_peak_nits = 0;
    if (config.rtx_hdr_peak_nits == peak_nits) {
      return false;
    }

    config.rtx_hdr_peak_nits = peak_nits;
    const auto metadata = synthesize_rtx_hdr_metadata(peak_nits);
    encode_session.set_hdr_metadata(metadata);
    raise_hdr_info_if_changed(
      hdr_event,
      last_hdr_info,
      std::make_unique<hdr_info_raw_t>(true, metadata)
    );
    BOOST_LOG(info) << "RTX HDR: updated live HDR10 metadata to peak " << peak_nits << " nits; forcing IDR.";
    return true;
  }
#endif

#ifdef _WIN32
  struct amf_main10_compatibility_override_t {
    avcodec_buffer_t device_ref;
    AVAMFDeviceContext *device_context;
    std::int64_t runtime_version;
  };

  // AMF packs a version as major << 48 | minor << 32 | release << 16 | build, with each
  // field 16 bits wide (see AMF/core/Version.h). Decode it so logs are human-readable.
  static std::string describe_amf_version(std::int64_t version) {
    std::ostringstream description;
    description << AMF_GET_MAJOR_VERSION(version) << '.'
                << AMF_GET_MINOR_VERSION(version) << '.'
                << AMF_GET_SUBMINOR_VERSION(version) << '.'
                << AMF_GET_BUILD_VERSION(version);
    return description.str();
  }

  // FFmpeg exposes the AMF runtime version only once a device context exists, and that
  // context is derived for the validated adapter alone. Query the runtime directly so
  // every AMD HEVC Main10 attempt records the version the decision was made against,
  // whichever way the decision goes.
  static void log_amf_runtime_version(const DXGI_ADAPTER_DESC &adapter_desc) {
    HMODULE amfrt = LoadLibraryW(AMF_DLL_NAME);
    if (!amfrt) {
      BOOST_LOG(info) << "AMF Main10 override: " << AMF_DLL_NAMEA << " could not be loaded; AMF runtime version unknown.";
      return;
    }

    auto unload_amfrt = util::fail_guard([amfrt]() {
      FreeLibrary(amfrt);
    });

    auto query_version = (AMFQueryVersion_Fn) GetProcAddress(amfrt, AMF_QUERY_VERSION_FUNCTION_NAME);
    amf_uint64 version = 0;
    if (!query_version || query_version(&version) != AMF_OK) {
      BOOST_LOG(info) << "AMF Main10 override: " << AMF_QUERY_VERSION_FUNCTION_NAME
                      << "() unavailable or failed; AMF runtime version unknown.";
      return;
    }

    BOOST_LOG(info) << "AMF Main10 override: AMF runtime version "
                    << describe_amf_version(static_cast<std::int64_t>(version)) << " on adapter "
                    << std::hex << adapter_desc.VendorId << ':' << adapter_desc.DeviceId << std::dec << '.';
  }

  static std::optional<amf_main10_compatibility_override_t> enable_amf_main10_compatibility_override(
    AVCodecContext *codec_context,
    AVBufferRef *d3d_device_ref
  ) {
    constexpr UINT kAmdVendorId = 0x1002;
    constexpr UINT kRadeonPro5500XtDeviceId = 0x7340;
    // Only the low 16 bits of the packed version hold the build number, so masking them
    // off compares major.minor.release alone. FFmpeg's amfenc P010 guard is a blanket
    // ">= 1.4.32" check, so every 1.4.31.x build is refused identically and needs the
    // same override; masking keeps 1.4.31.5 from silently falling through. Runtimes at
    // 1.4.32.0 or newer mask to a different value and are correctly left alone.
    constexpr std::int64_t kBuildFieldMask = 0xFFFF;
    constexpr std::int64_t kCompatibleRuntime = AMF_MAKE_FULL_VERSION(1, 4, 31, 0);
    constexpr std::int64_t kFfmpegMain10Minimum = AMF_MAKE_FULL_VERSION(1, 4, 32, 0);

    if (!codec_context || !d3d_device_ref || !d3d_device_ref->data) {
      return std::nullopt;
    }

    auto *device_context = reinterpret_cast<AVHWDeviceContext *>(d3d_device_ref->data);
    if (device_context->type != AV_HWDEVICE_TYPE_D3D11VA) {
      return std::nullopt;
    }

    auto *d3d_context = reinterpret_cast<AVD3D11VADeviceContext *>(device_context->hwctx);
    if (!d3d_context || !d3d_context->device) {
      return std::nullopt;
    }

    platf::dxgi::dxgi_t dxgi_device;
    using dxgi_adapter_t = util::safe_ptr<IDXGIAdapter, platf::dxgi::Release<IDXGIAdapter>>;
    dxgi_adapter_t dxgi_adapter;
    if (FAILED(d3d_context->device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
        FAILED(dxgi_device->GetAdapter(&dxgi_adapter))) {
      return std::nullopt;
    }

    DXGI_ADAPTER_DESC adapter_desc {};
    if (FAILED(dxgi_adapter->GetDesc(&adapter_desc))) {
      return std::nullopt;
    }

    log_amf_runtime_version(adapter_desc);

    // Fail closed on anything but the one adapter this workaround has been validated
    // against, before an AMF device context is derived.
    if (adapter_desc.VendorId != kAmdVendorId || adapter_desc.DeviceId != kRadeonPro5500XtDeviceId) {
      BOOST_LOG(info) << "AMF Main10 override: adapter " << std::hex << adapter_desc.VendorId << ':'
                      << adapter_desc.DeviceId << std::dec
                      << " is not the validated Radeon Pro 5500 XT (1002:7340); leaving FFmpeg's AMF Main10 gate untouched.";
      return std::nullopt;
    }

    avcodec_buffer_t amf_device_ref;
    if (auto status = av_hwdevice_ctx_create_derived(&amf_device_ref, AV_HWDEVICE_TYPE_AMF, d3d_device_ref, 0)) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(warning) << "AMF Main10 override: failed to derive AMF device context for Radeon Pro 5500 XT Main10 compatibility: "
                         << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);
      return std::nullopt;
    }

    auto *amf_hw_device = reinterpret_cast<AVHWDeviceContext *>(amf_device_ref->data);
    if (!amf_hw_device || amf_hw_device->type != AV_HWDEVICE_TYPE_AMF) {
      return std::nullopt;
    }

    auto *amf_context = reinterpret_cast<AVAMFDeviceContext *>(amf_hw_device->hwctx);
    if (!amf_context) {
      return std::nullopt;
    }

    const auto runtime_version = amf_context->version;
    if ((runtime_version & ~kBuildFieldMask) != kCompatibleRuntime) {
      BOOST_LOG(info) << "AMF Main10 override: AMF runtime " << describe_amf_version(runtime_version)
                      << " is outside the targeted 1.4.31.x range (FFmpeg requires "
                      << describe_amf_version(kFfmpegMain10Minimum)
                      << " for P010); leaving FFmpeg's AMF Main10 gate untouched.";
      return std::nullopt;
    }

    codec_context->hw_device_ctx = av_buffer_ref(amf_device_ref.get());
    if (!codec_context->hw_device_ctx) {
      return std::nullopt;
    }

    // FFmpeg applies its 1.4.32 guard to every P010 encoder, but issue artifacts
    // prove this exact adapter/runtime tuple can stream HEVC Main10. Present the
    // guarded version only during codec validation, then restore the real value.
    amf_context->version = kFfmpegMain10Minimum;
    BOOST_LOG(info) << "AMF Main10 override: reporting AMF " << describe_amf_version(kFfmpegMain10Minimum)
                    << " to FFmpeg during codec validation (real runtime " << describe_amf_version(runtime_version)
                    << ") so HEVC Main10/HDR is not refused.";
    return amf_main10_compatibility_override_t {
      std::move(amf_device_ref),
      amf_context,
      runtime_version
    };
  }
#endif

  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::avcodec_encode_device_t> encode_device
  ) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    bool hardware = platform_formats->avcodec_base_dev_type != AV_HWDEVICE_TYPE_NONE;

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || (disp && !disp->is_codec_supported(video_format.name, config))) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE]) {
      BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1 && !video_format[encoder_t::YUV444]) {
      BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 not supported"sv;
      return nullptr;
    }

    auto codec = avcodec_find_encoder_by_name(video_format.name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << video_format.name << ']';

      return nullptr;
    }

    auto colorspace = encode_device->colorspace;
    if (!encode_device->initialize_hardware_device()) {
      BOOST_LOG(error) << encoder.name << ": failed to initialize the hardware encode device"sv;
      return nullptr;
    }
    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit :
                                                                                   AV_PIX_FMT_NONE;

    // Allow up to 1 retry to apply the set of fallback options.
    //
    // Note: If we later end up needing multiple sets of
    // fallback options, we may need to allow more retries
    // to try applying each set.
    avcodec_ctx_t ctx;
    for (int retries = 0; retries < 2; retries++) {
#ifdef _WIN32
      std::optional<amf_main10_compatibility_override_t> amf_main10_compatibility_override;
      // The override mutates AMF runtime state that must be handed back before this
      // iteration ends, and avcodec_open2() is several hundred lines away with option
      // handling in between. Tie the restore to the iteration scope so the fallback
      // retry, the early return and any exception all restore the real version.
      auto restore_amf_main10_compatibility_override = util::fail_guard([&amf_main10_compatibility_override]() {
        if (amf_main10_compatibility_override) {
          amf_main10_compatibility_override->device_context->version =
            amf_main10_compatibility_override->runtime_version;
          amf_main10_compatibility_override.reset();
        }
      });
#endif
      ctx.reset(avcodec_alloc_context3(codec));
      ctx->width = config.width;
      ctx->height = config.height;
      ctx->time_base = AVRational {1, config.framerate};
      ctx->framerate = AVRational {config.framerate, 1};
      if (config.framerateX100 > 0) {
        AVRational fps = video::framerateX100_to_rational(config.framerateX100);
        ctx->framerate = fps;
        ctx->time_base = AVRational {fps.den, fps.num};
      }

      switch (config.videoFormat) {
        case 0:
          // 10-bit h264 encoding is not supported by our streaming protocol
          assert(!config.dynamicRange);
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_H264_HIGH_444_PREDICTIVE : AV_PROFILE_H264_HIGH;
          break;

        case 1:
          if (config.chromaSamplingType == 1) {
            // HEVC uses the same RExt profile for both 8 and 10 bit YUV 4:4:4 encoding
            ctx->profile = AV_PROFILE_HEVC_REXT;
          } else {
            ctx->profile = config.dynamicRange ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
          }
          break;

        case 2:
          // AV1 supports both 8 and 10 bit encoding with the same Main profile
          // but YUV 4:4:4 sampling requires High profile
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
          break;
      }

      // B-frames delay decoder output, so never use them
      ctx->max_b_frames = 0;

      // Use an infinite GOP length since I-frames are generated on demand
      // Exception: encoders with FIXED_GOP_SIZE flag don't support on-demand IDR
      if (encoder.flags & FIXED_GOP_SIZE) {
        // Fixed GOP for encoders that don't support on-demand IDR (e.g. Media Foundation)
        ctx->gop_size = 120;  // ~2 seconds at 60 FPS - larger to reduce oversized IDR frame frequency
        ctx->keyint_min = 120;
      } else {
        ctx->gop_size = encoder.flags & LIMITED_GOP_SIZE ?
                          std::numeric_limits<std::int16_t>::max() :
                          std::numeric_limits<int>::max();
        ctx->keyint_min = std::numeric_limits<int>::max();
      }

      // Some client decoders have limits on the number of reference frames
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        } else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // We forcefully reset the flags to avoid clash on reuse of AVCodecContext
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;

      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // Used by cbs::make_sps_hevc
      ctx->sw_pix_fmt = sw_fmt;

      if (hardware) {
        avcodec_buffer_t encoding_stream_context;

        ctx->pix_fmt = platform_formats->avcodec_dev_pix_fmt;

        // Create the base hwdevice context
        auto buf_or_error = platform_formats->init_avcodec_hardware_input_buffer(encode_device.get());
        if (buf_or_error.has_right()) {
          return nullptr;
        }
        encoding_stream_context = std::move(buf_or_error.left());

        // If this encoder requires derivation from the base, derive the desired type
        if (platform_formats->avcodec_derived_dev_type != AV_HWDEVICE_TYPE_NONE) {
          avcodec_buffer_t derived_context;

          // Allow the hwdevice to prepare for this type of context to be derived
          if (encode_device->prepare_to_derive_context(platform_formats->avcodec_derived_dev_type)) {
            return nullptr;
          }

          auto err = av_hwdevice_ctx_create_derived(&derived_context, platform_formats->avcodec_derived_dev_type, encoding_stream_context.get(), 0);
          if (err) {
            char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
            BOOST_LOG(error) << "Failed to derive device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

            return nullptr;
          }

          encoding_stream_context = std::move(derived_context);
        }

        // Initialize avcodec hardware frames
        {
          avcodec_buffer_t frame_ref {av_hwframe_ctx_alloc(encoding_stream_context.get())};

          auto frame_ctx = (AVHWFramesContext *) frame_ref->data;
          frame_ctx->format = ctx->pix_fmt;
          frame_ctx->sw_format = sw_fmt;
          frame_ctx->height = ctx->height;
          frame_ctx->width = ctx->width;
          frame_ctx->initial_pool_size = 0;

          // Allow the hwdevice to modify hwframe context parameters
          encode_device->init_hwframes(frame_ctx);

          if (auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
            return nullptr;
          }

          ctx->hw_frames_ctx = av_buffer_ref(frame_ref.get());
        }

#ifdef _WIN32
        // Only the FFmpeg-based rollback encoder reaches this function; native
        // "amdvce" builds on encoder_platform_formats_amf and never gets here.
        if (encoder.name == "amdvce_legacy"sv &&
            config.videoFormat == 1 &&
            config.dynamicRange &&
            sw_fmt == AV_PIX_FMT_P010) {
          amf_main10_compatibility_override =
            enable_amf_main10_compatibility_override(ctx.get(), encoding_stream_context.get());
        }
#endif

        ctx->slices = config.slicesPerFrame;
      } else /* software */ {
        ctx->pix_fmt = sw_fmt;

        // Clients will request for the fewest slices per frame to get the
        // most efficient encode, but we may want to provide more slices than
        // requested to ensure we have enough parallelism for good performance.
        ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);
      }

      if (encoder.flags & SINGLE_SLICE_ONLY) {
        ctx->slices = 1;
      }

      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = ctx->slices;

      AVDictionary *options {nullptr};
      auto handle_option = [&options, &config](const encoder_t::option_t &option) {
        std::visit(
          util::overloaded {
            [&](int v) {
              av_dict_set_int(&options, option.name.c_str(), v, 0);
            },
            [&](int *v) {
              av_dict_set_int(&options, option.name.c_str(), *v, 0);
            },
            [&](std::optional<int> *v) {
              if (*v) {
                av_dict_set_int(&options, option.name.c_str(), **v, 0);
              }
            },
            [&](const std::function<int()> &v) {
              av_dict_set_int(&options, option.name.c_str(), v(), 0);
            },
            [&](const encoder_t::option_t::optional_int_function_t &v) {
              if (const auto value = v.evaluate()) {
                av_dict_set_int(&options, option.name.c_str(), *value, 0);
              }
            },
            [&](const std::string &v) {
              av_dict_set(&options, option.name.c_str(), v.c_str(), 0);
            },
            [&](std::string *v) {
              if (!v->empty()) {
                av_dict_set(&options, option.name.c_str(), v->c_str(), 0);
              }
            },
            [&](const std::function<const std::string(const config_t &cfg)> &v) {
              av_dict_set(&options, option.name.c_str(), v(config).c_str(), 0);
            }
          },
          option.value
        );
      };

      // Apply common options, then format-specific overrides
      for (auto &option : video_format.common_options) {
        handle_option(option);
      }
      for (auto &option : (config.dynamicRange ? video_format.hdr_options : video_format.sdr_options)) {
        handle_option(option);
      }
      if (config.chromaSamplingType == 1) {
        for (auto &option : (config.dynamicRange ? video_format.hdr444_options : video_format.sdr444_options)) {
          handle_option(option);
        }
      }
      if (retries > 0) {
        for (auto &option : video_format.fallback_options) {
          handle_option(option);
        }
      }

      auto bitrate = config.bitrate * 1000;
      ctx->rc_max_rate = bitrate;
      ctx->bit_rate = bitrate;

      if (encoder.flags & CBR_WITH_VBR) {
        // Ensure rc_max_bitrate != bit_rate to force VBR mode
        ctx->bit_rate--;
      } else {
        ctx->rc_min_rate = bitrate;
      }

      if (encoder.flags & RELAXED_COMPLIANCE) {
        ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
      }

      if (!(encoder.flags & NO_RC_BUF_LIMIT)) {
        if (!hardware && (ctx->slices > 1 || config.videoFormat == 1)) {
          // Use a larger rc_buffer_size for software encoding when slices are enabled,
          // because libx264 can severely degrade quality if the buffer is too small.
          // libx265 encounters this issue more frequently, so always scale the
          // buffer by 1.5x for software HEVC encoding.
          ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
        } else {
          ctx->rc_buffer_size = bitrate / config.framerate;

#ifndef __APPLE__
          if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
            ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
          }
#endif
        }
      }

      // Allow the encoding device a final opportunity to set/unset or override any options
      encode_device->init_codec_options(ctx.get(), &options);

      auto status = avcodec_open2(ctx.get(), codec, &options);
      if (status) {
        char err_str[AV_ERROR_MAX_STRING_SIZE] {0};

        if (!video_format.fallback_options.empty() && retries == 0) {
          BOOST_LOG(info)
            << "Retrying with fallback configuration options for ["sv << video_format.name << "] after error: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          continue;
        } else {
          BOOST_LOG(error)
            << "Could not open codec ["sv
            << video_format.name << "]: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          return nullptr;
        }
      }

      // Successfully opened the codec
      break;
    }

    avcodec_frame_t frame {av_frame_alloc()};
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    // Attach HDR metadata to the AVFrame, using the metadata resolved at encode-device
    // creation so the SEI always matches the control-channel metadata (and never reflects
    // a display that transiently reads SDR during a reinit).
    if (colorspace_is_hdr(colorspace)) {
      if (encode_device->hdr_metadata_valid) {
        const auto &hdr_metadata = encode_device->hdr_metadata;
        auto mdm = av_mastering_display_metadata_create_side_data(frame.get());

        mdm->display_primaries[0][0] = av_make_q(hdr_metadata.displayPrimaries[0].x, 50000);
        mdm->display_primaries[0][1] = av_make_q(hdr_metadata.displayPrimaries[0].y, 50000);
        mdm->display_primaries[1][0] = av_make_q(hdr_metadata.displayPrimaries[1].x, 50000);
        mdm->display_primaries[1][1] = av_make_q(hdr_metadata.displayPrimaries[1].y, 50000);
        mdm->display_primaries[2][0] = av_make_q(hdr_metadata.displayPrimaries[2].x, 50000);
        mdm->display_primaries[2][1] = av_make_q(hdr_metadata.displayPrimaries[2].y, 50000);

        mdm->white_point[0] = av_make_q(hdr_metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(hdr_metadata.whitePoint.y, 50000);

        mdm->min_luminance = av_make_q(hdr_metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);

        mdm->has_luminance = hdr_metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = hdr_metadata.displayPrimaries[0].x != 0 ? 1 : 0;

        if (hdr_metadata.maxContentLightLevel != 0 || hdr_metadata.maxFrameAverageLightLevel != 0) {
          auto clm = av_content_light_metadata_create_side_data(frame.get());

          clm->MaxCLL = hdr_metadata.maxContentLightLevel;
          clm->MaxFALL = hdr_metadata.maxFrameAverageLightLevel;
        }
      } else {
        BOOST_LOG(warning) << "No HDR metadata available to attach to the encoded stream";
      }
    }

    std::unique_ptr<platf::avcodec_encode_device_t> encode_device_final;

    if (!encode_device->data) {
      auto software_encode_device = std::make_unique<avcodec_software_encode_device_t>();

      if (software_encode_device->init(width, height, frame.get(), sw_fmt, hardware)) {
        return nullptr;
      }
      software_encode_device->colorspace = colorspace;

      encode_device_final = std::move(software_encode_device);
    } else {
      encode_device_final = std::move(encode_device);
    }

    if (encode_device_final->set_frame(frame.release(), ctx->hw_frames_ctx)) {
      return nullptr;
    }

    encode_device_final->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device_final),

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - (int) video_format[encoder_t::VUI_PARAMETERS]) * (1 + config.videoFormat) : 0
    );

    return session;
  }

  std::unique_ptr<nvenc_encode_session_t> make_nvenc_encode_session(const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  using initialization_cancel_t = std::function<bool()>;

  bool acquire_amf_initialization_fence_until(
    std::chrono::steady_clock::time_point deadline,
    const initialization_cancel_t &cancelled) {
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      const auto poll_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(50ms);
      if (native_amf_lifecycle_gate.begin_initialization_for(std::min(remaining, poll_interval))) return true;
      if (native_amf_lifecycle_gate.is_quarantined()) return false;
    }
    return false;
  }

  template<typename Owned>
  void defer_contended_amf_teardown(
    Owned owned,
    std::shared_ptr<platf::display_t> display_lease,
    std::string_view reason);

  std::unique_ptr<amf_encode_session_t> make_amf_encode_session(
    const config_t &client_config,
    std::unique_ptr<platf::amf_encode_device_t> encode_device,
    std::chrono::steady_clock::time_point deadline,
    const initialization_cancel_t &cancelled,
    bool &operation_cancelled,
    bool &gate_contended) {
    operation_cancelled = false;
    gate_contended = false;
    // Native AMF can wedge inside Init/Terminate on a damaged driver. Run the
    // entire initialization attempt on an owning worker so probes, failed init,
    // and real-session fallback all regain control within one watchdog interval.
    // The caller already keeps the display alive for this synchronous wait. Do
    // not also leave it inside a worker that may be detached after the watchdog:
    // that stale owner would permanently block the next display generation.
    if (native_amf_lifecycle_gate.is_quarantined()) {
      BOOST_LOG(error) << "AMF: native runtime is quarantined after a watchdog timeout; refusing to re-enter the AMD runtime"sv;
      return nullptr;
    }

    const auto gate_deadline = std::min(deadline, std::chrono::steady_clock::now() + 1s);
    if (!acquire_amf_initialization_fence_until(gate_deadline, cancelled)) {
      operation_cancelled = cancelled();
      gate_contended = !operation_cancelled && !native_amf_lifecycle_gate.is_quarantined();
      BOOST_LOG(warning) << "AMF: native initialization could not acquire the AMD runtime fence before the session deadline"sv;
      return nullptr;
    }
    if (cancelled()) {
      operation_cancelled = true;
      native_amf_lifecycle_gate.cancel_initialization();
      return nullptr;
    }
    if (deadline - std::chrono::steady_clock::now() < 1s) {
      gate_contended = true;
      native_amf_lifecycle_gate.cancel_initialization();
      BOOST_LOG(warning) << "AMF: native initialization skipped because gate contention left less than one second of vendor budget"sv;
      return nullptr;
    }

    std::optional<amf::amf_hdr_metadata> resolved_hdr_metadata;
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (encode_device->hdr_metadata_valid) {
        const auto &hdr_metadata = encode_device->hdr_metadata;
        amf::amf_hdr_metadata amf_metadata;
        for (int i = 0; i < 3; i++) {
          amf_metadata.displayPrimaries[i].x = hdr_metadata.displayPrimaries[i].x;
          amf_metadata.displayPrimaries[i].y = hdr_metadata.displayPrimaries[i].y;
        }
        amf_metadata.whitePoint.x = hdr_metadata.whitePoint.x;
        amf_metadata.whitePoint.y = hdr_metadata.whitePoint.y;
        amf_metadata.maxDisplayLuminance = hdr_metadata.maxDisplayLuminance;
        amf_metadata.minDisplayLuminance = hdr_metadata.minDisplayLuminance;
        amf_metadata.maxContentLightLevel = hdr_metadata.maxContentLightLevel;
        amf_metadata.maxFrameAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel;
        resolved_hdr_metadata = amf_metadata;
      } else {
        BOOST_LOG(warning) << "AMF: no resolved HDR metadata is available for the native bitstream"sv;
      }
    }

    auto display_lease = encode_device->release_display_lease_for_initialization();
    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<std::unique_ptr<platf::amf_encode_device_t>>>();
    const auto colorspace = encode_device->colorspace;
    std::thread initialization_thread;
    try {
      initialization_thread = std::thread {
        [encode_device = std::move(encode_device), client_config, colorspace, resolved_hdr_metadata, handoff]() mutable {
          bool handed_off_to_caller = false;
          auto initialization_gate = util::fail_guard([&handed_off_to_caller]() {
            if (!handed_off_to_caller) {
              native_amf_lifecycle_gate.cancel_initialization();
            }
          });
          auto owned_encode_device = std::move(encode_device);
          try {
            if (!owned_encode_device->initialize_hardware_device() ||
                !owned_encode_device->init_encoder(client_config, colorspace) ||
                !owned_encode_device->finish_encoder_initialization(client_config, colorspace)) {
              // Failure destruction is part of this same watchdog interval. Only
              // publish failure after the D3D/AMF resources are gone, so legacy
              // fallback cannot race a still-unwinding native runtime.
              owned_encode_device.reset();
              handoff->publish(nullptr);
              return;
            }
            if (resolved_hdr_metadata && owned_encode_device->amf &&
                !owned_encode_device->amf->set_hdr_metadata(resolved_hdr_metadata)) {
              BOOST_LOG(warning) << "AMF: HDR stream will continue without mastering metadata"sv;
            }
            handed_off_to_caller = handoff->publish(std::move(owned_encode_device));
          } catch (...) {
            owned_encode_device.reset();
            handoff->publish(nullptr);
          }
        }
      };
    } catch (const std::system_error &err) {
      native_amf_lifecycle_gate.cancel_initialization();
      BOOST_LOG(error) << "AMF: could not start native initialization worker: " << err.what();
      return nullptr;
    }

    bool handoff_cancelled = false;
    auto accepted = handoff->accept_until(deadline, cancelled, &handoff_cancelled);
    if (!accepted) {
      operation_cancelled = handoff_cancelled;
      if (!handoff_cancelled) {
        native_amf_lifecycle_gate.quarantine_initialization();
        BOOST_LOG(error) << "AMF: native encoder initialization exceeded the vendor deadline; worker will reap ownership"sv;
      } else {
        BOOST_LOG(info) << "AMF: native encoder initialization cancelled; worker will reap ownership without quarantining AMD"sv;
      }
      initialization_thread.detach();
      return nullptr;
    }

    encode_device = std::move(*accepted);
    if (!encode_device) {
      initialization_thread.join();
      return nullptr;
    }
    initialization_thread.join();

    if (!native_amf_lifecycle_gate.finish_initialization()) {
      BOOST_LOG(error) << "AMF: initialization completed after a teardown/quarantine fence; discarding the native device"sv;
      const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
      switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
        case amf::lifecycle::teardown_admission_e::granted:
          break;
        case amf::lifecycle::teardown_admission_e::quarantined:
          (void) encode_device.release();
          return nullptr;
        case amf::lifecycle::teardown_admission_e::contended:
          defer_contended_amf_teardown(std::move(encode_device), std::move(display_lease), "discarded native device"sv);
          return nullptr;
      }
      // Fresh destruction budget once the fence is held — gate contention is
      // not driver teardown time.
      const bool discarded = amf::lifecycle::run_with_timeout(
        [encode_device = std::move(encode_device)]() mutable {
          encode_device.reset();
        },
        5s);
      native_amf_lifecycle_gate.finish_teardown(discarded);
      return nullptr;
    }
    encode_device->restore_display_lease_after_initialization(std::move(display_lease));

    return std::make_unique<amf_encode_session_t>(std::move(encode_device));
  }

  std::unique_ptr<encode_session_t> make_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::encode_device_t> encode_device,
    std::chrono::steady_clock::time_point initialization_deadline = std::chrono::steady_clock::time_point::max(),
    initialization_cancel_t cancelled = {},
    bool *operation_cancelled_out = nullptr,
    bool *gate_contended_out = nullptr) {
    if (!cancelled) cancelled = []() { return false; };
    if (operation_cancelled_out) *operation_cancelled_out = false;
    if (gate_contended_out) *gate_contended_out = false;
    bool operation_cancelled = false;
    bool gate_contended = false;
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));
    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));
    } else if (dynamic_cast<platf::amf_encode_device_t *>(encode_device.get())) {
      auto amf_encode_device = boost::dynamic_pointer_cast<platf::amf_encode_device_t>(std::move(encode_device));
      auto session = make_amf_encode_session(
        config, std::move(amf_encode_device), initialization_deadline, cancelled,
        operation_cancelled, gate_contended);
      if (operation_cancelled_out) *operation_cancelled_out = operation_cancelled;
      if (gate_contended_out) *gate_contended_out = gate_contended;
      return session;
    }

    return nullptr;
  }

  std::unique_ptr<platf::encode_device_t> make_encode_device(
    platf::display_t &disp,
    const encoder_t &encoder,
    const config_t &config,
    hdr_latch_t *hdr_latch,
    bool deferred_avcodec);

  void abandon_quarantined_session(
    std::unique_ptr<encode_session_t> &session,
    std::shared_ptr<platf::display_t> display_lease = {}) {
    // The AMD runtime is already known to have an abandoned vendor call. Running
    // another destructor can overlap it and hang/crash. Intentionally retain the
    // resources until process termination without registering a static destructor.
    (void) session.release();
    // The leaked encoder/device is detached from capture. Releasing this external
    // lease is required so display reinitialization can reach use_count()==1.
    display_lease.reset();
  }

  // Fence contention is not a wedged runtime, so the session waiting on it is
  // still healthy and still destroyable. Abandoning it would strand an AMF
  // hardware encode session (a finite VCN resource), its ID3D11Device, its
  // input-surface ring and its parked output-pump thread for the life of the
  // process -- and because no quarantine is set, the host keeps streaming and
  // repeats the leak on every later contended teardown until the GPU runs out of
  // concurrent encode sessions.
  //
  // Hand ownership to a detached supervisor instead. The display lease is
  // dropped immediately, exactly as an abandon would, so capture reinitialization
  // still converges on use_count()==1 at the same moment it does today; only the
  // destruction is deferred. The supervisor takes the runtime fence before
  // touching the vendor runtime, and additionally holds
  // encode_session_teardown_mutex so the late destructor cannot race the capture
  // side's shared-surface release.
  template<typename Owned>
  void defer_contended_amf_teardown(
    Owned owned,
    std::shared_ptr<platf::display_t> display_lease,
    std::string_view reason) {
    if (!owned) {
      return;
    }

    display_lease.reset();

    BOOST_LOG(warning) << "AMF: " << reason
                       << " teardown could not acquire the AMD runtime fence before its deadline; "
                          "deferring destruction instead of abandoning a healthy session"sv;

    // std::thread's constructor moves the callable into its shared state before
    // it can throw, so the owned pointer has to survive independently of the
    // lambda for the launch-failure path to be able to fall back.
    struct deferred_teardown_t {
      Owned owned;
      std::string reason;
    };

    auto deferred = std::make_shared<deferred_teardown_t>(
      deferred_teardown_t {std::move(owned), std::string {reason}}
    );

    try {
      std::thread supervisor {[deferred]() mutable {
        // Nothing watchdogs this thread, so it can outwait a peer initialization
        // that the caller's five-second budget could not.
        const auto gate_deadline = std::chrono::steady_clock::now() + 30s;
        if (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline) !=
            amf::lifecycle::teardown_admission_e::granted) {
          BOOST_LOG(error) << "AMF: deferred " << deferred->reason
                           << " teardown never acquired the runtime fence; abandoning the session"sv;
          (void) deferred->owned.release();
          return;
        }

        const bool completed = amf::lifecycle::run_with_timeout(
          [deferred]() mutable {
            std::lock_guard lg {encode_session_teardown_mutex};
            deferred->owned.reset();
          },
          5s);
        native_amf_lifecycle_gate.finish_teardown(completed);
        if (completed) {
          BOOST_LOG(info) << "AMF: deferred " << deferred->reason << " teardown completed"sv;
        } else {
          BOOST_LOG(error) << "AMF: deferred " << deferred->reason
                           << " teardown exceeded 5 seconds; quarantining AMD encoding until host restart"sv;
        }
      }};
      supervisor.detach();
    } catch (...) {
      BOOST_LOG(error) << "AMF: could not start the deferred " << reason
                       << " teardown supervisor; abandoning the session"sv;
      (void) deferred->owned.release();
    }
  }

#ifdef _WIN32
  struct legacy_amf_session_bundle_t {
    std::unique_ptr<encode_session_t> session;
    bool hdr_metadata_valid = false;
    SS_HDR_METADATA hdr_metadata {};
    hdr_latch_t hdr_latch {};
  };

  std::optional<legacy_amf_session_bundle_t> make_legacy_amf_session_bounded(
    std::shared_ptr<platf::display_t> disp,
    const config_t &config,
    int width,
    int height,
    hdr_latch_t *hdr_latch,
    std::chrono::steady_clock::time_point deadline,
    const initialization_cancel_t &cancelled,
    bool &operation_cancelled,
    bool &gate_contended) {
    operation_cancelled = false;
    gate_contended = false;
    if (!disp || native_amf_lifecycle_gate.is_quarantined()) {
      return std::nullopt;
    }

    const auto gate_deadline = std::min(deadline, std::chrono::steady_clock::now() + 1s);
    if (!acquire_amf_initialization_fence_until(gate_deadline, cancelled)) {
      operation_cancelled = cancelled();
      gate_contended = !operation_cancelled && !native_amf_lifecycle_gate.is_quarantined();
      BOOST_LOG(error) << "AMF: legacy initialization could not acquire the AMD runtime fence before the session deadline"sv;
      return std::nullopt;
    }
    if (cancelled()) {
      operation_cancelled = true;
      native_amf_lifecycle_gate.cancel_initialization();
      return std::nullopt;
    }
    if (deadline - std::chrono::steady_clock::now() < 1s) {
      gate_contended = true;
      native_amf_lifecycle_gate.cancel_initialization();
      BOOST_LOG(warning) << "AMF: legacy initialization skipped because gate contention left less than one second of vendor budget"sv;
      return std::nullopt;
    }

    auto local_latch = hdr_latch ? *hdr_latch : hdr_latch_t {};
    auto base_device = make_encode_device(*disp, amdvce_legacy, config, &local_latch, true);
    auto prepared_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(base_device));
    if (!prepared_device) {
      native_amf_lifecycle_gate.cancel_initialization();
      return std::nullopt;
    }

    legacy_amf_session_bundle_t prepared_bundle;
    prepared_bundle.hdr_metadata_valid = prepared_device->hdr_metadata_valid;
    prepared_bundle.hdr_metadata = prepared_device->hdr_metadata;
    prepared_bundle.hdr_latch = local_latch;
    auto display_lease = prepared_device->release_display_lease_for_initialization();
    auto handoff = std::make_shared<amf::lifecycle::worker_handoff_t<legacy_amf_session_bundle_t>>();
    std::thread initialization_thread;
    try {
      initialization_thread = std::thread {
        [config, width, height, prepared_device = std::move(prepared_device), prepared_bundle = std::move(prepared_bundle), handoff]() mutable {
          bool gate_finished = false;
          auto gate_cleanup = util::fail_guard([&gate_finished]() {
            if (!gate_finished) {
              native_amf_lifecycle_gate.cancel_initialization();
            }
          });
          try {
            if (prepared_device->is_codec_supported(amdvce_legacy.codec_from_config(config).name, config)) {
              prepared_bundle.session = make_encode_session(
                nullptr, amdvce_legacy, config, width, height, std::move(prepared_device));
            }
            const bool initialized = static_cast<bool>(prepared_bundle.session);
            const bool accepted = handoff->publish(std::move(prepared_bundle));
            gate_finished = initialized && accepted;
          } catch (...) {
            handoff->publish(legacy_amf_session_bundle_t {});
          }
        }
      };
    } catch (const std::system_error &err) {
      native_amf_lifecycle_gate.cancel_initialization();
      BOOST_LOG(error) << "AMF: could not start legacy initialization worker: " << err.what();
      return std::nullopt;
    }

    bool handoff_cancelled = false;
    auto accepted = handoff->accept_until(deadline, cancelled, &handoff_cancelled);
    if (!accepted) {
      operation_cancelled = handoff_cancelled;
      if (!handoff_cancelled) {
        native_amf_lifecycle_gate.quarantine_initialization();
        BOOST_LOG(error) << "AMF: complete legacy D3D/FFmpeg initialization exceeded the vendor deadline; worker will reap ownership"sv;
      } else {
        BOOST_LOG(info) << "AMF: legacy initialization cancelled; worker will reap ownership without quarantining AMD"sv;
      }
      initialization_thread.detach();
      return std::nullopt;
    }

    initialization_thread.join();
    auto bundle = std::move(*accepted);
    if (!bundle.session) return std::nullopt;
    if (!native_amf_lifecycle_gate.finish_initialization()) {
      // The caller owns the accepted value, but publication was fenced. Avoid
      // entering a quarantined runtime from this thread.
      bundle.session.release();
      return std::nullopt;
    }
    if (auto *legacy_session = dynamic_cast<avcodec_encode_session_t *>(bundle.session.get())) {
      legacy_session->restore_display_lease_after_initialization(std::move(display_lease));
    }
    if (hdr_latch) *hdr_latch = bundle.hdr_latch;
    return bundle;
  }

  bool destroy_legacy_amf_session_bounded(std::unique_ptr<encode_session_t> &session, std::string_view reason) {
    if (!session) return true;
    auto display_lease = static_cast<avcodec_encode_session_t *>(session.get())->release_display_lease_for_driver_work();
    const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
    switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
      case amf::lifecycle::teardown_admission_e::granted:
        break;
      case amf::lifecycle::teardown_admission_e::quarantined:
        BOOST_LOG(error) << "AMF: abandoning the legacy " << reason
                         << " session because the AMD runtime is quarantined"sv;
        abandon_quarantined_session(session, std::move(display_lease));
        return false;
      case amf::lifecycle::teardown_admission_e::contended:
        defer_contended_amf_teardown(std::move(session), std::move(display_lease), reason);
        return false;
    }
    // Fresh destruction budget once the fence is held — gate contention is
    // not driver teardown time.
    const bool completed = amf::lifecycle::run_with_timeout(
      [session = std::move(session), display_lease = std::move(display_lease)]() mutable {
        std::lock_guard lock {encode_session_teardown_mutex};
        session.reset();
        (void) display_lease;
      },
      5s);
    native_amf_lifecycle_gate.finish_teardown(completed);
    if (!completed) {
      BOOST_LOG(error) << "AMF: legacy " << reason << " teardown exceeded 5 seconds; quarantining AMD encoding"sv;
    }
    return completed;
  }
#endif

  bool destroy_encode_session_bounded(std::unique_ptr<encode_session_t> &session, std::string_view reason) {
    if (!session) {
      return true;
    }

    const bool native_amf_session = dynamic_cast<amf_encode_session_t *>(session.get()) != nullptr;
    std::shared_ptr<platf::display_t> display_lease;
    if (native_amf_session) {
      // Moving the shared_ptr itself cannot enter the D3D driver. Keep the old
      // display alive inside the watchdog worker for every potentially blocking
      // ClearState/COM/AMF destruction step. If it wedges, both the device and
      // display lease remain quarantined together; releasing the display first
      // would violate the shared-resource lifetime ordering.
      display_lease = static_cast<amf_encode_session_t *>(session.get())->release_display_lease_for_driver_work();
    }
    auto owned_session = std::move(session);
    if (native_amf_session) {
      // Gate contention is not driver teardown time. A healthy initialization is
      // allowed its own watchdog interval, so acquire the fence before starting
      // the independent five-second destruction deadline.
      const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
      switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
        case amf::lifecycle::teardown_admission_e::granted:
          break;
        case amf::lifecycle::teardown_admission_e::quarantined:
          BOOST_LOG(error) << "AMF: abandoning the native " << reason
                           << " session because the AMD runtime is quarantined"sv;
          abandon_quarantined_session(owned_session, std::move(display_lease));
          return false;
        case amf::lifecycle::teardown_admission_e::contended:
          defer_contended_amf_teardown(std::move(owned_session), std::move(display_lease), reason);
          return false;
      }
      // Fresh destruction budget once the fence is held — gate contention is
      // not driver teardown time.
      const bool completed = amf::lifecycle::run_with_timeout(
        [owned_session = std::move(owned_session), display_lease = std::move(display_lease)]() mutable {
          owned_session.reset();
          (void) display_lease;
        },
        5s);
      native_amf_lifecycle_gate.finish_teardown(completed);
      if (!completed) {
        BOOST_LOG(error) << "Encoder " << reason << " teardown exceeded 5 seconds; abandoning that session"sv;
      }
      return completed;
    }
    const bool completed = amf::lifecycle::run_with_timeout(
      [owned_session = std::move(owned_session)]() mutable {
        std::lock_guard lock {encode_session_teardown_mutex};
        owned_session.reset();
      },
      5s);
    if (!completed) {
      BOOST_LOG(error) << "Encoder " << reason << " teardown exceeded 5 seconds; abandoning that session"sv;
    }
    return completed;
  }

  enum class encode_run_result_e {
    completed,
    native_amf_failed,
    temporarily_busy,
    initialization_failed,
  };

  encode_run_result_e encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t &config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    std::unique_ptr<encode_session_t> prepared_session,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    hdr_latch_t *hdr_latch,
    void *channel_data,
    std::chrono::steady_clock::time_point initialization_deadline,
    initialization_cancel_t initialization_cancelled,
    std::optional<hdr_info_raw_t> &last_hdr_info,
    rtx_hdr_metadata_refresh_state_t &rtx_hdr_metadata_refresh
  ) {
    const encoder_t *session_encoder = &encoder;
    bool initialization_was_cancelled = false;
    bool initialization_gate_contended = false;
    auto session = prepared_session ?
                     std::move(prepared_session) :
                     make_encode_session(
                       disp.get(), encoder, config, disp->width, disp->height,
                       std::move(encode_device), initialization_deadline, initialization_cancelled,
                       &initialization_was_cancelled, &initialization_gate_contended);
#ifdef _WIN32
    if (initialization_was_cancelled) return encode_run_result_e::completed;
    if (!session && &encoder == &amdvce) {
      BOOST_LOG(error) << "AMF: native session initialization failed; refusing silent amdvce_legacy fallback"sv;
    }
#endif
    if (!session) {
      if (initialization_gate_contended) return encode_run_result_e::temporarily_busy;
      return encode_run_result_e::initialization_failed;
    }
    // One RTTI lookup for the whole session — the pointer stays valid until the
    // fail_guard teardown moves the session out after the encode loop exits.
    auto *const native_session = dynamic_cast<amf_encode_session_t *>(session.get());
    const bool native_amf_session = native_session != nullptr;
#ifdef _WIN32
    const bool legacy_amf_session = session_encoder == &amdvce_legacy;
#else
    const bool legacy_amf_session = false;
#endif
    bool native_amf_runtime_failed = false;
    const auto session_encoder_flags = session_encoder->flags;

    auto shutdown_event = mail->event<bool>(mail::shutdown);

    bool force_sync_teardown = false;

    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([session_encoder_flags, legacy_amf_session, &session, &force_sync_teardown, &reinit_event, shutdown_event] {
      const bool shutdown_teardown = shutdown_event && shutdown_event->peek();
      // A display reinit (resolution/HDR/colorspace change, e.g. alt-tabbing a game on a
      // virtual display) frees the shared capture surfaces this encoder's device has open.
      // Tearing the encoder down on a detached thread during that window abandons a hung
      // NVENC session that races the surface free and the GPU's deferred eviction, which can
      // bugcheck the kernel video memory manager (VIDEO_MEMORY_MANAGEMENT_INTERNAL 0x10e),
      // especially with a second client connected. Force an ordered synchronous teardown
      // whenever a reinit is in progress so this encoder releases its surfaces before the
      // capture side frees them.
      const bool sync_teardown = force_sync_teardown || shutdown_teardown || reinit_event.peek();
      if ((session_encoder_flags & ASYNC_TEARDOWN) && !sync_teardown) {
        const bool native_amf_session = dynamic_cast<amf_encode_session_t *>(session.get()) != nullptr;
        auto display_lease = native_amf_session ?
                               static_cast<amf_encode_session_t *>(session.get())->release_display_lease_for_driver_work() :
                             legacy_amf_session ?
                               static_cast<avcodec_encode_session_t *>(session.get())->release_display_lease_for_driver_work() :
                               nullptr;
        std::thread encoder_teardown_thread {[session = std::move(session), native_amf_session, legacy_amf_session, display_lease = std::move(display_lease)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          if (native_amf_session) {
            // Supervise the vendor destructor from this already-asynchronous path.
            // On timeout, the inner worker retains ownership while future sessions
            // refuse to re-enter either AMF backend instead of accumulating more
            // calls into the same wedged AMD runtime.
            const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
            switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
              case amf::lifecycle::teardown_admission_e::granted:
                break;
              case amf::lifecycle::teardown_admission_e::quarantined:
                BOOST_LOG(error) << "AMF: abandoning the native session in async teardown because the AMD runtime is quarantined"sv;
                abandon_quarantined_session(session, std::move(display_lease));
                return;
              case amf::lifecycle::teardown_admission_e::contended:
                defer_contended_amf_teardown(std::move(session), std::move(display_lease), "async native AMF"sv);
                return;
            }
            // Fresh destruction budget once the fence is held — gate contention
            // is not driver teardown time.
            const bool completed = amf::lifecycle::run_with_timeout(
              [session = std::move(session), display_lease = std::move(display_lease)]() mutable {
                session.reset();
                (void) display_lease;
              },
              5s);
            native_amf_lifecycle_gate.finish_teardown(completed);
            if (!completed) {
              BOOST_LOG(error) << "AMF: async teardown exceeded 5 seconds; quarantining native AMF until host restart"sv;
            }
          } else if (legacy_amf_session) {
            const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
            switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
              case amf::lifecycle::teardown_admission_e::granted:
                break;
              case amf::lifecycle::teardown_admission_e::quarantined:
                BOOST_LOG(error) << "AMF: abandoning the legacy session in async teardown because the AMD runtime is quarantined"sv;
                abandon_quarantined_session(session, std::move(display_lease));
                return;
              case amf::lifecycle::teardown_admission_e::contended:
                defer_contended_amf_teardown(std::move(session), std::move(display_lease), "async legacy AMF"sv);
                return;
            }
            // Fresh destruction budget once the fence is held — gate contention
            // is not driver teardown time.
            const bool completed = amf::lifecycle::run_with_timeout(
              [session = std::move(session), display_lease = std::move(display_lease)]() mutable {
                std::lock_guard lg {encode_session_teardown_mutex};
                session.reset();
                (void) display_lease;
              },
              5s);
            native_amf_lifecycle_gate.finish_teardown(completed);
            if (!completed) {
              BOOST_LOG(error) << "AMF: legacy async teardown exceeded 5 seconds; quarantining AMD encoding until host restart"sv;
            }
          } else {
            std::lock_guard lg {encode_session_teardown_mutex};
            session.reset();
          }
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      } else {
        if ((session_encoder_flags & ASYNC_TEARDOWN) && sync_teardown) {
          BOOST_LOG(debug) << "Using synchronous encoder teardown during "
                           << (shutdown_teardown ? "shutdown"sv : "capture reinit"sv);
        }

        // Destroy the session here, under the teardown mutex, rather than letting it die at
        // scope exit. During a capture reinit both video threads reach this point at the same
        // moment; unserialized concurrent NVENC/D3D11 device destruction crashes the NVIDIA UMD
        // (access violation in nvwgf2umx during cross-device shared-resource dependency cleanup).
        const bool native_amf_session = dynamic_cast<amf_encode_session_t *>(session.get()) != nullptr;
        if (native_amf_session) {
          destroy_encode_session_bounded(session, "native AMF"sv);
        } else if (dynamic_cast<avcodec_encode_session_t *>(session.get())) {
          // A wedged driver can block avcodec session destruction indefinitely.
          // During shutdown that blocks videoThread.join();
          // during reinit it prevents capture recovery. Run destruction on a helper
          // thread and abandon it if it overruns. An abandoned
          // session leaks, but the stream host survives.
          // NVENC keeps the fully synchronous teardown: its driver waits are already bounded
          // (nvenc_base) and its teardown-vs-surface-free ordering is load-bearing
          // (VIDEO_MEMORY_MANAGEMENT_INTERNAL 0x10e bugcheck).
          std::promise<void> done;
          auto done_future = done.get_future();
          auto legacy_display_lease = legacy_amf_session ?
                                        static_cast<avcodec_encode_session_t *>(session.get())->release_display_lease_for_driver_work() :
                                        nullptr;
          if (legacy_amf_session) {
            const auto gate_deadline = std::chrono::steady_clock::now() + 5s;
            switch (native_amf_lifecycle_gate.begin_teardown_until(gate_deadline)) {
              case amf::lifecycle::teardown_admission_e::granted:
                break;
              case amf::lifecycle::teardown_admission_e::quarantined:
                BOOST_LOG(error) << "AMF: abandoning the legacy session in sync teardown because the AMD runtime is quarantined"sv;
                abandon_quarantined_session(session, std::move(legacy_display_lease));
                return;
              case amf::lifecycle::teardown_admission_e::contended:
                defer_contended_amf_teardown(std::move(session), std::move(legacy_display_lease), "sync legacy AMF"sv);
                return;
            }
            std::thread teardown_thread {[session = std::move(session), done = std::move(done), display_lease = std::move(legacy_display_lease)]() mutable {
              std::lock_guard lg {encode_session_teardown_mutex};
              session.reset();
              (void) display_lease;
              done.set_value();
            }};
            // Fresh destruction budget once the fence is held — gate contention
            // is not driver teardown time.
            const bool completed = done_future.wait_for(5s) == std::future_status::ready;
            native_amf_lifecycle_gate.finish_teardown(completed);
            if (completed) teardown_thread.join();
            else {
              BOOST_LOG(error) << "Encoder teardown did not finish within its 5 second destruction budget; abandoning the session"sv;
              teardown_thread.detach();
            }
            return;
          }
          std::thread teardown_thread {[session = std::move(session), done = std::move(done), display_lease = std::move(legacy_display_lease)]() mutable {
            std::lock_guard lg {encode_session_teardown_mutex};
            session.reset();
            (void) display_lease;
            done.set_value();
          }};
          const bool completed = done_future.wait_for(5s) == std::future_status::ready;
          if (completed) {
            teardown_thread.join();
          } else {
            BOOST_LOG(error) << "Encoder teardown did not finish within 5 seconds; abandoning the session to keep the stream host alive"sv;
            teardown_thread.detach();
          }
        } else {
          std::lock_guard lg {encode_session_teardown_mutex};
          session.reset();
        }
      }
    });

    auto native_amf_failure = [&]() {
      force_sync_teardown = native_amf_session;
      return native_amf_session ? encode_run_result_e::native_amf_failed : encode_run_result_e::completed;
    };

    if (config.encodingFramerate <= 0) {
      const int fallback_fps = config.framerate > 0 ? config.framerate * 1000 : 60000;
      BOOST_LOG(warning) << "Encoding framerate missing; falling back to " << fallback_fps;
      config.encodingFramerate = fallback_fps;
    }

    // set max frame time based on client-requested target framerate.
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target * 1000 : std::max(config.encodingFramerate / 5, 10000);
    auto max_frametime = std::chrono::nanoseconds(1000ms) * 1000 / minimum_fps_target;
    auto encode_frame_threshold = std::chrono::nanoseconds(1000ms) * 1000 / config.encodingFramerate;
    auto frame_variation_threshold = encode_frame_threshold / 4;
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << (minimum_fps_target / 2000) << "fps ("sv << max_frametime * 2 << ")"sv;
    BOOST_LOG(info) << "Encoding Frame threshold: "sv << encode_frame_threshold;

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
    auto bitrate_events = mail->event<int>(mail::dynamic_bitrate);

    {
      // Load a dummy image into the AVFrame to ensure we have something to encode
      // even if we timeout waiting on the first frame. This is a relatively large
      // allocation which can be freed immediately after convert(), so we do this
      // in a separate scope.
      auto dummy_img = disp->alloc_img();
      if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        return native_amf_failure();
      }
    }

    if (config.input_only) {
      BOOST_LOG(info) << "Input only session, video will not be captured."sv;

      // Native AMF is asynchronous and may initially reject the dummy surface
      // with backpressure. Resubmit the same logical frame until it is accepted,
      // then keep draining until that accepted input actually emits.
      const auto now = std::chrono::steady_clock::now();
      const auto dummy_frame_index = static_cast<uint64_t>(frame_nr++);
      if (encode(dummy_frame_index, *session, packets, channel_data, now, now, now)) {
        BOOST_LOG(error) << "Could not encode dummy video packet"sv;
        return native_amf_failure();
      }

      if (auto *amf_session = dynamic_cast<amf_encode_session_t *>(session.get())) {
        const auto acceptance_deadline = std::chrono::steady_clock::now() + 2s;
        bool dummy_accepted = amf_session->was_last_input_accepted();
        while (!dummy_accepted && std::chrono::steady_clock::now() < acceptance_deadline) {
          auto delayed = amf_session->drain_frames(10ms);
          if (delayed.fatal || std::any_of(delayed.frames.begin(), delayed.frames.end(), [](const auto &frame) { return frame.fatal; })) {
            BOOST_LOG(error) << "AMF failed while accepting the input-only dummy packet"sv;
            return native_amf_failure();
          }
          deliver_amf_frames(dummy_frame_index, *amf_session, delayed.frames, packets, channel_data);
          if (encode(dummy_frame_index, *session, packets, channel_data, now, now, now)) {
            BOOST_LOG(error) << "Could not resubmit the AMF input-only dummy packet"sv;
            return native_amf_failure();
          }
          dummy_accepted = amf_session->was_last_input_accepted();
        }
        if (!dummy_accepted) {
          BOOST_LOG(error) << "AMF did not accept the input-only dummy packet within 2 seconds"sv;
          return native_amf_failure();
        }

        // A PA encoder deliberately holds its final lookahead frame until AMF Drain
        // marks end-of-input. Input-only sessions have no future live frame to do it.
        if (!amf_session->begin_drain()) {
          BOOST_LOG(error) << "Could not begin draining the AMF input-only session"sv;
          return native_amf_failure();
        }
        const auto drain_deadline = std::chrono::steady_clock::now() + 2s;
        while (!amf_session->has_emitted_frame(dummy_frame_index) && std::chrono::steady_clock::now() < drain_deadline) {
          auto delayed = amf_session->drain_frames(50ms);
          if (delayed.fatal || std::any_of(delayed.frames.begin(), delayed.frames.end(), [](const auto &frame) { return frame.fatal; })) {
            BOOST_LOG(error) << "AMF failed while draining the input-only dummy packet"sv;
            return native_amf_failure();
          }
          deliver_amf_frames(dummy_frame_index, *amf_session, delayed.frames, packets, channel_data);
        }
        if (!amf_session->has_emitted_frame(dummy_frame_index)) {
          BOOST_LOG(error) << "AMF did not emit the input-only dummy packet within 2 seconds"sv;
          return native_amf_failure();
        }
      }

      while (true) {
        if (shutdown_event->peek() || !images->running() || (reinit_event.peek())) {
          return encode_run_result_e::completed;
        } else {
          std::this_thread::sleep_for(300ms);
        }
      }
    }

    std::optional<std::chrono::steady_clock::time_point> encode_frame_timestamp;
    encode_bootstrap_state_t bootstrap_state {.allow_placeholder_before_first_real = frame_nr <= 1};

    // Per-session encode-loop accounting. When several clients share one capture target, a
    // single client can freeze while the others stream fine, and nothing else in the log
    // distinguishes a session that is encoding live frames from one that is starved or gated.
    // The channel_data pointer is a stable per-session tag for correlating these lines.
    struct {
      uint64_t popped_real = 0;
      uint64_t popped_placeholder = 0;
      uint64_t pop_timeouts = 0;
      uint64_t gate_skipped = 0;
      uint64_t encoded = 0;
      uint64_t dropped_submissions = 0;
      std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
    } loop_stats;

    while (true) {
      if (auto now = std::chrono::steady_clock::now(); now - loop_stats.last_log >= 10s) {
        BOOST_LOG(debug) << "Encode loop [" << channel_data << "] " << config.width << 'x' << config.height
                         << ": popped_real=" << loop_stats.popped_real
                         << " popped_placeholder=" << loop_stats.popped_placeholder
                         << " pop_timeouts=" << loop_stats.pop_timeouts
                         << " gate_skipped=" << loop_stats.gate_skipped
                         << " encoded=" << loop_stats.encoded
                         << " dropped_submissions=" << loop_stats.dropped_submissions
                         << " frame_nr=" << frame_nr;
        loop_stats = {};
        loop_stats.last_log = now;
      }
      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      const bool reinit_pending = reinit_event.peek() && frame_nr > 1;
      if (shutdown_event->peek() || !images->running() || reinit_pending) {
        force_sync_teardown = reinit_pending;
        break;
      }

      // Apply any runtime bitrate change, coalescing rapid ABR updates to the latest value so a
      // burst of requests causes at most one reconfigure/rebuild. NVENC reconfigures the live
      // encoder seamlessly; encoders that cannot (avcodec-based) report failure and we rebuild this
      // session by breaking out, so capture_async re-enters with config (held by reference) anew.
      std::optional<int> latest_bitrate;
      while (bitrate_events->peek()) {
        if (auto new_bitrate = bitrate_events->pop(0ms)) {
          latest_bitrate = *new_bitrate;
        }
      }
      if (latest_bitrate) {
        config.bitrate = *latest_bitrate;
        config.client_requested_bitrate = *latest_bitrate;
        if (session->set_bitrate(*latest_bitrate)) {
          BOOST_LOG(info) << "Applied runtime bitrate "sv << *latest_bitrate << " kbps (live)"sv;
        } else if (frame_nr > 1) {
          BOOST_LOG(info) << "Rebuilding encoder to apply runtime bitrate "sv << *latest_bitrate << " kbps"sv;
          break;
        }
      }

      bool requested_idr_frame = false;

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      std::optional<std::chrono::steady_clock::time_point> capture_timestamp;
      std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp;
      bool placeholder_input = bootstrap_state.current_input_placeholder;

      // Encode at a minimum FPS to avoid image quality issues with static content
      if (!requested_idr_frame || images->peek()) {
        auto image_wait_budget = max_frametime;
        if (bootstrap_state.should_encode_placeholder()) {
          // Prime a lookahead encoder immediately; waiting for minimum FPS between
          // bootstrap placeholders needlessly delays the first packet.
          image_wait_budget = decltype(max_frametime)::zero();
        }

        const auto image_wait_started = std::chrono::steady_clock::now();
        if (auto *amf_session = native_session;
            amf_session && image_wait_budget > decltype(max_frametime)::zero()) {
          // Never hold an already-completed packet behind conversion/submission
          // of a newer image. This zero-wait drain also runs during continuous
          // motion, where images->peek() remains true and the sparse loop below
          // intentionally yields immediately.
          if (amf_session->has_completed_output()) {
            auto ready = amf_session->drain_frames(0ms);
            if (ready.fatal || std::any_of(ready.frames.begin(), ready.frames.end(), [](const auto &frame) { return frame.fatal; })) {
              BOOST_LOG(error) << "AMF failed while delivering ready output"sv;
              native_amf_runtime_failed = true;
              break;
            }
            deliver_amf_frames(frame_nr, *amf_session, ready.frames, packets, channel_data);
          }

          // Wait for either the next capture or output that became due with the
          // previous native submission. This preserves high-refresh pipelining
          // when another image arrives, while delivering the final moving frame
          // promptly when WGC transitions to a sparse/static cadence.
          const auto wait_started = std::chrono::steady_clock::now();
          const auto delivery_grace = std::min(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(image_wait_budget),
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(32ms));
          const auto delivery_deadline = wait_started + delivery_grace;
          bool delayed_output_failed = false;
          while (!images->peek() && amf_session->has_output_due() &&
                 std::chrono::steady_clock::now() < delivery_deadline) {
            auto delayed = amf_session->drain_frames(1ms);
            if (delayed.fatal || std::any_of(delayed.frames.begin(), delayed.frames.end(), [](const auto &frame) { return frame.fatal; })) {
              BOOST_LOG(error) << "AMF failed while delivering sparse-capture output"sv;
              delayed_output_failed = true;
              break;
            }
            deliver_amf_frames(frame_nr, *amf_session, delayed.frames, packets, channel_data);
          }
          if (delayed_output_failed) {
            native_amf_runtime_failed = true;
            break;
          }

          const auto waited = std::chrono::steady_clock::now() - wait_started;
          image_wait_budget = waited < image_wait_budget ?
                                image_wait_budget - waited :
                                decltype(max_frametime)::zero();
        }

        if (auto *amf_session = native_session;
            amf_session && amf_session->has_retained_preanalysis_tail()) {
          // Check after sparse draining as well as before the final image wait:
          // QueryOutput may have completed during the drain and exposed PA's
          // retained tail. The one-shot session gate prevents the duplicate used
          // here from arming an endless negotiated-FPS duplicate train.
          const auto tail_flush_budget = std::chrono::duration_cast<decltype(image_wait_budget)>(
            encode_frame_threshold + 1ms);
          const auto tail_started = amf_session->input_accepted_at().value_or(image_wait_started);
          const auto elapsed = std::chrono::duration_cast<decltype(image_wait_budget)>(
            std::chrono::steady_clock::now() - tail_started);
          const auto remaining_tail_budget = elapsed < tail_flush_budget ?
                                               tail_flush_budget - elapsed :
                                               decltype(image_wait_budget)::zero();
          image_wait_budget = std::min(image_wait_budget, remaining_tail_budget);
        }

        if (auto img = images->pop(image_wait_budget)) {
          placeholder_input = is_placeholder_capture_image(*img);
          if (placeholder_input) {
            ++loop_stats.popped_placeholder;
          } else {
            ++loop_stats.popped_real;
          }
          if (!placeholder_input && bootstrap_state.current_input_placeholder) {
            session->request_idr_frame();
          }
          if (!placeholder_input) {
            capture_timestamp = img->frame_timestamp;
            frame_timestamp = capture_timestamp;
            host_processing_timestamp = img->host_processing_timestamp;
          }
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            native_amf_runtime_failed = native_amf_session;
            break;
          }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
          if (refresh_rtx_hdr_metadata_if_needed(
                config,
                *session,
                hdr_event,
                last_hdr_info,
                rtx_hdr_metadata_refresh
              )) {
            session->request_idr_frame();
          }
#endif

          bootstrap_state.current_input_placeholder = placeholder_input;

          if (!placeholder_input) {
            bootstrap_state.real_frame_seen = true;
            if (!encode_frame_timestamp) {
              encode_frame_timestamp = *frame_timestamp;
            }

            const auto time_diff = (*frame_timestamp > *encode_frame_timestamp)
              ? (*frame_timestamp - *encode_frame_timestamp)
              : (*encode_frame_timestamp - *frame_timestamp);
            if (time_diff < frame_variation_threshold) {
              *frame_timestamp = *encode_frame_timestamp;
            } else {
              *encode_frame_timestamp = *frame_timestamp;
            }

            *encode_frame_timestamp += encode_frame_threshold;
          } else {
            frame_timestamp.reset();
            host_processing_timestamp.reset();
          }

        } else if (!images->running()) {
          break;
        } else {
          ++loop_stats.pop_timeouts;
          placeholder_input = bootstrap_state.current_input_placeholder;
        }
      }

      if (placeholder_input && !bootstrap_state.should_encode_placeholder()) {
        ++loop_stats.gate_skipped;
        continue;
      }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp, capture_timestamp, host_processing_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        native_amf_runtime_failed = native_amf_session;
        break;
      }
      ++loop_stats.encoded;

      // A dropped submission leaves a hole in the wire frameIndex sequence, which
      // the client reads as loss. Reusing the index instead is NOT safe: several
      // recoverable conditions (a minimum-FPS duplicate finding no free surface,
      // exhausted submit backpressure) report "not accepted" indefinitely, and
      // withholding the increment turns those into a permanently frozen stream.
      // Count the drops so the condition stays observable.
      if (native_session && !native_session->was_last_input_accepted()) {
        ++loop_stats.dropped_submissions;
      }

      if (placeholder_input) {
        // PA can accept the first placeholder while intentionally emitting
        // nothing until its lookahead is primed. Keep submitting placeholders
        // until a packet actually exists; accepted input is not packet delivery.
        bootstrap_state.placeholder_encoded = !native_session || native_session->has_emitted_any_frame();
      }

      session->request_normal_frame();

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear
      // This is useful for KVM switch scenarios where mouse may disappear during streaming
      platf::enable_mouse_keys();
    }
    if (native_amf_runtime_failed) {
      force_sync_teardown = true;
    }
    return native_amf_runtime_failed ?
             encode_run_result_e::native_amf_failed :
             encode_run_result_e::completed;
  }

  input::touch_port_t make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    // we initialize scalar_tpcoords and logical dimensions to default values in case they are not set (non-KMS)
    float scalar_tpcoords = 1.0f;
    int display_env_logical_width = 0;
    int display_env_logical_height = 0;
    if (display->logical_width > 0 && display->logical_height > 0 &&
        display->env_logical_width > 0 && display->env_logical_height > 0) {
      float lwd = display->logical_width;
      float lhd = display->logical_height;
      scalar_tpcoords = std::fminf(wd / lwd, hd / lhd);
      display_env_logical_width = display->env_logical_width;
      display_env_logical_height = display->env_logical_height;
    }

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
      scalar_tpcoords,
      display_env_logical_width,
      display_env_logical_height
    };
  }

  std::unique_ptr<platf::encode_device_t> make_encode_device(
    platf::display_t &disp,
    const encoder_t &encoder,
    const config_t &config,
    hdr_latch_t *hdr_latch = nullptr,
    bool deferred_avcodec = false) {
    std::unique_ptr<platf::encode_device_t> result;

#ifdef _WIN32
    if (&encoder == &amdvce_legacy && native_amf_lifecycle_gate.is_quarantined()) {
      BOOST_LOG(error) << "AMF: refusing legacy initialization while the AMD runtime is quarantined"sv;
      return nullptr;
    }
#endif

    const bool display_is_hdr = disp.is_hdr();
    bool hdr_display = display_is_hdr;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    // When NVIDIA TrueHDR (RTX HDR) is enabled for an HDR stream, synthesize HDR from an
    // SDR capture. The source display may intentionally remain SDR; the actual SDR->HDR
    // conversion happens in the encode device's convert() step.
    const bool rtx_hdr_stream = config.rtx_hdr_active;
    hdr_display = hdr_display || rtx_hdr_stream;
#endif

    // HDR colorspace latch. A virtual display created for an HDR session can briefly
    // re-enumerate as SDR during a capture reinit (the double-refresh / HDR-profile mode
    // change that happens ~1s into a session momentarily drops the output to G22 8-bit).
    // Without this guard the encoder gets rebuilt for an 8-bit SDR colorspace mid-session
    // and streams SDR frames to an HDR client, which faults the client decoder
    // ("decoder reported error") and tears the session down. Once an HDR-requested session
    // has actually established HDR, keep the HDR colorspace for the rest of the session so a
    // momentary SDR reading during a reinit can no longer downgrade the wire colorspace.
    // (Genuinely-SDR sources never latch, because hdr_display is never true for them.)
    if (config.dynamicRange > 0 && !config.prefer_sdr_10bit && !config.force_sdr && hdr_latch) {
      if (hdr_display) {
        hdr_latch->latched = true;
      } else if (hdr_latch->latched) {
        BOOST_LOG(info) << "Display momentarily reported SDR during reinit; keeping HDR colorspace for this HDR session.";
        hdr_display = true;
      }
    }

    auto colorspace = colorspace_from_client_config(config, hdr_display);

    platf::pix_fmt_e pix_fmt;
    if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    } else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = encoder.codec_from_config(config).name;

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name);

      auto color_coding = colorspace.colorspace == colorspace_e::bt2020    ? "HDR (Rec. 2020 + SMPTE 2084 PQ)" :
                          colorspace.colorspace == colorspace_e::rec601    ? "SDR (Rec. 601)" :
                          colorspace.colorspace == colorspace_e::rec709    ? "SDR (Rec. 709)" :
                          colorspace.colorspace == colorspace_e::bt2020sdr ? "SDR (Rec. 2020)" :
                                                                             "unknown";

      BOOST_LOG(info) << "Color coding: " << color_coding;
      BOOST_LOG(info) << "Color depth: " << colorspace.bit_depth << "-bit";
      BOOST_LOG(info) << "Color range: " << (colorspace.full_range ? "JPEG" : "MPEG");
    }

    if (dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = deferred_avcodec ?
                 disp.make_deferred_avcodec_encode_device(pix_fmt) :
                 disp.make_avcodec_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_amf *>(encoder.platform_formats.get())) {
      result = disp.make_amf_encode_device(pix_fmt);
    }

    if (result) {
      result->colorspace = colorspace;
      result->rtx_hdr_active = config.rtx_hdr_active;

      // Resolve the stream's HDR metadata once, here, so every consumer (control-channel
      // HDR mode message, mastering-display SEI) reports the same values. Never query the
      // display while it reads SDR: on Windows get_hdr_metadata() succeeds unconditionally
      // and would return SDR-mode luminance, which clients like moonlight-xbox push straight
      // into the HDMI HDR InfoFrame.
      if (colorspace_is_hdr(colorspace)) {
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
        if (config.rtx_hdr_active) {
          // SDR source + TrueHDR: report the conversion's target peak, not the capture panel's.
          result->hdr_metadata = synthesize_rtx_hdr_metadata(config.rtx_hdr_peak_nits);
          result->hdr_metadata_valid = true;
          BOOST_LOG(info) << "RTX HDR: synthesized HDR10 metadata (peak " << result->hdr_metadata.maxDisplayLuminance << " nits) for SDR source";
        } else
#endif
        if (display_is_hdr && disp.get_hdr_metadata(result->hdr_metadata)) {
          result->hdr_metadata_valid = true;
          if (hdr_latch) {
            hdr_latch->metadata = result->hdr_metadata;
            hdr_latch->metadata_valid = true;
          }
        } else if (hdr_latch && hdr_latch->metadata_valid) {
          // Latched HDR colorspace but the display transiently reads SDR; reuse the
          // metadata captured when this session established HDR.
          result->hdr_metadata = hdr_latch->metadata;
          result->hdr_metadata_valid = true;
        } else {
          BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
        }
      }
    }

    return result;
  }

  std::optional<sync_session_t> make_synced_session(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {
    sync_session_t encode_session;

    encode_session.ctx = &ctx;
    const auto initialization_deadline = std::chrono::steady_clock::now() + 5s;
    initialization_cancel_t initialization_cancelled = [&]() {
      return ctx.shutdown_event && ctx.shutdown_event->peek();
    };

    std::unique_ptr<platf::encode_device_t> encode_device;
    std::unique_ptr<encode_session_t> session;
    bool session_hdr_metadata_valid = false;
    SS_HDR_METADATA session_hdr_metadata {};
#ifdef _WIN32
    if (&encoder == &amdvce_legacy) {
      bool legacy_cancelled = false;
      bool legacy_gate_contended = false;
      auto legacy = make_legacy_amf_session_bounded(
        disp, ctx.config, img.width, img.height, &ctx.hdr_latch,
        initialization_deadline, initialization_cancelled,
        legacy_cancelled, legacy_gate_contended);
      if (legacy_cancelled || legacy_gate_contended) return std::nullopt;
      if (legacy) {
        session = std::move(legacy->session);
        session_hdr_metadata_valid = legacy->hdr_metadata_valid;
        session_hdr_metadata = legacy->hdr_metadata;
      }
    } else
#endif
    {
      encode_device = make_encode_device(*disp, encoder, ctx.config, &ctx.hdr_latch);
      if (encode_device) {
        session_hdr_metadata_valid = encode_device->hdr_metadata_valid;
        session_hdr_metadata = encode_device->hdr_metadata;
      }
    }
    if (!encode_device && !session) return std::nullopt;

    // absolute mouse coordinates require that the dimensions of the screen are known
    ctx.touch_port_events->raise(make_port(disp.get(), ctx.config));

    // Update client with our current HDR stream state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (session_hdr_metadata_valid) {
      hdr_info = std::make_unique<hdr_info_raw_t>(true, session_hdr_metadata);
    }
    raise_hdr_info_if_changed(ctx.hdr_events, ctx.last_hdr_info, std::move(hdr_info));

    if (!session) {
      session = make_encode_session(
        disp.get(), encoder, ctx.config, img.width, img.height,
        std::move(encode_device), initialization_deadline, initialization_cancelled);
    }
    if (!session) {
      return std::nullopt;
    }

    // Load the initial image to prepare for encoding
    if (session->convert(img)) {
      BOOST_LOG(error) << "Could not convert initial image"sv;
      return std::nullopt;
    }

    encode_session.bootstrap.allow_placeholder_before_first_real = ctx.frame_nr <= 1;
    encode_session.session = std::move(session);

    return encode_session;
  }

  encode_e encode_run_sync(
    std::vector<std::unique_ptr<sync_session_ctx_t>> &synced_session_ctxs,
    encode_session_ctx_queue_t &encode_session_ctx_queue,
    std::vector<std::string> &display_names,
    int &display_p
  ) {
    const auto *enc_ptr = chosen_encoder;
    if (!enc_ptr) {
      BOOST_LOG(error) << "No encoder available for sync encoding"sv;
      return encode_e::error;
    }
    const auto &encoder = *enc_ptr;

    std::shared_ptr<platf::display_t> disp;

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    if (synced_session_ctxs.empty()) {
      auto ctx = encode_session_ctx_queue.pop();
      if (!ctx) {
        return encode_e::ok;
      }

      synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*ctx)));
    }

    while (encode_session_ctx_queue.running()) {
#ifdef _WIN32
      wait_for_recent_display_apply_stability();
#endif
      // Refresh display names since a display removal might have caused the reinitialization
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      if (!ensure_virtual_display_ready(display_names, display_p)) {
        std::this_thread::sleep_for(50ms);
        continue;
      }

      // Process any pending display switch with the new list of displays.
      // Negative values mean "reinit only; keep display selection logic intact".
      if (switch_display_event->peek()) {
        const int requested = *switch_display_event->pop();
        if (requested >= 0) {
          display_p = std::clamp(requested, 0, (int) display_names.size() - 1);
        }
      }

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], synced_session_ctxs.front()->config);
      if (disp) {
        break;
      }
    }

    if (!disp) {
      return encode_e::error;
    }

    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get())) {
      return encode_e::error;
    }

    std::vector<sync_session_t> synced_sessions;
    for (auto &ctx : synced_session_ctxs) {
      auto synced_session = make_synced_session(disp, encoder, *img, *ctx);
      if (!synced_session) {
        return encode_e::error;
      }

      synced_sessions.emplace_back(std::move(*synced_session));
    }

    auto ec = platf::capture_e::ok;
    while (encode_session_ctx_queue.running()) {
      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        while (encode_session_ctx_queue.peek()) {
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp, encoder, *img, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        KITTY_WHILE_LOOP(auto pos = std::begin(synced_sessions), pos != std::end(synced_sessions), {
          auto ctx = pos->ctx;
          if (ctx->shutdown_event->peek()) {
            // Let waiting thread know it can delete shutdown_event
            ctx->join_event->raise(true);

            pos = synced_sessions.erase(pos);
            synced_session_ctxs.erase(std::find_if(std::begin(synced_session_ctxs), std::end(synced_session_ctxs), [&ctx_p = ctx](auto &ctx) {
              return ctx.get() == ctx_p;
            }));

            if (synced_sessions.empty()) {
              return false;
            }

            continue;
          }

          if (ctx->idr_events->peek()) {
            pos->session->request_idr_frame();
            ctx->idr_events->pop();
          }
          if (ctx->bitrate_events->peek()) {
            // Coalesce rapid ABR updates to the latest requested value.
            std::optional<int> latest_bitrate;
            while (ctx->bitrate_events->peek()) {
              if (auto new_bitrate = ctx->bitrate_events->pop(0ms)) {
                latest_bitrate = *new_bitrate;
              }
            }
            if (latest_bitrate) {
              ctx->config.bitrate = *latest_bitrate;
              ctx->config.client_requested_bitrate = *latest_bitrate;
              if (pos->session->set_bitrate(*latest_bitrate)) {
                BOOST_LOG(info) << "Applied runtime bitrate "sv << *latest_bitrate << " kbps (live, sync)"sv;
              } else {
                // avcodec encoder: rebuild synced sessions from their (now-updated) ctx config.
                BOOST_LOG(info) << "Rebuilding encoder to apply runtime bitrate "sv << *latest_bitrate << " kbps (sync)"sv;
                ec = platf::capture_e::reinit;
                return false;
              }
            }
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          std::optional<std::chrono::steady_clock::time_point> capture_timestamp;
          std::optional<std::chrono::steady_clock::time_point> host_processing_timestamp;
          bool placeholder_input = pos->bootstrap.current_input_placeholder;

          if (frame_captured) {
            placeholder_input = is_placeholder_capture_image(*img);
            if (!placeholder_input && pos->bootstrap.current_input_placeholder) {
              pos->session->request_idr_frame();
            }

            if (!placeholder_input) {
              capture_timestamp = img->frame_timestamp;
              host_processing_timestamp = img->host_processing_timestamp;
            }

            if (pos->session->convert(*img)) {
              BOOST_LOG(error) << "Could not convert image"sv;
              ctx->shutdown_event->raise(true);

              continue;
            }

#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
            if (refresh_rtx_hdr_metadata_if_needed(
                  ctx->config,
                  *pos->session,
                  ctx->hdr_events,
                  ctx->last_hdr_info,
                  ctx->rtx_hdr_metadata_refresh
                )) {
              pos->session->request_idr_frame();
            }
#endif

            pos->bootstrap.current_input_placeholder = placeholder_input;

            if (!placeholder_input) {
              pos->bootstrap.real_frame_seen = true;
              frame_timestamp = capture_timestamp;
            }
          } else {
            placeholder_input = pos->bootstrap.current_input_placeholder;
          }

          if (placeholder_input && !pos->bootstrap.should_encode_placeholder()) {
            ++pos;
            continue;
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp, capture_timestamp, host_processing_timestamp)) {
            BOOST_LOG(error) << "Could not encode video packet"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          if (placeholder_input) {
            auto *amf_session = dynamic_cast<amf_encode_session_t *>(pos->session.get());
            pos->bootstrap.placeholder_encoded = !amf_session || amf_session->has_emitted_any_frame();
          }

          pos->session->request_normal_frame();

          ++pos;
        })

        if (switch_display_event->peek()) {
          ec = platf::capture_e::reinit;
          return false;
        }

        return true;
      };

      auto pull_free_image_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img;
        img_out->frame_timestamp.reset();
        img_out->capture_pacing_timestamp.reset();
        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return ec != platf::capture_e::ok ? ec : status;
      }
    }

    return encode_e::ok;
  }

  void captureThreadSync() {
    auto ref = capture_thread_sync.ref();

    std::vector<std::unique_ptr<sync_session_ctx_t>> synced_session_ctxs;

    auto &ctx = ref->encode_session_ctx_queue;
    auto lg = util::fail_guard([&]() {
      ctx.stop();

      for (auto &ctx : synced_session_ctxs) {
        ctx->shutdown_event->raise(true);
        ctx->join_event->raise(true);
      }

      for (auto &ctx : ctx.unsafe()) {
        ctx.shutdown_event->raise(true);
        ctx.join_event->raise(true);
      }
    });

    // Encoding and capture take place on this thread. Late frames here turn into
    // late hand-offs to the broadcast thread, producing the burst-then-idle pattern
    // the send pacer is meant to absorb. Runs at critical (THREAD_PRIORITY_HIGHEST /
    // nice -15, not a realtime class) — the same level the async capture thread uses.
    platf::set_thread_name("video::capture_sync");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    std::vector<std::string> display_names;
    int display_p = -1;
    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {}
  }

  void capture_async(
    safe::mail_t mail,
    config_t &config,
    void *channel_data
  ) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto ref = capture_thread_async.ref();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t {images, config});

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    // Per-session HDR latch and last-raised HDR info, persisting across capture reinits
    // so a transient SDR display reading cannot downgrade or re-signal an HDR stream.
    hdr_latch_t hdr_latch;
    std::optional<hdr_info_raw_t> last_hdr_info;
    rtx_hdr_metadata_refresh_state_t rtx_hdr_metadata_refresh;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);
    int consecutive_encoder_initialization_failures = 0;
#ifdef _WIN32
    int consecutive_native_amf_runtime_failures = 0;
#endif

    // Encoding takes place on this thread (async-capture mode; capture lives in
    // capture_thread_async at critical already). Match it so neither half of the
    // pipeline waits on the other for a scheduler quantum. Critical is
    // THREAD_PRIORITY_HIGHEST / nice -15, not a realtime class.
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          continue;
        }

        display = ref->display_wp->lock();
      }

      auto *enc_ptr = chosen_encoder;
      if (!enc_ptr) {
        BOOST_LOG(error) << "No encoder available for async capture"sv;
        return;
      }
      auto &encoder = *enc_ptr;
      const auto initialization_deadline = std::chrono::steady_clock::now() + 5s;
      initialization_cancel_t initialization_cancelled = [&]() {
        return shutdown_event->peek() || ref->reinit_event.peek() || !images->running();
      };

      std::unique_ptr<platf::encode_device_t> encode_device;
      std::unique_ptr<encode_session_t> prepared_session;
      bool session_hdr_metadata_valid = false;
      SS_HDR_METADATA session_hdr_metadata {};
      bool initialization_was_cancelled = false;
      bool initialization_gate_contended = false;
#ifdef _WIN32
      if (&encoder == &amdvce_legacy) {
        auto legacy = make_legacy_amf_session_bounded(
          display, config, display->width, display->height, &hdr_latch,
          initialization_deadline, initialization_cancelled,
          initialization_was_cancelled, initialization_gate_contended);
        if (legacy) {
          prepared_session = std::move(legacy->session);
          session_hdr_metadata_valid = legacy->hdr_metadata_valid;
          session_hdr_metadata = legacy->hdr_metadata;
        }
      } else
#endif
      {
        encode_device = make_encode_device(*display, encoder, config, &hdr_latch);
        if (encode_device) {
          session_hdr_metadata_valid = encode_device->hdr_metadata_valid;
          session_hdr_metadata = encode_device->hdr_metadata;
        }
      }
#ifdef _WIN32
      if (initialization_was_cancelled) continue;
      if (!encode_device && !prepared_session && &encoder == &amdvce) {
        BOOST_LOG(error) << "AMF: native device creation failed; refusing silent amdvce_legacy fallback"sv;
      }
#endif
      if (initialization_was_cancelled) continue;
      if (initialization_gate_contended && !encode_device && !prepared_session) {
        std::this_thread::sleep_for(100ms);
        continue;
      }
      if (!encode_device && !prepared_session) {
        return;
      }
      auto &session_encoder = *enc_ptr;

      // absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR stream state
      hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (session_hdr_metadata_valid) {
        hdr_info = std::make_unique<hdr_info_raw_t>(true, session_hdr_metadata);
      }
      raise_hdr_info_if_changed(hdr_event, last_hdr_info, std::move(hdr_info));

      const auto encode_result = encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        std::move(prepared_session),
        ref->reinit_event,
        session_encoder,
        &hdr_latch,
        channel_data,
        initialization_deadline,
        initialization_cancelled,
        last_hdr_info,
        rtx_hdr_metadata_refresh
      );
#ifdef _WIN32
      if (encode_result == encode_run_result_e::native_amf_failed && &session_encoder == &amdvce) {
        // Runtime fatals (TDR, sustained backpressure, output stalls) are
        // classified by the encoder layer as reinit requests. Rebuild the same
        // native session like every other encoder does — never a silent
        // amdvce_legacy fallback — but stay bounded so a wedged driver cannot
        // busy-loop the stream.
        if (native_amf_lifecycle_gate.is_quarantined()) {
          BOOST_LOG(error) << "AMF: native runtime failed while quarantined; ending the stream. Host restart is required before retrying AMD encoding"sv;
          return;
        }
        ++consecutive_native_amf_runtime_failures;
        if (consecutive_native_amf_runtime_failures >= 3) {
          BOOST_LOG(error) << "AMF: native runtime failed 3 times without a stable session; ending the stream without changing encoder implementations"sv;
          return;
        }
        BOOST_LOG(warning) << "AMF: native runtime failed; rebuilding the native session (attempt "
                           << consecutive_native_amf_runtime_failures << " of 3)"sv;
        continue;
      }
#endif
      if (encode_result == encode_run_result_e::initialization_failed) {
#ifdef _WIN32
        if (&session_encoder != &amdvce && &session_encoder != &amdvce_legacy) {
          continue;
        }
        if (native_amf_lifecycle_gate.is_quarantined()) {
          BOOST_LOG(error) << "AMF: ending the stream after a watchdog timeout; host restart is required before retrying AMD encoding"sv;
          return;
        }
        ++consecutive_encoder_initialization_failures;
        if (consecutive_encoder_initialization_failures >= 3) {
          BOOST_LOG(error) << "Encoder initialization failed 3 times; ending the stream instead of busy-looping"sv;
          return;
        }
        const auto retry_delay = std::chrono::milliseconds(100 * (1 << (consecutive_encoder_initialization_failures - 1)));
        BOOST_LOG(warning) << "Encoder initialization failed; retrying in " << retry_delay.count() << "ms";
        std::this_thread::sleep_for(retry_delay);
        continue;
#else
        continue;
#endif
      }
      if (encode_result == encode_run_result_e::temporarily_busy) {
        std::this_thread::sleep_for(100ms);
        continue;
      }
      consecutive_encoder_initialization_failures = 0;
#ifdef _WIN32
      consecutive_native_amf_runtime_failures = 0;
#endif
    }
  }

  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  ) {
    // Snapshot the encoder pointer to avoid races with concurrent probe_encoders() calls
    auto *encoder = chosen_encoder;
    if (!encoder) {
      BOOST_LOG(error) << "No encoder available for capture"sv;
      return;
    }

    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data);
    } else {
      safe::signal_t join_event;
      auto ref = capture_thread_sync.ref();
      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
        &join_event,
        mail->event<bool>(mail::shutdown),
        mail::man->queue<packet_t>(mail::video_packets),
        std::move(idr_events),
        mail->event<hdr_info_t>(mail::hdr),
        mail->event<input::touch_port_t>(mail::touch_port),
        mail->event<int>(mail::dynamic_bitrate),
        config,
        1,
        channel_data,
      });

      // Wait for join signal
      join_event.view();
    }
  }

  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    const int max_attempts = config.videoFormat >= 1 ? 3 : 1;  // HEVC/AV1 can fail transiently during probing
    // The tight submission/wall-clock bounds exist for AMF drivers that stall in
    // INPUT_FULL; probing for every other encoder keeps the pre-existing limits
    // so this AMD-only change cannot alter NVENC/QSV/software negotiation.
#ifdef _WIN32
    const bool amf_probe = &encoder == &amdvce || &encoder == &amdvce_legacy;
#else
    const bool amf_probe = false;
#endif
    const auto probe_timeout = amf_probe ? std::chrono::seconds {5} : std::chrono::seconds {60};
    const int max_probe_submissions = amf_probe ? 64 : 256;
    const auto probe_start = std::chrono::steady_clock::now();
    const auto probe_deadline = probe_start + probe_timeout;
    const auto codec_name = [&]() -> std::string_view {
      switch (config.videoFormat) {
        case 0:
          return "H.264"sv;
        case 1:
          return "HEVC"sv;
        case 2:
          return "AV1"sv;
        default:
          return "codec"sv;
      }
    }();

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
      auto validate_once = [&]() -> util::optional_t<int> {
        std::unique_ptr<encode_session_t> session;
#ifdef _WIN32
        if (&encoder == &amdvce_legacy) {
          bool legacy_cancelled = false;
          bool legacy_gate_contended = false;
          auto legacy = make_legacy_amf_session_bounded(
            disp, config, disp->width, disp->height, nullptr,
            probe_deadline, []() { return false; },
            legacy_cancelled, legacy_gate_contended);
          if (legacy_cancelled || legacy_gate_contended) {
            return util::false_v<util::optional_t<int>>;
          }
          if (legacy) session = std::move(legacy->session);
        } else
#endif
        {
          auto encode_device = make_encode_device(*disp, encoder, config);
          if (encode_device) {
            session = make_encode_session(
              disp.get(), encoder, config, disp->width, disp->height,
              std::move(encode_device), probe_deadline, []() { return false; });
          }
        }
        if (!session) {
          return util::false_v<util::optional_t<int>>;
        }
        auto bounded_probe_teardown = util::fail_guard([&]() {
#ifdef _WIN32
          if (&encoder == &amdvce_legacy) {
            destroy_legacy_amf_session_bounded(session, "probe"sv);
            return;
          }
#endif
          destroy_encode_session_bounded(session, "probe"sv);
        });

        // Keep the probe image alive while native AMF primes a lookahead pipeline.
        // Every PA input is rendered into a newly reserved ring surface; repeatedly
        // submitting the first surface cannot make progress if AMF still owns it.
        auto probe_img = disp->alloc_img();
        if (!probe_img || disp->dummy_img(probe_img.get()) || session->convert(*probe_img)) {
          return util::false_v<util::optional_t<int>>;
        }

        session->request_idr_frame();

        // Use a probe-local mail/queue to avoid stale packets from previous encoder sessions.
        auto probe_mail = std::make_shared<safe::mail_raw_t>();
        auto packets = probe_mail->queue<packet_t>(mail::video_packets);

        // Bound the whole codec probe by both submissions and wall time. An AMF driver
        // stalled in INPUT_FULL can make a single encode() call take hundreds of
        // milliseconds, so the old 256-attempt limit could block startup for minutes
        // and even outlive the service shutdown watchdog. The deadline is shared by
        // HEVC/AV1 retries below, keeping the complete validation below one watchdog
        // interval while still leaving ample time for a cold hardware encoder.
        for (int probe_attempts = 0; !packets->peek(); ++probe_attempts) {
          const auto now = std::chrono::steady_clock::now();
          if (probe_attempts >= max_probe_submissions || now >= probe_deadline) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - probe_start);
            BOOST_LOG(error) << "Encoder probe produced no packet after "sv << probe_attempts
                             << " submissions in " << elapsed.count() << "ms; treating "sv
                             << codec_name << " as unsupported."sv;
            return util::false_v<util::optional_t<int>>;
          }

          // Every submitted surface needs a unique PTS. Reusing frame index 1 for
          // hundreds of probe submissions violates the normal encoder contract and
          // makes delayed/catch-up output indistinguishable from a duplicate frame.
          if (probe_attempts > 0 && dynamic_cast<amf_encode_session_t *>(session.get()) &&
              session->convert(*probe_img)) {
            BOOST_LOG(error) << "Encoder probe could not prepare the next native AMF lookahead surface"sv;
            return util::false_v<util::optional_t<int>>;
          }
          const auto probe_frame_index = static_cast<int64_t>(probe_attempts) + 1;
          if (encode(probe_frame_index, *session, packets, nullptr, {}, {}, {})) {
            return util::false_v<util::optional_t<int>>;
          }
        }

        auto packet = packets->pop();
        if (!packet->is_idr()) {
          BOOST_LOG(error) << "First packet type is not an IDR frame"sv;
          return util::false_v<util::optional_t<int>>;
        }

        int flag = 0;

        // This check only applies for H.264 and HEVC
        if (config.videoFormat <= 1) {
          if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
            if (cbs::validate_sps(packet_avcodec->av_packet, config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264)) {
              flag |= VUI_PARAMS;
            }
          } else {
            // Don't check it for non-avcodec encoders.
            flag |= VUI_PARAMS;
          }
        }

        return flag;
      };

      auto result = validate_once();
      if (result) {
        return *result;
      }

      if (attempt < max_attempts && (!amf_probe || std::chrono::steady_clock::now() < probe_deadline)) {
        BOOST_LOG(debug) << "Encoder probe: failed to validate "sv << codec_name << " config (attempt "sv
                         << attempt << "/" << max_attempts << "), retrying."sv;
        std::this_thread::sleep_for(std::chrono::milliseconds {50});
      } else {
        break;
      }
    }

    return -1;
  }

  static thread_local std::shared_ptr<platf::display_t> cached_probe_display;
  static thread_local platf::mem_type_e cached_display_type = platf::mem_type_e::system;

  bool validate_encoder(
    encoder_t &encoder,
    const bool expect_failure,
    const std::optional<platf::adapter_id_t> &required_adapter,
    std::optional<platf::adapter_id_t> *actual_adapter,
    const std::string &probe_display_name
  ) {
    if (actual_adapter) {
      actual_adapter->reset();
    }
    std::shared_ptr<platf::display_t> disp;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    auto test_hevc = active_hevc_mode >= 2 || (active_hevc_mode == 0 && !(encoder.flags & H264_ONLY));
    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();

    auto clear_capabilities = [&]() {
      encoder.h264.capabilities.reset();
      encoder.hevc.capabilities.reset();
      encoder.av1.capabilities.reset();
    };

    // First, test encoder viability
    config_t config_max_ref_frames {1920, 1080, 60, 6000, 1000, 1, 1, 1, 0, 0, 0};
    config_t config_autoselect {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 0};

    // If the encoder isn't supported at all (not even H.264), bail early
    // Try to reuse cached display if same device type
    const auto cached_adapter = cached_probe_display ? cached_probe_display->capture_adapter_id() : std::nullopt;
    const bool cached_display_matches_required =
      !required_adapter || (cached_adapter && *cached_adapter == *required_adapter);
    if (cached_probe_display &&
        cached_display_type == encoder.platform_formats->dev_type &&
        cached_display_matches_required) {
      disp = cached_probe_display;
    } else {
      reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect, required_adapter);
      cached_probe_display = disp;
      cached_display_type = encoder.platform_formats->dev_type;
    }

    if (!disp) {
      clear_capabilities();
      return false;
    }
    const auto initial_probe_adapter = disp->capture_adapter_id();
    if (required_adapter && (!initial_probe_adapter || *initial_probe_adapter != *required_adapter)) {
      clear_capabilities();
      BOOST_LOG(error)
        << "Encoder probe display did not initialize on its required adapter; refusing cross-adapter validation.";
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      clear_capabilities();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      clear_capabilities();
      BOOST_LOG(warning) << "Encoder ["sv << encoder.name
                         << "] failed H.264 validation before HEVC/AV1 capability probing completed; higher codec support is unknown"sv;
      return false;
    } else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      {VUI_PARAMS, encoder_t::VUI_PARAMETERS},
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    } else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    if (test_av1) {
      config_max_ref_frames.videoFormat = 2;
      config_autoselect.videoFormat = 2;

      if (disp->is_codec_supported(encoder.av1.name, config_autoselect)) {
        auto max_ref_frames_av1 = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // AV1 to also succeed with max ref frames specified if AV1 is supported.
        auto autoselect_av1 = (max_ref_frames_av1 >= 0 || max_ref_frames_h264 >= 0) ?
                                max_ref_frames_av1 :
                                validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.av1[encoder_flag] = (max_ref_frames_av1 & validate_flag && autoselect_av1 & validate_flag);
        }

        encoder.av1[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_av1 >= 0;
        encoder.av1[encoder_t::PASSED] = max_ref_frames_av1 >= 0 || autoselect_av1 >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.av1.name << "] is not supported on this GPU"sv;
        encoder.av1.capabilities.reset();
      }
    } else {
      // Clear all cap bits for AV1 if we didn't probe it
      encoder.av1.capabilities.reset();
    }

    // Test HDR and YUV444 support
    {
      // H.264 is special because encoders may support YUV 4:4:4 without supporting 10-bit color depth
      if (encoder.flags & YUV444_SUPPORT) {
        config_t config_h264_yuv444 {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 1};
        encoder.h264[encoder_t::YUV444] = disp->is_codec_supported(encoder.h264.name, config_h264_yuv444) &&
                                          validate_config(disp, encoder, config_h264_yuv444) >= 0;
      } else {
        encoder.h264[encoder_t::YUV444] = false;
      }

      const config_t generic_hdr_config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, 1, 1, 0};

      // Reset the display since we're switching from SDR to HDR. Keep probing on the
      // current active display without attempting a display swap.
      // Clear the cache since we need a fresh display for HDR testing
      cached_probe_display.reset();
      reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, generic_hdr_config, required_adapter);
      if (!disp) {
        return false;
      }
      const auto hdr_probe_adapter = disp->capture_adapter_id();
      if (required_adapter && (!hdr_probe_adapter || *hdr_probe_adapter != *required_adapter)) {
        BOOST_LOG(error)
          << "HDR encoder probe display did not initialize on its required adapter; refusing cross-adapter validation.";
        return false;
      }

      auto test_hdr_and_yuv444 = [&](auto &flag_map, auto video_format) {
        auto config = generic_hdr_config;
        config.videoFormat = video_format;

        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        flag_map[encoder_t::YUV444] = false;

        // Test the mandatory HDR 4:2:0 path first. Some encoders support AV1/HEVC
        // Main10 but reject optional 4:4:4, and that must not mask HDR support.
        // Keep DYNAMIC_RANGE tentatively enabled while probing because validate_config()
        // gates dynamicRange configs on the current codec capability bit.
        config.chromaSamplingType = 0;
        if (disp->is_codec_supported(encoder_codec_name, config) &&
            validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE] = false;
          return;
        }

        // Test optional HDR 4:4:4 after 4:2:0 has already established HDR support.
        config.chromaSamplingType = 1;
        if ((encoder.flags & YUV444_SUPPORT) &&
            disp->is_codec_supported(encoder_codec_name, config) &&
            validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::YUV444] = true;
        }
      };

      // HDR is not supported with H.264. Don't bother even trying it.
      encoder.h264[encoder_t::DYNAMIC_RANGE] = false;

      test_hdr_and_yuv444(encoder.hevc, 1);
      test_hdr_and_yuv444(encoder.av1, 2);
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    if (actual_adapter) {
      *actual_adapter = disp->capture_adapter_id();
    }
    fg.disable();
    return true;
  }

  int probe_encoders() {
    std::lock_guard<std::mutex> lock(encoder_probe_mutex);
    const auto probe_target = resolve_probe_target();
    const auto &required_adapter = probe_target.required_adapter;
    const auto cache_key = build_probe_cache_key(&probe_target);
    mark_probe_attempted(cache_key);
    // Attempt ownership is frozen with the target selected at probe start.
    // A topology/configuration change during validation must remain unattempted
    // under its new key so the next request can probe it immediately.
    auto final_attempt_key_guard = util::fail_guard([cache_key]() {
      try {
        mark_probe_attempted(cache_key);
      } catch (...) {
        BOOST_LOG(warning) << "Unable to record the final encoder probe adapter key.";
      }
    });
#ifdef _WIN32
    {
      const auto adapter_resolution = platf::resolve_adapter(
        config::video.adapter_name,
        config::video.adapter_pnp_id
      );
      const auto wgc_adapter_luid = platf::dxgi::get_last_wgc_adapter_luid();
      BOOST_LOG(info)
        << "Encoder probe adapter identity: configured_name='" << config::video.adapter_name
        << "', configured_pnp_id='" << config::video.adapter_pnp_id
        << "', resolution=" << platf::adapter_resolution_status_name(adapter_resolution.status)
        << ", resolved_name='" << adapter_resolution.description
        << "', resolved_pnp_id='" << adapter_resolution.pnp_id
        << "', configured_output='" << config::get_active_output_name()
        << "', probe_output='" << probe_target.display_name
        << "', wgc_luid="
        << (wgc_adapter_luid ?
              std::to_string(wgc_adapter_luid->HighPart) + ":" + std::to_string(wgc_adapter_luid->LowPart) :
              std::string("<none>"))
        << ", effective_cache_adapter='" << cache_key.adapter_identity
        << "', effective_cache_source=" << cache_key.adapter_identity_source
        << '.';
    }
#endif
    const bool hevc_mode_auto = config::video.hevc_mode == 0;
    const bool av1_mode_auto = config::video.av1_mode == 0;
    const bool wants_hdr = (config::video.hevc_mode == 3) || (config::video.av1_mode == 3);
    const bool wants_hevc = config::video.hevc_mode >= 2 || hevc_mode_auto;
    const bool wants_hevc_hdr = config::video.hevc_mode == 3 || hevc_mode_auto;
    const bool wants_av1 = config::video.av1_mode >= 2 || av1_mode_auto;
    const bool wants_av1_hdr = config::video.av1_mode == 3 || av1_mode_auto;

    if (probe_cache_matches(cache_key, wants_hdr, wants_hevc, wants_hevc_hdr, wants_av1, wants_av1_hdr)) {
      BOOST_LOG(debug) << "Encoder probe skipped (cached success).";
      return 0;
    }

#ifdef _WIN32
    if (required_adapter && probe_target.display_name.empty()) {
      BOOST_LOG(info)
        << "Encoder probe deferred because the required adapter has no compatible capture output.";
      update_probe_cache(cache_key, false, false, false, false, false, false);
      return -1;
    }
#endif

    if (!allow_encoder_probing()) {
      // Error already logged
      update_probe_cache(cache_key, false, false, false, false, false, false);
      return -1;
    }
    const auto previous_active_hevc_mode = active_hevc_mode;
    const auto previous_active_av1_mode = active_av1_mode;
    const auto previous_last_ref_frames_invalidation = last_encoder_probe_supported_ref_frames_invalidation;
    const auto previous_last_yuv444_for_codec = last_encoder_probe_supported_yuv444_for_codec;
    auto previous_encoder = chosen_encoder;

    auto restore_previous_probe_state = util::fail_guard([&]() {
      active_hevc_mode = previous_active_hevc_mode;
      active_av1_mode = previous_active_av1_mode;
      last_encoder_probe_supported_ref_frames_invalidation = previous_last_ref_frames_invalidation;
      last_encoder_probe_supported_yuv444_for_codec = previous_last_yuv444_for_codec;
    });

    auto encoder_list = encoders;
#ifdef _WIN32
    const auto amf_selection_policy = amf::lifecycle::encoder_selection_policy(config::video.encoder);
    // amdvce_legacy is rollback-only. It participates in probing solely when
    // explicitly selected; native feature or capability failures must remain visible.
    if (!amf_selection_policy.include_legacy) {
      encoder_list.erase(std::remove(encoder_list.begin(), encoder_list.end(), &amdvce_legacy), encoder_list.end());
    }
#endif

    // Use a local variable for encoder selection during probing so that
    // chosen_encoder is never null while concurrent capture threads may read it.
    encoder_t *new_encoder = nullptr;
    std::optional<platf::adapter_id_t> candidate_probe_adapter;
    std::optional<platf::adapter_id_t> successful_probe_adapter;
    const auto validate_probe_encoder = [&](encoder_t &encoder, const bool expect_failure) {
      candidate_probe_adapter.reset();
      return validate_encoder(
        encoder,
        expect_failure,
        required_adapter,
        &candidate_probe_adapter,
        probe_target.display_name
      );
    };
    active_hevc_mode = config::video.hevc_mode;
    active_av1_mode = config::video.av1_mode;
    last_encoder_probe_supported_ref_frames_invalidation = false;
    last_encoder_probe_supported_yuv444_for_codec = {};

    // Clear any cached display from previous probes to ensure fresh start
    cached_probe_display.reset();

    auto adjust_encoder_constraints = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }

      if (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_av1_mode = 0;
      } else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    if (!config::video.encoder.empty()) {
      // If there is a specific encoder specified, use it if it passes validation
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!validate_probe_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints(encoder);

          successful_probe_adapter = candidate_probe_adapter;
          new_encoder = encoder;
          break;
        }

        pos++;
      });

      if (new_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
#ifdef _WIN32
        if (amf_selection_policy.fail_closed) {
          BOOST_LOG(error) << "Native AMF was explicitly selected; refusing automatic fallback to amdvce_legacy or another encoder"sv;
          return -1;
        }
#endif
      }
    }

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (new_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!validate_probe_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) ||
            (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) ||
            (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        successful_probe_adapter = candidate_probe_adapter;
        new_encoder = encoder;
        break;
      });

      if (new_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (new_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!validate_probe_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints(encoder);

        successful_probe_adapter = candidate_probe_adapter;
        new_encoder = encoder;
        break;
      });
    }

    if (new_encoder == nullptr) {
      const auto output_name = display_device::map_output_name(config::get_active_output_name());
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty() || !output_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      update_probe_cache(cache_key, false, false, false, false, false, false);
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *new_encoder;

#ifdef _WIN32
    if (encoder.name == "software"sv) {
      // Software is probed last, so reaching it means every hardware encoder —
      // including native AMF — failed validation. Make the degradation loud:
      // an AMD user should never discover software encoding from stutter alone.
      BOOST_LOG(error) << "No hardware encoder passed validation; the SOFTWARE encoder was selected."sv;
      BOOST_LOG(error) << "If this system has an AMD GPU, hardware encoding is NOT active. Check the AMD driver and AMF runtime, or set encoder = amdvce_legacy to try the FFmpeg AMF fallback."sv;
    }
#endif

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = (encoder_t::flag_e) x;
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    if (active_hevc_mode == 0) {
      active_hevc_mode = encoder.hevc[encoder_t::PASSED] ? (encoder.hevc[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = encoder.av1[encoder_t::PASSED] ? (encoder.av1[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    const bool hevc_passed = encoder.hevc[encoder_t::PASSED];
    const bool hevc_hdr_supported = encoder.hevc[encoder_t::DYNAMIC_RANGE];
    const bool av1_passed = encoder.av1[encoder_t::PASSED];
    const bool av1_hdr_supported = encoder.av1[encoder_t::DYNAMIC_RANGE];
    const bool cache_hdr_supported = hevc_hdr_supported || av1_hdr_supported;
    auto successful_cache_key = cache_key;
#ifdef _WIN32
    const auto required_adapter_identity = required_adapter ?
                                             std::optional<std::string> {
                                               adapter_cache_identity(*required_adapter)
                                             } :
                                             std::nullopt;
    const auto observed_adapter_identity = successful_probe_adapter ?
                                             std::optional<std::string> {
                                               adapter_cache_identity(*successful_probe_adapter)
                                             } :
                                             std::nullopt;
    const auto owned_cache_key = encoder_probe_policy::own_successful_cache_key(
      encoder_probe_policy::cache_key_t {
        .encoder_configuration = cache_key.encoder_configuration,
        .adapter_identity = cache_key.adapter_identity,
        .adapter_identity_resolved = cache_key.adapter_identity_resolved,
      },
      encoder_probe_policy::probe_observation_t {
        .required_adapter = required_adapter_identity,
        .observed_adapter = observed_adapter_identity,
      }
    );
    if (!owned_cache_key) {
      // Preserve the existing retry/non-required-adapter behavior: an explicit
      // adapter mismatch fails the probe, while an Automatic probe with no
      // observed identity clears any positive cache entry without publishing a
      // cache hit. Neither path may claim ownership for the pending hint.
      if (required_adapter) {
        BOOST_LOG(error)
          << "Encoder validation did not produce an observed adapter matching its required adapter; refusing to publish or cache its selection.";
        update_probe_cache(cache_key, false, false, false, false, false, false);
        return -1;
      }
      successful_cache_key.adapter_identity = "unresolved-actual-probe-adapter";
      successful_cache_key.adapter_identity_source = "actual-probe-display-unavailable";
      successful_cache_key.adapter_identity_resolved = false;
    } else {
      successful_cache_key.adapter_identity = owned_cache_key->adapter_identity;
      successful_cache_key.adapter_identity_source = "actual-probe-display";
      successful_cache_key.adapter_identity_resolved = owned_cache_key->adapter_identity_resolved;
    }
#endif
    const advertised_encoder_capabilities_t successful_capabilities {
      .hevc_mode = active_hevc_mode,
      .av1_mode = active_av1_mode,
      .yuv444_for_codec = last_encoder_probe_supported_yuv444_for_codec,
    };
    update_probe_cache(
      successful_cache_key,
      true,
      cache_hdr_supported,
      hevc_passed,
      hevc_hdr_supported,
      av1_passed,
      av1_hdr_supported,
      successful_capabilities
    );
    // Publish the new encoder only after the probe has fully succeeded,
    // so concurrent capture threads never observe a null chosen_encoder.
    chosen_encoder = new_encoder;
    restore_previous_probe_state.disable();
    return 0;
  }

  // Linux only declaration
  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->data) {
      if (((vaapi_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = config::video.adapter_name.empty() ? nullptr : config::video.adapter_name.c_str();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef SUNSHINE_BUILD_VULKAN
  typedef int (*vulkan_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    if (encode_device && encode_device->data) {
      if (((vulkan_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }
      return hw_device_buf;
    }

    auto render_device = config::video.adapter_name.empty() ? nullptr : config::video.adapter_name.c_str();
    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, render_device, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a Vulkan device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }
#endif

  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VideoToolbox device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

void do_nothing(void *) {
}

namespace video {
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf {av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = (ID3D11Device *) encode_device->data;

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::thread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }

  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  int start_capture_sync(capture_thread_sync_ctx_t &ctx) {
    std::thread {&captureThreadSync}.detach();
    return 0;
  }

  void end_capture_sync(capture_thread_sync_ctx_t &ctx) {
  }

  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return platf::mem_type_e::dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return platf::mem_type_e::vaapi;
      case AV_HWDEVICE_TYPE_CUDA:
        return platf::mem_type_e::cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return platf::mem_type_e::system;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return platf::mem_type_e::videotoolbox;
      default:
        return platf::mem_type_e::unknown;
    }

    return platf::mem_type_e::unknown;
  }

  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
