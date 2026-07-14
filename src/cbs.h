/**
 * @file src/cbs.h
 * @brief Declarations for FFmpeg Coded Bitstream API.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>

// local includes
#include "utility.h"

struct AVPacket;
struct AVCodecContext;

namespace cbs {

  struct nal_t {
    util::buffer_t<std::uint8_t> _new;
    util::buffer_t<std::uint8_t> old;
  };

  struct hevc_t {
    nal_t vps;
    nal_t sps;
  };

  struct h264_t {
    nal_t sps;
  };

  struct vui_parameters_t {
    bool full_range = false;
    int colour_primaries = 0;
    int transfer_characteristics = 0;
    int matrix_coefficients = 0;
    int bit_depth = 0;  // 0 leaves bit depth unchecked.
    int profile = -1;  // Negative leaves codec profile unchecked.
  };

  hevc_t make_sps_hevc(const AVCodecContext *ctx, const AVPacket *packet);
  h264_t make_sps_h264(const AVCodecContext *ctx, const AVPacket *packet);

  /**
   * @brief Validates the Sequence Parameter Set (SPS) of a given packet.
   * @param packet The packet to validate.
   * @param codec_id The ID of the codec used (either AV_CODEC_ID_H264 or AV_CODEC_ID_H265).
   * @return True if the SPS->VUI is present in the active SPS of the packet, false otherwise.
   */
  bool validate_sps(const AVPacket *packet, int codec_id);

  /**
   * @brief Validates a native encoder's sequence header and colour contract.
   * @param data Encoded packet bytes. The bytes are borrowed only for this call.
   * @param size Number of encoded bytes.
   * @param codec_id The ID of the codec (H.264, HEVC, or AV1).
   * @param expected Expected profile, bit depth, range, and H.273 colour values.
   * @return True if the active sequence header matches the complete contract.
   */
  bool validate_sequence_header(
    const std::uint8_t *data,
    std::size_t size,
    int codec_id,
    const vui_parameters_t &expected);
}  // namespace cbs
