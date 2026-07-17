/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Windows Game Capture (WGC) IPC display implementation with shared session helper and DXGI fallback.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <winsock2.h>
#include <dxgi1_2.h>
#include <optional>
#include <wrl/client.h>

// local includes
#include "ipc/ipc_session.h"
#include "ipc/misc_utils.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/display_vram.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

// platform includes
#include <winrt/base.h>

namespace platf::dxgi {
  namespace {
    struct wgc_dxgi_fallback_state_t {
      bool secure_desktop_active;
      bool recent_desktop_switch;
    };

    class adapter_luid_override_guard {
    public:
      explicit adapter_luid_override_guard(const std::optional<LUID> &luid) {
        previous_ = get_dxgi_adapter_luid_override();
        if (luid.has_value()) {
          set_dxgi_adapter_luid_override(luid);
        }
      }

      ~adapter_luid_override_guard() {
        set_dxgi_adapter_luid_override(previous_);
      }

    private:
      std::optional<LUID> previous_;
    };

    std::optional<wgc_dxgi_fallback_state_t> get_wgc_dxgi_fallback_state() {
      wgc_dxgi_fallback_state_t state {
        .secure_desktop_active = platf::dxgi::is_secure_desktop_active(),
        .recent_desktop_switch = recent_wgc_desktop_switch_grace_active()
      };

      if (!state.secure_desktop_active && !state.recent_desktop_switch) {
        return std::nullopt;
      }

      return state;
    }

    void log_wgc_dxgi_fallback_reason(const char *path_name, const wgc_dxgi_fallback_state_t &state) {
      if (state.secure_desktop_active && state.recent_desktop_switch) {
        BOOST_LOG(debug) << "Secure desktop detected and the desktop-switch grace window is active; "
                         << "using DXGI fallback for WGC capture (" << path_name << ")";
      } else if (state.secure_desktop_active) {
        BOOST_LOG(debug) << "Secure desktop detected, using DXGI fallback for WGC capture (" << path_name << ")";
      } else {
        BOOST_LOG(debug) << "Recent desktop switch grace window active, using DXGI fallback for WGC capture ("
                         << path_name << ")";
      }
    }

    bool is_wgc_constant_mode() {
      return config::video.capture == "wgcc";
    }

    /**
     * Owns both a pooled capture image and its move-only helper ring-slot
     * lease. The image returned to encoders aliases this owner, so the slot is
     * released only after the final consumer drops the image.
     */
    struct wgc_vram_frame_owner_t {
      wgc_vram_frame_owner_t(std::shared_ptr<platf::img_t> pooled_image, wgc_texture_slot_lease_t slot_lease):
          image(std::move(pooled_image)),
          lease(std::move(slot_lease)) {}

      std::shared_ptr<platf::img_t> image;
      wgc_texture_slot_lease_t lease;
    };

    std::chrono::milliseconds effective_wgc_timeout(std::chrono::milliseconds timeout, int client_framerate) {
      if (timeout.count() != 0) {
        return timeout;
      }

      // WGC's instantaneous publish cadence is vsync-stepped on the source
      // monitor (frame_qpc_delta tracks 240/N for an N integer), so the
      // window between our sleep_target wake and the next helper publish
      // can be up to one full helper interarrival. When DWM is composing
      // at 240/2 = 120 fps the gap is ~8.3 ms, but when it bounces between
      // 240 and 120 the average interarrival drops to ~6 ms and a 4 ms
      // grace times out frequently -- captured rate falls visibly below
      // the helper publish rate even though the helper is producing more
      // than enough frames. 6 ms covers the high-rate regime while still
      // staying under the 8.33 ms 120 Hz cadence with ~2 ms of slack for
      // the snapshot's CPU work.
      auto grace = std::chrono::milliseconds(6);

      if (is_wgc_constant_mode() && client_framerate > 0) {
        // In constant mode a timed-out zero-timeout snapshot forwards the
        // cached frame, so the pacing group must survive the full grace: the
        // snapshot returns at slot + grace, and if that lands past the next
        // slot the capture loop invalidates the pacing group and re-anchors
        // through a 200 ms blocking snapshot. Above ~166 fps the client frame
        // interval is shorter than the 6 ms grace, so an idle screen busts
        // the group on every forwarded frame and capture collapses to the
        // compositor's publish rate (vibepollo#267). Clamp the grace to leave
        // ~2 ms of the slot for wait/scheduling overhead.
        const auto frame_interval = std::chrono::nanoseconds(std::chrono::seconds(1)) / client_framerate;
        const auto clamped = std::chrono::duration_cast<std::chrono::milliseconds>(frame_interval - std::chrono::milliseconds(2));
        grace = std::clamp(clamped, std::chrono::milliseconds(1), grace);
      }

      return grace;
    }

    // The helper detects UAC/lock transitions via a desktop-switch hook, but it runs
    // with the user's token and its notification can be missed entirely (hook not yet
    // installed, pipe down, event lost). Without it the stream freezes on the last
    // pre-UAC frame forever. As a safety net, once frame delivery stalls, probe the
    // input desktop from this process; when the secure desktop is active, force a
    // reinit so the factory swaps to the DXGI fallback.
    bool wgc_stall_requires_dxgi_fallback(std::chrono::steady_clock::time_point &stall_start, std::chrono::steady_clock::time_point &last_probe) {
      constexpr auto stall_before_first_probe = std::chrono::milliseconds(200);
      constexpr auto probe_interval = std::chrono::milliseconds(250);

      const auto now = std::chrono::steady_clock::now();
      if (stall_start.time_since_epoch().count() == 0) {
        stall_start = now;
        last_probe = {};
      }

      if (now - stall_start < stall_before_first_probe) {
        return false;
      }

      if (last_probe.time_since_epoch().count() != 0 && now - last_probe < probe_interval) {
        return false;
      }

      last_probe = now;
      return is_secure_desktop_active();
    }

    capture_e forward_cached_wgc_frame(std::shared_ptr<platf::img_t> cached_frame, std::shared_ptr<platf::img_t> &img_out) {
      if (!cached_frame) {
        return capture_e::timeout;
      }

      const auto now = std::chrono::steady_clock::now();
      cached_frame->frame_timestamp = now;
      cached_frame->host_processing_timestamp = now;
      cached_frame->capture_pacing_timestamp = now;
      img_out = std::move(cached_frame);
      return capture_e::ok;
    }
  }  // namespace

  display_wgc_ipc_vram_t::display_wgc_ipc_vram_t() = default;

  display_wgc_ipc_vram_t::~display_wgc_ipc_vram_t() = default;

  int display_wgc_ipc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    _config = config;
    _display_name = display_name;

    if (display_base_t::init(config, display_name, true /* skip_dd_test: WGC doesn't use Desktop Duplication */)) {
      return -1;
    }

    capture_format = DXGI_FORMAT_UNKNOWN;  // Start with unknown format (prevents race condition/crash on first frame)

    const bool advanced_color_capture = is_hdr();

    // Create session
    _ipc_session = std::make_shared<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get(), advanced_color_capture)) {
      return -1;
    }

    for (auto &image_id : _slot_image_ids) {
      image_id = next_image_id++;
    }

    return 0;
  }

  capture_e display_wgc_ipc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    // We return capture::reinit for most scenarios because the logic in picking which mode to capture is all handled in the factory function.
    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    // Generally this only becomes true if the helper process has crashed or is otherwise not responding.
    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    timeout = effective_wgc_timeout(timeout, _config.framerate);

    auto capture_status = _ipc_session->wait_for_frame(timeout);
    if (capture_status != capture_e::ok) {
      if (capture_status == capture_e::timeout) {
        if (wgc_stall_requires_dxgi_fallback(_wgc_stall_start, _last_secure_desktop_probe)) {
          BOOST_LOG(info) << "WGC frames stalled while the secure desktop is active; falling back to DXGI capture";
          note_wgc_desktop_switch();
          return capture_e::reinit;
        }
        if (is_wgc_constant_mode()) {
          return forward_cached_wgc_frame(_last_cached_frame, img_out);
        }
      }
      return capture_status;
    }
    _wgc_stall_start = {};

    // Peek the shared texture's descriptor without taking the keyed mutex.
    // The descriptor is fixed at session setup; reading it here lets us know
    // the capture format on the very first frame so complete_img() can run
    // before we ever touch the shared IPC mutex.
    D3D11_TEXTURE2D_DESC desc;
    if (!_ipc_session->peek_shared_texture_desc(desc)) {
      return capture_e::reinit;
    }

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    // Display enumeration can race with mode changes and produce mismatched
    // image pool and desktop texture sizes. Detect that early.
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width_before_rotation << 'x' << height_before_rotation << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    // The capture format can change on the fly; if so, reinit to refresh the
    // image pool and detection state.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Use the shared capture-image pool so frame handoff has no per-frame
    // image allocation. The alias owner below keeps this pooled image checked
    // out until all encoder consumers have released the ring slot.
    std::shared_ptr<platf::img_t> pooled_image;
    if (!pull_free_image_cb(pooled_image)) {
      return capture_e::interrupted;
    }

    // Claim the helper-owned ring slot directly. The lease keeps the texture
    // immutable until every encoder and cached-frame reference has finished.
    shared_frame_t frame;
    capture_status = _ipc_session->claim_frame(frame);
    if (capture_status != capture_e::ok) {
      pooled_image.reset();
      return capture_status;
    }

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    const auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame.frame_qpc);

    // This is the only new owner allocation per frame. The returned shared_ptr
    // aliases it rather than allocating another image/control block.
    auto owner = std::make_shared<wgc_vram_frame_owner_t>(std::move(pooled_image), std::move(frame.lease));
    auto *img = static_cast<img_d3d_t *>(owner->image.get());
    img->capture_texture.reset();
    img->capture_rt.reset();
    img->capture_mutex.reset();
    img->encoder_texture_handle.reset();
    img->data = nullptr;
    img->width = width_before_rotation;
    img->height = height_before_rotation;
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->format = capture_format;
    img->id = _slot_image_ids[frame.texture_slot];
    img->dummy = false;
    img->blank = false;
    frame.texture.copy_to(&img->capture_texture);
    frame.keyed_mutex.copy_to(&img->capture_mutex);
    img->encoder_texture_handle = std::move(frame.encoder_texture_handle);
    img->data = reinterpret_cast<std::uint8_t *>(img->capture_texture.get());

    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    // Keep WGC's QPC-derived timestamp for RTP/client accounting, but do not
    // use compositor timestamp jitter as the capture-loop sleep anchor.
    img->capture_pacing_timestamp = host_processing_timestamp;
    img_out = std::shared_ptr<platf::img_t> {owner, owner->image.get()};
    if (is_wgc_constant_mode()) {
      _last_cached_frame = img_out;
    } else {
      _last_cached_frame.reset();
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_vram_t::release_snapshot() {
    return capture_e::ok;
  }

  int display_wgc_ipc_vram_t::dummy_img(platf::img_t *img_base) {
    // Use the base class implementation which creates a blank GPU texture directly,
    // avoiding Desktop Duplication which may fail on headless/disconnected sessions.
    return complete_img(img_base, true);
  }

  std::shared_ptr<display_t> display_wgc_ipc_vram_t::create(const ::video::config_t &config, const std::string &display_name) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("VRAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_vram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    } else {
      // Secure desktop not active, use WGC IPC
      BOOST_LOG(debug) << "Using WGC IPC implementation (VRAM)";
      auto disp = std::make_shared<display_wgc_ipc_vram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    }

    return nullptr;
  }

  display_wgc_ipc_ram_t::display_wgc_ipc_ram_t() = default;

  display_wgc_ipc_ram_t::~display_wgc_ipc_ram_t() = default;

  int display_wgc_ipc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    // Save config for later use
    _config = config;
    _display_name = display_name;

    // Initialize the base display class
    if (display_base_t::init(config, display_name, true /* skip_dd_test: WGC doesn't use Desktop Duplication */)) {
      return -1;
    }

    // Initialize capture format to unknown - will be determined from first frame
    capture_format = DXGI_FORMAT_UNKNOWN;

    // Note: WGC captures at monitor native resolution, not the requested config resolution.
    // The display helper handles resolution changes before capture starts if needed.
    // We use the dimensions set by display_base_t::init() which reflect the actual monitor size.

    const bool advanced_color_capture = is_hdr();

    // Create session
    _ipc_session = std::make_shared<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get(), advanced_color_capture)) {
      return -1;
    }

    return 0;
  }

  capture_e display_wgc_ipc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    // If the helper process crashed or was terminated forcefully by the user, we will re-initialize it.
    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    winrt::com_ptr<ID3D11Texture2D> gpu_tex;
    uint64_t frame_qpc = 0;
    timeout = effective_wgc_timeout(timeout, _config.framerate);
    auto status = _ipc_session->acquire(timeout, gpu_tex, frame_qpc);

    if (status != capture_e::ok) {
      if (status == capture_e::timeout) {
        if (wgc_stall_requires_dxgi_fallback(_wgc_stall_start, _last_secure_desktop_probe)) {
          BOOST_LOG(info) << "WGC frames stalled while the secure desktop is active; falling back to DXGI capture";
          note_wgc_desktop_switch();
          return capture_e::reinit;
        }
        if (is_wgc_constant_mode()) {
          return forward_cached_wgc_frame(_last_cached_frame, img_out);
        }
      }
      // For the default mode just return the capture status on timeouts.
      return status;
    }
    _wgc_stall_start = {};

    // Get description of the captured texture
    D3D11_TEXTURE2D_DESC desc;
    gpu_tex->GetDesc(&desc);

    // If we don't know the capture format yet, grab it from this texture
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    // Check for size changes - use width_before_rotation/height_before_rotation since WGC
    // captures textures in unrotated physical pixel dimensions, same as VRAM path
    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed [" << width_before_rotation << 'x' << height_before_rotation << " -> " << desc.Width << 'x' << desc.Height << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    // Check for format changes
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed [" << dxgi_format_to_string(capture_format) << " -> " << dxgi_format_to_string(desc.Format) << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    // Create or recreate staging texture if needed
    // Use unrotated dimensions to match the captured texture size
    if (!texture ||
        width_before_rotation != _last_width ||
        height_before_rotation != _last_height ||
        capture_format != _last_format) {
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.Format = capture_format;
      t.ArraySize = 1;
      t.MipLevels = 1;
      t.SampleDesc = {1, 0};
      t.Usage = D3D11_USAGE_STAGING;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto hr = device->CreateTexture2D(&t, nullptr, &texture);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to create staging texture: " << hr;
        _ipc_session->release();
        return capture_e::error;
      }

      _last_width = width_before_rotation;
      _last_height = height_before_rotation;
      _last_format = capture_format;

      BOOST_LOG(info) << "[display_wgc_ipc_ram_t] Created staging texture: "
                      << width_before_rotation << "x" << height_before_rotation << ", format: " << capture_format;
    }

    // Copy from shared texture to staging texture (queues GPU work)
    device_ctx->CopyResource(texture.get(), gpu_tex.get());

    // CRITICAL: Release the keyed mutex BEFORE blocking on Map()
    // The helper needs the mutex to write the next frame while we're reading this one
    _ipc_session->release();

    // Get a free image from the pool
    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }

    auto img = img_out.get();

    // If we don't know the final capture format yet, encode a dummy image
    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      if (dummy_img(img)) {
        return capture_e::error;
      }
    } else {
      // Map the staging texture for CPU access (blocks until GPU copy completes)
      auto hr = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to map staging texture: " << hr;
        return capture_e::error;
      }

      // Now that we know the capture format, we can finish creating the image
      if (complete_img(img, false)) {
        device_ctx->Unmap(texture.get(), 0);
        img_info.pData = nullptr;
        return capture_e::error;
      }

      // Copy data - use height_before_rotation since WGC captures unrotated texture
      std::copy_n((std::uint8_t *) img_info.pData, height_before_rotation * img_info.RowPitch, img->data);

      // Unmap the staging texture to allow GPU access again
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
    }

    // Set frame timestamp
    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame_qpc);
    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    img->capture_pacing_timestamp = host_processing_timestamp;
    if (is_wgc_constant_mode()) {
      _last_cached_frame = img_out;
    } else {
      _last_cached_frame.reset();
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_ram_t::release_snapshot() {
    // Not used in RAM path since we handle everything in snapshot()
    return capture_e::ok;
  }

  int display_wgc_ipc_ram_t::dummy_img(platf::img_t *img_base) {
    // Use the base class implementation directly,
    // avoiding Desktop Duplication which may fail on headless/disconnected sessions.
    return display_ram_t::dummy_img(img_base);
  }

  std::shared_ptr<display_t> display_wgc_ipc_ram_t::create(const ::video::config_t &config, const std::string &display_name) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("RAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_ram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    } else {
      // Secure desktop not active, use WGC IPC
      BOOST_LOG(debug) << "Using WGC IPC implementation (RAM)";
      auto disp = std::make_shared<display_wgc_ipc_ram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    }

    return nullptr;
  }

  capture_e temp_dxgi_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check periodically if secure desktop is still active
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    // Call parent DXGI duplication implementation
    return display_ddup_vram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

  capture_e temp_dxgi_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Check periodically if secure desktop is still active
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    // Call parent DXGI duplication implementation
    return display_ddup_ram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

}  // namespace platf::dxgi
