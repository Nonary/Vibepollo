/**
 * @file src/platform/windows/ipc/ipc_session.h
 * @brief Definitions for shared IPC session for WGC capture that can be used by both RAM and VRAM implementations.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <winsock2.h>
#include <d3d11.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// local includes
#include "misc_utils.h"
#include "pipes.h"
#include "process_handler.h"
#include "src/utility.h"
#include "src/video.h"

// platform includes
#include <winrt/base.h>

namespace platf::dxgi {

  /**
   * @brief Return whether the recent WGC desktop-switch grace window is still active.
   * @return `true` while DXGI should still be preferred after a helper desktop-switch notification.
   */
  bool recent_wgc_desktop_switch_grace_active();

  /**
   * @brief Record a desktop switch detected outside the helper (e.g. a secure-desktop
   * probe from the main process) so the DXGI-fallback grace window applies.
   */
  void note_wgc_desktop_switch();

  /** Receives the final release of a helper-owned WGC texture-ring slot. */
  class wgc_texture_slot_release_sink_t {
  public:
    virtual ~wgc_texture_slot_release_sink_t() = default;
    virtual void release_wgc_texture_slot(size_t slot, LONG64 frame_id) = 0;
  };

  /**
   * Move-only ownership of a helper texture-ring slot.
   *
   * The lease is moved into the aliasing image owner returned to encoders. Its
   * destructor returns the slot only after the final image consumer releases
   * that owner.
   */
  class wgc_texture_slot_lease_t {
  public:
    wgc_texture_slot_lease_t() = default;

    wgc_texture_slot_lease_t(std::shared_ptr<wgc_texture_slot_release_sink_t> sink, size_t slot, LONG64 frame_id):
        _sink(std::move(sink)),
        _slot(slot),
        _frame_id(frame_id) {}

    ~wgc_texture_slot_lease_t() {
      release();
    }

    wgc_texture_slot_lease_t(const wgc_texture_slot_lease_t &) = delete;
    wgc_texture_slot_lease_t &operator=(const wgc_texture_slot_lease_t &) = delete;

    wgc_texture_slot_lease_t(wgc_texture_slot_lease_t &&other) noexcept:
        _sink(std::move(other._sink)),
        _slot(other._slot),
        _frame_id(other._frame_id) {
      other._slot = WGC_IPC_TEXTURE_SLOT_COUNT;
      other._frame_id = 0;
    }

    wgc_texture_slot_lease_t &operator=(wgc_texture_slot_lease_t &&other) noexcept {
      if (this != &other) {
        release();
        _sink = std::move(other._sink);
        _slot = other._slot;
        _frame_id = other._frame_id;
        other._slot = WGC_IPC_TEXTURE_SLOT_COUNT;
        other._frame_id = 0;
      }
      return *this;
    }

  private:
    void release() noexcept {
      if (_sink) {
        auto sink = std::move(_sink);
        sink->release_wgc_texture_slot(_slot, _frame_id);
      }
      _slot = WGC_IPC_TEXTURE_SLOT_COUNT;
      _frame_id = 0;
    }

    std::shared_ptr<wgc_texture_slot_release_sink_t> _sink;
    size_t _slot = WGC_IPC_TEXTURE_SLOT_COUNT;
    LONG64 _frame_id = 0;
  };

  struct shared_frame_t {
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<IDXGIKeyedMutex> keyed_mutex;
    std::shared_ptr<winrt::handle> encoder_texture_handle;
    wgc_texture_slot_lease_t lease;
    size_t texture_slot = WGC_IPC_TEXTURE_SLOT_COUNT;
    uint64_t frame_id = 0;
    uint64_t frame_qpc = 0;
  };

  /**
   * @brief Shared WGC IPC session encapsulating helper process, control pipe, shared texture and sync primitives.
   * Manages lifecycle & communication with the helper process, duplication of shared textures, keyed mutex
   * coordination and event-driven frame availability signaling for both RAM & VRAM capture paths.
   */
  class ipc_session_t: public std::enable_shared_from_this<ipc_session_t>, private wgc_texture_slot_release_sink_t {
  public:
    /**
     * @brief Destructor. Stops the helper process and tears down IPC.
     */
    ~ipc_session_t();

    /**
     * @brief Initialize the IPC session.
     * @param config Video configuration.
     * @param display_name Display name for the session.
     * @param device D3D11 device for shared texture operations (not owned).
     * @param advanced_color_capture True when the target display is already HDR/Advanced Color.
     * @return `0` on success; non-zero otherwise.
     */
    int init(const ::video::config_t &config, std::string_view display_name, ID3D11Device *device, bool advanced_color_capture);

    /**
     * @brief Start the helper process and set up IPC connection if not already initialized.
     * Performs a no-op if already initialized.
     */
    void initialize_if_needed();

    /**
     * @brief Acquire the next frame, blocking until available or timeout.
     * @param timeout Maximum time to wait for a frame.
     * @param gpu_tex_out Output ComPtr for the GPU texture (set on success).
     * @param frame_qpc_out Output for the frame QPC timestamp (`0` if unavailable).
     * @return Capture result enum indicating success, timeout, or failure.
     */
    capture_e acquire(std::chrono::milliseconds timeout, winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out);

    /**
     * @brief Wait for a new frame event without taking the shared keyed mutex.
     * @param timeout Maximum time to wait for a frame.
     * @return Capture result enum indicating success, timeout, or failure.
     */
    capture_e wait_for_frame(std::chrono::milliseconds timeout);

    /**
     * @brief Lock the latest shared frame after wait_for_frame() succeeds.
     * @param gpu_tex_out Output ComPtr for the GPU texture (set on success).
     * @param frame_qpc_out Output for the frame QPC timestamp (`0` if unavailable).
     * @return Capture result enum indicating success, timeout, or failure.
     */
    capture_e lock_frame(winrt::com_ptr<ID3D11Texture2D> &gpu_tex_out, uint64_t &frame_qpc_out);

    /**
     * @brief Claim the latest helper-owned texture-ring slot for direct encoder use.
     * The returned lease keeps the slot immutable until all consumers release it.
     */
    capture_e claim_frame(shared_frame_t &frame_out);

    /**
     * @brief Release the keyed mutex.
     */
    void release();

    /**
     * @brief Check if the session should swap to DXGI due to secure desktop.
     * @return `true` if a swap to DXGI is needed, `false` otherwise.
     */
    bool should_swap_to_dxgi() const {
      return _should_swap_to_dxgi;
    }

    /**
     * @brief Check if the session should be reinitialized due to helper process issues.
     * @return `true` if reinitialization is needed, `false` otherwise.
     */
    bool should_reinit() const {
      return _force_reinit.load();
    }

    /**
     * @brief Get the width of the shared texture.
     * @return Width in pixels.
     */
    UINT width() const {
      return _width;
    }

    /**
     * @brief Get the height of the shared texture.
     * @return Height in pixels.
     */
    UINT height() const {
      return _height;
    }

    /**
     * @brief Check if the IPC session is initialized.
     * @return `true` if initialized, `false` otherwise.
     */
    bool is_initialized() const {
      return _initialized;
    }

    /**
     * @brief Read the static descriptor of the shared texture without acquiring the keyed mutex.
     * The shared texture is created once at session setup and its descriptor never changes for
     * the lifetime of the session, so it is safe to read at any time.
     * @param[out] desc_out Populated on success.
     * @return `true` if the shared texture is available; `false` otherwise.
     */
    bool peek_shared_texture_desc(D3D11_TEXTURE2D_DESC &desc_out) const {
      if (!_shared_textures[0]) {
        return false;
      }
      _shared_textures[0]->GetDesc(&desc_out);
      return true;
    }

  private:
    /**
     * @brief Set up shared texture and frame signaling handles by duplicating them from the helper.
     * @param handle_data Shared handles and texture metadata from the helper process.
     * @return `true` if setup was successful, `false` otherwise.
     */
    bool setup_shared_resources_from_shared_handles(const shared_handle_data_t &handle_data);

    /**
     * @brief Return a leased slot to the helper after its last encoder consumer exits.
     */
    void release_frame_slot(size_t slot, LONG64 frame_id);

    void release_wgc_texture_slot(size_t slot, LONG64 frame_id) override {
      release_frame_slot(slot, frame_id);
    }

    /**
     * @brief Handle a desktop-switch notification from the helper process.
     * @param msg The message data received from the helper process.
     */
    void handle_desktop_switch_message(std::span<const uint8_t> msg);

    /**
     * @brief Retrieve the adapter LUID for the current D3D11 device.
     * @param[out] luid_out Set to the adapter's LUID on success.
     * @return `true` if the adapter LUID was retrieved; `false` otherwise.
     */
    bool try_get_adapter_luid(LUID &luid_out);

    /**
     * @brief Stop the helper process (best effort) and note teardown time.
     */
    void stop_helper_process();

    // --- members ---
    std::unique_ptr<ProcessHandler> _process_helper;  ///< Helper process owner.
    std::unique_ptr<AsyncNamedPipe> _pipe;  ///< Async control/message pipe.
    std::array<winrt::com_ptr<IDXGIKeyedMutex>, WGC_IPC_TEXTURE_SLOT_COUNT> _keyed_mutexes;  ///< Keyed mutexes for the shared texture ring.
    std::array<winrt::com_ptr<ID3D11Texture2D>, WGC_IPC_TEXTURE_SLOT_COUNT> _shared_textures;  ///< Shared textures duplicated from helper.
    std::array<std::shared_ptr<winrt::handle>, WGC_IPC_TEXTURE_SLOT_COUNT> _shared_texture_handles;  ///< Stable local handle owners for encoder devices.
    winrt::com_ptr<ID3D11Device> _device;  ///< D3D11 device pointer (not owned).
    winrt::handle _frame_ready_event;  ///< Duplicated auto-reset event signaled by the helper per frame.
    winrt::handle _frame_metadata_mapping;  ///< Duplicated shared-memory mapping for frame metadata.
    frame_metadata_t *_frame_metadata = nullptr;  ///< Mapped frame metadata view.
    LONG64 _last_frame_id {0};  ///< Last frame id consumed from shared metadata.
    size_t _locked_texture_slot = WGC_IPC_TEXTURE_SLOT_COUNT;  ///< Ring slot currently locked by lock_frame().
    LONG64 _locked_frame_id {0};  ///< Generation of the ring slot currently locked by lock_frame().
    uint64_t _frame_qpc {0};  ///< QPC timestamp of latest frame.
    std::atomic<bool> _initializing {false};  ///< True while an initialization attempt is in progress.
    std::atomic<bool> _initialized {false};  ///< True once the most recent initialization attempt succeeded.
    std::atomic<bool> _should_swap_to_dxgi {false};  ///< True if capture should fallback.
    std::atomic<bool> _force_reinit {false};  ///< True if reinit required due to errors.
    std::atomic<uint64_t> _frames_acquired {0};  ///< Count of consumed IPC frames for sampled diagnostics.
    std::atomic<uint64_t> _slow_event_waits {0};  ///< Count of sampled/slow frame-ready waits.
    std::atomic<uint64_t> _slow_mutex_waits {0};  ///< Count of slow keyed mutex waits.
    UINT _width = 0;  ///< Shared texture width.
    UINT _height = 0;  ///< Shared texture height.
    ::video::config_t _config;  ///< Cached video config.
    std::string _display_name;  ///< Display name copy.
    bool _advanced_color_capture = false;  ///< True when target display is already Advanced Color/HDR.
    std::chrono::steady_clock::time_point _last_helper_stop {};  ///< Last time we tore down the helper.
  };

}  // namespace platf::dxgi
