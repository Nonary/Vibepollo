/**
 * @file src/nvenc/nvenc_intra_refresh.h
 * @brief Intra refresh setup shared by the H.264 and HEVC encoder configs.
 */
#pragma once

#include "nvenc_config.h"

#include <ffnvcodec/nvEncodeAPI.h>

namespace nvenc::intra_refresh {

  enum class result_e {
    not_requested,  ///< Neither the client nor nvenc_intra_refresh asked for it
    enabled,  ///< Intra refresh is on with a single slice
    enabled_without_single_slice,  ///< Intra refresh is on, but the encoder can't do it in a single slice
    unsupported,  ///< Requested, but the encoder doesn't support intra refresh
  };

  /**
   * @brief Turn on intra refresh in an H.264 or HEVC config if the client or nvenc_intra_refresh asks for it.
   * @param format_config NV_ENC_CONFIG_H264 or NV_ENC_CONFIG_HEVC.
   * @param client_enable_intra_refresh The client's request, video::config_t::enableIntraRefresh.
   * @param config Host NVENC settings. config.intra_refresh is nvenc_intra_refresh.
   * @param has_cap Returns nonzero if the encoder supports an NV_ENC_CAPS value. Only called when intra refresh is requested.
   * @return What was done, so the caller can log it.
   */
  template<typename FormatConfig, typename HasCap>
  result_e apply(FormatConfig &format_config, int client_enable_intra_refresh, const nvenc_config &config, HasCap &&has_cap) {
    // nvenc_intra_refresh forces it on for clients that don't request it
    if (client_enable_intra_refresh != 1 && !config.intra_refresh) {
      return result_e::not_requested;
    }
    if (!has_cap(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH)) {
      return result_e::unsupported;
    }

    format_config.enableIntraRefresh = 1;
    format_config.intraRefreshPeriod = 300;
    format_config.intraRefreshCnt = 299;
    format_config.outputRecoveryPointSEI = 1;
    if (!has_cap(NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH)) {
      return result_e::enabled_without_single_slice;
    }
    format_config.singleSliceIntraRefresh = 1;
    return result_e::enabled;
  }

}  // namespace nvenc::intra_refresh
