/**
 * @file src/nvenc/nvenc_log_flags.h
 * @brief Flags for the NvEnc created encoder log line.
 */
#pragma once

#include <ffnvcodec/nvEncodeAPI.h>
#include <string_view>

namespace nvenc::log_flags {

  /**
   * @brief Intra refresh flag for the created encoder log line.
   * @param config The encoder config built for this session.
   * @param video_format 0 for H.264, 1 for HEVC, 2 for AV1.
   * @param driver_defaults True if NvEncInitializeEncoder() succeeded only with encodeConfig=nullptr.
   * @return " intra-refresh", " intra-refresh=unknown", or empty if intra refresh wasn't requested.
   */
  inline std::string_view intra_refresh(const NV_ENC_CONFIG &config, int video_format, bool driver_defaults) {
    const bool requested = (video_format == 0 && config.encodeCodecConfig.h264Config.enableIntraRefresh) ||
                           (video_format == 1 && config.encodeCodecConfig.hevcConfig.enableIntraRefresh);
    if (!requested) {
      return {};
    }
    // The driver-default retry drops config, so the requested setting may not have reached the encoder
    return driver_defaults ? " intra-refresh=unknown" : " intra-refresh";
  }

}  // namespace nvenc::log_flags
