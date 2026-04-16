#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <ffnvcodec/nvEncodeAPI.h>

namespace nvenc::api {

  constexpr uint32_t make_api_version(uint32_t major, uint32_t minor) {
    return major | (minor << 24);
  }

  constexpr uint32_t compiled_api_version = make_api_version(NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

  constexpr uint32_t api_version_major(uint32_t api_version) {
    return api_version & 0xFFu;
  }

  constexpr uint32_t api_version_minor(uint32_t api_version) {
    return (api_version >> 24) & 0xFFu;
  }

  constexpr int compare_api_versions(uint32_t lhs, uint32_t rhs) {
    if (api_version_major(lhs) < api_version_major(rhs)) {
      return -1;
    }

    if (api_version_major(lhs) > api_version_major(rhs)) {
      return 1;
    }

    if (api_version_minor(lhs) < api_version_minor(rhs)) {
      return -1;
    }

    if (api_version_minor(lhs) > api_version_minor(rhs)) {
      return 1;
    }

    return 0;
  }

  constexpr bool api_version_less(uint32_t lhs, uint32_t rhs) {
    return compare_api_versions(lhs, rhs) < 0;
  }

  constexpr bool api_version_less_or_equal(uint32_t lhs, uint32_t rhs) {
    return compare_api_versions(lhs, rhs) <= 0;
  }

  constexpr bool api_version_greater(uint32_t lhs, uint32_t rhs) {
    return compare_api_versions(lhs, rhs) > 0;
  }

  constexpr bool driver_supports_api_version(uint32_t max_driver_api_version, uint32_t api_version) {
    return api_version_less_or_equal(api_version, max_driver_api_version);
  }

  constexpr bool is_reviewed_api_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
      case make_api_version(13U, 0U):
        return true;

      default:
        return false;
    }
  }

  constexpr uint32_t make_struct_version(uint32_t api_version, uint32_t struct_version, bool extended = false) {
    return api_version | (struct_version << 16) | (0x7u << 28) | (extended ? (1u << 31) : 0u);
  }

  inline std::string version_string(uint32_t api_version) {
    return std::to_string(api_version_major(api_version)) + "." + std::to_string(api_version_minor(api_version));
  }

  inline std::vector<uint32_t> filter_to_api_version(std::vector<uint32_t> candidates, uint32_t max_api_version) {
    auto it = std::remove_if(candidates.begin(), candidates.end(), [max_api_version](uint32_t candidate) {
      return api_version_greater(candidate, max_api_version);
    });
    candidates.erase(it, candidates.end());
    return candidates;
  }

  inline std::vector<uint32_t> filter_to_compiled(std::vector<uint32_t> candidates) {
    return filter_to_api_version(std::move(candidates), compiled_api_version);
  }

  inline std::vector<uint32_t> h264_api_candidates() {
    return filter_to_compiled({
      make_api_version(13U, 0U),
      make_api_version(12U, 2U),
      make_api_version(12U, 1U),
      make_api_version(12U, 0U),
      make_api_version(11U, 0U),
    });
  }

  inline std::vector<uint32_t> codec_api_candidates(int video_format) {
    switch (video_format) {
      case 0:
        return h264_api_candidates();

      case 1:
        return h264_api_candidates();

      case 2:
        return filter_to_compiled({
          make_api_version(13U, 0U),
          make_api_version(12U, 2U),
          make_api_version(12U, 1U),
        });

      default:
        return {};
    }
  }

  constexpr bool supports_implicit_split_frame(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
      case make_api_version(13U, 0U):
        return true;

      default:
        return false;
    }
  }

  constexpr bool supports_separate_bit_depth_fields(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
      case make_api_version(13U, 0U):
        return true;

      default:
        return false;
    }
  }

  constexpr uint32_t function_list_version(uint32_t api_version) {
    return make_struct_version(api_version, 2U);
  }

  constexpr uint32_t open_encode_session_ex_params_version(uint32_t api_version) {
    return make_struct_version(api_version, 1U);
  }

  constexpr uint32_t caps_param_version(uint32_t api_version) {
    return make_struct_version(api_version, 1U);
  }

  constexpr uint32_t initialize_params_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
      case make_api_version(12U, 0U):
        return make_struct_version(api_version, 5U, true);

      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 6U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 7U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t config_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
        return make_struct_version(api_version, 7U, true);

      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 8U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 9U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t preset_config_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 4U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 5U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t reconfigure_params_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 1U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 2U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t create_bitstream_buffer_version(uint32_t api_version) {
    return make_struct_version(api_version, 1U);
  }

  constexpr uint32_t event_params_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 2U);

      default:
        return make_struct_version(api_version, 1U);
    }
  }

  constexpr uint32_t map_input_resource_version(uint32_t api_version) {
    return make_struct_version(api_version, 4U);
  }

  constexpr uint32_t pic_params_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
        return make_struct_version(api_version, 4U, true);

      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 6U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 7U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t lock_bitstream_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
        return make_struct_version(api_version, 1U);

      case make_api_version(12U, 0U):
        return make_struct_version(api_version, 2U);

      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 1U, true);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 2U, true);

      default:
        return 0;
    }
  }

  constexpr uint32_t register_resource_version(uint32_t api_version) {
    switch (api_version) {
      case make_api_version(11U, 0U):
        return make_struct_version(api_version, 3U);

      case make_api_version(12U, 0U):
      case make_api_version(12U, 1U):
      case make_api_version(12U, 2U):
        return make_struct_version(api_version, 4U);

      case make_api_version(13U, 0U):
        return make_struct_version(api_version, 5U);

      default:
        return 0;
    }
  }

}  // namespace nvenc::api
