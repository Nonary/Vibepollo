/**
 * @file src/cbs.cpp
 * @brief Definitions for FFmpeg Coded Bitstream API.
 */
extern "C" {
// lib includes
#include <libavcodec/avcodec.h>
// FFmpeg's internal C AV1 CBS context names its AVClass field `class`, which
// is a C++ keyword. Rename only that token while importing the C layout.
#define class class_
#include <libavcodec/cbs_av1.h>
#undef class
#include <libavcodec/cbs_h264.h>
#include <libavcodec/cbs_h265.h>
#include <libavcodec/h264_levels.h>
#include <libavutil/pixdesc.h>
}

// standard includes
#include <limits>

// local includes
#include "cbs.h"
#include "logging.h"
#include "utility.h"

using namespace std::literals;

namespace cbs {
  void close(CodedBitstreamContext *c) {
    ff_cbs_close(&c);
  }

  using ctx_t = util::safe_ptr<CodedBitstreamContext, close>;

  class frag_t: public CodedBitstreamFragment {
  public:
    frag_t(frag_t &&o) {
      std::copy((std::uint8_t *) &o, (std::uint8_t *) (&o + 1), (std::uint8_t *) this);

      o.data = nullptr;
      o.units = nullptr;
    };

    frag_t() {
      std::fill_n((std::uint8_t *) this, sizeof(*this), 0);
    }

    frag_t &operator=(frag_t &&o) {
      std::copy((std::uint8_t *) &o, (std::uint8_t *) (&o + 1), (std::uint8_t *) this);

      o.data = nullptr;
      o.units = nullptr;

      return *this;
    };

    ~frag_t() {
      if (data || units) {
        ff_cbs_fragment_free(this);
      }
    }
  };

  util::buffer_t<std::uint8_t> write(cbs::ctx_t &cbs_ctx, std::uint8_t nal, void *uh, AVCodecID codec_id) {
    cbs::frag_t frag;
    auto err = ff_cbs_insert_unit_content(&frag, -1, nal, uh, nullptr);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not insert NAL unit SPS: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    err = ff_cbs_write_fragment_data(cbs_ctx.get(), &frag);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not write fragment data: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    // frag.data_size * 8 - frag.data_bit_padding == bits in fragment
    util::buffer_t<std::uint8_t> data {frag.data_size};
    std::copy_n(frag.data, frag.data_size, std::begin(data));

    return data;
  }

  util::buffer_t<std::uint8_t> write(std::uint8_t nal, void *uh, AVCodecID codec_id) {
    cbs::ctx_t cbs_ctx;
    ff_cbs_init(&cbs_ctx, codec_id, nullptr);

    return write(cbs_ctx, nal, uh, codec_id);
  }

  h264_t make_sps_h264(const AVCodecContext *avctx, const AVPacket *packet) {
    cbs::ctx_t ctx;
    if (ff_cbs_init(&ctx, AV_CODEC_ID_H264, nullptr)) {
      return {};
    }

    cbs::frag_t frag;

    int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    auto sps_p = ((CodedBitstreamH264Context *) ctx->priv_data)->active_sps;

    // This is a very large struct that cannot safely be stored on the stack
    auto sps = std::make_unique<H264RawSPS>(*sps_p);

    if (avctx->refs > 0) {
      sps->max_num_ref_frames = avctx->refs;
    }

    sps->vui_parameters_present_flag = 1;

    auto &vui = sps->vui;
    std::memset(&vui, 0, sizeof(vui));

    vui.video_format = 5;
    vui.colour_description_present_flag = 1;
    vui.video_signal_type_present_flag = 1;
    vui.video_full_range_flag = avctx->color_range == AVCOL_RANGE_JPEG;
    vui.colour_primaries = avctx->color_primaries;
    vui.transfer_characteristics = avctx->color_trc;
    vui.matrix_coefficients = avctx->colorspace;

    vui.low_delay_hrd_flag = 1 - vui.fixed_frame_rate_flag;

    vui.bitstream_restriction_flag = 1;
    vui.motion_vectors_over_pic_boundaries_flag = 1;
    vui.log2_max_mv_length_horizontal = 16;
    vui.log2_max_mv_length_vertical = 16;
    vui.max_num_reorder_frames = 0;
    vui.max_dec_frame_buffering = sps->max_num_ref_frames;

    cbs::ctx_t write_ctx;
    ff_cbs_init(&write_ctx, AV_CODEC_ID_H264, nullptr);

    return h264_t {
      write(write_ctx, sps->nal_unit_header.nal_unit_type, (void *) &sps->nal_unit_header, AV_CODEC_ID_H264),
      write(ctx, sps_p->nal_unit_header.nal_unit_type, (void *) &sps_p->nal_unit_header, AV_CODEC_ID_H264)
    };
  }

  hevc_t make_sps_hevc(const AVCodecContext *avctx, const AVPacket *packet) {
    cbs::ctx_t ctx;
    if (ff_cbs_init(&ctx, AV_CODEC_ID_H265, nullptr)) {
      return {};
    }

    cbs::frag_t frag;

    int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    auto vps_p = ((CodedBitstreamH265Context *) ctx->priv_data)->active_vps;
    auto sps_p = ((CodedBitstreamH265Context *) ctx->priv_data)->active_sps;

    // These are very large structs that cannot safely be stored on the stack
    auto sps = std::make_unique<H265RawSPS>(*sps_p);
    auto vps = std::make_unique<H265RawVPS>(*vps_p);

    vps->profile_tier_level.general_profile_compatibility_flag[4] = 1;
    sps->profile_tier_level.general_profile_compatibility_flag[4] = 1;

    auto &vui = sps->vui;
    std::memset(&vui, 0, sizeof(vui));

    sps->vui_parameters_present_flag = 1;

    // skip sample aspect ratio

    vui.video_format = 5;
    vui.colour_description_present_flag = 1;
    vui.video_signal_type_present_flag = 1;
    vui.video_full_range_flag = avctx->color_range == AVCOL_RANGE_JPEG;
    vui.colour_primaries = avctx->color_primaries;
    vui.transfer_characteristics = avctx->color_trc;
    vui.matrix_coefficients = avctx->colorspace;

    vui.vui_timing_info_present_flag = vps->vps_timing_info_present_flag;
    vui.vui_num_units_in_tick = vps->vps_num_units_in_tick;
    vui.vui_time_scale = vps->vps_time_scale;
    vui.vui_poc_proportional_to_timing_flag = vps->vps_poc_proportional_to_timing_flag;
    vui.vui_num_ticks_poc_diff_one_minus1 = vps->vps_num_ticks_poc_diff_one_minus1;
    vui.vui_hrd_parameters_present_flag = 0;

    vui.bitstream_restriction_flag = 1;
    vui.motion_vectors_over_pic_boundaries_flag = 1;
    vui.restricted_ref_pic_lists_flag = 1;
    vui.max_bytes_per_pic_denom = 0;
    vui.max_bits_per_min_cu_denom = 0;
    vui.log2_max_mv_length_horizontal = 15;
    vui.log2_max_mv_length_vertical = 15;

    cbs::ctx_t write_ctx;
    ff_cbs_init(&write_ctx, AV_CODEC_ID_H265, nullptr);

    return hevc_t {
      nal_t {
        write(write_ctx, vps->nal_unit_header.nal_unit_type, (void *) &vps->nal_unit_header, AV_CODEC_ID_H265),
        write(ctx, vps_p->nal_unit_header.nal_unit_type, (void *) &vps_p->nal_unit_header, AV_CODEC_ID_H265),
      },

      nal_t {
        write(write_ctx, sps->nal_unit_header.nal_unit_type, (void *) &sps->nal_unit_header, AV_CODEC_ID_H265),
        write(ctx, sps_p->nal_unit_header.nal_unit_type, (void *) &sps_p->nal_unit_header, AV_CODEC_ID_H265),
      },
    };
  }

  namespace {
    bool validate_sequence_header_impl(
      const AVPacket *packet,
      int codec_id,
      const vui_parameters_t *expected,
      bool allow_unique_inactive_sps) {
      if (!packet || !packet->data || packet->size <= 0 ||
          (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_H265 && codec_id != AV_CODEC_ID_AV1)) {
        return false;
      }

      cbs::ctx_t ctx;
      if (ff_cbs_init(&ctx, (AVCodecID) codec_id, nullptr)) {
        return false;
      }

      cbs::frag_t frag;

      int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
      if (err < 0) {
        char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
        BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

        return false;
      }

      auto vui_matches = [expected](const auto *sps) {
        if (!sps || !sps->vui_parameters_present_flag) return false;
        if (!expected) return true;

        const auto &vui = sps->vui;
        return vui.video_signal_type_present_flag &&
               vui.colour_description_present_flag &&
               static_cast<bool>(vui.video_full_range_flag) == expected->full_range &&
               vui.colour_primaries == expected->colour_primaries &&
               vui.transfer_characteristics == expected->transfer_characteristics &&
               vui.matrix_coefficients == expected->matrix_coefficients;
      };

      // Header-only probes may not contain a slice that activates a parameter
      // set. Accept that form only when it contains exactly one unambiguous
      // SPS; a packet with multiple inactive SPS values cannot prove which
      // stream contract will actually be used.
      auto active_or_unique_sps = [](const auto *active, const auto &parameter_sets) -> decltype(active) {
        if (active) return active;

        decltype(active) unique = nullptr;
        for (const auto *candidate : parameter_sets) {
          if (!candidate) continue;
          if (unique) return nullptr;
          unique = candidate;
        }
        return unique;
      };

      if (codec_id == AV_CODEC_ID_H264) {
        auto h264 = (CodedBitstreamH264Context *) ctx->priv_data;
        const auto *sps = h264 ? h264->active_sps : nullptr;
        if (!sps && h264 && allow_unique_inactive_sps) {
          sps = active_or_unique_sps(h264->active_sps, h264->sps);
        }
        if (!sps || !vui_matches(sps)) return false;
        if (!expected) return true;
        const bool bit_depth_matches = expected->bit_depth <= 0 ||
                                       (8 + sps->bit_depth_luma_minus8 == expected->bit_depth &&
                                        8 + sps->bit_depth_chroma_minus8 == expected->bit_depth);
        const bool profile_matches = expected->profile < 0 || sps->profile_idc == expected->profile;
        return bit_depth_matches && profile_matches;
      }

      if (codec_id == AV_CODEC_ID_H265) {
        auto hevc = (CodedBitstreamH265Context *) ctx->priv_data;
        const auto *sps = hevc ? hevc->active_sps : nullptr;
        if (!sps && hevc && allow_unique_inactive_sps) {
          sps = active_or_unique_sps(hevc->active_sps, hevc->sps);
        }
        if (!sps || !vui_matches(sps)) return false;
        if (!expected) return true;

        const auto &profile_tier_level = sps->profile_tier_level;
        const bool profile_matches = expected->profile < 0 ||
                                     profile_tier_level.general_profile_idc == expected->profile;
        const bool bit_depth_matches = expected->bit_depth <= 0 ||
                                       (8 + sps->bit_depth_luma_minus8 == expected->bit_depth &&
                                        8 + sps->bit_depth_chroma_minus8 == expected->bit_depth);
        return profile_matches && bit_depth_matches;
      }

      auto av1 = (CodedBitstreamAV1Context *) ctx->priv_data;
      const auto *sequence_header = av1 ? av1->sequence_header : nullptr;
      if (!sequence_header) return false;
      if (!expected) return true;

      const auto &color = sequence_header->color_config;
      const int bit_depth = !color.high_bitdepth ? 8 : color.twelve_bit ? 12 : 10;
      return (expected->profile < 0 || sequence_header->seq_profile == expected->profile) &&
             (expected->bit_depth <= 0 || bit_depth == expected->bit_depth) &&
             color.color_description_present_flag &&
             static_cast<bool>(color.color_range) == expected->full_range &&
             color.color_primaries == expected->colour_primaries &&
             color.transfer_characteristics == expected->transfer_characteristics &&
             color.matrix_coefficients == expected->matrix_coefficients;
    }
  }  // namespace

  bool validate_sps(const AVPacket *packet, int codec_id) {
    if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_H265) {
      return false;
    }
    return validate_sequence_header_impl(packet, codec_id, nullptr, false);
  }

  bool validate_sequence_header(
    const std::uint8_t *data,
    std::size_t size,
    int codec_id,
    const vui_parameters_t &expected) {
    if (!data || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return false;
    }

    // A packet without an AVBufferRef makes CBS copy the borrowed bytes into
    // its own padded fragment before parsing, so native encoder vectors do not
    // need FFmpeg's AV_INPUT_BUFFER_PADDING_SIZE tail allocation.
    AVPacket packet {};
    packet.data = const_cast<std::uint8_t *>(data);
    packet.size = static_cast<int>(size);
    return validate_sequence_header_impl(&packet, codec_id, &expected, true);
  }
}  // namespace cbs
