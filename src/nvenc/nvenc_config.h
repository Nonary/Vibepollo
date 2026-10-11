/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

#include <string_view>

namespace nvenc {

  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  enum class split_encode_mode {
    auto_mode,  ///< Let the NVIDIA driver decide when split-frame encoding should be used
    enabled,  ///< Force split-frame encoding when supported
    disabled,  ///< Disable split-frame encoding even when it would otherwise be auto-enabled
  };

  struct encoder_selection_policy_t {
    bool include_native = true;
    bool fail_closed = false;
  };

  inline constexpr std::string_view canonical_encoder_name(
    std::string_view requested_encoder) noexcept {
    if (requested_encoder == "nvenc_experimental") {
      return "nvenc";
    }
    return requested_encoder;
  }

  /** Native Linux NVENC is preferred unless the legacy backend is requested. */
  inline constexpr encoder_selection_policy_t encoder_selection_policy(
    std::string_view requested_encoder) noexcept {
    const auto canonical_encoder = canonical_encoder_name(requested_encoder);
    return {
      canonical_encoder != "nvenc_legacy",
      canonical_encoder == "nvenc",
    };
  }

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;

    // Frames between forced IDRs. 0 = infinite GOP (stock behaviour: IDR only on demand).
    // A short GOP exists for CO-OP SPLIT-SCREEN: the client splices this host's slices into
    // a composite picture, and a dropped pane can only be re-anchored by an IDR. With an
    // infinite GOP that costs a keyframe from BOTH hosts at the same instant; with a short
    // one this host re-anchors alone. Measured cost of gop=30 at 3840x1080: ~4% bitrate
    // (55.2 vs 53.0 Mbps at 165fps, qp28), i.e. far cheaper than all-intra's 2x.
    int gop_length = 0;

    // ONE SLICE PER CTB ROW (sliceMode 2 / sliceModeData 1) instead of "N slices per
    // picture" (sliceMode 3). The VERTICAL co-op split needs exactly one slice per row:
    // composite row r is host A's row at CTB address r*Wc and host B's at r*Wc+Wp, and a
    // slice spanning two rows is NOT a contiguous run in the composite, so approximate
    // slice counts do not work. Measured cost of the slicing itself: ~+17% bitrate.
    bool slice_rows = false;

    // Constrained encoding: -1 = automatic (on whenever slicesPerFrame > 1, the shipped
    // behaviour), 0 = force OFF, 1 = force ON. The override exists to MEASURE what it
    // costs: it restricts inter prediction to the slice region, which is exactly right
    // when slice == pane but confines motion search to a 32px band when slice == one CTB
    // row. ffmpeg cannot answer this -- its -constrained-encoding arms the feature without
    // ever setting the per-picture constrainedFrame flag, so its output is byte-identical.
    int constrained = -1;

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // Percentage increase of VBV/HRD from the default single frame, allows low-latency variable bitrate
    int vbv_percentage_increase = 0;

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;

    // Allocate more bitrate to frames with more motion, reduces block-artefact pulsing in moving scenes
    bool temporal_aq = false;

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;

    // Min QP value for H.264 when enable_min_qp is selected
    unsigned min_qp_h264 = 19;

    // Min QP value for HEVC when enable_min_qp is selected
    unsigned min_qp_hevc = 23;

    // Min QP value for AV1 when enable_min_qp is selected
    unsigned min_qp_av1 = 23;

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;

    // Control split-frame encoding for supported HEVC/AV1 sessions
    nvenc::split_encode_mode split_encode_mode = nvenc::split_encode_mode::auto_mode;
    // Code H.264 as MAIN profile instead of HIGH. This exists for CO-OP SPLIT-SCREEN across
    // MIXED GPU VENDORS. The client splices two hosts' slices into one picture that carries a
    // single SPS/PPS, so both encoders must agree on every field the decoder reads. NVENC's HIGH
    // profile sets transform_8x8_mode_flag=1 in the PPS extension; AMD's AMF omits the extension
    // entirely (=0) and exposes no control over it. Decoding AMF slices under an NVENC HIGH PPS
    // makes the decoder expect a transform_size_8x8_flag in every macroblock that AMF never
    // coded, and the WHOLE picture desynchronises. MAIN has no 8x8 transform, so this makes the
    // two vendors' bitstreams spliceable. Measured 2026-08-30: with this set, a real NVENC pane
    // and a real AMF pane compose BIT-EXACTLY.
    bool h264_main_profile = false;

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;

    // Intra refresh for clients that doesn't request keyframe correctly
    bool intra_refresh = false;
  };

}  // namespace nvenc
