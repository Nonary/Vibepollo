// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#include "pyrowave_decoder.hpp"
#include "device.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "math.hpp"
#include "pyrowave_common.hpp"
#include <algorithm>

namespace PyroWave
{
using namespace Granite;
using namespace Vulkan;

struct DequantizerPushData
{
	ivec2 resolution;
	int32_t output_layer;
	int32_t block_offset_32x32;
	int32_t block_stride_32x32;
};

struct Decoder::Impl final : public WaveletBuffers
{
	BufferHandle dequant_offset_buffer, payload_data;
	BufferViewHandle payload_u32_view, payload_u16_view, payload_u8_view;

	// Turbo-hacky path.
	DeviceAllocationOwnerHandle linear_memory;
	ImageHandle payload_r8_image, payload_r16_image, payload_r32_image;
	bool need_image_transition = true;

	std::vector<uint32_t> dequant_offset_buffer_cpu;
	std::vector<uint32_t> payload_data_cpu;
	int decoded_blocks = 0;
	int total_blocks_in_sequence = 0;
	uint32_t last_seq = UINT32_MAX;
	bool decoded_frame_for_current_sequence = false;

	bool push_packet(const void *data, size_t size);
	bool decode(CommandBuffer &cmd, const ViewBuffers &views);
	bool decode_is_ready(bool allow_partial_frame) const;
	bool decode_is_ready(bool allow_partial_frame, int pristine_bands, float received_ratio,
	                     const uint32_t *active_block_mask, size_t word_count) const;
	bool has_pristine_bands(int bands, const uint32_t *active_block_mask, size_t word_count) const;

	bool decode_packet(const BitstreamHeader *header);

	bool dequant(CommandBuffer &cmd);
	bool idwt(CommandBuffer &cmd, const ViewBuffers &views);
	bool idwt_fragment(CommandBuffer &cmd, const ViewBuffers &views);
	void init_block_meta() override;
	void clear();

	void upload_payload(CommandBuffer &cmd);

	void check_linear_texture_support();
};

Decoder::Decoder()
{
	impl.reset(new Impl);
}

Decoder::~Decoder()
{
}

void Decoder::Impl::upload_payload(CommandBuffer &cmd)
{
	VkDeviceSize required_size = payload_data_cpu.size() * sizeof(uint32_t);

	// Avoid edge case OOB access without robustness on the payload buffer during dequant.
	VkDeviceSize required_size_padded = required_size + 16;

	if (!payload_data || required_size_padded > payload_data->get_create_info().size)
	{
		BufferCreateInfo bufinfo;
		bufinfo.size = std::max<VkDeviceSize>(64 * 1024, required_size_padded * 2);
		bufinfo.usage =
			VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
		bufinfo.domain = BufferDomain::Device;
		payload_data = device->create_buffer(bufinfo);
		device->set_name(*payload_data, "payload-data");

		if (use_readonly_texel_buffer)
		{
			BufferViewCreateInfo view_info = {};
			view_info.buffer = payload_data.get();
			view_info.range = VK_WHOLE_SIZE;

			view_info.format = VK_FORMAT_R8_UINT;
			payload_u8_view = device->create_buffer_view(view_info);
			view_info.format = VK_FORMAT_R16_UINT;
			payload_u16_view = device->create_buffer_view(view_info);
			view_info.format = VK_FORMAT_R32_UINT;
			payload_u32_view = device->create_buffer_view(view_info);
		}

		// This shouldn't happen to demote to texel buffers if we need to deal with massive gigantic payloads.
		payload_r8_image.reset();
		payload_r16_image.reset();
		payload_r32_image.reset();
	}

	if (need_image_transition)
	{
		cmd.begin_barrier_batch();
		const Image *imgs[] = { payload_r8_image.get(), payload_r16_image.get(), payload_r32_image.get() };
		for (auto *img : imgs)
		{
			if (img)
			{
				cmd.image_barrier(*img, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_GENERAL,
				                  0, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
			}
		}
		cmd.end_barrier_batch();
		need_image_transition = false;
	}

	if (!payload_data_cpu.empty())
		memcpy(cmd.update_buffer(*payload_data, 0, required_size), payload_data_cpu.data(), required_size);
}

bool Decoder::Impl::decode_packet(const BitstreamHeader *header)
{
	auto &offset = dequant_offset_buffer_cpu[header->block_index];
	if (offset == UINT32_MAX)
	{
		decoded_blocks++;
		offset = payload_data_cpu.size();
	}
	else
	{
		return true;
	}

	auto *payload_words = reinterpret_cast<const uint32_t *>(header);

	if (sizeof(*header) / sizeof(uint32_t) > header->payload_words)
	{
		LOGE("payload_words is not large enough.\n");
		return false;
	}

	payload_data_cpu.insert(
			payload_data_cpu.end(),
			payload_words,
			payload_words + header->payload_words);
	return true;
}

bool Decoder::Impl::push_packet(const void *data_, size_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);
	while (size >= sizeof(BitstreamHeader))
	{
		auto *header = reinterpret_cast<const BitstreamHeader *>(data);

		if (header->extended != 0)
		{
			auto *seq = reinterpret_cast<const BitstreamSequenceHeader *>(header);

			if (sizeof(*header) > size)
			{
				LOGE("Parsing sequence header, but only %zu bytes left to parse.\n", size);
				return false;
			}

			if (seq->chroma_resolution != int(chroma))
			{
				LOGE("Chroma resolution mismatch!\n");
				return false;
			}

			uint8_t diff = (header->sequence - last_seq) & SequenceCountMask;
			if (last_seq != UINT32_MAX && diff > (SequenceCountMask / 2))
			{
				return true;
			}

			if (last_seq == UINT32_MAX || diff != 0)
			{
				clear();
				last_seq = header->sequence;
			}

			if (seq->code == BITSTREAM_EXTENDED_CODE_START_OF_FRAME)
			{
				if (seq->width_minus_1 + 1 != width || seq->height_minus_1 + 1 != height)
				{
					LOGE("Dimension mismatch in seq packet, (%d, %d) != (%d, %d)\n",
					     seq->width_minus_1 + 1, seq->height_minus_1 + 1, width, height);
					return false;
				}

				total_blocks_in_sequence = int(seq->total_blocks);
			}
			else
			{
				LOGE("Unrecognized sequence header mode %u.\n", seq->code);
				return false;
			}

			data += sizeof(*header);
			size -= sizeof(*header);

			continue;
		}

		size_t packet_size = header->payload_words * sizeof(uint32_t);

		// A block is at least its own header. Without this check a duplicate
		// block_index with payload_words == 0 passes decode_packet()'s duplicate
		// early-out and the cursor never advances, spinning the decoder forever
		// on network input (found by Punktfunk's security review).
		if (header->payload_words < sizeof(*header) / sizeof(uint32_t))
		{
			LOGE("payload_words (%u) is smaller than the block header.\n", unsigned(header->payload_words));
			return false;
		}

		if (packet_size > size)
		{
			LOGE("Packet header states %zu bytes, but only %zu bytes left to parse.\n", packet_size, size);
			return false;
		}

		bool restart;

		if (last_seq == UINT32_MAX)
		{
			restart = true;
		}
		else
		{
			uint8_t diff = (header->sequence - last_seq) & SequenceCountMask;
			if (diff > (SequenceCountMask / 2))
			{
				return true;
			}
			restart = diff != 0;
		}

		if (restart)
		{
			clear();
			last_seq = header->sequence;
		}

		if (header->block_index >= uint32_t(block_count_32x32))
		{
			LOGE("block_index %u is out of bounds (>= %d).\n", header->block_index, block_count_32x32);
			return false;
		}

		if (!decode_packet(header))
			return false;

		data += packet_size;
		size -= packet_size;
	}

	if (size != 0)
	{
		LOGE("Did not consume packet completely.\n");
		return false;
	}

	return true;
}

void Decoder::Impl::init_block_meta()
{
	WaveletBuffers::init_block_meta();

	BufferCreateInfo info;
	info.domain = BufferDomain::Device;
	info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	info.size = block_count_32x32 * sizeof(uint32_t);
	dequant_offset_buffer = device->create_buffer(info);
	device->set_name(*dequant_offset_buffer, "meta-buffer");
	dequant_offset_buffer_cpu.resize(block_count_32x32);

	payload_data_cpu.reserve(1024 * 1024);
}

bool Decoder::Impl::dequant(CommandBuffer &cmd)
{
	DequantizerPushData push = {};

	cmd.set_specialization_constant_mask(0);
	cmd.enable_subgroup_size_control(true);

	if (device->supports_subgroup_size_log2(true, 4, 7))
	{
		cmd.set_subgroup_size_log2(true, 4, 7);
	}
	else if (device->supports_subgroup_size_log2(true, 2, 7))
	{
		cmd.set_subgroup_size_log2(true, 2, 7);
	}
	else
	{
		LOGE("No compatible subgroup size config.\n");
		return false;
	}

	if (payload_r8_image && payload_r16_image && payload_r32_image)
		cmd.set_program(shaders.wavelet_dequant[2]);
	else if (use_readonly_texel_buffer)
		cmd.set_program(shaders.wavelet_dequant[1]);
	else
		cmd.set_program(shaders.wavelet_dequant[0]);

	cmd.begin_region("DWT dequant");
	auto start_dequant = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	cmd.image_barrier(*wavelet_img_high_res, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	if (wavelet_img_low_res)
	{
		cmd.image_barrier(*wavelet_img_low_res, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	// De-quantize
	for (int level = 0; level < DecompositionLevels; level++)
	{
		for (int component = 0; component < NumComponents; component++)
		{
			// Ignore top-level CbCr when doing 420 subsampling.
			if (level == 0 && component != 0 && chroma == ChromaSubsampling::Chroma420)
				continue;

			char label[128];
			snprintf(label, sizeof(label), "level %d - component %d", level, component);
			cmd.begin_region(label);

			for (int band = (level == DecompositionLevels - 1 ? 0 : 1); band < 4; band++)
			{
				push.resolution.x = wavelet_img_high_res->get_width(level);
				push.resolution.y = wavelet_img_high_res->get_height(level);
				push.output_layer = band;
				push.block_offset_32x32 = block_meta[component][level][band].block_offset_32x32;
				push.block_stride_32x32 = block_meta[component][level][band].block_stride_32x32;
				cmd.push_constants(&push, 0, sizeof(push));

				cmd.set_storage_texture(0, 0, *component_layer_views[component][level]);
				cmd.set_storage_buffer(0, 1, *dequant_offset_buffer);

				if (payload_r8_image && payload_r16_image && payload_r32_image)
				{
					cmd.set_texture(0, 2, payload_r32_image->get_view());
					cmd.set_texture(0, 3, payload_r16_image->get_view());
					cmd.set_texture(0, 4, payload_r8_image->get_view());
				}
				else if (use_readonly_texel_buffer)
				{
					cmd.set_buffer_view(0, 2, *payload_u32_view);
					cmd.set_buffer_view(0, 3, *payload_u16_view);
					cmd.set_buffer_view(0, 4, *payload_u8_view);
				}
				else
					cmd.set_storage_buffer(0, 2, *payload_data);

				cmd.dispatch((push.resolution.x + 31) / 32, (push.resolution.y + 31) / 32, 1);
			}

			cmd.end_region();
		}
	}

	cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	            fragment_path ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

	auto end_dequant = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	cmd.end_region();
	cmd.enable_subgroup_size_control(false);
	device->register_time_interval("GPU", std::move(start_dequant), std::move(end_dequant), "Dequant");

	return true;
}

bool Decoder::Impl::idwt_fragment(CommandBuffer &cmd, const ViewBuffers &views)
{
	const auto add_discard = [&](const Vulkan::Image *img) {
		if (img)
		{
			cmd.image_barrier(*img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
			                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		}
	};

	const auto add_read_only = [&](const Vulkan::Image *img) {
		if (img)
		{
			cmd.image_barrier(*img, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
			                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		}
	};

	cmd.begin_barrier_batch();
	for (auto &level : fragment.levels)
	{
		for (auto &vert : level.vert)
			for (auto &comp : vert)
				add_discard(comp.get());
		for (auto &comp : level.horiz)
			add_discard(comp.get());
	}
	cmd.end_barrier_batch();

	auto start_idwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

	struct Push
	{
		float u_offset;
		float v_offset;
		float half_texel_offset_u;
		float half_texel_offset_v;
		float vp_scale;
		uint32_t pivot_size;
	} push = {};

	for (int input_level = DecompositionLevels - 1; input_level >= 0; input_level--)
	{
		int output_level = input_level - 1;

		char label[128];
		if (output_level >= 0)
			snprintf(label, sizeof(label), "Fragment iDWT level %u", output_level);
		else
			snprintf(label, sizeof(label), "Fragment iDWT final");
		cmd.begin_region(label);

		Vulkan::RenderPassInfo rp_info = {};

		bool has_chroma_output = output_level >= 0 || chroma == ChromaSubsampling::Chroma444;

		Vulkan::Program *vert_prog;
		Vulkan::Program *horiz_prog;

		if (has_chroma_output)
		{
			rp_info.store_attachments = 0x3;
			rp_info.num_color_attachments = 2;
			vert_prog = device->request_program(shaders.idwt_vs, shaders.idwt_fs[1]);
			horiz_prog = device->request_program(shaders.idwt_vs, shaders.idwt_fs[2]);
		}
		else
		{
			rp_info.store_attachments = 0x1;
			rp_info.num_color_attachments = 1;
			vert_prog = device->request_program(shaders.idwt_vs, shaders.idwt_fs[0]);
			horiz_prog = vert_prog;
		}

		// Vertical passes.
		for (int vert_pass = 0; vert_pass < 2; vert_pass++)
		{
			rp_info.color_attachments[0] = &fragment.levels[input_level].vert[vert_pass][0]->get_view();
			if (has_chroma_output)
				rp_info.color_attachments[1] = &fragment.levels[input_level].vert[vert_pass][1]->get_view();

			cmd.begin_render_pass(rp_info);
			cmd.set_program(vert_prog);
			cmd.set_opaque_sprite_state();
			cmd.set_specialization_constant_mask(0x1 | 0x8);
			cmd.set_specialization_constant(0, true);

			cmd.set_texture(0, 0, *fragment.levels[input_level].decoded[0][vert_pass + 0]);
			cmd.set_texture(0, 1, *fragment.levels[input_level].decoded[0][vert_pass + 2]);
			cmd.set_sampler(0, 2, *mirror_repeat_sampler);

			if (has_chroma_output)
			{
				cmd.set_texture(0, 3, *fragment.levels[input_level].decoded[1][vert_pass + 0]);
				cmd.set_texture(0, 4, *fragment.levels[input_level].decoded[1][vert_pass + 2]);
				cmd.set_texture(0, 5, *fragment.levels[input_level].decoded[2][vert_pass + 0]);
				cmd.set_texture(0, 6, *fragment.levels[input_level].decoded[2][vert_pass + 2]);
			}

			uint32_t render_width = rp_info.color_attachments[0]->get_view_width();
			uint32_t render_height = rp_info.color_attachments[0]->get_view_height();

			// Set mirror point.
			// Work around broken Mali r38.1 compiler.
			// If it sees negative texture offsets it breaks the output for whatever reason (!?!?!?!).
			auto *input_view = fragment.levels[input_level].decoded[0][0].get();
			push.u_offset = 0.0f;
			push.v_offset = -2.0f / float(input_view->get_view_height());
			push.half_texel_offset_u = 0.5f / float(input_view->get_view_width());
			push.half_texel_offset_v = 0.5f / float(input_view->get_view_height());
			push.vp_scale = cmd.get_viewport().height;
			push.pivot_size = render_height;
			cmd.push_constants(&push, 0, sizeof(push));

			// Render top edge condition.
			cmd.set_specialization_constant(3, -1);
			cmd.set_scissor({{ 0, 0 }, { render_width, 8 }});
			cmd.draw(3);

			// Render normal path
			cmd.set_specialization_constant(3, 0);
			cmd.set_scissor({{ 0, 8 }, { render_width, render_height - 16 }});
			cmd.draw(3);

			// Render bottom edge condition
			cmd.set_specialization_constant(3, +1);
			cmd.set_scissor({{ 0, int(render_height) - 8 }, { render_width, 8 }});
			cmd.draw(3);

			cmd.end_render_pass();
		}

		cmd.begin_barrier_batch();
		for (auto &vert : fragment.levels[input_level].vert)
			for (auto &comp : vert)
				add_read_only(comp.get());
		cmd.end_barrier_batch();

		if (has_chroma_output)
		{
			rp_info.num_color_attachments = 3;
			rp_info.store_attachments = 0x7;
		}
		else
		{
			rp_info.num_color_attachments = 1;
			rp_info.store_attachments = 0x1;
		}

		for (uint32_t comp = 0; comp < rp_info.num_color_attachments; comp++)
		{
			if (output_level < 0 || (output_level == 0 && chroma == ChromaSubsampling::Chroma420 && comp != 0))
				rp_info.color_attachments[comp] = views.planes[comp];
			else
				rp_info.color_attachments[comp] = &fragment.levels[output_level].horiz[comp]->get_view();
		}

		cmd.begin_render_pass(rp_info);
		cmd.set_program(horiz_prog);
		cmd.set_opaque_sprite_state();
		cmd.set_specialization_constant_mask(0xf);
		cmd.set_specialization_constant(0, false);
		cmd.set_specialization_constant(1, output_level < 0);
		cmd.set_specialization_constant(2, output_level < 0 || (output_level == 0 && chroma == ChromaSubsampling::Chroma420));

		cmd.set_texture(0, 0, fragment.levels[input_level].vert[0][0]->get_view());
		cmd.set_texture(0, 1, fragment.levels[input_level].vert[1][0]->get_view());
		cmd.set_sampler(0, 2, *mirror_repeat_sampler);

		if (has_chroma_output)
		{
			cmd.set_texture(0, 3, fragment.levels[input_level].vert[0][1]->get_view());
			cmd.set_texture(0, 4, fragment.levels[input_level].vert[1][1]->get_view());
		}

		uint32_t aligned_render_width = aligned_width >> (output_level + 1);
		uint32_t aligned_render_height = aligned_height >> (output_level + 1);

		// Chroma output might be smaller than Y in output_level == 0 due to not using alignment.
		// This is reflected in the actual render area, which is equal to default viewport.
		auto render_width = uint32_t(cmd.get_viewport().width);
		auto render_height = uint32_t(cmd.get_viewport().height);

		// In case we're rendering to an output texture,
		// the render area might be smaller than we expect for purposes of alignment.
		// Use properly scaled viewport that we scissor away as needed.
		cmd.set_viewport({ 0, 0, float(aligned_render_width), float(aligned_render_height), 0, 1 });

		// Set mirror point.
		auto *input_view = &fragment.levels[input_level].vert[0][0]->get_view();
		push.u_offset = -2.0f / float(input_view->get_view_width());
		push.v_offset = 0.0f;
		push.half_texel_offset_u = 0.5f / float(input_view->get_view_width());
		push.half_texel_offset_v = 0.5f / float(input_view->get_view_height());
		push.vp_scale = cmd.get_viewport().width;
		push.pivot_size = aligned_render_width;
		cmd.push_constants(&push, 0, sizeof(push));

		// Render left edge condition.
		cmd.set_specialization_constant(3, -1);
		cmd.set_scissor({{ 0, 0 }, { 8, render_height }});
		cmd.draw(3);

		// Render normal condition
		cmd.set_specialization_constant(3, 0);
		cmd.set_scissor({{ 8, 0 }, { std::min<uint32_t>(render_width - 8, aligned_render_width - 16), render_height }});
		cmd.draw(3);

		uint32_t aligned_x = aligned_render_width - 8;
		if (aligned_x < render_width)
		{
			// Render right edge condition
			cmd.set_specialization_constant(3, +1);
			cmd.set_scissor({{ int(aligned_x), 0 }, { render_width - aligned_x, render_height }});
			cmd.draw(3);
		}

		cmd.end_render_pass();

		// If chroma is subsampled, we cannot render the fully padded region in one render pass due to
		// rules regarding renderArea. renderArea cannot exceed the smallest image in the render pass.
		// We cannot use subpasses either, so split the render pass, but that's mostly fine,
		// since renderArea is non-overlapping.
		if (output_level == 0 && chroma == ChromaSubsampling::Chroma420)
		{
			rp_info.num_color_attachments = 1;
			rp_info.store_attachments = 0x1;

			VkMemoryBarrier2 by_region = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			by_region.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			by_region.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			by_region.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			by_region.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dep.memoryBarrierCount = 1;
			dep.pMemoryBarriers = &by_region;
			dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			// Need vertical fixup (very common for 1080p).
			if (rp_info.color_attachments[1]->get_view_height() < rp_info.color_attachments[0]->get_view_height())
			{
				// Insert a simple by_region barrier to ensure we follow Vulkan rules for RW access.
				cmd.barrier(dep);

				rp_info.render_area.extent.width = rp_info.color_attachments[0]->get_view_width();
				rp_info.render_area.extent.height =
						rp_info.color_attachments[0]->get_view_height() -
						rp_info.color_attachments[1]->get_view_height();
				rp_info.render_area.offset.x = 0;
				rp_info.render_area.offset.y = rp_info.color_attachments[1]->get_view_height();

				cmd.begin_render_pass(rp_info);
				cmd.set_program(device->request_program(shaders.idwt_vs, shaders.idwt_fs[0]));
				cmd.set_opaque_sprite_state();
				cmd.set_texture(0, 0, fragment.levels[input_level].vert[0][0]->get_view());
				cmd.set_texture(0, 1, fragment.levels[input_level].vert[1][0]->get_view());
				cmd.set_sampler(0, 2, *mirror_repeat_sampler);
				cmd.set_viewport({ 0, 0, float(aligned_render_width), float(aligned_render_height), 0, 1 });
				cmd.set_specialization_constant_mask(0x8);
				cmd.push_constants(&push, 0, sizeof(push));
				cmd.set_specialization_constant(3, 1); // Always consider edge handling.
				cmd.draw(3);
				cmd.end_render_pass();
			}

			// Need horizontal fixup (very rare).
			if (rp_info.color_attachments[1]->get_view_width() < rp_info.color_attachments[0]->get_view_width())
			{
				cmd.barrier(dep);

				rp_info.render_area.extent.width =
						rp_info.color_attachments[0]->get_view_width() -
						rp_info.color_attachments[1]->get_view_width();
				rp_info.render_area.extent.height = rp_info.color_attachments[0]->get_view_height();
				rp_info.render_area.offset.x = rp_info.color_attachments[1]->get_view_width();
				rp_info.render_area.offset.y = 0;

				cmd.begin_render_pass(rp_info);
				cmd.set_program(device->request_program(shaders.idwt_vs, shaders.idwt_fs[0]));
				cmd.set_opaque_sprite_state();
				cmd.set_texture(0, 0, fragment.levels[input_level].vert[0][0]->get_view());
				cmd.set_texture(0, 1, fragment.levels[input_level].vert[1][0]->get_view());
				cmd.set_sampler(0, 2, *mirror_repeat_sampler);
				cmd.set_viewport({ 0, 0, float(aligned_render_width), float(aligned_render_height), 0, 1 });
				cmd.set_specialization_constant_mask(0x8);
				cmd.push_constants(&push, 0, sizeof(push));
				cmd.set_specialization_constant(3, 1); // Always consider edge handling.
				cmd.draw(3);
				cmd.end_render_pass();
			}
		}

		if (output_level >= 0)
		{
			cmd.begin_barrier_batch();
			for (auto &comp: fragment.levels[output_level].horiz)
				add_read_only(comp.get());
			cmd.end_barrier_batch();
		}

		cmd.end_region();
	}

	auto end_idwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	device->register_time_interval("GPU", std::move(start_idwt), std::move(end_idwt), "iDWT fragment");

	cmd.set_specialization_constant_mask(0);

	// Avoid WAR hazard for dequantization.
	cmd.barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);

	return true;
}

bool Decoder::Impl::idwt(CommandBuffer &cmd, const ViewBuffers &views)
{
	cmd.set_program(shaders.idwt[Configuration::get().get_precision()]);
	cmd.enable_subgroup_size_control(false);

	auto start_idwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	struct
	{
		ivec2 resolution;
		vec2 inv_resolution;
	} push = {};

	for (int input_level = DecompositionLevels - 1; input_level >= 0; input_level--)
	{
		// Transposed.
		push.resolution.x = component_layer_views[0][input_level]->get_view_height();
		push.resolution.y = component_layer_views[0][input_level]->get_view_width();
		push.inv_resolution.x = 1.0f / float(push.resolution.x);
		push.inv_resolution.y = 1.0f / float(push.resolution.y);
		cmd.push_constants(&push, 0, sizeof(push));
		cmd.set_specialization_constant_mask(1);
		cmd.set_specialization_constant(0, false);

		if (input_level == 0)
		{
			cmd.set_specialization_constant(0, true);
			if (chroma == ChromaSubsampling::Chroma444)
			{
				for (int c = 0; c < NumComponents; c++)
				{
					char label[64];
					snprintf(label, sizeof(label), "iDWT final, component %u", c);
					cmd.begin_region(label);
					cmd.set_storage_texture(0, 1, *views.planes[c]);
					cmd.set_texture(0, 0, *component_layer_views[c][input_level], *mirror_repeat_sampler);
					cmd.dispatch((push.resolution.x + 15) / 16, (push.resolution.y + 15) / 16, 1);
					cmd.end_region();
				}
			}
			else
			{
				cmd.set_storage_texture(0, 1, *views.planes[0]);
				cmd.begin_region("iDWT final");
				cmd.set_texture(0, 0, *component_layer_views[0][input_level], *mirror_repeat_sampler);
				cmd.dispatch((push.resolution.x + 15) / 16, (push.resolution.y + 15) / 16, 1);
				cmd.end_region();
			}
		}
		else
		{
			for (int c = 0; c < NumComponents; c++)
			{
				cmd.set_texture(0, 0, *component_layer_views[c][input_level], *mirror_repeat_sampler);

				if (chroma == ChromaSubsampling::Chroma420 && c != 0 && input_level == 1)
				{
					cmd.set_storage_texture(0, 1, *views.planes[c]);
					cmd.set_specialization_constant(0, true);
				}
				else
					cmd.set_storage_texture(0, 1, *component_ll_views[c][input_level - 1]);

				char label[64];
				snprintf(label, sizeof(label), "iDWT level %u, component %u", input_level - 1, c);
				cmd.begin_region(label);
				cmd.dispatch((push.resolution.x + 15) / 16, (push.resolution.y + 15) / 16, 1);
				cmd.end_region();
			}
		}

		cmd.set_specialization_constant_mask(0);
		cmd.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}

	auto end_idwt = cmd.write_timestamp(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	device->register_time_interval("GPU", std::move(start_idwt), std::move(end_idwt), "iDWT");
	return true;
}

bool Decoder::Impl::has_pristine_bands(int bands, const uint32_t *active_block_mask, size_t word_count) const
{
	// Account for 4:2:0 where level0 will not have packets.
	// It's somewhat meaningless to ask for pristine bands all the way up to that point though.
	assert(bands < DecompositionLevels);

	// This analysis assumes that there are no "null" blocks present.
	// For the lowest frequency bands, that is vanishingly unlikely to happen,
	// and worst case we get a false positive rejection.
	// TODO: It's possible to improve this by sending the "active block mask"
	// as sideband data from encoder if need be.

	const auto block_is_missing = [&](uint32_t block_index)
	{
		if (dequant_offset_buffer_cpu[block_index] != UINT32_MAX)
			return false;

		uint32_t word_index = block_index / 32;

		if (!active_block_mask || word_index >= word_count)
			return true;

		// If the block wasn't expected to be active anyway, just pass it through.
		return ((active_block_mask[word_index] >> (block_index % 32)) & 1) != 0;
	};

	for (int band = 0; band < bands; band++)
	{
		for (auto &component : block_meta)
		{
			if (band == 0)
			{
				auto &meta = component[DecompositionLevels - 1][0];
				for (int i = 0; i < meta.block_count_32x32; i++)
					if (block_is_missing(meta.block_offset_32x32 + i))
						return false;
			}
			else
			{
				// If we can reconstruct the LH, HL, HH bands, we can generate the higher-resolution LL band.
				for (int high_freq_bands = 1; high_freq_bands < 4; high_freq_bands++)
				{
					auto &meta = component[DecompositionLevels - band][high_freq_bands];
					for (int i = 0; i < meta.block_count_32x32; i++)
						if (block_is_missing(meta.block_offset_32x32 + i))
							return false;
				}
			}
		}
	}

	return true;
}

bool Decoder::Impl::decode_is_ready(bool allow_partial_frame, int pristine_bands, float received_ratio,
                                    const uint32_t *active_block_mask, size_t word_count) const
{
	if (decoded_frame_for_current_sequence)
		return false;

	if (last_seq == UINT32_MAX)
		return false;

	if (decoded_blocks < total_blocks_in_sequence)
	{
		if (!allow_partial_frame)
			return false;

		if (!has_pristine_bands(pristine_bands, active_block_mask, word_count))
			return false;
		if (float(decoded_blocks) <= float(total_blocks_in_sequence) * received_ratio)
			return false;
	}

	return true;
}

bool Decoder::Impl::decode_is_ready(bool allow_partial_frame) const
{
	// At the very least, we want some LL bands to be received properly,
	// otherwise we get extreme artifacts.
	return decode_is_ready(allow_partial_frame, 2, 0.9f, nullptr, 0);
}

bool Decoder::Impl::decode(CommandBuffer &cmd, const ViewBuffers &views)
{
	cmd.begin_region("Decode uploads");
	{
		upload_payload(cmd);

		memcpy(cmd.update_buffer(*dequant_offset_buffer, 0,
		                         dequant_offset_buffer_cpu.size() * sizeof(dequant_offset_buffer_cpu.front())),
		       dequant_offset_buffer_cpu.data(), dequant_offset_buffer_cpu.size() * sizeof(dequant_offset_buffer_cpu.front()));

		cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		            use_readonly_texel_buffer ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	}
	cmd.end_region();

	if (!dequant(cmd))
		return false;

	cmd.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	if (fragment_path)
	{
		if (!idwt_fragment(cmd, views))
			return false;
	}
	else
	{
		if (!idwt(cmd, views))
			return false;
	}

	decoded_frame_for_current_sequence = true;
	return true;
}

void Decoder::Impl::clear()
{
	std::fill(dequant_offset_buffer_cpu.begin(), dequant_offset_buffer_cpu.end(), UINT32_MAX);
	decoded_blocks = 0;
	last_seq = UINT32_MAX;
	decoded_frame_for_current_sequence = false;
	total_blocks_in_sequence = block_count_32x32;
	payload_data_cpu.clear();
}

bool Decoder::device_prefers_fragment_path(Vulkan::Device &device)
{
	switch (device.get_device_features().driver_id)
	{
	// QCOM hardware struggles with compute in general and prefers fragment.
	// Turnip seems to like compute path just fine though ...
	case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:
		return true;

	// Mali heavily favors texture sampling over LS heavy content.
	case VK_DRIVER_ID_ARM_PROPRIETARY:
	case VK_DRIVER_ID_MESA_PANVK:
		return true;

	default:
		return false;
	}
}

void Decoder::Impl::check_linear_texture_support()
{
	if (!use_readonly_texel_buffer)
		return;

	// Texel buffers hit LS path on at least Mali, and most likely they hit slow paths on most mobile IHVs.
	// Try to promote to linear 2D images instead if we can get away with it.
	// Texture sampling performance is what mobile IHVs tend to optimize for.
	const struct
	{
		VkFormat fmt;
		uint32_t width;
		ImageHandle *out_handle;
	} reqs[] = {
		{ VK_FORMAT_R8_UINT, 4096, &payload_r8_image },
		{ VK_FORMAT_R16_UINT, 2048, &payload_r16_image },
		{ VK_FORMAT_R32_UINT, 1024, &payload_r32_image }
	};

	// Just assume this works. Can't imagine any GPU where this wouldn't work tightly packed.

	BufferCreateInfo bufinfo;
	bufinfo.size = 4 * 1024 * 1024;
	bufinfo.usage =
			VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
	bufinfo.domain = BufferDomain::Device;
	bufinfo.allocation_requirements.alignment = 64 * 1024;
	bufinfo.allocation_requirements.size = 4 * 1024 * 1024;
	bufinfo.allocation_requirements.memoryTypeBits = UINT32_MAX;
	payload_data = device->create_buffer(bufinfo);
	device->set_name(*payload_data, "payload-data");

	const auto *alias = &payload_data->get_allocation();

	// Try to force all linear images to alias each other.
	for (auto &req : reqs)
	{
		VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
		if (device->get_image_format_properties(req.fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
		                                        VK_IMAGE_USAGE_SAMPLED_BIT, 0,
		                                        nullptr, &props2))
		{
			if (props2.imageFormatProperties.maxExtent.width >= req.width &&
			    props2.imageFormatProperties.maxExtent.height >= 1024)
			{
				auto info = ImageCreateInfo::immutable_2d_image(req.width, 1024, req.fmt);
				info.domain = ImageDomain::LinearHost;
				info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
				info.num_memory_aliases = 1;
				info.layout = ImageLayout::General;
				info.memory_aliases = &alias;
				*req.out_handle = device->create_image(info);
			}
		}
	}

	if (payload_r8_image && payload_r16_image && payload_r32_image)
		LOGI("Using linear textures instead of texel buffers.\n");
}

bool Decoder::init(Vulkan::Device *device, int width, int height, ChromaSubsampling chroma_, bool fragment_path_)
{
	auto ops = device->get_device_features().vk11_props.subgroupSupportedOperations;
	constexpr VkSubgroupFeatureFlags required_features =
			VK_SUBGROUP_FEATURE_VOTE_BIT |
			VK_SUBGROUP_FEATURE_BALLOT_BIT |
			VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
			VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
			VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT |
			VK_SUBGROUP_FEATURE_BASIC_BIT;

	if ((ops & required_features) != required_features)
	{
		LOGE("There are missing subgroup features. Device supports #%x, but requires #%x.\n",
		     ops, required_features);
		return false;
	}

	// The decoder is more lenient.
	if (!device->supports_subgroup_size_log2(true, 2, 7))
	{
		LOGE("Device doesn't support basic subgroup size control.\n");
		return false;
	}

	if (!impl->init(device, width, height, chroma_, fragment_path_))
	{
		LOGE("Failed to initialize.\n");
		return false;
	}

	if (!device->get_device_features().vk12_features.storageBuffer8BitAccess &&
	    !impl->use_readonly_texel_buffer)
	{
		LOGE("Device doesn't support 8-bit storage or large texel buffers.\n");
		return false;
	}

	impl->check_linear_texture_support();

	clear();
	return true;
}

void Decoder::clear()
{
	impl->clear();
}

bool Decoder::push_packet(const void *data, size_t size)
{
	return impl->push_packet(data, size);
}

bool Decoder::decode(Vulkan::CommandBuffer &cmd, const ViewBuffers &views)
{
	return impl->decode(cmd, views);
}

bool Decoder::decode_is_ready(bool allow_partial_frame) const
{
	return impl->decode_is_ready(allow_partial_frame);
}

bool Decoder::decode_is_ready(bool allow_partial_frame, int num_pristine_bands, float minimum_packet_ratio,
                              const uint32_t *active_block_mask, size_t word_count) const
{
	return impl->decode_is_ready(allow_partial_frame, num_pristine_bands, minimum_packet_ratio, active_block_mask, word_count);
}
}
