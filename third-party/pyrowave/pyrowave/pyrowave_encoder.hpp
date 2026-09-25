// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include "pyrowave_config.hpp"

namespace Vulkan
{
class Device;
class Buffer;
class ImageView;
class CommandBuffer;
}

namespace PyroWave
{
class Encoder
{
public:
	Encoder();
	~Encoder();

	struct BitstreamBuffers
	{
		struct
		{
			const Vulkan::Buffer *buffer;
			uint64_t offset;
			uint64_t size;
		} meta, bitstream;
		size_t target_size;
	};

	bool init(Vulkan::Device *device, int width, int height, ChromaSubsampling chroma);
	bool encode(Vulkan::CommandBuffer &cmd, const ViewBuffers &views, const BitstreamBuffers &buffers);

	// Debug hackery
	const Vulkan::ImageView &get_wavelet_band(int component, int level);
	bool encode_pre_transformed(Vulkan::CommandBuffer &cmd, const BitstreamBuffers &buffers, float quant_scale);
	//

	uint64_t get_meta_required_size() const;

	struct Packet
	{
		size_t offset;
		size_t size;
	};

	// Padding size is used to reserve a certain number of bytes in the first packet for application defined scratch
	// space. It only affects the first split point for packets.
	// bitstream should still point to the start of data that is written by encoder,
	// and size is the valid size of bitstream.
	size_t compute_num_packets(const void *mapped_meta, size_t packet_boundary, size_t padding_size = 0) const;
	size_t packetize(Packet *packets, size_t packet_boundary,
					 void *bitstream, size_t size,
					 const void *mapped_meta, const void *mapped_bitstream,
					 size_t padding_size = 0) const;

	// Debug
	void report_stats(const void *mapped_meta, const void *mapped_bitstream) const;

	// For advanced error correction purposes.
	// If bands = 1, blocks for the lowest resolution band is included.
	// For bands = 2, HL/LH/HH for last decomposition is included to reconstruct next resolution LL band, etc.
	// Returns number of blocks which are possibly included in this range.
	// bands = 2 or 3 is a good default if using the more advanced model.
	// bands = 1 will return some redundant information due to the nature of the bitstream.
	size_t get_num_active_blocks(int bands) const;

	// Size of words should be round_up(get_num_active_blocks(bands) / 32), and word_count should be at least this large.
	// The resulting stream can be sent as side-band data to the decoder if desired.
	// For low values of bands, like bands = 1, it is expected that all bits are set, and it's not necessary to send
	// the side-band data in that case.
	void compute_block_active_words(int bands, uint32_t *words, size_t word_count, const void *mapped_meta) const;

	// When using advanced model for error correction, queries the number of network packets which will contain
	// "critically" encoded data. The first number of returned packets will correspond to these "critical" packets.
	// It may be advantageous to add some FEC to these packets only, rather than error correcting the entire bitstream.
	size_t compute_num_critical_packets(int bands, const void *mapped_meta, size_t packet_boundary, size_t padding_size = 0) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
}