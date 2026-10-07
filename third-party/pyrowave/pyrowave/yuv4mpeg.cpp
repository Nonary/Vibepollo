// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#include "yuv4mpeg.hpp"
#include <string.h>
#include <stdint.h>

bool YUV4MPEGFile::open_read(const std::string &path)
{
	return open(path, Mode::Read);
}

bool YUV4MPEGFile::open_write(const std::string &path, const std::string &params_)
{
	params = params_;
	return open(path, Mode::Write);
}

int YUV4MPEGFile::format_to_bytes_per_component(Format format)
{
	switch (format)
	{
	case Format::YUV420P16:
	case Format::YUV444P16:
		return 2;

	default:
		return 1;
	}
}

bool YUV4MPEGFile::format_has_subsampling(Format format)
{
	switch (format)
	{
	case Format::YUV420P:
	case Format::YUV420P16:
		return true;

	default:
		return false;
	}
}

bool YUV4MPEGFile::open(const std::string &path, Mode mode_)
{
	mode = mode_;
	file.reset(fopen(path.c_str(), mode == Mode::Read ? "rb" : "wb"));
	if (!file)
	{
		fprintf(stderr, "Failed to open %s\n", path.c_str());
		return false;
	}

	if (mode == Mode::Read)
	{
		char magic[11] = {};
		if (fread(magic, 1, sizeof(magic) - 1, file.get()) != sizeof(magic) - 1)
		{
			fprintf(stderr, "Failed to read magic.\n");
			return false;
		}

		if (strcmp(magic, "YUV4MPEG2 ") != 0)
		{
			fprintf(stderr, "Invalid magic\n");
			return false;
		}

		char c;
		while (fread(&c, 1, 1, file.get()) == 1 && c != '\n')
			params += c;
		params += '\n';
	}
	else
	{
		if (!write("YUV4MPEG2 ", 10))
			return false;

		if (!write(params.data(), params.size()))
			return false;
	}

	auto w_pos = params.find_first_of('W');
	if (w_pos == std::string::npos)
		return false;
	width = strtol(params.c_str() + w_pos + 1, nullptr, 0);

	auto h_pos = params.find_first_of('H');
	if (h_pos == std::string::npos)
		return false;
	height = strtol(params.c_str() + h_pos + 1, nullptr, 0);

	auto f_pos = params.find_first_of('F');
	if (f_pos != std::string::npos)
	{
		char *end_ptr;
		frame_rate_num = strtol(params.c_str() + f_pos + 1, &end_ptr, 0);
		if (*end_ptr == ':')
			frame_rate_den = strtol(end_ptr + 1, nullptr, 0);
	}

	if (params.find("C420p10") != std::string::npos)
	{
		// Crude up-convert.
		format = Format::YUV420P16;
		unorm_scale = float(1 << 10) - 1.0f;
	}
	else if (params.find("C420p12") != std::string::npos)
	{
		format = Format::YUV420P16;
		unorm_scale = float(1 << 12) - 1.0f;
	}
	else if (params.find("C420p14") != std::string::npos)
	{
		format = Format::YUV420P16;
		unorm_scale = float(1 << 14) - 1.0f;
	}
	else if (params.find("C420p16") != std::string::npos)
	{
		format = Format::YUV420P16;
		unorm_scale = float(0xffff);
	}
	else if (params.find("C444p10") != std::string::npos)
	{
		format = Format::YUV444P16;
		unorm_scale = float(1 << 10) - 1.0f;
	}
	else if (params.find("C444p12") != std::string::npos)
	{
		format = Format::YUV444P16;
		unorm_scale = float(1 << 12) - 1.0f;
	}
	else if (params.find("C444p14") != std::string::npos)
	{
		format = Format::YUV444P16;
		unorm_scale = float(1 << 14) - 1.0f;
	}
	else if (params.find("C444p16") != std::string::npos)
	{
		format = Format::YUV444P16;
		unorm_scale = float(0xffff);
	}
	else if (params.find("C444") != std::string::npos)
	{
		format = Format::YUV444P;
	}
	else
	{
		// Fallback.
		format = Format::YUV420P;
	}

	if (params.find("XCOLORRANGE=LIMITED") != std::string::npos)
		full_range = false;
	else if (params.find("XCOLORRANGE=FULL") != std::string::npos)
		full_range = true;

	center_chroma = params.find("jpeg") != std::string::npos ||
	                params.find("JPEG") != std::string::npos;

	if (mode == Mode::Read)
		initial_position = ftell(file.get());

	return width > 0 && height > 0;
}

bool YUV4MPEGFile::rewind()
{
	if (mode != Mode::Read)
		return false;
	return fseek(file.get(), initial_position, SEEK_SET) == 0;
}

bool YUV4MPEGFile::begin_frame()
{
	if (mode == Mode::Write)
	{
		return fwrite("FRAME\n", 1, 6, file.get()) == 6;
	}
	else
	{
		std::string line;
		char c;

		while (fread(&c, 1, 1, file.get()) == 1 && c != '\n')
			line += c;

		return line == "FRAME";
	}
}

bool YUV4MPEGFile::read(void *pixels, size_t size)
{
	if (format == Format::YUV420P16)
	{
		// Recale to P016.
		auto *out = static_cast<uint16_t *>(pixels);
		uint16_t buffer[1024];
		size /= 2;

		while (size)
		{
			auto to_read = std::min<size_t>(size, 1024);
			if (fread(buffer, 2, to_read, file.get()) != to_read)
				return false;
			for (size_t i = 0; i < to_read; i++)
				out[i] = uint16_t(std::min<float>(1.0f, float(buffer[i]) / unorm_scale) * float(0xffff) + 0.5f);

			size -= to_read;
			out += to_read;
		}

		return true;
	}
	else
		return fread(pixels, 1, size, file.get()) == size;
}

bool YUV4MPEGFile::write(const void *pixels, size_t size)
{
	if (format == Format::YUV420P16)
	{
		auto *inputs = static_cast<const uint16_t *>(pixels);
		uint16_t buffer[1024];
		size /= 2;

		while (size)
		{
			auto to_write = std::min<size_t>(size, 1024);
			for (size_t i = 0; i < to_write; i++)
				buffer[i] = uint16_t(unorm_scale * float(inputs[i]) / float(0xffff) + 0.5f);
			if (fwrite(buffer, 2, to_write, file.get()) != to_write)
				return false;

			size -= to_write;
			inputs += to_write;
		}

		return true;
	}
	else
		return fwrite(pixels, 1, size, file.get()) == size;
}

int YUV4MPEGFile::get_width() const
{
	return width;
}

int YUV4MPEGFile::get_height() const
{
	return height;
}

int YUV4MPEGFile::get_frame_rate_num() const
{
	return frame_rate_num;
}

int YUV4MPEGFile::get_frame_rate_den() const
{
	return frame_rate_den;
}

const std::string &YUV4MPEGFile::get_params() const
{
	return params;
}

YUV4MPEGFile::Format YUV4MPEGFile::get_format() const
{
	return format;
}

bool YUV4MPEGFile::is_full_range() const
{
	return full_range;
}

bool YUV4MPEGFile::is_center_chroma() const
{
	return center_chroma;
}
