/**
 * @file src/amf/amf_encoder.h
 * @brief Declarations for standalone AMF encoder interface.
 */
#pragma once

#include "amf_config.h"
#include "amf_encoded_frame.h"
#include "amf_lifecycle.h"

#include "src/platform/common.h"
#include "src/video.h"
#include "src/video_colorspace.h"

#include <chrono>
#include <optional>

namespace amf {

  using bitrate_update_result_e = lifecycle::bitrate_update_result_e;

  /**
   * @brief Standalone AMF encoder interface.
   */
  class amf_encoder {
  public:
    virtual ~amf_encoder() = default;

    /**
     * @brief Create the encoder.
     * @param config AMF encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error
     */
    virtual bool
    create_encoder(const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format) = 0;

    /**
     * @brief Destroy the encoder.
     */
    virtual void
    destroy_encoder() = 0;

    /**
     * @brief Encode the next frame using platform-specific input surface.
     * @param frame_index Unique frame identifier.
     * @param force_idr Whether to encode frame as forced IDR.
     * More than one frame may be returned when a transient encoder delay left
     * completed output queued from an earlier call. Returning the whole ready batch
     * lets the caller catch up without permanently adding frame-interval latency.
     * @return Encoded frames in presentation order.
     */
    virtual amf_encode_result
    encode_frame(uint64_t frame_index, bool force_idr) = 0;

    /**
     * @brief Wait for and return output from already accepted inputs without submitting another frame.
     * @param timeout Maximum time to wait for output-pump progress.
     */
    virtual amf_encode_result
    drain_output(std::chrono::milliseconds timeout) = 0;

    /** @brief Whether accepted input has output due for delivery or already queued. */
    virtual bool
    has_output_due() = 0;

    /** @brief Whether an encoded packet is already queued for immediate delivery. */
    virtual bool
    has_completed_output() = 0;

    /** @brief Whether PA retains a final input that needs one future submission. */
    virtual bool
    has_retained_preanalysis_tail() = 0;

    /** @brief Whether a PA probe stalled at a user-selected queue below AMF's default. */
    virtual bool
    preanalysis_queue_fallback_recommended() = 0;

    /** @brief Whether a bounded SubmitInput call exceeded its deadline. */
    virtual bool
    driver_submission_timed_out() = 0;

    /**
     * @brief Tell AMF that no more input will be submitted and flush delayed output.
     * @return `true` when the drain request was accepted.
     *
     * This is used by input-only sessions, where a PreAnalysis pipeline cannot be
     * primed with a future live frame.
     */
    virtual bool
    begin_drain() = 0;

    /**
     * @brief Perform reference frame invalidation (RFI).
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` on error (caller should force IDR).
     */
    virtual bool
    invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) = 0;

    /**
     * @brief Set the bitrate for the encoder dynamically.
     * @param bitrate_kbps Bitrate in kilobits per second.
     */
    virtual bitrate_update_result_e
    set_bitrate(int bitrate_kbps) = 0;

    /** Bound in-flight vendor calls by the current AMD caller's startup/probe deadline. */
    virtual void
    set_caller_deadline(std::optional<std::chrono::steady_clock::time_point> deadline) = 0;

    /**
     * @brief Set HDR metadata for the encoder.
     * @param metadata HDR metadata, or nullopt to disable.
     * @return `true` when the requested metadata state was applied.
     */
    virtual bool
    set_hdr_metadata(const std::optional<amf_hdr_metadata> &metadata) = 0;

    /**
     * @brief Get the D3D11 input texture the encoder reads from.
     * @return Pointer to ID3D11Texture2D, or nullptr.
     */
    virtual void *
    get_input_texture() = 0;
  };

}  // namespace amf
