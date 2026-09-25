// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#include "vulkan/vulkan.h"
#include "pyrowave.h"
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>
#include <memory>

// Smoke test the C API.

#define ASSERT_THAT(x) do { \
	if (!(x)) { fprintf(stderr, "Fatal error executing %s at line %d.\n", #x, __LINE__); std::terminate(); } \
} while(false)

#define CHECKED(x) do { \
	pyrowave_result res = x; \
	if (res != PYROWAVE_SUCCESS) { fprintf(stderr, "Got pyrowave result %d while executing %s at line %d.\n", res, #x, __LINE__); std::terminate(); } \
} while(false)

static void test_encoder_create_validation()
{
	pyrowave_encoder_create_info info = {};
	info.width = 64;
	info.height = 64;

	// No device.
	pyrowave_encoder encoder, dummy;
	ASSERT_THAT(pyrowave_encoder_create(&info, &encoder) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	info.device = device;
	CHECKED(pyrowave_encoder_create(&info, &encoder));

	// 0 size not allowed.
	info.width = 0;
	info.height = 0;
	ASSERT_THAT(pyrowave_encoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Odd size not allowed.
	info.width = 65;
	info.height = 64;
	ASSERT_THAT(pyrowave_encoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	info.width = 64;
	info.height = 65;
	ASSERT_THAT(pyrowave_encoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Odd size allowed for 444.
	info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	CHECKED(pyrowave_encoder_create(&info, &dummy));
	pyrowave_encoder_destroy(dummy);

	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
}

static void test_decoder_create_validation()
{
	pyrowave_decoder_create_info info = {};
	info.width = 64;
	info.height = 64;

	// No device.
	pyrowave_decoder decoder, dummy;
	ASSERT_THAT(pyrowave_decoder_create(&info, &decoder) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	info.device = device;
	CHECKED(pyrowave_decoder_create(&info, &decoder));

	// 0 size not allowed.
	info.width = 0;
	info.height = 0;
	ASSERT_THAT(pyrowave_decoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Odd size not allowed.
	info.width = 65;
	info.height = 64;
	ASSERT_THAT(pyrowave_decoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	info.width = 64;
	info.height = 65;
	ASSERT_THAT(pyrowave_decoder_create(&info, &dummy) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Odd size allowed for 444.
	info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	CHECKED(pyrowave_decoder_create(&info, &dummy));
	pyrowave_decoder_destroy(dummy);

	// Smoke test that creating device on fragment path doesn't explode.
	info.fragment_path = true;
	CHECKED(pyrowave_decoder_create(&info, &dummy));
	pyrowave_decoder_destroy(dummy);

	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(device);
}

static void test_decode_cpu_buffer_validation(bool fragment_path)
{
	pyrowave_decoder_create_info info = {};
	info.width = 16;
	info.height = 16;
	info.fragment_path = fragment_path;

	pyrowave_decoder decoder;
	CHECKED(pyrowave_create_default_device(&info.device));
	CHECKED(pyrowave_decoder_create(&info, &decoder));

	// This shouldn't be ready.
	ASSERT_THAT(!pyrowave_decoder_decode_is_ready(decoder, false));
	ASSERT_THAT(!pyrowave_decoder_decode_is_ready(decoder, true));

	pyrowave_cpu_buffer cpu_buffer = {};

	uint8_t luma[16][16] = {};
	uint8_t cb[8][16] = {}; // Test strided readback while we're at it.
	uint8_t cr[8][16] = {};

	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	cpu_buffer.row_stride_in_bytes[0] = 16;
	cpu_buffer.row_stride_in_bytes[1] = 16;
	cpu_buffer.row_stride_in_bytes[2] = 16;
	cpu_buffer.plane_size_in_bytes[0] = 16 * 16;
	cpu_buffer.plane_size_in_bytes[1] = 16 * 8;
	cpu_buffer.plane_size_in_bytes[2] = 16 * 8;
	cpu_buffer.data[0] = &luma[0][0];
	cpu_buffer.data[1] = &cb[0][0];
	cpu_buffer.data[2] = &cr[0][0];

	cpu_buffer.width = 16;
	cpu_buffer.height = 16;

	// NV12 is banned for decode.
	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_NV12;
	ASSERT_THAT(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffer) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffer));

	// Assert that we do indeed decode a gray image.
	for (uint32_t y = 0; y < 16; y++)
		for (uint32_t x = 0; x < 16; x++)
			ASSERT_THAT(luma[y][x] == 0x7f || luma[y][x] == 0x80);

	for (uint32_t y = 0; y < 8; y++)
		for (uint32_t x = 0; x < 8; x++)
			ASSERT_THAT(cb[y][x] == 0x7f || cb[y][x] == 0x80);

	for (uint32_t y = 0; y < 8; y++)
		for (uint32_t x = 0; x < 8; x++)
			ASSERT_THAT(cr[y][x] == 0x7f || cr[y][x] == 0x80);

	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(info.device);
}

static void test_encode_cpu_buffer_validation(bool nv12)
{
	pyrowave_encoder_create_info info = {};
	info.width = 16;
	info.height = 16;

	pyrowave_encoder encoder;
	CHECKED(pyrowave_create_default_device(&info.device));
	CHECKED(pyrowave_encoder_create(&info, &encoder));

	const pyrowave_rate_control rate_control = { 1024 };
	pyrowave_cpu_buffer cpu_buffer = {};

	uint8_t y[16][16] = {};
	uint8_t cb[8][8] = {};
	uint8_t cr[8][8] = {};

	cpu_buffer.format = nv12 ? PYROWAVE_CPU_BUFFER_FORMAT_NV12 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	cpu_buffer.row_stride_in_bytes[0] = 16;
	cpu_buffer.row_stride_in_bytes[1] = nv12 ? 16 : 8;
	cpu_buffer.row_stride_in_bytes[2] = nv12 ? 0 : 8;
	cpu_buffer.plane_size_in_bytes[0] = 16 * 16;
	cpu_buffer.plane_size_in_bytes[1] = (nv12 ? 2 : 1) * 8 * 8;
	cpu_buffer.plane_size_in_bytes[2] = nv12 ? 0 : 8 * 8;
	cpu_buffer.data[0] = &y[0][0];
	cpu_buffer.data[1] = &cb[0][0];
	cpu_buffer.data[2] = &cr[0][0];

	cpu_buffer.width = 16;
	cpu_buffer.height = 16;
	CHECKED(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control));

	// Mismatching width/height against encoder.
	cpu_buffer.width = 15;
	cpu_buffer.height = 16;
	ASSERT_THAT(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	cpu_buffer.width = 16;
	cpu_buffer.height = 15;
	ASSERT_THAT(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Too small row strides.
	cpu_buffer.width = 16;
	cpu_buffer.height = 16;
	cpu_buffer.row_stride_in_bytes[1] = nv12 ? 15 : 7;
	ASSERT_THAT(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	// Too small plane size.
	cpu_buffer.row_stride_in_bytes[1] = nv12 ? 16 : 8;
	cpu_buffer.plane_size_in_bytes[1] = (nv12 ? 2 : 1) * 8 * 8 - 1;
	ASSERT_THAT(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control) == PYROWAVE_ERROR_INVALID_ARGUMENT);

	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(info.device);
}

static void test_error_correction_api()
{
	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	constexpr int Width = 1920;
	constexpr int Height = 1080;

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = device;
	decoder_info.width = Width; // Test somewhat odd size. Quite relevant for fragment path as well.
	decoder_info.height = Height;
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = device;
	encoder_info.width = Width;
	encoder_info.height = Height;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

	pyrowave_decoder decoder;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	std::unique_ptr<uint8_t[]> luma(new uint8_t[Width * Height]);
	pyrowave_cpu_buffer cpu_buffer = {};

	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	cpu_buffer.row_stride_in_bytes[0] = Width;
	cpu_buffer.row_stride_in_bytes[1] = Width / 2;
	cpu_buffer.row_stride_in_bytes[2] = Width / 2;
	cpu_buffer.plane_size_in_bytes[0] = Width * Height;
	cpu_buffer.plane_size_in_bytes[1] = Width * Height / 4;
	cpu_buffer.plane_size_in_bytes[2] = Width * Height / 4;
	cpu_buffer.data[0] = luma.get();
	cpu_buffer.data[1] = luma.get();
	cpu_buffer.data[2] = luma.get();

	const auto mirror = [](int value)
	{
		value &= 511;
		if (value >= 256)
			value = 511 - value;
		return value;
	};

	for (int y = 0; y < Height; y++)
		for (int x = 0; x < Width; x++)
			luma[y * Width + x] = uint8_t(mirror(13 * x + 17 * y));

	cpu_buffer.width = Width;
	cpu_buffer.height = Height;
	const pyrowave_rate_control rate_control = { 256 * 1024 };
	CHECKED(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control));

	size_t num_packets;
	CHECKED(pyrowave_encoder_compute_num_packets_with_padding(encoder, 4 * 1024, 2000, &num_packets));

	std::vector<uint8_t> bitstream(256 * 1024);
	std::vector<pyrowave_packet> packets(num_packets);
	CHECKED(pyrowave_encoder_packetize_with_padding(encoder, packets.data(), 4 * 1024, 2000, &num_packets, bitstream.data(), bitstream.size()));
	ASSERT_THAT(num_packets > 1);
	ASSERT_THAT(packets[0].offset == 0);
	ASSERT_THAT(packets[0].size < 4 * 1024 - 2000);
	for (size_t i = 1; i < num_packets; i++)
		ASSERT_THAT(packets[i].size <= 4 * 1024);

	{
		const void *mapped_bitstream = nullptr;
		const void *mapped_meta = nullptr;
		size_t mapped_bitstream_size = 0;
		size_t mapped_metadata_size = 0;
		CHECKED(pyrowave_encoder_get_mapped_raw_bitstream(encoder, &mapped_bitstream, &mapped_bitstream_size, &mapped_meta, &mapped_metadata_size));
		ASSERT_THAT(mapped_bitstream);
		ASSERT_THAT(mapped_meta);
		ASSERT_THAT(mapped_bitstream_size == rate_control.maximum_bitstream_size + mapped_metadata_size);
	}

	// Test a theoretical situation.
	for (int bands = 1; bands < 4; bands++)
	{
		size_t num_critical_packets;
		size_t num_active_blocks;
		CHECKED(pyrowave_encoder_get_num_active_blocks(encoder, bands, &num_active_blocks));

		std::vector<uint32_t> active_words((num_active_blocks + 31) / 32);
		CHECKED(pyrowave_encoder_compute_block_active_words(encoder, bands, active_words.data(), active_words.size()));

		CHECKED(pyrowave_encoder_compute_num_critical_packets(encoder, bands, 4 * 1024, 2000, &num_critical_packets));

		pyrowave_decoder_clear(decoder);

		for (size_t i = 0; i < num_critical_packets; i++)
		{
			CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.data() + packets[i].offset, packets[i].size));
			bool should_be_ready = i + 1 == num_critical_packets;
			ASSERT_THAT(pyrowave_decoder_decode_is_ready_with_sideband(
				decoder, true, bands, 0.0f,
				active_words.data(), active_words.size()) == should_be_ready);

			// If minimum packet ratio is large, we will fail due to that too.
			ASSERT_THAT(!pyrowave_decoder_decode_is_ready_with_sideband(
				decoder, true, bands, 0.5f,
				active_words.data(), active_words.size()));
		}
	}

	pyrowave_decoder_destroy(decoder);
	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
}

static void test_basic_encoder_roundtrip(bool fragment_decode, bool nv12_encode, pyrowave_chroma_subsampling chroma)
{
	if (chroma == PYROWAVE_CHROMA_SUBSAMPLING_444 && nv12_encode)
		return;

	if (fragment_decode)
		return;

	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	constexpr int Width = 34;
	constexpr int Height = 30;

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = device;
	decoder_info.width = Width; // Test somewhat odd size. Quite relevant for fragment path as well.
	decoder_info.height = Height;
	decoder_info.fragment_path = fragment_decode;
	decoder_info.chroma = chroma;

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = device;
	encoder_info.width = Width;
	encoder_info.height = Height;
	encoder_info.chroma = chroma;

	pyrowave_decoder decoder;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	uint8_t luma[Height][Width] = {};
	uint8_t cb[Height][Width] = {};
	uint8_t cr[Height][Width] = {};
	uint8_t cbcr[Height][Width][2] = {};

	uint8_t decode_luma[Height][Width] = {};
	uint8_t decode_cb[Height][Width] = {};
	uint8_t decode_cr[Height][Width] = {};

	pyrowave_cpu_buffer cpu_buffer = {};

	cpu_buffer.format =
			nv12_encode
				? PYROWAVE_CPU_BUFFER_FORMAT_NV12
				: (chroma == PYROWAVE_CHROMA_SUBSAMPLING_444
					   ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P
					   : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P);

	cpu_buffer.row_stride_in_bytes[0] = Width;
	cpu_buffer.row_stride_in_bytes[1] = nv12_encode ? sizeof(cbcr[0]) : sizeof(cb[0]);
	cpu_buffer.row_stride_in_bytes[2] = nv12_encode ? 0 : sizeof(cr[0]);
	cpu_buffer.plane_size_in_bytes[0] = sizeof(luma);
	cpu_buffer.plane_size_in_bytes[1] = nv12_encode ? sizeof(cbcr) : sizeof(cb);
	cpu_buffer.plane_size_in_bytes[2] = nv12_encode ? 0 : sizeof(cr);
	cpu_buffer.data[0] = &luma[0][0];

	if (nv12_encode)
	{
		cpu_buffer.data[1] = &cbcr[0][0][0];
	}
	else
	{
		cpu_buffer.data[1] = &cb[0][0];
		cpu_buffer.data[2] = &cr[0][0];
	}

	for (int y = 0; y < Height; y++)
	{
		for (int x = 0; x < Width; x++)
		{
			luma[y][x] = uint8_t(3 * x + 5 * y);

			uint8_t cb_signal = 7 * x + 3 * y;
			uint8_t cr_signal = 3 * x + 5 * y;

			if (nv12_encode)
			{
				cbcr[y][x][0] = cb_signal;
				cbcr[y][x][1] = cr_signal;
			}
			else
			{
				cb[y][x] = cb_signal;
				cr[y][x] = cr_signal;
			}
		}
	}

	cpu_buffer.width = Width;
	cpu_buffer.height = Height;
	const pyrowave_rate_control rate_control = { 64 * 1024 }; // Just give it something massive.
	CHECKED(pyrowave_encoder_encode_cpu_synchronous(encoder, &cpu_buffer, &rate_control));

	size_t num_packets;
	CHECKED(pyrowave_encoder_compute_num_packets(encoder, 64 * 1024, &num_packets));
	ASSERT_THAT(num_packets == 1);

	CHECKED(pyrowave_encoder_compute_num_packets_with_padding(encoder, 64 * 1024, 64 * 1024 - 4, &num_packets));
	ASSERT_THAT(num_packets == 2);

	std::vector<uint8_t> bitstream(64 * 1024);
	std::vector<uint8_t> bitstream_padded(64 * 1024);
	pyrowave_packet packet = {};
	CHECKED(pyrowave_encoder_packetize(encoder, &packet, 64 * 1024, &num_packets, bitstream.data(), bitstream.size()));
	ASSERT_THAT(num_packets == 1);
	ASSERT_THAT(packet.offset == 0);
	ASSERT_THAT(packet.size != 0);
	ASSERT_THAT(packet.size <= bitstream.size());
	bitstream.resize(packet.size);

	// Just padding on its own should not change the bitstream in any way if the splits don't happen.
	CHECKED(pyrowave_encoder_packetize_with_padding(encoder, &packet, 64 * 1024, 16, &num_packets,
		bitstream_padded.data(), bitstream_padded.size()));
	ASSERT_THAT(num_packets == 1);
	ASSERT_THAT(packet.offset == 0);
	ASSERT_THAT(packet.size != 0);
	ASSERT_THAT(packet.size <= bitstream_padded.size());
	bitstream_padded.resize(packet.size);
	ASSERT_THAT(packet.size == bitstream.size());
	ASSERT_THAT(std::memcmp(bitstream.data(), bitstream_padded.data(), packet.size) == 0);

	CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.data() + packet.offset, packet.size));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready_with_sideband(decoder, false, 4, 0.0f, nullptr, 0));
	pyrowave_decoder_clear(decoder);
	ASSERT_THAT(!pyrowave_decoder_decode_is_ready(decoder, false));
	ASSERT_THAT(!pyrowave_decoder_decode_is_ready_with_sideband(decoder, false, 4, 0.0f, nullptr, 0));
	CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.data() + packet.offset, packet.size));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready_with_sideband(decoder, false, 4, 0.0f, nullptr, 0));

	cpu_buffer.data[0] = &decode_luma[0][0];
	cpu_buffer.data[1] = &decode_cb[0][0];
	cpu_buffer.data[2] = &decode_cr[0][0];
	cpu_buffer.row_stride_in_bytes[1] = sizeof(decode_cb[0]);
	cpu_buffer.row_stride_in_bytes[2] = sizeof(decode_cr[0]);
	cpu_buffer.plane_size_in_bytes[1] = sizeof(decode_cb);
	cpu_buffer.plane_size_in_bytes[2] = sizeof(decode_cr);
	cpu_buffer.format = chroma == PYROWAVE_CHROMA_SUBSAMPLING_444 ?
			PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

	CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffer));

	for (int y = 0; y < Height; y++)
	{
		for (int x = 0; x < Width; x++)
		{
			int d = std::abs(int(decode_luma[y][x]) - int(luma[y][x]));
			// With the "infinite" bitrate we get here,
			// accept a maximum 1 ULP error.
			ASSERT_THAT(d <= 1);

			if (chroma == PYROWAVE_CHROMA_SUBSAMPLING_444 || (!nv12_encode && y < Height / 2 && x < Width / 2))
			{
				// Allow more error for chroma.
				d = std::abs(int(decode_cb[y][x]) - int(cb[y][x]));
				ASSERT_THAT(d <= 1);
				d = std::abs(int(decode_cr[y][x]) - int(cr[y][x]));
				ASSERT_THAT(d <= 1);
			}
		}
	}

	if (nv12_encode)
	{
		for (int y = 0; y < Height / 2; y++)
		{
			for (int x = 0; x < Width / 2; x++)
			{
				int d = std::abs(int(decode_cb[y][x]) - int(cbcr[y][x][0]));
				ASSERT_THAT(d <= 1);
				d = std::abs(int(decode_cr[y][x]) - int(cbcr[y][x][1]));
				ASSERT_THAT(d <= 1);
			}
		}
	}

	pyrowave_decoder_destroy(decoder);
	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
}

static void test_basic_system_stability()
{
	pyrowave_device device;
	CHECKED(pyrowave_create_default_device(&device));

	// 4K, upper bound of normal usage.
	constexpr int Width = 3840;
	constexpr int Height = 2160;

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = device;
	decoder_info.width = Width; // Test somewhat odd size. Quite relevant for fragment path as well.
	decoder_info.height = Height;

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = device;
	encoder_info.width = Width;
	encoder_info.height = Height;

	pyrowave_decoder decoder;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	std::vector<uint8_t> luma(Width * Height);
	std::vector<uint16_t> cbcr(Width * Height / 4);
	std::vector<uint8_t> decode_luma(Width * Height);
	std::vector<uint8_t> decode_cb(Width * Height / 4);
	std::vector<uint8_t> decode_cr(Width * Height / 4);

	pyrowave_cpu_buffer encode_buffer = {}, decode_buffer = {};

	encode_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_NV12;
	encode_buffer.row_stride_in_bytes[0] = Width;
	encode_buffer.row_stride_in_bytes[1] = Width;
	encode_buffer.plane_size_in_bytes[0] = Width * Height;
	encode_buffer.plane_size_in_bytes[1] = Width * Height / 2;
	encode_buffer.data[0] = luma.data();
	encode_buffer.data[1] = cbcr.data();
	encode_buffer.width = Width;
	encode_buffer.height = Height;

	decode_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
	decode_buffer.row_stride_in_bytes[0] = Width;
	decode_buffer.row_stride_in_bytes[1] = Width / 2;
	decode_buffer.row_stride_in_bytes[2] = Width / 2;
	decode_buffer.plane_size_in_bytes[0] = Width * Height;
	decode_buffer.plane_size_in_bytes[1] = Width * Height / 4;
	decode_buffer.plane_size_in_bytes[2] = Width * Height / 4;
	decode_buffer.data[0] = decode_luma.data();
	decode_buffer.data[1] = decode_cb.data();
	decode_buffer.data[2] = decode_cr.data();
	decode_buffer.width = Width;
	decode_buffer.height = Height;

	const auto mirror = [](int v) -> uint8_t
	{
		v &= 511;
		if (v > 255)
			v = 511 - v;
		ASSERT_THAT(v >= 0 && v <= 255);
		return uint8_t(v);
	};

	// Just generate a synthetic signal.
	for (int y = 0; y < Height; y++)
		for (int x = 0; x < Width; x++)
			luma[y * Width + x] = mirror(3 * x + 5 * y);

	for (int y = 0; y < Height / 2; y++)
		for (int x = 0; x < Width / 2; x++)
			cbcr[y * Width / 2 + x] = mirror(7 * x + y * 3) * 0x100 + mirror(y * 5 + x * 7);

	std::vector<uint8_t> bitstream;
	std::vector<pyrowave_packet> packets;

	for (int iter = 0; iter < 100; iter++)
	{
		// 240mbit equivalent for 60 fps.
		const pyrowave_rate_control rate_control = { 500000 };

		// Get some test coverage for async compute path.
		CHECKED(pyrowave_device_set_queue_type(device, iter % 2 ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_GRAPHICS_BIT));

		bitstream.reserve(rate_control.maximum_bitstream_size);
		CHECKED(pyrowave_encoder_encode_cpu_synchronous(encoder, &encode_buffer, &rate_control));

		size_t num_packets, after_packets;
		CHECKED(pyrowave_encoder_compute_num_packets(encoder, 8 * 1024, &num_packets));
		ASSERT_THAT(num_packets > 1);

		packets.resize(num_packets);
		bitstream.resize(rate_control.maximum_bitstream_size);
		CHECKED(pyrowave_encoder_packetize(encoder, packets.data(), 8 * 1024, &after_packets, bitstream.data(), bitstream.size()));
		ASSERT_THAT(num_packets == after_packets);

		// Verify that the bitstream is sound. We should be able to decode it.
		size_t total_bitstream_size = 0;

		for (auto &packet : packets)
		{
			CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.data() + packet.offset, packet.size));
			// When we push the last packet, we should get a complete frame.
			ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false) == (&packet == &packets.back()));
			total_bitstream_size += packet.size;
		}

		// Verify that we tightly hit our rate control budget.
		ASSERT_THAT(total_bitstream_size <= rate_control.maximum_bitstream_size);
		ASSERT_THAT(total_bitstream_size >= 95 * rate_control.maximum_bitstream_size / 100);

		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &decode_buffer));
	}

	pyrowave_decoder_destroy(decoder);
	pyrowave_encoder_destroy(encoder);

	// Verify that PSNR is under control.
	double y_error = 0.0;
	double cb_error = 0.0;
	double cr_error = 0.0;

	for (int i = 0; i < Width * Height; i++)
	{
		double y_d = double(luma[i]) - double(decode_luma[i]);
		y_error += y_d * y_d;
	}

	for (int i = 0; i < Width * Height / 4; i++)
	{
		double cb_d = double(cbcr[i] & 0xff) - double(decode_cb[i]);
		cb_error += cb_d * cb_d;
		double cr_d = double(cbcr[i] >> 8) - double(decode_cr[i]);
		cr_error += cr_d * cr_d;
	}

	double y_signal = 255.0 * 255.0 * Width * Height;
	double chroma_signal = 255.0 * 255.0 * (Width * Height / 4);
	double y_psnr = y_signal / y_error;
	double cb_psnr = chroma_signal / cb_error;
	double cr_psnr = chroma_signal / cr_error;

	// 40 dB, arbitrarily chosen for testing purposes.
	ASSERT_THAT(y_psnr > 10000.0);
	ASSERT_THAT(cb_psnr > 10000.0);
	ASSERT_THAT(cr_psnr > 10000.0);

	// We're not hitting 60 dB. That'd mean we cheated or something or added bugs in the test code.
	ASSERT_THAT(y_psnr < 1000000.0);
	ASSERT_THAT(cb_psnr < 1000000.0);
	ASSERT_THAT(cr_psnr < 1000000.0);

	int blah = 0;
	pyrowave_device_report_performance_stats(device, [](void *userdata, const char *msg)
	{
		*static_cast<int *>(userdata) = 42;
		printf("performance cb: %s\n", msg);
	}, &blah, true);

	// Verify that userdata gets passed correctly.
	ASSERT_THAT(blah == 42);

	pyrowave_device_destroy(device);
}

int main()
{
	printf("Running system stability test ...\n");
	test_basic_system_stability();

	// Correctness tests for small-ish outputs.
	for (int variant = 0; variant < 8; variant++)
	{
		printf("Running roundtrip variant %d test ...\n", variant);
		test_basic_encoder_roundtrip(
			(variant & 1) != 0, (variant & 2) != 0,
			(variant & 4) != 0 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420);
	}

	test_error_correction_api();

	// Validate that we handle error inputs gracefully.
	printf("Running error handling tests ...\n");
	test_decode_cpu_buffer_validation(false);
	test_decode_cpu_buffer_validation(true);
	test_encode_cpu_buffer_validation(false);
	test_encode_cpu_buffer_validation(true);
	test_encoder_create_validation();
	test_decoder_create_validation();

	printf("Passed all tests :)\n");
}
