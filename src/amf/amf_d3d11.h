/**
 * @file src/amf/amf_d3d11.h
 * @brief Declarations for AMF D3D11 encoder.
 */
#pragma once

#include "amf_encoder.h"
#include "amf_lifecycle.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <d3d11.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <wrl/client.h>

#include <AMF/components/Component.h>
#include <AMF/core/Context.h>
#include <AMF/core/Data.h>
#include <AMF/core/Factory.h>

namespace amf {

  /**
   * @brief AMF encoder using D3D11 for texture input.
   */
  class amf_d3d11: public amf_encoder {
  public:
    explicit amf_d3d11(ID3D11Device *d3d_device);
    ~amf_d3d11();

    bool
    create_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format) override;

    void
    destroy_encoder() override;

    amf_encode_result
    encode_frame(uint64_t frame_index, bool force_idr) override;

    amf_encode_result
    drain_output(std::chrono::milliseconds timeout) override;

    bool
    has_output_due() override;

    bool
    has_completed_output() override;

    bool
    has_retained_preanalysis_tail() override;

    bool
    preanalysis_queue_fallback_recommended() override;

    bool
    driver_submission_timed_out() override;

    bool
    begin_drain() override;

    bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) override;

    bitrate_update_result_e
    set_bitrate(int bitrate_kbps) override;

    void
    set_caller_deadline(std::optional<std::chrono::steady_clock::time_point> deadline) override;

    bool
    set_hdr_metadata(const std::optional<amf_hdr_metadata> &metadata) override;

    void *
    get_input_texture() override;

    ID3D11Texture2D *
    acquire_input_texture_for_render();

    bool
    commit_input_texture_for_render();

    void
    cancel_input_texture_for_render();

  private:
    bool
    init_amf_library();

    bool
    configure_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace);

    AMF_SURFACE_FORMAT
    get_amf_format(platf::pix_fmt_e buffer_format, int bit_depth);

    const wchar_t *
    get_codec_id();

    amf_encoded_frame
    extract_encoded_frame(const ::amf::AMFDataPtr &output_data);

    void
    process_encoded_frame_locked(amf_encoded_frame encoded_frame);

    void
    output_pump(std::stop_token stop_token) noexcept;

    void
    submission_pump(std::stop_token stop_token) noexcept;

    std::optional<AMF_RESULT>
    submit_input_until(const ::amf::AMFSurfacePtr &surface,
      std::chrono::steady_clock::time_point deadline);

    std::optional<AMF_RESULT>
    drain_until(std::chrono::steady_clock::time_point scheduling_deadline);

    std::optional<AMF_RESULT>
    driver_call_until(const ::amf::AMFSurfacePtr &surface,
      bool drain,
      std::function<AMF_RESULT()> operation,
      std::chrono::steady_clock::time_point scheduling_deadline);

    std::optional<AMF_RESULT>
    property_call_until(std::function<AMF_RESULT()> operation,
      std::chrono::steady_clock::time_point scheduling_deadline);

    void
    on_input_surface_released(std::size_t slot_index) noexcept;

    bool
    ensure_input_surface_count(std::size_t count);

    void
    promote_released_replay_source(std::size_t slot_index, uint64_t frame_index) noexcept;

    ID3D11Device *device = nullptr;
    ::amf::AMFFactory *factory = nullptr;
    ::amf::AMFContextPtr context;
    ::amf::AMFComponentPtr encoder;
    HMODULE amf_dll = nullptr;
    amf_uint64 runtime_version = 0;

    // The converter renders directly into a reserved pool surface. AMF may retain an
    // accepted native surface while encoding it, so a slot is recycled exclusively by
    // AMFSurfaceObserver. The pool starts at the queue/lookahead-aware working depth
    // and allocates additional slots lazily during a transient driver backlog.
    static constexpr std::size_t INPUT_SURFACE_RING_SIZE = lifecycle::maximum_input_surface_count;
    using input_surface_state_e = lifecycle::input_surface_state_e;

    struct input_surface_release_observer_t final: ::amf::AMFSurfaceObserver {
      amf_d3d11 *owner = nullptr;
      std::size_t slot_index = 0;

      void AMF_STD_CALL OnSurfaceDataRelease(::amf::AMFSurface *surface) override;
    };
    struct input_surface_slot_t: lifecycle::input_surface_state_t {
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    };
    std::array<input_surface_slot_t, INPUT_SURFACE_RING_SIZE> input_surface_ring;
    std::array<input_surface_release_observer_t, INPUT_SURFACE_RING_SIZE> input_surface_release_observers;
    std::size_t active_input_surface_count = lifecycle::minimum_input_surface_count;
    std::size_t encoder_input_queue_size = lifecycle::default_amf_input_queue_size;
    D3D11_TEXTURE2D_DESC input_surface_desc {};
    std::size_t next_input_surface_slot = 0;
    std::optional<std::size_t> prepared_input_surface_slot;
    // Static refreshes, bootstrap frames, and IDR repeats copy from this dedicated
    // texture. It is never wrapped in an AMFSurface, so output completion and
    // AMFSurfaceObserver release ordering cannot invalidate the repeat source.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> replay_texture;
    bool replay_texture_valid = false;
    std::optional<std::size_t> replay_source_surface_slot;
    uint64_t replay_source_frame_index = 0;
    uint64_t replay_snapshot_copy_count = 0;

    // Encoder state
    video::config_t current_config {};
    int video_format = 0;  // 0=H264, 1=HEVC, 2=AV1
    AMF_SURFACE_FORMAT surface_format = AMF_SURFACE_NV12;
    int encode_width = 0;
    int encode_height = 0;
    bool rfi_pending = false;
    uint64_t last_rfi_ltr_index = 0;
    int max_ltr_frames = 0;
    bool rfi_enabled = false;

    // Current LTR state for RFI.
    // Slot 0 is reserved as the IDR baseline (set on every IDR, never overwritten by
    // periodic marks) so RFI always has a known-good fallback even when every recent
    // periodic-marked frame was inside a packet-loss window. Slots 1..N-1 form a
    // sliding window of more recent anchors.
    static constexpr int MAX_LTR_SLOTS = 4;
    static constexpr uint64_t LTR_MARK_INTERVAL = 4;  // Mark LTR every N frames
    int effective_ltr_slots = 0;    // Clamped to min(max_ltr_frames, MAX_LTR_SLOTS)
    int current_ltr_slot = 0;      // Which LTR slot to mark next
    std::array<bool, MAX_LTR_SLOTS> ltr_slots_valid {};
    std::array<uint64_t, MAX_LTR_SLOTS> ltr_slot_frame_index {};  // Frame index when each LTR slot was marked

    // QueryOutput is owned by a dedicated pump. This keeps AMF driver completion work
    // off Sunshine's latency-critical encode thread and gives input-slot ownership one
    // synchronization boundary.
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::jthread output_thread;
    std::deque<amf_encoded_frame> completed_outputs;
    std::deque<amf_encoded_frame> pending_unconfirmed_outputs;
    std::unordered_map<uint64_t, bool> frame_rfi_flags;
    uint64_t last_completed_frame_index = 0;
    lifecycle::output_pts_tracker_t output_pts_tracker;
    uint64_t last_submitted_frame_index = 0;
    bool output_fatal = false;
    bool drain_requested = false;
    bool drain_complete = false;
    bool output_poll_requested = false;
    std::size_t active_output_poll_waiters = 0;

    // SubmitInput is a vendor call and can block inside a damaged/ incompatible
    // runtime. One owning worker serializes it; the encode thread waits only to
    // its frame deadline. A timeout fences the complete AMF session at the video
    // layer, which intentionally retains this object and its shared resources.
    std::mutex submission_mutex;
    std::condition_variable submission_cv;
    std::jthread submission_thread;
    ::amf::AMFSurfacePtr submission_surface;
    AMF_RESULT submission_result = AMF_FAIL;
    bool submission_requested = false;
    bool submission_in_progress = false;
    bool submission_response_ready = false;
    bool submission_request_is_drain = false;
    std::function<AMF_RESULT()> submission_operation;
    bool submission_timed_out = false;
    std::chrono::steady_clock::time_point submission_driver_deadline {};
    std::optional<std::chrono::steady_clock::time_point> caller_deadline;

    // Input ownership and output production are deliberately tracked separately.
    // AMF explicitly does not guarantee a one-to-one input/output relationship;
    // AMFSurfaceObserver owns the first count while QueryOutput owns the totals.
    std::size_t input_surfaces_in_flight = 0;
    uint64_t accepted_input_count = 0;
    uint64_t completed_output_count = 0;
    std::deque<uint64_t> accepted_frame_indices;
    bool preanalysis_enabled = false;
    int preanalysis_lookahead_depth = 0;
    bool query_timeout_supported = false;
    bool user_configured_rate_control = false;
    bool enforce_hrd_enabled = false;
    bool constant_qp_mode = false;
    lifecycle::rate_control_policy_t rate_control_policy {};
    std::optional<int64_t> applied_target_bitrate;
    std::optional<int64_t> applied_peak_bitrate;
    std::optional<int64_t> applied_vbv_buffer_size;

    // Statistics feedback state
    bool statistics_enabled = false;
    bool psnr_enabled = false;
    bool ssim_enabled = false;

    // Runtime fault watchdog: count consecutive failures so we can signal
    // a fatal error to the upper layer (triggering a real reinit) instead
    // of silently producing no output forever. Threshold is derived from
    // client framerate in create_encoder() so the watchdog fires after
    // roughly the same wall-clock time regardless of fps.
    int consecutive_submit_failures = 0;
    int consecutive_surface_failures = 0;
    int consecutive_query_failures = 0;
    int consecutive_output_failures = 0;
    // Consecutive submissions whose bounded coalescing target was not reached.
    int consecutive_catchup_misses = 0;
    uint64_t catchup_batch_count = 0;
    int max_consecutive_failures = 60;  // Set to ~1s of frames in create_encoder()
    std::chrono::steady_clock::time_point last_output_progress {};
    std::chrono::steady_clock::time_point query_failure_started {};
    std::chrono::steady_clock::time_point submit_backpressure_started {};
    bool submit_capacity_saturated = false;

    std::string last_error_string;
  };

  /**
   * @brief Create an AMF D3D11 encoder instance.
   * @param d3d_device The D3D11 device to use.
   * @return AMF encoder or nullptr on failure.
   */
  std::unique_ptr<amf_d3d11>
  create_amf_d3d11(ID3D11Device *d3d_device);

}  // namespace amf
