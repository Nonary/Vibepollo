// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#include "volk.h"
#include "pyrowave.h"
#include "cli_parser.hpp"
#include <stdint.h>
#include <string.h>
#include <future>
#include <utility>
#include <chrono>
#include <thread>
#include <vector>
#include "logging.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace Util;

enum
{
	EXIT_CODE_BAD_CLI = 1,
	EXIT_CODE_NO_VULKAN_DEVICE = 2,
	EXIT_CODE_TIMEOUT = 3,
	EXIT_CODE_MISSING_SUPPORT = 4,
	EXIT_CODE_ROUNDTRIP_FAILURE = 5,
};

static void print_help()
{
	LOGI("Usage: pyrowave-device-validation\n"
		"\t[--luid <luid encoded as a 64-bit hexadecimal string e.g. \"00012354f\">] (Windows only)\n"
		"\t[--vid <vendorId of device encoded as hexadecimal>]\n"
		"\t[--pid <deviceId of device encoded as hexadecimal>]\n"
		"\t[--external (verifies support for importing external handles)]\n"
		"\t[--roundtrip (verifies that encoding and decoding produces sound output)]\n"
		"\t[--timeout <seconds>]\n"
		"\tIf no device compatibility information is provided, the first Vulkan device will be used.\n");
}

static bool verify_roundtrip(pyrowave_device device)
{
	constexpr int Width = 34;
	constexpr int Height = 30;

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = device;
	decoder_info.width = Width; // Test somewhat odd size. Quite relevant for fragment path as well.
	decoder_info.height = Height;
	decoder_info.fragment_path = pyrowave_decoder_device_prefers_fragment_path(device);
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = device;
	encoder_info.width = Width;
	encoder_info.height = Height;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

	struct EncoderDeleter { void operator()(pyrowave_encoder encoder) { if (encoder) pyrowave_encoder_destroy(encoder); }};
	struct DecoderDeleter { void operator()(pyrowave_decoder decoder) { if (decoder) pyrowave_decoder_destroy(decoder); }};
	std::unique_ptr<pyrowave_encoder_opaque, EncoderDeleter> encoder;
	std::unique_ptr<pyrowave_decoder_opaque, DecoderDeleter> decoder;

	pyrowave_encoder encoder_tmp;
	pyrowave_decoder decoder_tmp;

	if (pyrowave_decoder_create(&decoder_info, &decoder_tmp) != PYROWAVE_SUCCESS)
		return false;
	decoder.reset(decoder_tmp);

	if (pyrowave_encoder_create(&encoder_info, &encoder_tmp) != PYROWAVE_SUCCESS)
		return false;
	encoder.reset(encoder_tmp);

	uint8_t luma[Height][Width] = {};
	uint8_t cb[Height][Width] = {};
	uint8_t cr[Height][Width] = {};

	uint8_t decode_luma[Height][Width] = {};
	uint8_t decode_cb[Height][Width] = {};
	uint8_t decode_cr[Height][Width] = {};

	pyrowave_cpu_buffer cpu_buffer = {};

	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

	cpu_buffer.row_stride_in_bytes[0] = Width;
	cpu_buffer.row_stride_in_bytes[1] = sizeof(cb[0]);
	cpu_buffer.row_stride_in_bytes[2] = sizeof(cr[0]);
	cpu_buffer.plane_size_in_bytes[0] = sizeof(luma);
	cpu_buffer.plane_size_in_bytes[1] = sizeof(cb);
	cpu_buffer.plane_size_in_bytes[2] = sizeof(cr);
	cpu_buffer.data[0] = &luma[0][0];
	cpu_buffer.data[1] = &cb[0][0];
	cpu_buffer.data[2] = &cr[0][0];

	for (int y = 0; y < Height; y++)
	{
		for (int x = 0; x < Width; x++)
		{
			luma[y][x] = uint8_t(3 * x + 5 * y);
			uint8_t cb_signal = 7 * x + 3 * y;
			uint8_t cr_signal = 3 * x + 5 * y;
			cb[y][x] = cb_signal;
			cr[y][x] = cr_signal;
		}
	}

	cpu_buffer.width = Width;
	cpu_buffer.height = Height;
	const pyrowave_rate_control rate_control = { 64 * 1024 }; // Just give it something massive.
	if (pyrowave_encoder_encode_cpu_synchronous(encoder.get(), &cpu_buffer, &rate_control) != PYROWAVE_SUCCESS)
		return false;

	size_t num_packets;
	if (pyrowave_encoder_compute_num_packets(encoder.get(), 64 * 1024, &num_packets) != PYROWAVE_SUCCESS)
		return false;
	if (num_packets != 1)
		return false;

	std::vector<uint8_t> bitstream(64 * 1024);
	pyrowave_packet packet = {};
	if (pyrowave_encoder_packetize(encoder.get(), &packet, 64 * 1024, &num_packets, bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS)
		return false;
	if (num_packets != 1 || packet.offset != 0 || packet.size == 0 || packet.size > bitstream.size())
		return false;
	bitstream.resize(packet.size);

	if (pyrowave_decoder_push_packet(decoder.get(), bitstream.data() + packet.offset, packet.size) != PYROWAVE_SUCCESS)
		return false;
	if (!pyrowave_decoder_decode_is_ready(decoder.get(), false))
		return false;

	cpu_buffer.data[0] = &decode_luma[0][0];
	cpu_buffer.data[1] = &decode_cb[0][0];
	cpu_buffer.data[2] = &decode_cr[0][0];
	cpu_buffer.row_stride_in_bytes[1] = sizeof(decode_cb[0]);
	cpu_buffer.row_stride_in_bytes[2] = sizeof(decode_cr[0]);
	cpu_buffer.plane_size_in_bytes[1] = sizeof(decode_cb);
	cpu_buffer.plane_size_in_bytes[2] = sizeof(decode_cr);
	cpu_buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

	if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder.get(), &cpu_buffer) != PYROWAVE_SUCCESS)
		return false;

	for (int y = 0; y < Height; y++)
	{
		for (int x = 0; x < Width; x++)
		{
			int d = std::abs(int(decode_luma[y][x]) - int(luma[y][x]));

			// With the "infinite" bitrate we get here,
			// accept a maximum 1 ULP error.
			if (d > 1)
				return false;

			if (y < Height / 2 && x < Width / 2)
			{
				// Allow more error for chroma.
				d = std::abs(int(decode_cb[y][x]) - int(cb[y][x]));
				if (d > 2)
					return false;
				d = std::abs(int(decode_cr[y][x]) - int(cr[y][x]));
				if (d > 2)
					return false;
			}
		}
	}

	return true;
}

int main(int argc, char **argv)
{
	CLICallbacks cbs;
	uint32_t vid = 0;
	uint32_t pid = 0;
	pyrowave_luid luid = {};
	bool use_luid = false;
	bool external = false;
	bool roundtrip = false;
	int timeout = 0;
	static_assert(sizeof(luid) == sizeof(uint64_t), "Unexpected LUID size.\n");

	cbs.add("--luid", [&](CLIParser &parser)
	{
		uint64_t luid_value = strtoull(parser.next_string(), nullptr, 16);
		memcpy(&luid, &luid_value, sizeof(luid));
		use_luid = true;
	});

	cbs.add("--vid", [&](CLIParser &parser) { vid = strtoul(parser.next_string(), nullptr, 16); });
	cbs.add("--pid", [&](CLIParser &parser) { pid = strtoul(parser.next_string(), nullptr, 16); });
	cbs.add("--external", [&](CLIParser &) { external = true; });
	cbs.add("--roundtrip", [&](CLIParser &) { roundtrip = true; });
	cbs.add("--timeout", [&](CLIParser &parser) { timeout = int(parser.next_uint()); });
	cbs.add("--help", [](CLIParser &parser) { parser.end(); });

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
	{
		print_help();
		return EXIT_CODE_BAD_CLI;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	// Run this in a thread since the test could just timeout due to hangs/stalls, and we'd have to force-kill the process.
	std::future<int> async_task = std::async(std::launch::async, [=]() -> int
	{
		pyrowave_device device;
		auto ret = pyrowave_create_device_by_compat(
			vid, pid, nullptr, nullptr, use_luid ? &luid : nullptr, &device);

		if (ret != PYROWAVE_SUCCESS)
			return EXIT_CODE_NO_VULKAN_DEVICE;

		if (external && !pyrowave_device_confirm_interop_support(device))
		{
			pyrowave_device_destroy(device);
			return EXIT_CODE_MISSING_SUPPORT;
		}

		if (roundtrip && !verify_roundtrip(device))
		{
			pyrowave_device_destroy(device);
			return EXIT_CODE_ROUNDTRIP_FAILURE;
		}

		pyrowave_device_destroy(device);
		return EXIT_SUCCESS;
	});

	if (timeout > 0 && async_task.wait_for(std::chrono::seconds(timeout)) == std::future_status::timeout)
	{
#ifdef _WIN32
		TerminateProcess(GetCurrentProcess(), EXIT_CODE_TIMEOUT);
#else
		std::quick_exit(EXIT_CODE_TIMEOUT);
#endif
	}

	return async_task.get();
}
