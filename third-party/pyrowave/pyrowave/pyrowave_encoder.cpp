// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#include "pyrowave_encoder.hpp"
#include "device.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "math.hpp"
#include "pyrowave_common.hpp"
#include <algorithm>
#include <cmath>

namespace PyroWave
{
using namespace Granite;
using namespace Vulkan;

static constexpr int BlockSpaceSubdivision = 16;
static constexpr int NumRDOBuckets = 128;
static constexpr int RDOBucketOffset = 64;

static int compute_block_count_per_subdivision(int num_blocks)
{
	int per_subdivision = align(num_blocks, BlockSpaceSubdivision) / BlockSpaceSubdivision;
	per_subdivision = int(Util::next_pow2(per_subdivision));
	return per_subdivision;
}

struct QuantizerPushData
{
	ivec2 resolution;
	ivec2 resolution_8x8_blocks;
	vec2 inv_resolution;
	float input_layer;
	float quant_resolution;
	int32_t block_offset;
	int32_t block_stride;
	float rdo_distortion_scale;
};

struct BlockPackingPushData
{
	ivec2 resolution;
	ivec2 resolution_32x32_blocks;
	ivec2 resolution_8x8_blocks;
	uint32_t quant_resolution_code;
	uint32_t sequence_count;
	uint32_t block_offset_32x32;
	uint32_t block_stride_32x32;
	uint32_t block_offset_8x8;
	uint32_t block_stride_8x8;
};

struct AnalyzeRateControlPushData
{
	ivec2 resolution;
	ivec2 resolution_8x8_blocks;
	int32_t block_offset_8x8;
	int32_t block_stride_8x8;
	int32_t block_offset_32x32;
	int32_t block_stride_32x32;
	uint32_t total_wg_count;
	uint32_t num_blocks_aligned;
	uint32_t block_index_shamt;
};

struct RDOperation
{
	int32_t quant;
	uint16_t block_offset;
	uint16_t block_saving;
};

struct Encoder::Impl final : public WaveletBuffers
{
	BufferHandle bucket_buffer, meta_buffer, block_stat_buffer, payload_data, quant_buffer;

	bool encode(CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers);
	bool encode_pre_transformed(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
	bool encode_quant_and_coding(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);

	bool dwt(CommandBuffer &cmd, const ViewBuffers &views);
	bool quant(CommandBuffer &cmd, float quant_scale);
	bool analyze_rdo(CommandBuffer &cmd);
	bool resolve_rdo(CommandBuffer &cmd, size_t target_payload_size);
	bool block_packing(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);

	float get_noise_power_normalized_quant_resolution(int level, int component, int band) const;
	float get_quant_resolution(int level, int component, int band) const;
	float get_quant_rdo_distortion_scale(int level, int component, int band) const;

	void init_block_meta() override;

	size_t compute_num_packets(const void *meta, size_t packet_boundary, size_t padding_space) const;
	size_t compute_num_critical_packets(int bands, const void *meta, size_t packet_boundary, size_t padding_space) const;

	size_t packetize(Packet *packets, size_t packet_boundary, size_t padding_space,
	                 void *bitstream, size_t size,
	                 const void *mapped_meta, const void *mapped_bitstream) const;

	void report_stats(const void *mapped_meta, const void *mapped_bitstream) const;
	void analyze_alternative_packing(const void *mapped_meta, const void *mapped_bitstream) const;

	bool validate_bitstream(const uint32_t *bitstream_u32, const BitstreamPacket *meta, uint32_t block_index) const;

	size_t get_num_active_blocks(int bands) const;
	void compute_block_active_words(int bands, uint32_t *words, size_t word_count, const void *mapped_meta) const;

	uint32_t sequence_count = 0;
};

float Encoder::Impl::get_quant_rdo_distortion_scale(int level, int component, int band) const
{
	// From my Linelet master thesis. Copy paste 11 years later, ah yes :D
	float horiz_midpoint = (band & 1) ? 0.75f : 0.25f;
	float vert_midpoint = (band & 2) ? 0.75f : 0.25f;

	// Normal PC monitors.
	constexpr float dpi = 96.0f;
	// Compromise between couch gaming and desktop.
	constexpr float viewing_distance = 1.0f;
	constexpr float cpd_nyquist = 0.34f * viewing_distance * dpi;

	float cpd = std::sqrt(horiz_midpoint * horiz_midpoint + vert_midpoint * vert_midpoint) *
	                  cpd_nyquist * std::exp2(-float(level));

	// Don't allow a situation where we're quantizing LL band hard.
	cpd = std::max(cpd, 8.0f);

	float csf = 2.6f * (0.0192f + 0.114f * cpd) * std::exp(-std::pow(0.114f * cpd, 1.1f));

	// Heavily discount chroma quality.
	if (component != 0 && level != DecompositionLevels - 1)
	{
		// Consider chroma a little more important if we're not subsampling.
		if (chroma == ChromaSubsampling::Chroma420)
			csf *= 0.6f;
	}

	// Due to filtering, distortion in lower bands will result in more noise power.
	// By scaling the distortion by this factor, we ensure uniform results.
	float resolution = get_noise_power_normalized_quant_resolution(level, component, band);
	float weighted_resolution = csf * resolution;

	// The distortion is scaled in terms of power, not amplitude.
	return weighted_resolution * weighted_resolution;
}

float Encoder::Impl::get_quant_resolution(int level, int component, int band) const
{
	// FP16 range is limited, and this is more than a good enough initial estimate.
	return std::min<float>(
			Configuration::get().get_precision() >= 1 ? 4096.0f : 512.0f,
			get_noise_power_normalized_quant_resolution(level, component, band));
}

float Encoder::Impl::get_noise_power_normalized_quant_resolution(int level, int component, int band) const
{
	// The initial quantization resolution aims for a flat spectrum with noise power normalization.
	// The low-pass gain for CDF 9/7 is 6 dB (1 bit). Every decomposition level subtracts 6 dB.

	// Maybe make this based on the max rate to have a decent initial estimate.
	int bits = Configuration::get().get_precision() >= 1 ? 8 : 6;

	if (band == 0)
		bits += 2;
	else if (band < 3)
		bits += 1;

	bits += level;

	// Chroma starts at level 1, subtract one bit.
	if (component != 0)
		bits--;

	return float(1 << bits);
}

void Encoder::Impl::init_block_meta()
{
	WaveletBuffers::init_block_meta();

	BufferCreateInfo info;
	info.domain = BufferDomain::Device;
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

	info.size = block_count_8x8 * sizeof(BlockStats);
	block_stat_buffer = device->create_buffer(info);
	device->set_name(*block_stat_buffer, "block-stat-buffer");

	info.size = block_count_8x8 * sizeof(BlockMeta);
	meta_buffer = device->create_buffer(info);
	device->set_name(*meta_buffer, "meta-buffer");

	// Worst case estimate. 4:4:4 carries 3 samples per pixel against 1.5 for
	// 4:2:0, so it needs twice the headroom or busy content overruns this
	// buffer on the GPU (found and validated by Punktfunk).
	info.size = VkDeviceSize(aligned_width) * aligned_height *
	            (chroma == ChromaSubsampling::Chroma444 ? 4 : 2);
	payload_data = device->create_buffer(info);
	device->set_name(*payload_data, "payload-data");

	info.size = block_count_32x32 * sizeof(uint32_t);
	quant_buffer = device->create_buffer(info);
	device->set_name(*quant_buffer, "quant-buffer");

	info.size = RDOBucketOffset;
	info.size += NumRDOBuckets * BlockSpaceSubdivision * sizeof(uint32_t);
	info.size += NumRDOBuckets * compute_block_count_per_subdivision(block_count_32x32) *
	             BlockSpaceSubdivision * sizeof(RDOperation);
	bucket_buffer = device->create_buffer(info);
	device->set_name(*bucket_buffer, "bucket-buffer");
}

bool Encoder::Impl::block_packing(CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
	cmd.begin_region("DWT block packing");
	auto start_packing = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	cmd.set_program(shaders.block_packing);
	cmd.set_storage_buffer(0, 0, *buffers.bitstream.buffer, buffers.bitstream.offset, buffers.bitstream.size);
	cmd.set_storage_buffer(0, 1, *buffers.meta.buffer, buffers.meta.offset, buffers.meta.size);
	cmd.set_storage_buffer(0, 2, *meta_buffer);
	cmd.set_storage_buffer(0, 3, *payload_data);
	cmd.set_storage_buffer(0, 4, *block_stat_buffer);
	cmd.set_storage_buffer(0, 5, *quant_buffer);

	if (device->supports_subgroup_size_log2(true, 4, 6))
	{
		cmd.set_subgroup_size_log2(true, 4, 6);
	}
	else
	{
		LOGI("No compatible subgroup size config.\n");
		return false;
	}

	for (int level = 0; level < DecompositionLevels; level++)
	{
		auto level_width = wavelet_img_high_res->get_width(level);
		auto level_height = wavelet_img_high_res->get_height(level);

		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && chroma == ChromaSubsampling::Chroma420)
				continue;

			char label[128];
			snprintf(label, sizeof(label), "level %d, component %d", level, component);
			cmd.begin_region(label);

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
			{
				BlockPackingPushData packing_push = {};
				packing_push.resolution = ivec2(level_width, level_height);
				packing_push.resolution_32x32_blocks = ivec2((level_width + 31) / 32, (level_height + 31) / 32);
				packing_push.resolution_8x8_blocks = ivec2((level_width + 7) / 8, (level_height + 7) / 8);

				auto quant_res = quant_scale < 0.0f ? get_quant_resolution(level, component, band) : quant_scale;
				packing_push.quant_resolution_code = encode_quant(1.0f / quant_res);
				packing_push.sequence_count = sequence_count;

				auto &meta = block_meta[component][level][band];

				packing_push.block_offset_32x32 = meta.block_offset_32x32;
				packing_push.block_stride_32x32 = meta.block_stride_32x32;
				packing_push.block_offset_8x8 = meta.block_offset_8x8;
				packing_push.block_stride_8x8 = meta.block_stride_8x8;
				cmd.push_constants(&packing_push, 0, sizeof(packing_push));

				cmd.dispatch((packing_push.resolution_32x32_blocks.x + 1) / 2,
				             (packing_push.resolution_32x32_blocks.y + 1) / 2,
				             1);
			}

			cmd.end_region();
		}
	}

	auto end_packing = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

	device->register_time_interval("GPU", std::move(start_packing), std::move(end_packing), "Packing");
	cmd.end_region();

	return true;
}

bool Encoder::Impl::resolve_rdo(CommandBuffer &cmd, size_t target_payload_size)
{
	cmd.begin_region("DWT resolve");

	auto start_resolve = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	if (target_payload_size >= sizeof(BitstreamSequenceHeader))
		target_payload_size -= sizeof(BitstreamSequenceHeader);

	cmd.set_specialization_constant_mask(1);

	if (device->supports_subgroup_size_log2(true, 6, 6))
	{
		cmd.set_specialization_constant(0, 64);
		cmd.set_subgroup_size_log2(true, 6, 6);
	}
	else if (device->supports_subgroup_size_log2(true, 4, 4))
	{
		cmd.set_specialization_constant(0, 16);
		cmd.set_subgroup_size_log2(true, 4, 4);
	}
	else if (device->supports_subgroup_size_log2(true, 5, 5))
	{
		cmd.set_specialization_constant(0, 32);
		cmd.set_subgroup_size_log2(true, 5, 5);
	}
	else
	{
		LOGI("No compatible subgroup size config.\n");
		return false;
	}

	cmd.set_program(shaders.resolve_rate_control);

	struct
	{
		uint32_t target_payload_size;
		uint32_t num_blocks_per_subdivision;
	} push = {};

	push.target_payload_size = target_payload_size / sizeof(uint32_t);
	push.num_blocks_per_subdivision = compute_block_count_per_subdivision(block_count_32x32);
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.set_storage_buffer(0, 0, *bucket_buffer);
	cmd.set_storage_buffer(0, 1, *quant_buffer);
	cmd.dispatch(NumRDOBuckets * BlockSpaceSubdivision, 1, 1);

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	cmd.end_region();

	auto end_resolve = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	device->register_time_interval("GPU", std::move(start_resolve), std::move(end_resolve), "Resolve");
	cmd.set_specialization_constant_mask(0);
	return true;
}

bool Encoder::Impl::analyze_rdo(CommandBuffer &cmd)
{
	auto start_analyze = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	cmd.begin_region("DWT analyze");
	cmd.set_program(shaders.analyze_rate_control);

	if (device->supports_subgroup_size_log2(true, 4, 6))
	{
		cmd.set_subgroup_size_log2(true, 4, 6);
	}
	else
	{
		LOGI("No compatible subgroup size config.\n");
		return false;
	}

	// Quantize
	for (int level = 0; level < DecompositionLevels; level++)
	{
		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && chroma == ChromaSubsampling::Chroma420)
				continue;

			AnalyzeRateControlPushData push = {};

			char label[128];
			snprintf(label, sizeof(label), "level %d, component %d", level, component);
			cmd.begin_region(label);

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
			{
				auto level_width = wavelet_img_high_res->get_width(level);
				auto level_height  = wavelet_img_high_res->get_height(level);

				push.resolution.x = level_width;
				push.resolution.y = level_height;
				push.resolution_8x8_blocks.x = (level_width + 7) / 8;
				push.resolution_8x8_blocks.y = (level_height + 7) / 8;
				push.block_offset_8x8 = block_meta[component][level][band].block_offset_8x8;
				push.block_stride_8x8 = block_meta[component][level][band].block_stride_8x8;
				push.block_offset_32x32 = block_meta[component][level][band].block_offset_32x32;
				push.block_stride_32x32 = block_meta[component][level][band].block_stride_32x32;
				push.total_wg_count = block_count_32x32;
				push.num_blocks_aligned = compute_block_count_per_subdivision(block_count_32x32) * BlockSpaceSubdivision;
				push.block_index_shamt = Util::floor_log2(compute_block_count_per_subdivision(block_count_32x32));

				cmd.push_constants(&push, 0, sizeof(push));

				cmd.set_storage_buffer(0, 0, *bucket_buffer);
				cmd.set_storage_buffer(0, 1, *block_stat_buffer);

				cmd.dispatch((level_width + 31) / 32, (level_height + 31) / 32, 1);
			}

			cmd.end_region();
		}
	}

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

	cmd.set_program(shaders.analyze_rate_control_finalize);
	cmd.dispatch(1, 1, 1);

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

	cmd.end_region();
	auto end_analyze = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	device->register_time_interval("GPU", std::move(start_analyze), std::move(end_analyze), "Analyze");
	return true;
}

bool Encoder::Impl::quant(CommandBuffer &cmd, float quant_scale)
{
	auto start_quant = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	cmd.begin_region("DWT quantize");
	cmd.set_program(shaders.wavelet_quant);

	cmd.set_specialization_constant_mask(0);
	if (device->supports_subgroup_size_log2(true, 3, 7))
	{
		cmd.set_subgroup_size_log2(true, 3, 7);
	}
	else
	{
		LOGI("No compatible subgroup size config.\n");
		return false;
	}

	// Quantize
	for (int level = 0; level < DecompositionLevels; level++)
	{
		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && chroma == ChromaSubsampling::Chroma420)
				continue;

			QuantizerPushData push = {};

			char label[128];
			snprintf(label, sizeof(label), "DWT quant, level %d, component %d", level, component);
			cmd.begin_region(label);

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
			{
				float quant_res = quant_scale < 0.0f ? get_quant_resolution(level, component, band) : quant_scale;

				push.resolution.x = wavelet_img_high_res->get_width(level);
				push.resolution.y = wavelet_img_high_res->get_height(level);
				push.resolution_8x8_blocks.x = (push.resolution.x + 7) / 8;
				push.resolution_8x8_blocks.y = (push.resolution.y + 7) / 8;
				push.inv_resolution.x = 1.0f / float(push.resolution.x);
				push.inv_resolution.y = 1.0f / float(push.resolution.y);
				push.input_layer = float(band);
				push.quant_resolution = 1.0f / decode_quant(encode_quant(1.0f / quant_res));
				push.rdo_distortion_scale = get_quant_rdo_distortion_scale(level, component, band) * (1.0f / 256.0f);

				int blocks_x = (push.resolution.x + 31) / 32;
				int blocks_y = (push.resolution.y + 31) / 32;

				push.block_offset = block_meta[component][level][band].block_offset_8x8;
				push.block_stride = block_meta[component][level][band].block_stride_8x8;

				cmd.push_constants(&push, 0, sizeof(push));

				cmd.set_texture(0, 0, *component_layer_views[component][level], *border_sampler);
				cmd.set_storage_buffer(0, 1, *meta_buffer);
				cmd.set_storage_buffer(0, 2, *block_stat_buffer);
				cmd.set_storage_buffer(0, 3, *payload_data);

				cmd.dispatch(blocks_x, blocks_y, 1);
			}

			cmd.end_region();
		}
	}

	cmd.end_region();
	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

	auto end_quant = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	device->register_time_interval("GPU", std::move(start_quant), std::move(end_quant), "Quant");
	return true;
}

bool Encoder::Impl::dwt(CommandBuffer &cmd, const ViewBuffers &views)
{
	struct Push
	{
		uvec2 resolution;
		vec2 inv_resolution;
		uvec2 aligned_resolution;
	} push = {};

	// Forward transforms.
	cmd.set_program(shaders.dwt[Configuration::get().get_precision()]);

	// Only need simple 2-lane swaps.
	cmd.set_subgroup_size_log2(true, 2, 7);
	cmd.set_specialization_constant_mask(1);
	cmd.set_specialization_constant(0, false);

	auto start_dwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	for (int output_level = 0; output_level < DecompositionLevels; output_level++)
	{
		if (output_level > 0)
		{
			push.resolution = uvec2(component_ll_views[0][output_level - 1]->get_view_width(),
			                        component_ll_views[0][output_level - 1]->get_view_height());
			push.aligned_resolution = push.resolution;
		}
		else
		{
			push.resolution = uvec2(views.planes[0]->get_view_width(), views.planes[0]->get_view_height());
			push.aligned_resolution.x = aligned_width;
			push.aligned_resolution.y = aligned_height;
		}

		push.inv_resolution.x = 1.0f / float(push.resolution.x);
		push.inv_resolution.y = 1.0f / float(push.resolution.y);
		cmd.push_constants(&push, 0, sizeof(push));

		if (output_level == 0)
		{
			cmd.set_specialization_constant(0, true);

			if (chroma == ChromaSubsampling::Chroma444)
			{
				for (int c = 0; c < NumComponents; c++)
				{
					char label[64];
					snprintf(label, sizeof(label), "DWT level 0, component %u", c);
					cmd.begin_region(label);
					cmd.set_texture(0, 0, *views.planes[c], *mirror_repeat_sampler);
					cmd.set_storage_texture(0, 1, *component_layer_views[c][output_level]);
					cmd.dispatch((push.aligned_resolution.x + 31) / 32, (push.aligned_resolution.y + 31) / 32, 1);
					cmd.end_region();
				}
			}
			else
			{
				cmd.set_texture(0, 0, *views.planes[0], *mirror_repeat_sampler);
				cmd.set_storage_texture(0, 1, *component_layer_views[0][output_level]);
				cmd.begin_region("DWT level 0 Y");
				cmd.dispatch((push.aligned_resolution.x + 31) / 32, (push.aligned_resolution.y + 31) / 32, 1);
				cmd.end_region();
			}
		}
		else
		{
			for (int c = 0; c < NumComponents; c++)
			{
				if (chroma == ChromaSubsampling::Chroma420 && c != 0 && output_level == 1)
				{
					push.resolution = uvec2(views.planes[c]->get_view_width(), views.planes[c]->get_view_height());
					push.aligned_resolution.x = aligned_width >> output_level;
					push.aligned_resolution.y = aligned_height >> output_level;
					push.inv_resolution.x = 1.0f / float(push.resolution.x);
					push.inv_resolution.y = 1.0f / float(push.resolution.y);
					cmd.push_constants(&push, 0, sizeof(push));
					cmd.set_texture(0, 0, *views.planes[c], *mirror_repeat_sampler);
					cmd.set_specialization_constant(0, true);
				}
				else
				{
					cmd.set_texture(0, 0, *component_ll_views[c][output_level - 1], *mirror_repeat_sampler);
				}

				cmd.set_storage_texture(0, 1, *component_layer_views[c][output_level]);

				char label[64];
				snprintf(label, sizeof(label), "DWT level %u, component %u", output_level, c);
				cmd.begin_region(label);
				cmd.dispatch((push.aligned_resolution.x + 31) / 32, (push.aligned_resolution.y + 31) / 32, 1);
				cmd.end_region();
			}
		}

		cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

		cmd.set_specialization_constant(0, false);
	}

	auto end_dwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	device->register_time_interval("GPU", std::move(start_dwt), std::move(end_dwt), "DWT");
	cmd.set_specialization_constant_mask(0);
	return true;
}

size_t Encoder::Impl::compute_num_critical_packets(int bands, const void *meta_, size_t packet_boundary, size_t padding_size) const
{
	auto *meta = static_cast<const BitstreamPacket *>(meta_);
	size_t num_packets = 0;
	size_t size_in_packet = 0;

	size_in_packet += sizeof(BitstreamSequenceHeader);

	int block_count = bands >= 0 ? int(get_num_active_blocks(bands)) : block_count_32x32;

	for (int i = 0; i < block_count; i++)
	{
		size_t packet_size = meta[i].num_words * sizeof(uint32_t);
		if (!packet_size)
			continue;

		if (size_in_packet + packet_size + padding_size > packet_boundary)
		{
			size_in_packet = 0;
			padding_size = 0;
			num_packets++;
		}

		size_in_packet += packet_size;
	}

	if (size_in_packet)
		num_packets++;

	return num_packets;
}

size_t Encoder::Impl::compute_num_packets(const void *meta, size_t packet_boundary, size_t padding_size) const
{
	return compute_num_critical_packets(-1, meta, packet_boundary, padding_size);
}

#if 0
static int max_magnitude(const int (&values)[64][64], int off_x, int off_y, int w, int h)
{
	int max_magnitude = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			max_magnitude = std::max<int>(max_magnitude, std::abs(values[off_y + y][off_x + x]));
	return max_magnitude;
}

static int num_significant_values(const int (&values)[64][64], int off_x, int off_y, int w, int h)
{
	int num_significant = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (values[off_y + y][off_x + x] != 0)
				num_significant++;
	return num_significant;
}

static bool has_significant_value(const int (&values)[64][64], int off_x, int off_y, int w, int h)
{
	return max_magnitude(values, off_x, off_y, w, h) != 0;
}

template <int PacketBlockWidth, int PacketBlockHeight, int SubBlockWidth, int SubBlockHeight>
static int analyze_cost(const int (&values)[64][64])
{
	int cost = 0;

	for (int y = 0; y < 64; y += PacketBlockHeight)
		for (int x = 0; x < 64; x += PacketBlockWidth)
			if (has_significant_value(values, x, y, PacketBlockWidth, PacketBlockHeight))
				cost += 8;

	constexpr int BlockWidth = PacketBlockWidth / 4;
	constexpr int BlockHeight = PacketBlockHeight / 4;

	for (int y = 0; y < 64; y += BlockHeight)
	{
		for (int x = 0; x < 64; x += BlockWidth)
		{
			int mag = max_magnitude(values, x, y, BlockWidth, BlockHeight);

			constexpr int NumSubBlocksX = BlockWidth / SubBlockWidth;
			constexpr int NumSubBlocksY = BlockHeight / SubBlockHeight;

			if (mag != 0)
			{
				cost += 2 * NumSubBlocksX * NumSubBlocksY / 8; // 2 bits to encode planes.
				cost += 1; // 4 bits to encode Q_bits, 4 bits to encode quant scale per block.
			}
			else
			{
				continue;
			}

			constexpr int MaxDeltaQ = 3;

			uint32_t q_bits = 0;
			{
				uint32_t num_magnitude_bits = 32 - Util::leading_zeroes(mag);
				if (num_magnitude_bits > MaxDeltaQ)
					q_bits = num_magnitude_bits - MaxDeltaQ;
			}

			int weight_bits = 0;

			for (int subblock_y = 0; subblock_y < NumSubBlocksY; subblock_y++)
			{
				for (int subblock_x = 0; subblock_x < NumSubBlocksX; subblock_x++)
				{
					int subblock_mag = max_magnitude(values,
					                                 x + subblock_x * SubBlockWidth,
					                                 y + subblock_y * SubBlockHeight,
					                                 SubBlockWidth, SubBlockHeight);

					uint32_t num_magnitude_bits = subblock_mag != 0 ? (32 - Util::leading_zeroes(subblock_mag)) : 0;
					num_magnitude_bits = std::max(num_magnitude_bits, q_bits);
					weight_bits += SubBlockWidth * SubBlockHeight * num_magnitude_bits;
				}
			}

			int num_sign_bits = num_significant_values(values, x, y, BlockWidth, BlockHeight);
			//cost += 4 * ((weight_bits + num_sign_bits + 31) / 32);
			cost += ((weight_bits + num_sign_bits + 7) / 8);
		}
	}

	return cost;
}

void Encoder::Impl::analyze_alternative_packing(const void *mapped_meta, const void *mapped_bitstream) const
{
	auto *meta = static_cast<const BitstreamPacket *>(mapped_meta);
	auto *bitstream = static_cast<const uint32_t *>(mapped_bitstream);

	int cost_32x32_quad = 0;
	int cost_32x32_horiz = 0;
	int cost_32x32_vert = 0;
	int cost_64x32 = 0;
	int cost_64x64 = 0;

	for (int component = 0; component < NumComponents; component++)
	{
		for (int level = 0; level < DecompositionLevels; level++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0)
				continue;

			auto band_width = wavelet_img->get_width(level);
			auto band_height = wavelet_img->get_height(level);
			int blocks_x_64x64 = (band_width + 63) / 64;
			int blocks_y_64x64 = (band_height + 63) / 64;

			for (int band = 3; band >= (level == DecompositionLevels - 1 ? 0 : 1); band--)
			{
				auto &block_mapping = block_meta[component][level][band];

				for (int block_y = 0; block_y < blocks_y_64x64; block_y++)
				{
					for (int block_x = 0; block_x < blocks_x_64x64; block_x++)
					{
						int block_index = block_mapping.block_offset_64x64 + block_y * block_mapping.block_stride_64x64 + block_x;
						if (meta[block_index].num_words == 0)
							continue;

						int dequant_values[64][64] = {};

						const auto &mapping = block_64x64_to_16x16_mapping[block_index];

						auto *header = reinterpret_cast<const BitstreamHeader *>(bitstream + meta[block_index].offset_u32);
						int blocks_16x16 = int(Util::popcount32(header->ballot));

						auto *control_words = bitstream + meta[block_index].offset_u32 + 2;
						auto *payload_words = control_words + blocks_16x16;

						Util::for_each_bit(header->ballot, [&](unsigned bit) {
							int block_16x16_x = int(bit & 3);
							int block_16x16_y = int(bit >> 2);
							int block_16x16 = mapping.block_offset_16x16 + mapping.block_stride_16x16 * block_16x16_y + block_16x16_x;
							auto &mapping_16x16 = block_meta_16x16[block_16x16];
							auto q_bits = (*control_words >> 16) & 0xf;

							Util::for_each_bit(mapping_16x16.block_mask, [&](unsigned bit_offset) {
								auto num_planes = q_bits + ((*control_words >> bit_offset) & 0x3);
								if (num_planes == 0)
									return;
								num_planes++;

								int subblock_x = int(bit_offset >> 3u) & 1;
								int subblock_y = int(bit_offset >> 1u) & 3;

								int base_x = block_16x16_x * 16 + subblock_x * 8;
								int base_y = block_16x16_y * 16 + subblock_y * 4;

								for (int y = 0; y < 4; y++)
								{
									for (int x = 0; x < 8; x++)
									{
										int swizzled = 0;
										swizzled |= ((x >> 0) & 1) << 0;
										swizzled |= ((y >> 0) & 3) << 1;
										swizzled |= ((x >> 1) & 3) << 3;
										assert(swizzled < 32);

										for (uint32_t plane = 1; plane < num_planes; plane++)
										{
											dequant_values[base_y + y][base_x + x] <<= 1;
											dequant_values[base_y + y][base_x + x] |= int(payload_words[plane] >> swizzled) & 1;
										}

										if ((payload_words[0] & (1u << swizzled)) != 0)
											dequant_values[base_y + y][base_x + x] *= -1;
									}
								}

								payload_words += num_planes;
							});

							control_words++;
						});

						cost_32x32_quad += analyze_cost<32, 32, 2, 2>(dequant_values);
						cost_32x32_horiz += analyze_cost<32, 32, 4, 2>(dequant_values);
						cost_32x32_vert += analyze_cost<32, 32, 2, 4>(dequant_values);
						cost_64x32 += analyze_cost<64, 32, 4, 4>(dequant_values);
						cost_64x64 += analyze_cost<64, 64, 8, 4>(dequant_values);

						auto payload_offset = payload_words - (bitstream + meta[block_index].offset_u32 + meta[block_index].num_words);
						if (payload_offset != 0)
							abort();
					}
				}
			}
		}
	}

	LOGI("32x32 (2x2) cost: %d bytes\n", cost_32x32_quad);
	LOGI("32x32 (4x2) cost: %d bytes\n", cost_32x32_horiz);
	LOGI("32x32 (2x4) cost: %d bytes\n", cost_32x32_vert);
	LOGI("64x32 cost: %d bytes\n", cost_64x32);
	LOGI("64x64 cost: %d bytes\n", cost_64x64);
}

void Encoder::Impl::report_stats(const void *mapped_meta, const void *mapped_bitstream) const
{
	auto *meta = static_cast<const BitstreamPacket *>(mapped_meta);
	auto *bitstream = static_cast<const uint32_t *>(mapped_bitstream);

	int total_pixels = 0;
	int total_words = 0;

	static const char *components[] = { "Y", "Cb", "Cr" };
	static const char *bands[] = { "LL", "HL", "LH", "HH" };

	constexpr int MaxPlanes = 16;
	int plane_histogram[MaxPlanes][256] = {};
	int total_planes[MaxPlanes] = {};

	for (int component = 0; component < NumComponents; component++)
	{
		for (int level = 0; level < DecompositionLevels; level++)
		{
			int total_words_in_level = 0;

			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0)
				continue;

			auto band_width = wavelet_img->get_width(level);
			auto band_height = wavelet_img->get_height(level);
			int blocks_x_64x64 = (band_width + 63) / 64;
			int blocks_y_64x64 = (band_height + 63) / 64;

			for (int band = 3; band >= (level == DecompositionLevels - 1 ? 0 : 1); band--)
			{
				auto &block_mapping = block_meta[component][level][band];

				int words = 0;
				for (int block_y = 0; block_y < blocks_y_64x64; block_y++)
				{
					for (int block_x = 0; block_x < blocks_x_64x64; block_x++)
					{
						int block_index = block_mapping.block_offset_64x64 + block_y * block_mapping.block_stride_64x64 + block_x;
						if (meta[block_index].num_words == 0)
							continue;

						const auto &mapping = block_64x64_to_16x16_mapping[block_index];

						words += meta[block_index].num_words;

						auto *header = reinterpret_cast<const BitstreamHeader *>(bitstream + meta[block_index].offset_u32);
						int blocks_16x16 = int(Util::popcount32(header->ballot));

						auto *control_words = bitstream + meta[block_index].offset_u32 + 2;
						auto *payload_words = control_words + blocks_16x16;

						Util::for_each_bit(header->ballot, [&](unsigned bit) {
							int x = int(bit & 3);
							int y = int(bit >> 2);
							int block_16x16 = mapping.block_offset_16x16 + mapping.block_stride_16x16 * y + x;
							auto &mapping_16x16 = block_meta_16x16[block_16x16];
							auto q_bits = (*control_words >> 16) & 0xf;

							Util::for_each_bit(mapping_16x16.block_mask, [&](unsigned bit_offset) {
								auto num_planes = q_bits + ((*control_words >> bit_offset) & 0x3);
								if (num_planes != 0)
									num_planes++;

								for (uint32_t j = 0; j < num_planes; j++)
								{
									plane_histogram[j][(payload_words[j] >> 0) & 0xff]++;
									plane_histogram[j][(payload_words[j] >> 8) & 0xff]++;
									plane_histogram[j][(payload_words[j] >> 16) & 0xff]++;
									plane_histogram[j][(payload_words[j] >> 24) & 0xff]++;
									total_planes[j] += 4;
								}
								payload_words += num_planes;
							});

							control_words++;
						});

						auto payload_offset = payload_words - (bitstream + meta[block_index].offset_u32 + meta[block_index].num_words);
						if (payload_offset != 0)
							abort();
					}
				}

				int bytes = words * 4;
				double bpp = (bytes * 8.0) / (band_width * band_height);

				LOGI("%s: decomposition level %d, band %s: %.3f bpp\n",
				     components[component], level, bands[band], bpp);

				total_words += words;

				if (component == 0)
					total_pixels += band_width * band_height;

				total_words_in_level += words;
			}

			LOGI("%s: decomposition level %d: %d bytes\n", components[component], level, total_words_in_level * 4);
		}
	}

	double plane_entropy[MaxPlanes] = {};

	for (int i = 0; i < 256; i++)
	{
		for (int j = 0; j < MaxPlanes; j++)
		{
			if (total_planes[j] && plane_histogram[j][i])
			{
				auto p = double(plane_histogram[j][i]) / double(total_planes[j]);
				plane_entropy[j] -= p * log2(p);
			}
		}
	}

	for (int i = 0; i < MaxPlanes; i++)
	{
		LOGI("    Plane %d entropy: %.3f %%\n", i, 100.0 * plane_entropy[i] / 8.0);
		LOGI("    Plane %d bytes: %d\n", i, total_planes[i]);
	}

	LOGI("Overall: %.3f bpp\n", (total_words * 32.0) / total_pixels);

	analyze_alternative_packing(mapped_meta, mapped_bitstream);
}
#endif

void Encoder::Impl::compute_block_active_words(int bands, uint32_t *words, size_t word_count, const void *mapped_meta) const
{
	auto *meta = static_cast<const BitstreamPacket *>(mapped_meta);
	memset(words, 0, sizeof(uint32_t) * word_count);

	size_t num_active_blocks = get_num_active_blocks(bands);
	assert(word_count * 32 >= num_active_blocks);

	for (size_t i = 0; i < num_active_blocks; i++)
		if (meta[i].num_words)
			words[i / 32] |= 1u << (i % 32);
}

size_t Encoder::Impl::get_num_active_blocks(int bands) const
{
	assert(bands < DecompositionLevels);
	if (bands <= 0)
		return 0;

	int last_subband = bands == 1 ? 0 : 3;
	int last_band = bands - 1;

	auto &meta = block_meta[NumComponents - 1][DecompositionLevels - std::max<int>(1, last_band)][last_subband];
	return meta.block_offset_32x32 + meta.block_count_32x32;
}

bool Encoder::Impl::validate_bitstream(
		const uint32_t *bitstream_u32, const BitstreamPacket *meta, uint32_t block_index) const
{
	if (meta[block_index].num_words == 0)
		return true;

	bitstream_u32 += meta[block_index].offset_u32;
	auto *header = reinterpret_cast<const BitstreamHeader *>(bitstream_u32);
	if (header->block_index != block_index)
	{
		LOGI("Mismatch in block index. header: %u, meta: %u\n", header->block_index, block_index);
		return false;
	}

	if (header->payload_words != meta[block_index].num_words)
	{
		LOGI("Mismatch in payload words, header: %u, meta: %u\n", header->payload_words, meta[block_index].num_words);
		return false;
	}

	// 32x32 block layout:
	// N = popcount(ballot)
	// N * u16 control words. 2 bits per active 4x2 block.
	// N * u8 control words. 4 bits Q, 4 bits quant scale.
	// Plane data: M * u8.
	// Tightly packed sign data follows. Depends on number of significant values while decoding plane data.

	int blocks_8x8 = int(Util::popcount32(header->ballot));
	auto *bitstream_u8 = reinterpret_cast<const uint8_t *>(bitstream_u32);
	auto *block_control_words = reinterpret_cast<const uint16_t *>(bitstream_u32 + 2);
	auto *q_control_words = reinterpret_cast<const uint8_t *>(block_control_words + blocks_8x8);
	uint32_t offset = sizeof(BitstreamHeader) + 3 * blocks_8x8;

	if (offset > header->payload_words * 4)
	{
		LOGE("payload_words is not large enough.\n");
		return false;
	}

	const auto &mapping = block_32x32_to_8x8_mapping[header->block_index];
	bool invalid_packet = false;
	int num_significant_values = 0;

	Util::for_each_bit(header->ballot, [&](unsigned bit) {
		int x = int(bit & 3);
		int y = int(bit >> 2);

		if (x < mapping.block_width_8x8 && y < mapping.block_height_8x8)
		{
			auto q_bits = *q_control_words & 0xf;

			for (int subblock_offset = 0; subblock_offset < 16; subblock_offset += 2)
			{
				int num_planes = q_bits + ((*block_control_words >> subblock_offset) & 3);
				int plane_significance = 0;
				for (int plane = 0; plane < num_planes; plane++)
					plane_significance |= bitstream_u8[offset++];
				num_significant_values += int(Util::popcount32(plane_significance));
			}

			block_control_words++;
			q_control_words++;
		}
		else
		{
			LOGE("block_index %u: 8x8 block is out of bounds. (%d, %d) >= (%d, %d)\n",
			     block_index, x, y, mapping.block_width_8x8, mapping.block_height_8x8);
			invalid_packet = true;
		}
	});

	if (invalid_packet)
		return false;

	// We expect this many sign bits to have come through.
	offset += (num_significant_values + 7) / 8;

	auto offset_words = (offset + 3) / 4;

	if (offset_words != header->payload_words)
	{
		LOGE("Block index %u, offset %u != %u\n", block_index, offset_words, header->payload_words);
		return false;
	}

	return true;
}

size_t Encoder::Impl::packetize(Packet *packets, size_t packet_boundary, size_t padding_size,
                                void *output_bitstream_, size_t size,
                                const void *mapped_meta,
                                const void *mapped_bitstream) const
{
	size_t num_packets = 0;
	size_t size_in_packet = 0;
	size_t packet_offset = 0;
	size_t output_offset = 0;
	auto *meta = static_cast<const BitstreamPacket *>(mapped_meta);
	auto *input_bitstream = static_cast<const uint32_t *>(mapped_bitstream);
	auto *output_bitstream = static_cast<uint8_t *>(output_bitstream_);
	(void)size;

	size_t num_non_zero_blocks = 0;
	for (int i = 0; i < block_count_32x32; i++)
		if (meta[i].num_words != 0)
			num_non_zero_blocks++;

	BitstreamSequenceHeader header = {};
	header.width_minus_1 = width - 1;
	header.height_minus_1 = height - 1;
	header.sequence = reinterpret_cast<const BitstreamHeader *>(input_bitstream + meta[0].offset_u32)->sequence;
	header.extended = 1;
	header.code = BITSTREAM_EXTENDED_CODE_START_OF_FRAME;
	header.total_blocks = num_non_zero_blocks;
	header.chroma_resolution = chroma == ChromaSubsampling::Chroma444 ? CHROMA_RESOLUTION_444 : CHROMA_RESOLUTION_420;

	assert(sizeof(header) <= size);
	memcpy(output_bitstream, &header, sizeof(header));
	output_offset += sizeof(header);
	size_in_packet += sizeof(header);

	//for (int i = 0; i < block_count_32x32; i++)
	//	if (!validate_bitstream(input_bitstream, meta, i))
	//		return false;

	for (int i = 0; i < block_count_32x32; i++)
	{
		size_t packet_size = meta[i].num_words * sizeof(uint32_t);
		if (!packet_size)
			continue;

		if (size_in_packet + packet_size + padding_size > packet_boundary)
		{
			packets[num_packets++] = { packet_offset, size_in_packet };
			size_in_packet = 0;
			packet_offset = output_offset;
			padding_size = 0;
		}

		assert(output_offset + packet_size <= size);
		assert(packet_size >= sizeof(BitstreamHeader) / sizeof(uint32_t));

		uint16_t block = reinterpret_cast<const BitstreamHeader *>(input_bitstream + meta[i].offset_u32)->block_index;
		(void)block;
		assert(block == i);

		memcpy(output_bitstream + output_offset, input_bitstream + meta[i].offset_u32, packet_size);

		output_offset += packet_size;
		size_in_packet += packet_size;
	}

	if (size_in_packet)
		packets[num_packets++] = { packet_offset, size_in_packet };

	return num_packets;
}

bool Encoder::Impl::encode_quant_and_coding(
		Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
	cmd.enable_subgroup_size_control(true);

	if (!quant(cmd, quant_scale))
		return false;

	if (!analyze_rdo(cmd))
		return false;

	if (!resolve_rdo(cmd, buffers.target_size))
		return false;

	if (!block_packing(cmd, buffers, quant_scale))
		return false;

	cmd.enable_subgroup_size_control(false);
	return true;
}

bool Encoder::Impl::encode_pre_transformed(
		Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
	cmd.fill_buffer(*payload_data, 0, 0, 2 * sizeof(uint32_t));
	cmd.fill_buffer(*bucket_buffer, 0);
	cmd.fill_buffer(*quant_buffer, 0);

	// Don't need to read the payload offset counter until quantizer.
	cmd.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	return encode_quant_and_coding(cmd, buffers, quant_scale);
}

bool Encoder::Impl::encode(CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers)
{
	sequence_count = (sequence_count + 1) & SequenceCountMask;

	cmd.image_barrier(*wavelet_img_high_res, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	if (wavelet_img_low_res)
	{
		cmd.image_barrier(*wavelet_img_low_res, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	cmd.fill_buffer(*payload_data, 0, 0, 2 * sizeof(uint32_t));
	cmd.fill_buffer(*bucket_buffer, 0);
	cmd.fill_buffer(*quant_buffer, 0);

	cmd.enable_subgroup_size_control(true);
	if (!dwt(cmd, views))
		return false;
	cmd.enable_subgroup_size_control(false);

	// Don't need to read the payload offset counter until quantizer.
	cmd.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	return encode_quant_and_coding(cmd, buffers, -1.0f);
}

Encoder::Encoder()
{
	impl.reset(new Impl);
}

bool Encoder::init(Device *device, int width_, int height_, ChromaSubsampling chroma_)
{
	auto ops = device->get_device_features().vk11_props.subgroupSupportedOperations;
	constexpr VkSubgroupFeatureFlags required_features =
			VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
			VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
			VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
			VK_SUBGROUP_FEATURE_VOTE_BIT |
			VK_SUBGROUP_FEATURE_BALLOT_BIT |
			VK_SUBGROUP_FEATURE_CLUSTERED_BIT |
			VK_SUBGROUP_FEATURE_BASIC_BIT;

	if ((ops & required_features) != required_features)
	{
		LOGE("There are missing subgroup features. Device supports #%x, but requires #%x.\n",
		     ops, required_features);
		return false;
	}

	if (!device->get_device_features().vk12_features.storageBuffer8BitAccess)
		return false;
	if (!device->get_device_features().enabled_features.shaderInt16)
		return false;

	// This should cover any HW I care about.
	if (!device->supports_subgroup_size_log2(true, 4, 4) &&
	    !device->supports_subgroup_size_log2(true, 5, 5) &&
	    !device->supports_subgroup_size_log2(true, 6, 6))
		return false;

	return impl->init(device, width_, height_, chroma_, false);
}

bool Encoder::encode(CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers)
{
	return impl->encode(cmd, views, buffers);
}

const Vulkan::ImageView &Encoder::get_wavelet_band(int component, int level)
{
	return *impl->component_layer_views[component][level];
}

bool Encoder::encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale)
{
	return impl->encode_pre_transformed(cmd, buffers, quant_scale);
}

size_t Encoder::compute_num_packets(const void *meta, size_t packet_boundary, size_t padding_size) const
{
	return impl->compute_num_packets(meta, packet_boundary, padding_size);
}

size_t Encoder::compute_num_critical_packets(int bands, const void *meta, size_t packet_boundary, size_t padding_size) const
{
	return impl->compute_num_critical_packets(bands, meta, packet_boundary, padding_size);
}

size_t Encoder::packetize(Packet *packets, size_t packet_boundary,
                          void *bitstream, size_t size,
                          const void *mapped_meta, const void *mapped_bitstream,
                          size_t padding_size) const
{
	return impl->packetize(packets, packet_boundary, padding_size, bitstream, size, mapped_meta, mapped_bitstream);
}

void Encoder::report_stats(const void *, const void *) const
{
	//impl->report_stats(mapped_meta, mapped_bitstream);
}

uint64_t Encoder::get_meta_required_size() const
{
	return impl->block_count_32x32 * sizeof(BitstreamPacket);
}

size_t Encoder::get_num_active_blocks(int bands) const
{
	return impl->get_num_active_blocks(bands);
}

void Encoder::compute_block_active_words(int bands, uint32_t *words, size_t word_count, const void *mapped_meta) const
{
	impl->compute_block_active_words(bands, words, word_count, mapped_meta);
}

Encoder::~Encoder()
{
}
}
