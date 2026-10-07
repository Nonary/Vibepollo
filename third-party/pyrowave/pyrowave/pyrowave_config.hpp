// Copyright (c) 2025 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT
#pragma once

namespace Vulkan
{
class ImageView;
}

namespace PyroWave
{
struct ViewBuffers
{
	const Vulkan::ImageView *planes[3];
};

enum class ChromaSubsampling
{
	Chroma420,
	Chroma444
};
}
