/**
 * @file src/nvenc/nvenc_base.cpp
 * @brief Definitions for abstract platform-agnostic base of standalone NVENC encoder.
 */
// this include
#include "nvenc_base.h"
#include "nvenc_api.h"

// standard includes
#include <format>
#include <optional>

// Make sure we check backwards compatibility when bumping the Video Codec SDK version
// Things to look out for:
// - NV_ENC_*_VER definitions where the value inside NVENCAPI_STRUCT_VERSION() was increased
// - Incompatible struct changes in nvEncodeAPI.h (fields removed, semantics changed, etc.)
// - Test both old and new drivers with all supported codecs
#if !((NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION <= 2) || (NVENCAPI_MAJOR_VERSION == 13 && NVENCAPI_MINOR_VERSION == 0))
  #error Check and update NVENC code for backwards compatibility!
#endif

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"

namespace {

  GUID quality_preset_guid_from_number(unsigned number) {
    if (number > 7) {
      number = 7;
    }

    switch (number) {
      case 1:
      default:
        return NV_ENC_PRESET_P1_GUID;

      case 2:
        return NV_ENC_PRESET_P2_GUID;

      case 3:
        return NV_ENC_PRESET_P3_GUID;

      case 4:
        return NV_ENC_PRESET_P4_GUID;

      case 5:
        return NV_ENC_PRESET_P5_GUID;

      case 6:
        return NV_ENC_PRESET_P6_GUID;

      case 7:
        return NV_ENC_PRESET_P7_GUID;
    }
  };

  bool equal_guids(const GUID &guid1, const GUID &guid2) {
    return std::memcmp(&guid1, &guid2, sizeof(GUID)) == 0;
  }

  auto quality_preset_string_from_guid(const GUID &guid) {
    if (equal_guids(guid, NV_ENC_PRESET_P1_GUID)) {
      return "P1";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P2_GUID)) {
      return "P2";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P3_GUID)) {
      return "P3";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P4_GUID)) {
      return "P4";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P5_GUID)) {
      return "P5";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P6_GUID)) {
      return "P6";
    }
    if (equal_guids(guid, NV_ENC_PRESET_P7_GUID)) {
      return "P7";
    }
    return "Unknown";
  }

  const char *codec_name_from_video_format(int video_format) {
    switch (video_format) {
      case 0:
        return "H.264";

      case 1:
        return "HEVC";

      case 2:
        return "AV1";

      default:
        return "Unknown";
    }
  }

  std::optional<GUID> encode_guid_from_video_format(int video_format) {
    switch (video_format) {
      case 0:
        return NV_ENC_CODEC_H264_GUID;

      case 1:
        return NV_ENC_CODEC_HEVC_GUID;

      case 2:
        return NV_ENC_CODEC_AV1_GUID;

      default:
        return std::nullopt;
    }
  }

}  // namespace

namespace nvenc {

  nvenc_base::nvenc_base(NV_ENC_DEVICE_TYPE device_type):
      device_type(device_type) {
  }

  nvenc_base::~nvenc_base() {
    // Use destroy_encoder() instead
  }

  bool nvenc_base::create_encoder(const nvenc_config &config, const video::config_t &client_config, const nvenc_colorspace_t &colorspace, NV_ENC_BUFFER_FORMAT buffer_format) {
    const auto encode_guid = encode_guid_from_video_format(client_config.videoFormat);
    if (!encode_guid) {
      BOOST_LOG(error) << "NvEnc: unknown video format " << client_config.videoFormat;
      return false;
    }

    const auto api_candidates = api::codec_api_candidates(client_config.videoFormat);
    if (api_candidates.empty()) {
      BOOST_LOG(error) << "NvEnc: no API candidates available for video format " << client_config.videoFormat;
      return false;
    }

    if (encoder) {
      destroy_encoder();
    }
    auto fail_guard = util::fail_guard([this] {
      destroy_encoder();
    });

    encoder_params.width = client_config.width;
    encoder_params.height = client_config.height;
    encoder_params.buffer_format = buffer_format;
    encoder_params.rfi = true;

    selected_api_version = 0;

    NV_ENC_INITIALIZE_PARAMS init_params {};
    NV_ENC_PRESET_CONFIG preset_config {};
    bool have_preset_config = false;

    auto destroy_api_attempt = [&]() {
      if (encoder) {
        nvenc->nvEncDestroyEncoder(encoder);
        encoder = nullptr;
      }
      selected_api_version = 0;
    };

    auto retry_with_next_api = [&](std::string_view operation, uint32_t api_version) {
      BOOST_LOG(debug) << "NvEnc: "sv << operation << " rejected API "sv << api::version_string(api_version);
      destroy_api_attempt();
    };

    auto buffer_is_10bit = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    auto buffer_is_yuv444 = [&]() {
      return buffer_format == NV_ENC_BUFFER_FORMAT_AYUV || buffer_format == NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    };

    for (const auto api_version : api_candidates) {
      if (!init_library(api_version)) {
        if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
          continue;
        }
        return false;
      }

      NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {api::open_encode_session_ex_params_version(api_version)};
      session_params.device = device;
      session_params.deviceType = device_type;
      session_params.apiVersion = api_version;
      if (!nvenc_failed(nvenc->nvEncOpenEncodeSessionEx(&session_params, &encoder))) {
        selected_api_version = api_version;
      } else {
        if (last_nvenc_status != NV_ENC_ERR_INVALID_VERSION) {
          BOOST_LOG(error) << "NvEnc: NvEncOpenEncodeSessionEx() failed for API "
                           << api::version_string(api_version) << ": " << last_nvenc_error_string;
          return false;
        }

        retry_with_next_api("NvEncOpenEncodeSessionEx()"sv, api_version);
        continue;
      }

      uint32_t encode_guid_count = 0;
      if (nvenc_failed(nvenc->nvEncGetEncodeGUIDCount(encoder, &encode_guid_count))) {
        if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
          retry_with_next_api("NvEncGetEncodeGUIDCount()"sv, api_version);
          continue;
        }

        BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDCount() failed: " << last_nvenc_error_string;
        return false;
      };

      std::vector<GUID> encode_guids(encode_guid_count);
      if (nvenc_failed(nvenc->nvEncGetEncodeGUIDs(encoder, encode_guids.data(), (uint32_t) encode_guids.size(), &encode_guid_count))) {
        if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
          retry_with_next_api("NvEncGetEncodeGUIDs()"sv, api_version);
          continue;
        }

        BOOST_LOG(error) << "NvEnc: NvEncGetEncodeGUIDs() failed: " << last_nvenc_error_string;
        return false;
      }

      init_params = {api::initialize_params_version(selected_api_version)};
      init_params.encodeGUID = *encode_guid;

      {
        auto search_predicate = [&](const GUID &guid) {
          return equal_guids(init_params.encodeGUID, guid);
        };
        if (std::find_if(encode_guids.begin(), encode_guids.end(), search_predicate) == encode_guids.end()) {
          BOOST_LOG(error) << "NvEnc: encoding format is not supported by the gpu";
          return false;
        }
      }

      auto get_encoder_cap = [&](NV_ENC_CAPS cap) -> std::optional<int> {
        NV_ENC_CAPS_PARAM param = {api::caps_param_version(selected_api_version), cap};
        int value = 0;
        if (nvenc_failed(nvenc->nvEncGetEncodeCaps(encoder, init_params.encodeGUID, &param, &value))) {
          return std::nullopt;
        }
        return value;
      };

      auto require_encoder_cap = [&](NV_ENC_CAPS cap, std::string_view operation) -> std::optional<int> {
        auto value = get_encoder_cap(cap);
        if (value) {
          return value;
        }

        if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
          retry_with_next_api(operation, api_version);
          return std::nullopt;
        }

        BOOST_LOG(error) << "NvEnc: "sv << operation << " failed: "sv << last_nvenc_error_string;
        return std::nullopt;
      };

      auto supported_width = require_encoder_cap(NV_ENC_CAPS_WIDTH_MAX, "NvEncGetEncodeCaps(width)"sv);
      if (!supported_width) {
        if (!encoder) {
          continue;
        }
        return false;
      }

      auto supported_height = require_encoder_cap(NV_ENC_CAPS_HEIGHT_MAX, "NvEncGetEncodeCaps(height)"sv);
      if (!supported_height) {
        if (!encoder) {
          continue;
        }
        return false;
      }

      if (encoder_params.width > *supported_width || encoder_params.height > *supported_height) {
        BOOST_LOG(error) << "NvEnc: gpu max encode resolution " << *supported_width << "x" << *supported_height << ", requested " << encoder_params.width << "x" << encoder_params.height;
        return false;
      }

      if (buffer_is_10bit()) {
        auto supports_10bit = require_encoder_cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE, "NvEncGetEncodeCaps(10-bit)"sv);
        if (!supports_10bit) {
          if (!encoder) {
            continue;
          }
          return false;
        }
        if (!*supports_10bit) {
          BOOST_LOG(error) << "NvEnc: gpu doesn't support 10-bit encode";
          return false;
        }
      }

      if (buffer_is_yuv444()) {
        auto supports_yuv444 = require_encoder_cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, "NvEncGetEncodeCaps(YUV444)"sv);
        if (!supports_yuv444) {
          if (!encoder) {
            continue;
          }
          return false;
        }
        if (!*supports_yuv444) {
          BOOST_LOG(error) << "NvEnc: gpu doesn't support YUV444 encode";
          return false;
        }
      }

      if (async_event_handle) {
        auto supports_async = require_encoder_cap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT, "NvEncGetEncodeCaps(async)"sv);
        if (!supports_async) {
          if (!encoder) {
            continue;
          }
          return false;
        }
        if (!*supports_async) {
          BOOST_LOG(warning) << "NvEnc: gpu doesn't support async encode";
          async_event_handle = nullptr;
        }
      }

      auto supports_rfi = require_encoder_cap(NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION, "NvEncGetEncodeCaps(rfi)"sv);
      if (!supports_rfi) {
        if (!encoder) {
          continue;
        }
        return false;
      }
      encoder_params.rfi = *supports_rfi;

      auto supports_weighted_prediction = require_encoder_cap(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION, "NvEncGetEncodeCaps(weighted prediction)"sv);
      if (!supports_weighted_prediction) {
        if (!encoder) {
          continue;
        }
        return false;
      }

      init_params.presetGUID = quality_preset_guid_from_number(config.quality_preset);
      init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
      init_params.enablePTD = 1;
      init_params.enableEncodeAsync = async_event_handle ? 1 : 0;
      init_params.enableWeightedPrediction = config.weighted_prediction && *supports_weighted_prediction;

      init_params.encodeWidth = encoder_params.width;
      init_params.darWidth = encoder_params.width;
      init_params.encodeHeight = encoder_params.height;
      init_params.darHeight = encoder_params.height;
      init_params.frameRateNum = client_config.framerate;
      init_params.frameRateDen = 1;
      if (client_config.framerateX100 > 0) {
        AVRational fps = video::framerateX100_to_rational(client_config.framerateX100);
        init_params.frameRateNum = fps.num;
        init_params.frameRateDen = fps.den;
      }

      preset_config = {api::preset_config_version(selected_api_version), {api::config_version(selected_api_version)}};

      BOOST_LOG(debug) << "NvEnc: querying preset config for API " << api::version_string(api_version)
                       << " (preset_config_ver=0x" << std::hex << api::preset_config_version(selected_api_version)
                       << " config_ver=0x" << api::config_version(selected_api_version) << std::dec << ")";

      if (nvenc_failed(nvenc->nvEncGetEncodePresetConfigEx(encoder, init_params.encodeGUID, init_params.presetGUID, init_params.tuningInfo, &preset_config))) {
        const auto preset_config_ex_status = last_nvenc_status;
        const auto preset_config_ex_error = last_nvenc_error_string;

        BOOST_LOG(debug) << "NvEnc: NvEncGetEncodePresetConfigEx() returned " << preset_config_ex_error
                         << " for API " << api::version_string(api_version);

        if (client_config.videoFormat != 2 &&
            (preset_config_ex_status == NV_ENC_ERR_INVALID_VERSION || preset_config_ex_status == NV_ENC_ERR_UNSUPPORTED_PARAM)) {
          preset_config = {api::preset_config_version(selected_api_version), {api::config_version(selected_api_version)}};
          if (!nvenc_failed(nvenc->nvEncGetEncodePresetConfig(encoder, init_params.encodeGUID, init_params.presetGUID, &preset_config))) {
            BOOST_LOG(debug) << "NvEnc: falling back to NvEncGetEncodePresetConfig() for API "
                             << api::version_string(api_version);
            have_preset_config = true;
            break;
          }

          BOOST_LOG(debug) << "NvEnc: NvEncGetEncodePresetConfig() returned " << last_nvenc_error_string
                           << " for API " << api::version_string(api_version);

          if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION || last_nvenc_status == NV_ENC_ERR_UNSUPPORTED_PARAM) {
            BOOST_LOG(warning) << "NvEnc: preset config queries were rejected for API "
                               << api::version_string(api_version)
                               << "; will initialize with presetGUID + tuningInfo defaults";
            break;
          }

          BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfigEx() failed: " << preset_config_ex_error;
          BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfig() failed: " << last_nvenc_error_string;
          return false;
        }

        if (preset_config_ex_status == NV_ENC_ERR_INVALID_VERSION || preset_config_ex_status == NV_ENC_ERR_UNSUPPORTED_PARAM) {
          BOOST_LOG(warning) << "NvEnc: NvEncGetEncodePresetConfigEx() was rejected for API "
                             << api::version_string(api_version)
                             << "; will initialize with presetGUID + tuningInfo defaults";
          break;
        }

        BOOST_LOG(error) << "NvEnc: NvEncGetEncodePresetConfigEx() failed: " << preset_config_ex_error;
        return false;
      }

      have_preset_config = true;

      break;
    }

    if (!encoder || !selected_api_version) {
      BOOST_LOG(error) << "NvEnc: no compatible driver API found for " << codec_name_from_video_format(client_config.videoFormat);
      return false;
    }

    auto get_encoder_cap = [&](NV_ENC_CAPS cap) {
      NV_ENC_CAPS_PARAM param = {api::caps_param_version(selected_api_version), cap};
      int value = 0;
      if (nvenc_failed(nvenc->nvEncGetEncodeCaps(encoder, init_params.encodeGUID, &param, &value))) {
        BOOST_LOG(warning) << "NvEnc: NvEncGetEncodeCaps() failed after API selection: " << last_nvenc_error_string;
        return 0;
      }
      return value;
    };

    NV_ENC_CONFIG enc_config {};
    if (have_preset_config) {
      enc_config = preset_config.presetCfg;
    } else {
      enc_config.version = api::config_version(selected_api_version);
    }
    enc_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
    enc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
    enc_config.frameIntervalP = 1;
    enc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_config.rcParams.zeroReorderDelay = 1;
    enc_config.rcParams.enableLookahead = 0;
    enc_config.rcParams.lowDelayKeyFrameScale = 1;
    enc_config.rcParams.multiPass = config.two_pass == nvenc_two_pass::quarter_resolution ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION :
                                    config.two_pass == nvenc_two_pass::full_resolution    ? NV_ENC_TWO_PASS_FULL_RESOLUTION :
                                                                                            NV_ENC_MULTI_PASS_DISABLED;

    enc_config.rcParams.enableAQ = config.adaptive_quantization;
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      enc_config.rcParams.vbvBufferSize = client_config.bitrate * 1000 / client_config.framerate;
      if (config.vbv_percentage_increase > 0) {
        enc_config.rcParams.vbvBufferSize += enc_config.rcParams.vbvBufferSize * config.vbv_percentage_increase / 100;
      }
    }

    auto set_h264_hevc_common_format_config = [&](auto &format_config) {
      format_config.repeatSPSPPS = 1;
      format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
      format_config.sliceMode = 3;
      format_config.sliceModeData = client_config.slicesPerFrame;
      format_config.chromaFormatIDC = buffer_is_yuv444() ? 3 : 1;
      format_config.enableFillerDataInsertion = config.insert_filler_data;
    };

    auto set_ref_frames = [&](uint32_t &ref_frames_option, NV_ENC_NUM_REF_FRAMES &L0_option, uint32_t ref_frames_default) {
      if (client_config.numRefFrames > 0) {
        ref_frames_option = client_config.numRefFrames;
      } else {
        ref_frames_option = ref_frames_default;
      }
      if (ref_frames_option > 0 && !get_encoder_cap(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES)) {
        ref_frames_option = 1;
        encoder_params.rfi = false;
      }
      encoder_params.ref_frames_in_dpb = ref_frames_option;
      // This limits ref frames any frame can use to 1, but allows larger buffer size for fallback if some frames are invalidated through rfi
      L0_option = NV_ENC_NUM_REF_FRAMES_1;
    };

    auto set_minqp_if_enabled = [&](int value) {
      if (config.enable_min_qp) {
        enc_config.rcParams.enableMinQP = 1;
        enc_config.rcParams.minQP.qpInterP = value;
        enc_config.rcParams.minQP.qpIntra = value;
      }
    };

    auto fill_h264_hevc_vui = [&](auto &vui_config) {
      vui_config.videoSignalTypePresentFlag = 1;
      vui_config.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
      vui_config.videoFullRangeFlag = colorspace.full_range;
      vui_config.colourDescriptionPresentFlag = 1;
      vui_config.colourPrimaries = colorspace.primaries;
      vui_config.transferCharacteristics = colorspace.tranfer_function;
      vui_config.colourMatrix = colorspace.matrix;
      vui_config.chromaSampleLocationFlag = buffer_is_yuv444() ? 0 : 1;
      vui_config.chromaSampleLocationTop = 0;
      vui_config.chromaSampleLocationBot = 0;
    };

      const auto set_hevc_10bit_format = [&](auto &format_config) {
#if NVENCAPI_MAJOR_VERSION > 12 || (NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION >= 1)
        if (api::supports_separate_bit_depth_fields(selected_api_version)) {
          format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
          format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
        } else {
          // SDK 13 removed the old HEVC bit-depth field names, but the v11/v12.0
          // layout still occupies these reserved bits.
          format_config.reserved3 = 2;
        }
#else
        format_config.pixelBitDepthMinus8 = 2;
#endif
      };

      const auto set_av1_10bit_format = [&](auto &format_config) {
#if NVENCAPI_MAJOR_VERSION > 12 || (NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION >= 1)
        format_config.inputBitDepth = NV_ENC_BIT_DEPTH_10;
        format_config.outputBitDepth = NV_ENC_BIT_DEPTH_10;
#else
        format_config.inputPixelBitDepthMinus8 = 2;
        format_config.pixelBitDepthMinus8 = 2;
#endif
      };

      switch (client_config.videoFormat) {
      case 0:
        {
          // H.264
          enc_config.profileGUID = buffer_is_yuv444() ? NV_ENC_H264_PROFILE_HIGH_444_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
          auto &format_config = enc_config.encodeCodecConfig.h264Config;
          set_h264_hevc_common_format_config(format_config);
          if (config.h264_cavlc || !get_encoder_cap(NV_ENC_CAPS_SUPPORT_CABAC)) {
            format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;
          } else {
            format_config.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
          }
          set_ref_frames(format_config.maxNumRefFrames, format_config.numRefL0, 5);
          set_minqp_if_enabled(config.min_qp_h264);
          fill_h264_hevc_vui(format_config.h264VUIParameters);
          break;
        }

      case 1:
        {
          // HEVC
          auto &format_config = enc_config.encodeCodecConfig.hevcConfig;
          set_h264_hevc_common_format_config(format_config);
          if (buffer_is_10bit()) {
            set_hevc_10bit_format(format_config);
          }
          set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numRefL0, 5);
          set_minqp_if_enabled(config.min_qp_hevc);
          fill_h264_hevc_vui(format_config.hevcVUIParameters);
          if (client_config.enableIntraRefresh == 1) {
            if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_INTRA_REFRESH)) {
              format_config.enableIntraRefresh = 1;
              format_config.intraRefreshPeriod = 300;
              format_config.intraRefreshCnt = 299;
              if (get_encoder_cap(NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH)) {
                format_config.singleSliceIntraRefresh = 1;
              } else {
                BOOST_LOG(warning) << "NvEnc: Single Slice Intra Refresh not supported";
              }
            } else {
              BOOST_LOG(error) << "NvEnc: Client asked for intra-refresh but the encoder does not support intra-refresh";
            }
          }
          break;
        }

      case 2:
        {
          // AV1
          enc_config.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
          auto &format_config = enc_config.encodeCodecConfig.av1Config;
          format_config.level = NV_ENC_LEVEL_AV1_AUTOSELECT;
          format_config.repeatSeqHdr = 1;
          format_config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
          format_config.chromaFormatIDC = buffer_is_yuv444() ? 3 : 1;
          format_config.enableBitstreamPadding = config.insert_filler_data;
          if (buffer_is_10bit()) {
            set_av1_10bit_format(format_config);
          } else {
            format_config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
            format_config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
          }
          format_config.colorPrimaries = colorspace.primaries;
          format_config.transferCharacteristics = colorspace.tranfer_function;
          format_config.matrixCoefficients = colorspace.matrix;
          format_config.colorRange = colorspace.full_range;
          format_config.chromaSamplePosition = buffer_is_yuv444() ? 0 : 1;
          set_ref_frames(format_config.maxNumRefFramesInDPB, format_config.numFwdRefs, 8);
          set_minqp_if_enabled(config.min_qp_av1);

          if (client_config.slicesPerFrame > 1) {
            // NVENC only supports slice counts that are powers of two, so we'll pick powers of two
            // with bias to rows due to hopefully more similar macroblocks with a row vs a column.
            format_config.numTileRows = std::pow(2, std::ceil(std::log2(client_config.slicesPerFrame) / 2));
            format_config.numTileColumns = std::pow(2, std::floor(std::log2(client_config.slicesPerFrame) / 2));
          }
          break;
        }
    }

    std::string split_frame_status = "split-frame=n/a";
    if (client_config.videoFormat == 1 || client_config.videoFormat == 2) {
      if (!api::supports_implicit_split_frame(selected_api_version)) {
        split_frame_status = "split-frame=disabled(api<12.1)";
      } else if (client_config.videoFormat == 1 && init_params.enableWeightedPrediction) {
        split_frame_status = "split-frame=disabled(weighted-prediction)";
      } else {
#if NVENCAPI_MAJOR_VERSION > 12 || (NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION >= 1)
        if (config.force_split_encode) {
          init_params.splitEncodeMode = NV_ENC_SPLIT_AUTO_FORCED_MODE;
          split_frame_status = "split-frame=forced";
        } else {
          init_params.splitEncodeMode = NV_ENC_SPLIT_AUTO_MODE;
          split_frame_status = "split-frame=auto";
        }
#else
        split_frame_status = "split-frame=disabled(header<12.1)";
#endif
      }
    }

    init_params.encodeConfig = &enc_config;

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
      auto init_error = last_nvenc_error_string;
      auto init_status = last_nvenc_status;

      // Log detailed error string from the driver if available
      if (nvenc->nvEncGetLastErrorString) {
        auto driver_msg = nvenc->nvEncGetLastErrorString(encoder);
        if (driver_msg && driver_msg[0]) {
          BOOST_LOG(error) << "NvEnc: driver error detail: " << driver_msg;
        }
      }

      if (!have_preset_config && init_status == NV_ENC_ERR_INVALID_PARAM && client_config.videoFormat != 2) {
        // Without preset config, our manual config may have missed required fields.
        // Try once more with encodeConfig=nullptr so the driver uses presetGUID+tuningInfo defaults.
        BOOST_LOG(debug) << "NvEnc: explicit config rejected (" << init_error
                         << "); retrying NvEncInitializeEncoder() with driver defaults";

        // Must destroy and re-open the session since NvEncInitializeEncoder can only be called once
        nvenc->nvEncDestroyEncoder(encoder);
        encoder = nullptr;

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {api::open_encode_session_ex_params_version(selected_api_version)};
        session_params.device = device;
        session_params.deviceType = device_type;
        session_params.apiVersion = selected_api_version;
        if (nvenc_failed(nvenc->nvEncOpenEncodeSessionEx(&session_params, &encoder))) {
          BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << init_error;
          return false;
        }

        init_params.encodeConfig = nullptr;
        if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
          BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed with both explicit config (" << init_error
                           << ") and driver defaults (" << last_nvenc_error_string << ")";
          return false;
        }

        BOOST_LOG(info) << "NvEnc: initialized with driver defaults (preset config was unavailable)";
      } else if (!have_preset_config && init_status == NV_ENC_ERR_INVALID_PARAM && client_config.videoFormat == 2) {
        BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << init_error
                         << " (AV1 preset fallback unavailable; skipping driver-default retry)";
        return false;
      } else {
        BOOST_LOG(error) << "NvEnc: NvEncInitializeEncoder() failed: " << init_error;
        return false;
      }
    }

    if (async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = {api::event_params_version(selected_api_version)};
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncRegisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncRegisterAsyncEvent() failed: " << last_nvenc_error_string;
        return false;
      }
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER create_bitstream_buffer = {api::create_bitstream_buffer_version(selected_api_version)};
    if (nvenc_failed(nvenc->nvEncCreateBitstreamBuffer(encoder, &create_bitstream_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncCreateBitstreamBuffer() failed: " << last_nvenc_error_string;
      return false;
    }
    output_bitstream = create_bitstream_buffer.bitstreamBuffer;

    if (!create_and_register_input_buffer()) {
      return false;
    }

    {
      auto f = stat_trackers::two_digits_after_decimal();
      BOOST_LOG(debug) << "NvEnc: requested encoded frame size " << f % (client_config.bitrate / 8. / client_config.framerate) << " kB";
    }

    {
      std::string extra;
      extra += std::format(" api={}", api::version_string(selected_api_version));
      if (init_params.enableEncodeAsync) {
        extra += " async";
      }
      if (buffer_is_yuv444()) {
        extra += " yuv444";
      }
      if (buffer_is_10bit()) {
        extra += " 10-bit";
      }
      if (enc_config.rcParams.multiPass != NV_ENC_MULTI_PASS_DISABLED) {
        extra += " two-pass";
      }
      if (config.vbv_percentage_increase > 0 && get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
        extra += std::format(" vbv+{}", config.vbv_percentage_increase);
      }
      if (encoder_params.rfi) {
        extra += " rfi";
      }
      if (init_params.enableWeightedPrediction) {
        extra += " weighted-prediction";
      }
      if (enc_config.rcParams.enableAQ) {
        extra += " spatial-aq";
      }
      if (enc_config.rcParams.enableMinQP) {
        extra += std::format(" qpmin={}", enc_config.rcParams.minQP.qpInterP);
      }
      if (config.insert_filler_data) {
        extra += " filler-data";
      }
      extra += " " + split_frame_status;

      BOOST_LOG(info) << "NvEnc: created encoder " << codec_name_from_video_format(client_config.videoFormat) << " "
                      << quality_preset_string_from_guid(init_params.presetGUID) << extra;
    }

    encoder_state = {};
    fail_guard.disable();
    return true;
  }

  void nvenc_base::destroy_encoder() {
    if (output_bitstream) {
      if (nvenc_failed(nvenc->nvEncDestroyBitstreamBuffer(encoder, output_bitstream))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyBitstreamBuffer() failed: " << last_nvenc_error_string;
      }
      output_bitstream = nullptr;
    }
    if (encoder && async_event_handle) {
      NV_ENC_EVENT_PARAMS event_params = {api::event_params_version(selected_api_version)};
      event_params.completionEvent = async_event_handle;
      if (nvenc_failed(nvenc->nvEncUnregisterAsyncEvent(encoder, &event_params))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterAsyncEvent() failed: " << last_nvenc_error_string;
      }
    }
    if (registered_input_buffer) {
      if (nvenc_failed(nvenc->nvEncUnregisterResource(encoder, registered_input_buffer))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnregisterResource() failed: " << last_nvenc_error_string;
      }
      registered_input_buffer = nullptr;
    }
    if (encoder) {
      if (nvenc_failed(nvenc->nvEncDestroyEncoder(encoder))) {
        BOOST_LOG(error) << "NvEnc: NvEncDestroyEncoder() failed: " << last_nvenc_error_string;
      }
      encoder = nullptr;
    }

    encoder_state = {};
    encoder_params = {};
    selected_api_version = 0;
  }

  nvenc_encoded_frame nvenc_base::encode_frame(uint64_t frame_index, bool force_idr) {
    if (!encoder) {
      return {};
    }

    assert(registered_input_buffer);
    assert(output_bitstream);

    if (!synchronize_input_buffer()) {
      BOOST_LOG(error) << "NvEnc: failed to synchronize input buffer";
      return {};
    }

    NV_ENC_MAP_INPUT_RESOURCE mapped_input_buffer = {api::map_input_resource_version(selected_api_version)};
    mapped_input_buffer.registeredResource = registered_input_buffer;

    if (nvenc_failed(nvenc->nvEncMapInputResource(encoder, &mapped_input_buffer))) {
      BOOST_LOG(error) << "NvEnc: NvEncMapInputResource() failed: " << last_nvenc_error_string;
      return {};
    }
    auto unmap_guard = util::fail_guard([&] {
      if (nvenc_failed(nvenc->nvEncUnmapInputResource(encoder, mapped_input_buffer.mappedResource))) {
        BOOST_LOG(error) << "NvEnc: NvEncUnmapInputResource() failed: " << last_nvenc_error_string;
      }
    });

    NV_ENC_PIC_PARAMS pic_params = {api::pic_params_version(selected_api_version)};
    pic_params.inputWidth = encoder_params.width;
    pic_params.inputHeight = encoder_params.height;
    pic_params.encodePicFlags = force_idr ? NV_ENC_PIC_FLAG_FORCEIDR : 0;
    pic_params.inputTimeStamp = frame_index;
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic_params.inputBuffer = mapped_input_buffer.mappedResource;
    pic_params.bufferFmt = mapped_input_buffer.mappedBufferFmt;
    pic_params.outputBitstream = output_bitstream;
    pic_params.completionEvent = async_event_handle;

    if (nvenc_failed(nvenc->nvEncEncodePicture(encoder, &pic_params))) {
      BOOST_LOG(error) << "NvEnc: NvEncEncodePicture() failed: " << last_nvenc_error_string;
      return {};
    }

    NV_ENC_LOCK_BITSTREAM lock_bitstream = {api::lock_bitstream_version(selected_api_version)};
    lock_bitstream.outputBitstream = output_bitstream;
    lock_bitstream.doNotWait = async_event_handle ? 1 : 0;

    if (async_event_handle && !wait_for_async_event(100)) {
      BOOST_LOG(error) << "NvEnc: frame " << frame_index << " encode wait timeout";
      return {};
    }

    if (nvenc_failed(nvenc->nvEncLockBitstream(encoder, &lock_bitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncLockBitstream() failed: " << last_nvenc_error_string;
      return {};
    }

    auto data_pointer = (uint8_t *) lock_bitstream.bitstreamBufferPtr;
    nvenc_encoded_frame encoded_frame {
      {data_pointer, data_pointer + lock_bitstream.bitstreamSizeInBytes},
      lock_bitstream.outputTimeStamp,
      lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR,
      encoder_state.rfi_needs_confirmation,
    };

    if (encoder_state.rfi_needs_confirmation) {
      // Invalidation request has been fulfilled, and video network packet will be marked as such
      encoder_state.rfi_needs_confirmation = false;
    }

    encoder_state.last_encoded_frame_index = frame_index;

    if (encoded_frame.idr) {
      BOOST_LOG(debug) << "NvEnc: idr frame " << encoded_frame.frame_index;
    }

    if (nvenc_failed(nvenc->nvEncUnlockBitstream(encoder, lock_bitstream.outputBitstream))) {
      BOOST_LOG(error) << "NvEnc: NvEncUnlockBitstream() failed: " << last_nvenc_error_string;
    }

    encoder_state.frame_size_logger.collect_and_log(encoded_frame.data.size() / 1000.);

    return encoded_frame;
  }

  bool nvenc_base::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    if (!encoder || !encoder_params.rfi) {
      return false;
    }

    if (first_frame >= encoder_state.last_rfi_range.first &&
        last_frame <= encoder_state.last_rfi_range.second) {
      BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " already done";
      return true;
    }

    encoder_state.rfi_needs_confirmation = true;

    if (last_frame < first_frame) {
      BOOST_LOG(error) << "NvEnc: invaid rfi request " << first_frame << "-" << last_frame << ", generating IDR";
      return false;
    }

    BOOST_LOG(debug) << "NvEnc: rfi request " << first_frame << "-" << last_frame << " expanding to last encoded frame " << encoder_state.last_encoded_frame_index;
    last_frame = encoder_state.last_encoded_frame_index;

    encoder_state.last_rfi_range = {first_frame, last_frame};

    if (last_frame - first_frame + 1 >= encoder_params.ref_frames_in_dpb) {
      BOOST_LOG(debug) << "NvEnc: rfi request too large, generating IDR";
      return false;
    }

    for (auto i = first_frame; i <= last_frame; i++) {
      if (nvenc_failed(nvenc->nvEncInvalidateRefFrames(encoder, i))) {
        BOOST_LOG(error) << "NvEnc: NvEncInvalidateRefFrames() " << i << " failed: " << last_nvenc_error_string;
        return false;
      }
    }

    return true;
  }

  bool nvenc_base::nvenc_failed(NVENCSTATUS status) {
    last_nvenc_status = status;
    auto status_string = [](NVENCSTATUS status) -> std::string {
      switch (status) {
#define nvenc_status_case(x) \
  case x: \
    return #x;
        nvenc_status_case(NV_ENC_SUCCESS);
        nvenc_status_case(NV_ENC_ERR_NO_ENCODE_DEVICE);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_DEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_ENCODERDEVICE);
        nvenc_status_case(NV_ENC_ERR_INVALID_DEVICE);
        nvenc_status_case(NV_ENC_ERR_DEVICE_NOT_EXIST);
        nvenc_status_case(NV_ENC_ERR_INVALID_PTR);
        nvenc_status_case(NV_ENC_ERR_INVALID_EVENT);
        nvenc_status_case(NV_ENC_ERR_INVALID_PARAM);
        nvenc_status_case(NV_ENC_ERR_INVALID_CALL);
        nvenc_status_case(NV_ENC_ERR_OUT_OF_MEMORY);
        nvenc_status_case(NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
        nvenc_status_case(NV_ENC_ERR_UNSUPPORTED_PARAM);
        nvenc_status_case(NV_ENC_ERR_LOCK_BUSY);
        nvenc_status_case(NV_ENC_ERR_NOT_ENOUGH_BUFFER);
        nvenc_status_case(NV_ENC_ERR_INVALID_VERSION);
        nvenc_status_case(NV_ENC_ERR_MAP_FAILED);
        nvenc_status_case(NV_ENC_ERR_NEED_MORE_INPUT);
        nvenc_status_case(NV_ENC_ERR_ENCODER_BUSY);
        nvenc_status_case(NV_ENC_ERR_EVENT_NOT_REGISTERD);
        nvenc_status_case(NV_ENC_ERR_GENERIC);
        nvenc_status_case(NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
        nvenc_status_case(NV_ENC_ERR_UNIMPLEMENTED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_REGISTER_FAILED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_REGISTERED);
        nvenc_status_case(NV_ENC_ERR_RESOURCE_NOT_MAPPED);
#ifdef NV_ENC_ERR_NEED_MORE_OUTPUT
        nvenc_status_case(NV_ENC_ERR_NEED_MORE_OUTPUT);
#endif
        // Newer versions of sdk may add more constants, look for them at the end of NVENCSTATUS enum
#undef nvenc_status_case
        default:
          return std::to_string(status);
      }
    };

    last_nvenc_error_string.clear();
    if (status != NV_ENC_SUCCESS) {
      /* This API function gives broken strings more often than not
      if (nvenc && encoder) {
        last_nvenc_error_string = nvenc->nvEncGetLastErrorString(encoder);
        if (!last_nvenc_error_string.empty()) last_nvenc_error_string += " ";
      }
      */
      last_nvenc_error_string += status_string(status);
      return true;
    }

    return false;
  }
}  // namespace nvenc
