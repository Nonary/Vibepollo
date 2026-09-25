// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#define INITGUID

#include "device.hpp"
#include "context.hpp"
#include "image.hpp"

#include "pyrowave.h"
#include <stdio.h>
#include <cstdlib>
#include <exception>
#include <vector>
#include <cmath>

#include "shaders/slangmosh_test.hpp"

#ifdef _WIN32
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi.h>
#include "com_ptr.hpp"
#endif

using namespace Vulkan;

#define ASSERT_THAT(x) do { \
	if (!(x)) { fprintf(stderr, "Fatal error executing %s at line %d.\n", #x, __LINE__); std::terminate(); } \
} while(false)

#define CHECKED(x) do { \
	pyrowave_result _res = x; \
	if (_res != PYROWAVE_SUCCESS) { fprintf(stderr, "Got pyrowave result %d while executing %s at line %d.\n", _res, #x, __LINE__); std::terminate(); } \
} while(false)

#define CHECK_HRESULT(x) ASSERT_THAT(SUCCEEDED(x))

static pyrowave_device create_device_from_granite(Device &device)
{
	// Verify that we can create a device from UUID/LUID.
	VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
	vkGetPhysicalDeviceProperties2(device.get_physical_device(), &props2);

	pyrowave_uuid device_uuid, driver_uuid;
	pyrowave_luid device_luid;

	memcpy(device_uuid.uuid, ids.deviceUUID, VK_UUID_SIZE);
	memcpy(driver_uuid.uuid, ids.driverUUID, VK_UUID_SIZE);
	memcpy(device_luid.luid, ids.deviceLUID, VK_LUID_SIZE);

	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device_by_compat(device.get_gpu_properties().vendorID,
		device.get_gpu_properties().deviceID, &device_uuid, &driver_uuid,
		ids.deviceLUIDValid ? &device_luid : nullptr, &pyro_device));

	return pyro_device;
}

static pyrowave_sync_object create_sync_object_from_timeline(pyrowave_device device, SemaphoreHolder &sem)
{
	auto exported_timeline = sem.export_to_handle();
	ASSERT_THAT(exported_timeline);

	pyrowave_sync_object_create_info sync_info = {};
	sync_info.device = device;
	sync_info.handle_type = exported_timeline.semaphore_handle_type;
	sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
	sync_info.external_handle = (pyrowave_os_handle)exported_timeline.handle;
	pyrowave_sync_object imported_timeline;
	CHECKED(pyrowave_sync_object_create(&sync_info, &imported_timeline));
	return imported_timeline;
}

static pyrowave_sync_object create_sync_object_from_binary(pyrowave_device device, SemaphoreHolder &sem)
{
	auto exported_timeline = sem.export_to_handle();
	ASSERT_THAT(exported_timeline);

	pyrowave_sync_object_create_info sync_info = {};
	sync_info.device = device;
	sync_info.handle_type = exported_timeline.semaphore_handle_type;
	sync_info.semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
	sync_info.external_handle = (pyrowave_os_handle)exported_timeline.handle;
	sync_info.import_flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
	pyrowave_sync_object imported_timeline;
	CHECKED(pyrowave_sync_object_create(&sync_info, &imported_timeline));
	return imported_timeline;
}

static pyrowave_image create_imported_image(pyrowave_device pyro_device, Device &device, Image &img)
{
	auto exported = img.export_handle();
	ASSERT_THAT(exported);

	VkImageCreateInfo image_create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_create_info.flags = img.get_create_info().flags;
	image_create_info.usage = img.get_create_info().usage;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.format = img.get_create_info().format;
	image_create_info.samples = img.get_create_info().samples;
	image_create_info.mipLevels = img.get_create_info().levels;
	image_create_info.arrayLayers = img.get_create_info().layers;
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_create_info.imageType = img.get_create_info().type;
	image_create_info.extent = { img.get_width(), img.get_height(), img.get_depth() };

	pyrowave_image_create_info image_info = {};
	image_info.device = pyro_device;
	image_info.handle_type = exported.memory_handle_type;
	image_info.external_handle = (pyrowave_os_handle)exported.handle;
	image_info.image_create_info = &image_create_info;

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info =
		{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
	VkImageFormatListCreateInfo format_list =
		{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
	std::vector<VkSubresourceLayout> drm_plane_layouts;

	if (exported.memory_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
	{
		image_create_info.pNext = &modifier_info;
		image_create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

		if (img.get_format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		{
			static const VkFormat nv12_formats[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };
			format_list.viewFormatCount = 2;
			format_list.pViewFormats = nv12_formats;
		}
		else if (img.get_format() == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
		{
			static const VkFormat yuv420p_formats[] = { VK_FORMAT_R8_UNORM };
			format_list.viewFormatCount = 1;
			format_list.pViewFormats = yuv420p_formats;
		}
		else
		{
			format_list.viewFormatCount = 1;
			format_list.pViewFormats = &image_create_info.format;
		}

		// Query which DRM modifier the implementation picked for us and pass it along.
		VkImageDrmFormatModifierPropertiesEXT props = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
		device.get_device_table().vkGetImageDrmFormatModifierPropertiesEXT(
			device.get_device(), img.get_image(), &props);
		modifier_info.drmFormatModifier = props.drmFormatModifier;

		VkDrmFormatModifierPropertiesListEXT modifiers = { VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
		VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, &modifiers };
		device.get_format_properties(image_create_info.format, &props3);
		std::vector<VkDrmFormatModifierPropertiesEXT> modifiers_props(modifiers.drmFormatModifierCount);
		modifiers.pDrmFormatModifierProperties = modifiers_props.data();
		device.get_format_properties(image_create_info.format, &props3);

		auto itr = std::find_if(modifiers_props.begin(), modifiers_props.end(), [&](const VkDrmFormatModifierPropertiesEXT &prop)
		{
			return prop.drmFormatModifier == props.drmFormatModifier;
		});

		ASSERT_THAT(itr != modifiers_props.end());
		uint32_t num_memory_planes = itr->drmFormatModifierPlaneCount;
		drm_plane_layouts.resize(num_memory_planes);

		// DRM format modifiers have per-plane stride information that needs to be queried ...
		for (uint32_t i = 0; i < num_memory_planes; i++)
		{
			VkImageSubresource subresource = {
				VkImageAspectFlags(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i),
				0, 0
			};

			device.get_device_table().vkGetImageSubresourceLayout(device.get_device(),
				img.get_image(), &subresource, &drm_plane_layouts[i]);

			// On import, these must be cleared to be valid.
			drm_plane_layouts[i].size = 0;
			drm_plane_layouts[i].depthPitch = 0;
			if (image_create_info.arrayLayers == 1)
				drm_plane_layouts[i].arrayPitch = 0;
		}

		// Pass it along to import.
		modifier_info.drmFormatModifierPlaneCount = num_memory_planes;
		modifier_info.pPlaneLayouts = drm_plane_layouts.data();
		modifier_info.pNext = &format_list;
	}

	pyrowave_image imported_image;
	CHECKED(pyrowave_image_create(&image_info, &imported_image));
	return imported_image;
}

static uint8_t mirror(int v)
{
	v &= 511;
	if (v > 255)
		v = 511 - v;
	ASSERT_THAT(v >= 0 && v <= 255);
	return uint8_t(v);
}

static ImageHandle create_exportable_test_image(Device &device, VkExternalMemoryHandleTypeFlagBits handle_type,
                                                VkFormat format)
{
	auto info = ImageCreateInfo::immutable_2d_image(1280, 720, format);
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
	             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.misc = IMAGE_MISC_NO_DEFAULT_VIEWS_BIT | IMAGE_MISC_EXTERNAL_MEMORY_BIT;
	info.layout = ImageLayout::General;
	info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
	info.external.memory_handle_type = handle_type;

	// DRM format modifiers require explicit cast list when MUTABLE is used.
	VkImageFormatListCreateInfo format_list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };

	VkImageDrmFormatModifierListCreateInfoEXT allowed_modifiers_info =
		{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT };
	std::vector<uint64_t> allowed_modifiers;

	if (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
	{
		// Query all modifiers potentially supported by a format.
		VkDrmFormatModifierPropertiesListEXT modifiers = { VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
		VkFormatProperties3 props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, &modifiers };
		device.get_format_properties(info.format, &props3);
		std::vector<VkDrmFormatModifierPropertiesEXT> modifiers_props(modifiers.drmFormatModifierCount);
		modifiers.pDrmFormatModifierProperties = modifiers_props.data();
		device.get_format_properties(info.format, &props3);

		if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		{
			static const VkFormat nv12_formats[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM };
			format_list.viewFormatCount = 2;
			format_list.pViewFormats = nv12_formats;
		}
		else if (format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM)
		{
			static const VkFormat yuv420p_formats[] = { VK_FORMAT_R8_UNORM };
			format_list.viewFormatCount = 1;
			format_list.pViewFormats = yuv420p_formats;
		}
		else
		{
			format_list.viewFormatCount = 1;
			format_list.pViewFormats = &format;
		}

		for (uint32_t i = 0; i < modifiers.drmFormatModifierCount; i++)
		{
			auto &mod = modifiers.pDrmFormatModifierProperties[i];
			VkPhysicalDeviceImageDrmFormatModifierInfoEXT modinfo =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &format_list };

			modinfo.drmFormatModifier = mod.drmFormatModifier;

			VkImageFormatProperties2 props2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };

			// For DRM modifiers, it's a bit involved.
			// Step 1 is to query which modifiers are supported for a given image.
			if (device.get_image_format_properties(info.format, info.type,
				VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
				info.usage, info.flags, &modinfo, &props2) &&
				info.width <= props2.imageFormatProperties.maxExtent.width &&
				info.height <= props2.imageFormatProperties.maxExtent.height)
			{
				allowed_modifiers.push_back(mod.drmFormatModifier);
			}
		}

		ASSERT_THAT(!allowed_modifiers.empty());

		// Implementation picks one of the allowed modifiers.
		allowed_modifiers_info.drmFormatModifierCount = allowed_modifiers.size();
		allowed_modifiers_info.pDrmFormatModifiers = allowed_modifiers.data();
		allowed_modifiers_info.pNext = &format_list;
		info.pnext = &allowed_modifiers_info;
	}

	auto img = device.create_image(info);

	if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
	{
		auto cmd = device.request_command_buffer();

		auto *luma = static_cast<uint8_t *>(cmd->update_image(*img, {}, { 1280, 720, 1 }, 1280, 720, { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 }));
		auto *chroma = static_cast<uint16_t *>(cmd->update_image(*img, {}, { 640, 360, 1 }, 640, 360, { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 }));

		for (int y = 0; y < 720; y++)
			for (int x = 0; x < 1280; x++)
				luma[y * 1280 + x] = mirror(y * 3 + 5 * x);

		for (int y = 0; y < 360; y++)
			for (int x = 0; x < 640; x++)
				chroma[y * 640 + x] = (uint16_t(mirror(y * 7 + 5 * x)) << 8) | mirror(y + 3 * x);

		device.submit(cmd);
	}

	return img;
}

static constexpr size_t BitstreamSize = 1000000;

static void send_image_to_encoder(pyrowave_image pyro_image,
		pyrowave_sync_object pyro_sync_acquire, uint64_t acquire_value,
		pyrowave_sync_object pyro_sync_release, uint64_t release_value,
		pyrowave_encoder encoder)
{
	pyrowave_gpu_external_reference ref = { pyro_image, VK_QUEUE_FAMILY_EXTERNAL };

	pyrowave_gpu_sync_operation acquire = {};
	pyrowave_gpu_sync_operation release = {};
	pyrowave_gpu_buffers buffers = {};
	pyrowave_image_view view = {};
	pyrowave_rate_control rate_control = { BitstreamSize };

	// Exercise the NV12 scaler path as well.
	CHECKED(pyrowave_image_get_image_view(pyro_image,
		VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[0]));
	CHECKED(pyrowave_image_get_image_view(pyro_image,
		VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[1]));
	CHECKED(pyrowave_image_get_image_view(pyro_image,
		VK_IMAGE_ASPECT_PLANE_2_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[2]));

	if (buffers.planes[0].image_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
	{
		CHECKED(pyrowave_image_get_image_view(pyro_image,
				VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &view));
	}

	acquire.num_images = 1;
	acquire.images = &ref;
	acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync_acquire);
	acquire.sync.value = acquire_value;

	release.num_images = 1;
	release.images = &ref;
	release.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync_release);
	release.sync.value = release_value;

	if (buffers.planes[0].image_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
	{
		pyrowave_scaled_encode_info scaled_info = {};
		scaled_info.view = view;
		scaled_info.force_linear_filtering = true;
		scaled_info.skip_dither = true;
		scaled_info.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		scaled_info.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		scaled_info.intermediate_plane_format = VK_FORMAT_R8_UNORM;
		CHECKED(pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, &acquire, &release, &scaled_info, &rate_control));
	}
	else
		CHECKED(pyrowave_encoder_encode_gpu_synchronous(encoder, &acquire, &release, &buffers, &rate_control));
}

static void send_granite_image_to_encoder(Device &device, Image &granite_image, pyrowave_image pyro_image,
                                          SemaphoreHolder &granite_sync, pyrowave_sync_object pyro_sync_acquire, uint64_t acquire_value,
                                          pyrowave_sync_object pyro_sync_release, uint64_t release_value,
                                          pyrowave_encoder encoder)
{
	auto cmd = device.request_command_buffer();
	cmd->release_image_barrier(granite_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_EXTERNAL);
	device.submit(cmd);

	if (acquire_value)
	{
		auto signal = device.request_timeline_semaphore_as_binary(granite_sync, acquire_value);
		device.submit_empty(CommandBuffer::Type::Generic, nullptr, signal.get());
	}

#ifdef VULKAN_DEBUG
	// VVL doesn't quite understand it when a different device increments the shared timeline.
	// It thinks that we're doing a rewind, but that's not the case.
	// Only observed on Windows for some reason, but avoids a dumb false positive.
	device.wait_idle();
#endif

	send_image_to_encoder(pyro_image, pyro_sync_acquire, acquire_value,
		pyro_sync_release, release_value, encoder);

	// Verify that the release value was signaled properly in finite time.
	VkSemaphoreWaitInfo wait_info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
	wait_info.pSemaphores = &granite_sync.get_semaphore();
	wait_info.semaphoreCount = 1;
	wait_info.pValues = &release_value;
	auto vr = device.get_device_table().vkWaitSemaphores(device.get_device(), &wait_info, 1000000000);
	ASSERT_THAT(vr == VK_SUCCESS);
}

static void decode_image(pyrowave_image *pyro_image, bool multiplane,
                         pyrowave_sync_object pyro_sync, uint64_t release_value,
                         pyrowave_decoder decoder)
{
	// Discard the image(s).
	pyrowave_gpu_external_reference acquire_ref[3] = {};
	pyrowave_gpu_external_reference release_ref[3] = {};

	pyrowave_gpu_sync_operation acquire = {};
	pyrowave_gpu_sync_operation release = {};
	pyrowave_gpu_buffers buffers = {};

	for (int plane = 0; plane < (multiplane ? 1 : 3); plane++)
	{
		// Discard the output.
		acquire_ref[plane] = { pyro_image[plane], VK_QUEUE_FAMILY_IGNORED };
		// Release back to other device.
		release_ref[plane] = { pyro_image[plane], VK_QUEUE_FAMILY_EXTERNAL };
	}

	acquire.num_images = multiplane ? 1 : 3;
	acquire.images = acquire_ref;

	release.num_images = multiplane ? 1 : 3;
	release.images = release_ref;
	release.sync.semaphore = pyrowave_sync_object_get_semaphore(pyro_sync);
	release.sync.value = release_value;

	CHECKED(pyrowave_image_get_image_view(pyro_image[0],
		VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_USAGE_STORAGE_BIT, &buffers.planes[0]));
	CHECKED(pyrowave_image_get_image_view(pyro_image[multiplane ? 0 : 1],
		VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT, &buffers.planes[1]));
	CHECKED(pyrowave_image_get_image_view(pyro_image[multiplane ? 0 : 2],
		VK_IMAGE_ASPECT_PLANE_2_BIT, VK_IMAGE_USAGE_STORAGE_BIT, &buffers.planes[2]));

	CHECKED(pyrowave_decoder_decode_gpu_buffer(decoder, &acquire, &release, &buffers));
}

static void send_payload_to_decoder(pyrowave_encoder encoder, pyrowave_decoder decoder)
{
	size_t num_packets;
	CHECKED(pyrowave_encoder_compute_num_packets(encoder, BitstreamSize, &num_packets));
	ASSERT_THAT(num_packets == 1);

	pyrowave_packet packet;
	std::unique_ptr<uint8_t[]> bitstream(new uint8_t[BitstreamSize]);
	CHECKED(pyrowave_encoder_packetize(encoder, &packet, BitstreamSize, &num_packets, bitstream.get(), BitstreamSize));
	CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.get() + packet.offset, packet.size));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));
}

static void validate_mirror_buffer(const uint8_t *ptr, int width, int height, int row_stride, int dx, int dy)
{
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			int reference = mirror(x * dx + y * dy);
			int value = ptr[y * row_stride + x];
			int d = std::abs(reference - value);
			ASSERT_THAT(d <= 1);
		}
	}
}

static void validate_mirror_buffer(Device &device, Buffer &buf, int width, int height, int dx, int dy)
{
	auto *ptr = static_cast<const uint8_t *>(device.map_host_buffer(buf, MEMORY_ACCESS_READ_BIT));
	validate_mirror_buffer(ptr, width, height, width, dx, dy);
}

static void validate_granite_image(Device &device, Image &img, SemaphoreHolder &sem, uint64_t acquire_value)
{
	auto wait_sem = device.request_timeline_semaphore_as_binary(sem, acquire_value);
	wait_sem->signal_external();
	device.add_wait_semaphore(CommandBuffer::Type::Generic, std::move(wait_sem), VK_PIPELINE_STAGE_2_COPY_BIT, true);

	auto cmd = device.request_command_buffer();

	BufferCreateInfo bufinfo = {};
	bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufinfo.size = img.get_width() * img.get_height();
	bufinfo.domain = BufferDomain::CachedHost;

	auto y = device.create_buffer(bufinfo);

	bufinfo.size /= 4;
	auto cb = device.create_buffer(bufinfo);
	auto cr = device.create_buffer(bufinfo);

	cmd->acquire_image_barrier(img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL);

	cmd->copy_image_to_buffer(*y, img, 0, {}, { img.get_width(), img.get_height(), 1 }, 1280, 720,
		{ VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 });
	cmd->copy_image_to_buffer(*cb, img, 0, {}, { img.get_width() / 2, img.get_height() / 2, 1 }, 640, 360,
		{ VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 });
	cmd->copy_image_to_buffer(*cr, img, 0, {}, { img.get_width() / 2, img.get_height() / 2, 1 }, 640, 360,
		{ VK_IMAGE_ASPECT_PLANE_2_BIT, 0, 0, 1 });

	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT);

	Fence fence;
	device.submit(cmd, &fence);
	fence->wait();

	validate_mirror_buffer(device, *y, 1280, 720, 5, 3);
	validate_mirror_buffer(device, *cb, 640, 360, 3, 1);
	validate_mirror_buffer(device, *cr, 640, 360, 5, 7);
}

static void test_direct_interop()
{
	ASSERT_THAT(Context::init_loader(nullptr));

	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.apiVersion = VK_API_VERSION_1_3;
	app_info.pApplicationName = "pyrowave-c-test";
	app_info.pEngineName = "Granite";
	ctx.set_application_info(&app_info);

	ASSERT_THAT(ctx.init_instance_and_device(nullptr, 0, nullptr, 0));

	Device device;
	device.set_context(ctx);

	// Fill in a proxy instance create info.
	VkInstanceCreateInfo instance_create_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_create_info.enabledExtensionCount = device.get_device_features().num_instance_extensions;
	instance_create_info.ppEnabledExtensionNames = device.get_device_features().instance_extensions;
	instance_create_info.pApplicationInfo = &ctx.get_application_info();

	// Fill in a proxy device create info.
	VkDeviceCreateInfo device_create_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue_info.queueFamilyIndex = device.get_queue_info().family_indices[QUEUE_INDEX_GRAPHICS];
	queue_info.queueCount = 1;
	device_create_info.pNext = ctx.get_enabled_device_features().pdf2;
	device_create_info.enabledExtensionCount = ctx.get_enabled_device_features().num_device_extensions;
	device_create_info.ppEnabledExtensionNames = ctx.get_enabled_device_features().device_extensions;
	device_create_info.queueCreateInfoCount = 1;
	device_create_info.pQueueCreateInfos = &queue_info;

	// Hand over a concrete VkQueue we want implementation to use.
	pyrowave_device_create_queue_info device_queue_info = {};
	device_queue_info.familyIndex = queue_info.queueFamilyIndex;
	device_queue_info.index = 0;
	device_queue_info.queue = device.get_queue_info().queues[QUEUE_INDEX_GRAPHICS];

	pyrowave_device_create_info info = {};
	info.GetInstanceProcAddr = vkGetInstanceProcAddr;
	info.instance = ctx.get_instance();
	info.physical_device = ctx.get_gpu();
	info.device = ctx.get_device();
	info.device_create_info = &device_create_info;
	info.instance_create_info = &instance_create_info;
	info.queue_info_count = 1;
	info.queue_info = &device_queue_info;
	info.userdata = &device;
	info.queue_lock_callback = [](void *userdata) { static_cast<Device *>(userdata)->external_queue_lock(); };
	info.queue_unlock_callback = [](void *userdata) { static_cast<Device *>(userdata)->external_queue_unlock(); };

	pyrowave_encoder encoder;
	pyrowave_decoder decoder;
	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device(&info, &pyro_device));

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = pyro_device;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	encoder_info.width = 64;
	encoder_info.height = 64;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	decoder_info.width = 64;
	decoder_info.height = 64;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	uint8_t plane_data[3][64][64];

	for (int y = 0; y < 64; y++)
	{
		for (int x = 0; x < 64; x++)
		{
			plane_data[0][y][x] = y + x + 1;
			plane_data[1][y][x] = y + x + 2;
			plane_data[2][y][x] = y + x + 3;
		}
	}

	auto image_info = ImageCreateInfo::immutable_2d_image(64, 64, VK_FORMAT_R8_UNORM);
	image_info.layers = 3;
	image_info.initial_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
	ImageInitialData initial_data[3] = { { plane_data[0] }, { plane_data[1] }, { plane_data[2] } };
	auto input_image = device.create_image(image_info, initial_data);
	ASSERT_THAT(input_image);

	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	image_info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
	auto output_image = device.create_image(image_info);
	ASSERT_THAT(output_image);

	pyrowave_rate_control rate_control = { 100000 };
	pyrowave_gpu_buffers gpu_buffers = {};

	for (int i = 0; i < 3; i++)
	{
		gpu_buffers.planes[i].image = input_image->get_image();
		gpu_buffers.planes[i].width = 64;
		gpu_buffers.planes[i].height = 64;
		gpu_buffers.planes[i].image_format = VK_FORMAT_R8_UNORM;
		gpu_buffers.planes[i].view_format = VK_FORMAT_R8_UNORM;
		gpu_buffers.planes[i].layer = i;
		gpu_buffers.planes[i].layout = input_image->get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
		gpu_buffers.planes[i].mip_level = 0;
		gpu_buffers.planes[i].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	auto cmd = device.request_command_buffer();

	// Encode to provided cmd.
	// Redirect commands here.
	pyrowave_device_set_command_buffer(pyro_device, cmd->get_command_buffer());
	CHECKED(pyrowave_encoder_encode_gpu_synchronous(encoder, nullptr, nullptr, &gpu_buffers, &rate_control));
	pyrowave_device_set_command_buffer(pyro_device, VK_NULL_HANDLE);

	// Wait on CPU before we call packetization.
	Fence fence;
	device.submit(cmd, &fence);
	fence->wait();

	size_t num_packets;
	CHECKED(pyrowave_encoder_compute_num_packets(encoder, rate_control.maximum_bitstream_size, &num_packets));
	ASSERT_THAT(num_packets == 1);

	std::unique_ptr<uint8_t[]> bitstream(new uint8_t[rate_control.maximum_bitstream_size]);
	pyrowave_packet packet;
	CHECKED(pyrowave_encoder_packetize(encoder, &packet, rate_control.maximum_bitstream_size, &num_packets,
		bitstream.get(), rate_control.maximum_bitstream_size));

	CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.get() + packet.offset, packet.size));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));

	for (auto &plane : gpu_buffers.planes)
	{
		plane.image = output_image->get_image();
		plane.layout = VK_IMAGE_LAYOUT_GENERAL;
	}

	cmd = device.request_command_buffer();
	// Redirect commands here.
	pyrowave_device_set_command_buffer(pyro_device, cmd->get_command_buffer());
	CHECKED(pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &gpu_buffers));
	pyrowave_device_set_command_buffer(pyro_device, VK_NULL_HANDLE);

	cmd->image_barrier(*output_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

	BufferCreateInfo bufinfo = {};
	bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufinfo.size = 64 * 64 * 3;
	bufinfo.domain = BufferDomain::CachedHost;

	auto readback_buffer = device.create_buffer(bufinfo);
	cmd->copy_image_to_buffer(*readback_buffer, *output_image, 0,  {}, { 64, 64, 1 }, 0, 0,
		{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 3 });
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	fence.reset();
	device.submit(cmd, &fence);
	fence->wait();

	auto *readback_ptr = static_cast<const uint8_t *>(device.map_host_buffer(*readback_buffer, MEMORY_ACCESS_READ_BIT));

	for (int y = 0; y < 64; y++)
	{
		for (int x = 0; x < 64; x++)
		{
			int y_delta = std::abs(readback_ptr[0 * 64 * 64 + y * 64 + x] - (y + x + 1));
			int cb_delta = std::abs(readback_ptr[1 * 64 * 64 + y * 64 + x] - (y + x + 2));
			int cr_delta = std::abs(readback_ptr[2 * 64 * 64 + y * 64 + x] - (y + x + 3));
			ASSERT_THAT(y_delta <= 1);
			ASSERT_THAT(cb_delta <= 1);
			ASSERT_THAT(cr_delta <= 1);
		}
	}

	pyrowave_encoder_destroy(encoder);
	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(pyro_device);
}

static void test_direct_interop_scaling()
{
	ASSERT_THAT(Context::init_loader(nullptr));

	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.apiVersion = VK_API_VERSION_1_3;
	app_info.pApplicationName = "pyrowave-c-test";
	app_info.pEngineName = "Granite";
	ctx.set_application_info(&app_info);

	ASSERT_THAT(ctx.init_instance_and_device(nullptr, 0, nullptr, 0));

	Device device;
	device.set_context(ctx);

	bool has_rdoc = Device::init_renderdoc_capture();
	if (has_rdoc)
		device.begin_renderdoc_capture();

	// Fill in a proxy instance create info.
	VkInstanceCreateInfo instance_create_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_create_info.enabledExtensionCount = device.get_device_features().num_instance_extensions;
	instance_create_info.ppEnabledExtensionNames = device.get_device_features().instance_extensions;
	instance_create_info.pApplicationInfo = &ctx.get_application_info();

	// Fill in a proxy device create info.
	VkDeviceCreateInfo device_create_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue_info.queueFamilyIndex = device.get_queue_info().family_indices[QUEUE_INDEX_GRAPHICS];
	queue_info.queueCount = 1;
	device_create_info.pNext = ctx.get_enabled_device_features().pdf2;
	device_create_info.enabledExtensionCount = ctx.get_enabled_device_features().num_device_extensions;
	device_create_info.ppEnabledExtensionNames = ctx.get_enabled_device_features().device_extensions;
	device_create_info.queueCreateInfoCount = 1;
	device_create_info.pQueueCreateInfos = &queue_info;

	// Hand over a concrete VkQueue we want implementation to use.
	pyrowave_device_create_queue_info device_queue_info = {};
	device_queue_info.familyIndex = queue_info.queueFamilyIndex;
	device_queue_info.index = 0;
	device_queue_info.queue = device.get_queue_info().queues[QUEUE_INDEX_GRAPHICS];

	pyrowave_device_create_info info = {};
	info.GetInstanceProcAddr = vkGetInstanceProcAddr;
	info.instance = ctx.get_instance();
	info.physical_device = ctx.get_gpu();
	info.device = ctx.get_device();
	info.device_create_info = &device_create_info;
	info.instance_create_info = &instance_create_info;
	info.queue_info_count = 1;
	info.queue_info = &device_queue_info;
	info.userdata = &device;
	info.queue_lock_callback = [](void *userdata) { static_cast<Device *>(userdata)->external_queue_lock(); };
	info.queue_unlock_callback = [](void *userdata) { static_cast<Device *>(userdata)->external_queue_unlock(); };

	pyrowave_encoder encoder;
	pyrowave_decoder decoder;
	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device(&info, &pyro_device));

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = pyro_device;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	encoder_info.width = 64;
	encoder_info.height = 64;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_444;
	decoder_info.width = 64;
	decoder_info.height = 64;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	uint32_t plane_data[66][67];

	for (int y = 0; y < 66; y++)
	{
		for (int x = 0; x < 67; x++)
		{
#if 1
			int r = mirror(128 + y * 3 + x * 1);
			int g = mirror(128 + y * 5 + x * 3);
			int b = mirror(128 + y * 7 + x * 5);
#else
			int r = 128;
			int g = 128;
			int b = 128;
#endif
			plane_data[y][x] = r | (g << 8) | (b << 16);
		}
	}

	auto image_info = ImageCreateInfo::immutable_2d_image(67, 66, VK_FORMAT_R8G8B8A8_SRGB);
	image_info.initial_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
	image_info.misc = IMAGE_MISC_MUTABLE_SRGB_BIT;
	ImageInitialData initial_data = { plane_data };
	auto input_image = device.create_image(image_info, &initial_data);
	ASSERT_THAT(input_image);

	image_info.format = VK_FORMAT_R16_UNORM;
	image_info.width = 64;
	image_info.height = 64;
	image_info.misc = 0;
	image_info.layers = 3;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	image_info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
	auto output_image = device.create_image(image_info);
	ASSERT_THAT(output_image);

	pyrowave_rate_control rate_control = { 500000 };
	pyrowave_gpu_buffers gpu_buffers = {};

	pyrowave_image_view view = {};

	view.image = input_image->get_image();
	view.width = input_image->get_width();
	view.height = input_image->get_height();
	view.image_format = input_image->get_format();
	view.view_format = VK_FORMAT_R8G8B8A8_UNORM;
	view.layer = 0;
	view.layout = input_image->get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
	view.mip_level = 0;
	view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

	auto cmd = device.request_command_buffer();

	// Encode to provided cmd.
	// Redirect commands here.
	pyrowave_scaled_encode_info scaling = {};
	scaling.intermediate_plane_format = VK_FORMAT_R16_UNORM;
	scaling.view = view;
	scaling.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaling.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaling.ycbcr_chroma_midpoint = 130.0f / 255.0f;
	scaling.force_linear_filtering = true;
	VkRect2D crop_rect = { { 2, 1 }, { 64, 64 } };
	scaling.crop_rect = &crop_rect;

	pyrowave_device_set_command_buffer(pyro_device, cmd->get_command_buffer());
	CHECKED(pyrowave_encoder_encode_gpu_scaled_synchronous(encoder, nullptr, nullptr, &scaling, &rate_control));
	pyrowave_device_set_command_buffer(pyro_device, VK_NULL_HANDLE);

	// Wait on CPU before we call packetization.
	Fence fence;
	device.submit(cmd, &fence);
	fence->wait();

	size_t num_packets;
	CHECKED(pyrowave_encoder_compute_num_packets(encoder, rate_control.maximum_bitstream_size, &num_packets));
	ASSERT_THAT(num_packets == 1);

	std::unique_ptr<uint8_t[]> bitstream(new uint8_t[rate_control.maximum_bitstream_size]);
	pyrowave_packet packet;
	CHECKED(pyrowave_encoder_packetize(encoder, &packet, rate_control.maximum_bitstream_size, &num_packets,
		bitstream.get(), rate_control.maximum_bitstream_size));

	CHECKED(pyrowave_decoder_push_packet(decoder, bitstream.get() + packet.offset, packet.size));
	ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));

	for (auto &plane : gpu_buffers.planes)
	{
		plane.image = output_image->get_image();
		plane.layout = VK_IMAGE_LAYOUT_GENERAL;
		plane.width = 64;
		plane.height = 64;
		plane.image_format = VK_FORMAT_R16_UNORM;
		plane.view_format = VK_FORMAT_R16_UNORM;
		plane.layer = &plane - gpu_buffers.planes;
		plane.layout = input_image->get_layout(VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
		plane.mip_level = 0;
		plane.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	}

	cmd = device.request_command_buffer();
	// Redirect commands here.
	pyrowave_device_set_command_buffer(pyro_device, cmd->get_command_buffer());
	CHECKED(pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &gpu_buffers));
	pyrowave_device_set_command_buffer(pyro_device, VK_NULL_HANDLE);

	cmd->image_barrier(*output_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
	                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

	BufferCreateInfo bufinfo = {};
	bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufinfo.size = 64 * 64 * 3 * sizeof(uint16_t);
	bufinfo.domain = BufferDomain::CachedHost;

	auto readback_buffer = device.create_buffer(bufinfo);
	cmd->copy_image_to_buffer(*readback_buffer, *output_image, 0,  {}, { 64, 64, 1 }, 0, 0,
		{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 3 });
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
	             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	fence.reset();
	device.submit(cmd, &fence);
	fence->wait();

	auto *readback_ptr = static_cast<const uint16_t *>(device.map_host_buffer(*readback_buffer, MEMORY_ACCESS_READ_BIT));

#if 1
	for (int y = 0; y < 64; y++)
	{
		for (int x = 0; x < 64; x++)
		{
#if 1
			auto r = float(mirror(128 + (y + 1) * 3 + (x + 2) * 1)) / 255.0f;
			auto g = float(mirror(128 + (y + 1) * 5 + (x + 2) * 3)) / 255.0f;
			auto b = float(mirror(128 + (y + 1) * 7 + (x + 2) * 5)) / 255.0f;
#else
			float r = 128.0f / 255.0f;
			float g = 128.0f / 255.0f;
			float b = 128.0f / 255.0f;
#endif

			auto Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			auto Cb = 130.0f / 255.0f - 0.114572f * r - 0.385428f * g + 0.5f * b;
			auto Cr = 130.0f / 255.0f + 0.5f * r - 0.454153f * g - 0.0458471f * b;

			float readback_y = float(readback_ptr[0 * 64 * 64 + y * 64 + x]) / float(0xffff);
			float readback_cb = float(readback_ptr[1 * 64 * 64 + y * 64 + x]) / float(0xffff);
			float readback_cr = float(readback_ptr[2 * 64 * 64 + y * 64 + x]) / float(0xffff);

			float y_delta = std::abs(readback_y - Y);
			float cb_delta = std::abs(readback_cb - Cb);
			float cr_delta = std::abs(readback_cr - Cr);
			ASSERT_THAT(y_delta <= 1.0f / 255.0f);
			ASSERT_THAT(cb_delta <= 2.0f / 255.0f);
			ASSERT_THAT(cr_delta <= 2.0f / 255.0f);
		}
	}
#endif

	pyrowave_encoder_destroy(encoder);
	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(pyro_device);

	if (has_rdoc)
		device.end_renderdoc_capture();
}

// Most basic interop scenario, OPAQUE_FD for everything.
static void test_opaque_interop(bool win32_kmt)
{
	ASSERT_THAT(Context::init_loader(nullptr));

	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});
	ASSERT_THAT(ctx.init_instance_and_device(nullptr, 0, nullptr, 0));

	Device device;
	device.set_context(ctx);

	pyrowave_device pyro_device = create_device_from_granite(device);

	auto semaphore_type = win32_kmt ?
		VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT : ExternalHandle::get_opaque_semaphore_handle_type();
	auto memory_type = win32_kmt ?
		VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT : ExternalHandle::get_opaque_memory_handle_type();

	// No impl seems to support OPAQUE_KMT timelines.
	auto timeline_sem = device.request_semaphore_external(VK_SEMAPHORE_TYPE_TIMELINE, ExternalHandle::get_opaque_semaphore_handle_type());

	ASSERT_THAT(timeline_sem);
	pyrowave_sync_object imported_timeline = create_sync_object_from_timeline(pyro_device, *timeline_sem);

	auto exportable_nv12_image = create_exportable_test_image(
		device, memory_type,
		VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	auto exportable_yuv420p_image = create_exportable_test_image(
		device, memory_type,
		VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM);

	pyrowave_image imported_nv12_image = create_imported_image(pyro_device, device, *exportable_nv12_image);
	pyrowave_image imported_yuv420p_image = create_imported_image(pyro_device, device, *exportable_yuv420p_image);

	auto binary_sem = device.request_semaphore_external(VK_SEMAPHORE_TYPE_BINARY, semaphore_type);
	ASSERT_THAT(binary_sem);
	device.submit_empty(CommandBuffer::Type::Generic, nullptr, binary_sem.get());
	pyrowave_sync_object imported_binary = create_sync_object_from_binary(pyro_device, *binary_sem);

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = pyro_device;
	encoder_info.width = 1280;
	encoder_info.height = 720;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	// Test the two ways we can send a sync payload, binary + temporary or persistent timeline.
	// Verify it doesn't explode.
	send_granite_image_to_encoder(device, *exportable_nv12_image, imported_nv12_image,
	                              *timeline_sem, imported_timeline, 1,
	                              imported_timeline, 2,
	                              encoder);

	send_granite_image_to_encoder(device, *exportable_nv12_image, imported_nv12_image,
	                              *timeline_sem, imported_binary, 0,
	                              imported_timeline, 3,
	                              encoder);

	// Pyrowave (or rather, Granite) API destroys objects in a deferred way.
	pyrowave_sync_object_destroy(imported_binary);

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.width = 1280;
	decoder_info.height = 720;
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_decoder decoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	send_payload_to_decoder(encoder, decoder);
	decode_image(&imported_yuv420p_image, true, imported_timeline, 4, decoder);
	validate_granite_image(device, *exportable_yuv420p_image, *timeline_sem, 4);

	pyrowave_sync_object_destroy(imported_timeline);
	pyrowave_image_destroy(imported_nv12_image);
	pyrowave_image_destroy(imported_yuv420p_image);
	pyrowave_encoder_destroy(encoder);
	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(pyro_device);
}

static void test_drm_modifier_interop()
{
	ASSERT_THAT(Context::init_loader(nullptr));

	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});
	ASSERT_THAT(ctx.init_instance_and_device(nullptr, 0, nullptr, 0));

	Device device;
	device.set_context(ctx);

	if (!device.get_device_features().supports_drm_modifiers)
	{
		printf("Device does not support DRM modifiers, skipping test ...\n");
		return;
	}

	pyrowave_device pyro_device = create_device_from_granite(device);

	auto timeline_sem = device.request_semaphore_external(VK_SEMAPHORE_TYPE_TIMELINE,
		ExternalHandle::get_opaque_semaphore_handle_type());
	ASSERT_THAT(timeline_sem);
	pyrowave_sync_object imported_timeline = create_sync_object_from_timeline(pyro_device, *timeline_sem);

	auto exportable_nv12_image = create_exportable_test_image(
		device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	auto exportable_yuv420p_image = create_exportable_test_image(
		device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM);

	pyrowave_image imported_nv12_image = create_imported_image(pyro_device, device, *exportable_nv12_image);
	pyrowave_image imported_yuv420p_image = create_imported_image(pyro_device, device, *exportable_yuv420p_image);

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = pyro_device;
	encoder_info.width = 1280;
	encoder_info.height = 720;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	// Test the two ways we can send a sync payload, binary + temporary or persistent timeline.
	// Verify it doesn't explode.
	send_granite_image_to_encoder(device, *exportable_nv12_image, imported_nv12_image,
	                              *timeline_sem, imported_timeline, 1,
	                              imported_timeline, 2,
	                              encoder);

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.width = 1280;
	decoder_info.height = 720;
	decoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	pyrowave_decoder decoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	send_payload_to_decoder(encoder, decoder);
	decode_image(&imported_yuv420p_image, true, imported_timeline, 3, decoder);
	validate_granite_image(device, *exportable_yuv420p_image, *timeline_sem, 3);

	pyrowave_sync_object_destroy(imported_timeline);
	pyrowave_image_destroy(imported_nv12_image);
	pyrowave_image_destroy(imported_yuv420p_image);
	pyrowave_encoder_destroy(encoder);
	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(pyro_device);
}

#ifdef _WIN32

static VkFormat convert_dxgi_format(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_NV12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
	default: ASSERT_THAT(0 && "augment as needed");
	}

	return VK_FORMAT_UNDEFINED;
}

static pyrowave_image create_pyrowave_image_from_d3d12(pyrowave_device pyro_device, ID3D12Device *device, ID3D12Resource *resource, bool storage)
{
	HANDLE shared_handle;
	CHECK_HRESULT(device->CreateSharedHandle(resource, nullptr, GENERIC_ALL, nullptr, &shared_handle));

	auto desc = resource->GetDesc();

	VkImageCreateInfo image_create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.extent = { uint32_t(desc.Width), desc.Height, 1u };
	image_create_info.mipLevels = desc.MipLevels;
	// MUTABLE is needed since we will take plane views. Extended usage since the base planar format doesn't support STORAGE.
	image_create_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	// The usage flags don't matter that much.
	image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (storage)
		image_create_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

	image_create_info.format = convert_dxgi_format(desc.Format);
	image_create_info.samples = static_cast<VkSampleCountFlagBits>(desc.SampleDesc.Count);
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.arrayLayers = desc.DepthOrArraySize;

	pyrowave_image_create_info info = {};
	info.device = pyro_device;
	info.external_handle = (pyrowave_os_handle)shared_handle;
	info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
	info.image_create_info = &image_create_info;

	pyrowave_image image;
	CHECKED(pyrowave_image_create(&info, &image));
	return image;
}

static pyrowave_image create_pyrowave_image_from_d3d11(pyrowave_device pyro_device, ID3D11Texture2D *resource, bool kmt)
{
	HANDLE shared_handle;
	ComPtr<IDXGIResource1> res;
	resource->QueryInterface(IID_IDXGIResource1, res.ppv());

	if (kmt)
	{
		CHECK_HRESULT(res->GetSharedHandle(&shared_handle));
	}
	else
	{
		CHECK_HRESULT(res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_handle));
	}

	D3D11_TEXTURE2D_DESC desc;
	resource->GetDesc(&desc);

	VkImageCreateInfo image_create_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_create_info.imageType = VK_IMAGE_TYPE_2D;
	image_create_info.extent = { uint32_t(desc.Width), desc.Height, 1u };
	image_create_info.mipLevels = desc.MipLevels;
	// MUTABLE is needed since we will take plane views. Extended usage since the base planar format doesn't support STORAGE.
	image_create_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	// The usage flags don't matter that much.
	image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	image_create_info.format = convert_dxgi_format(desc.Format);
	image_create_info.samples = static_cast<VkSampleCountFlagBits>(desc.SampleDesc.Count);
	image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_create_info.arrayLayers = desc.ArraySize;

	pyrowave_image_create_info info = {};
	info.device = pyro_device;
	info.external_handle = (pyrowave_os_handle)shared_handle;
	info.handle_type = kmt ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	info.image_create_info = &image_create_info;

	pyrowave_image image;
	CHECKED(pyrowave_image_create(&info, &image));
	return image;
}

static pyrowave_sync_object create_pyrowave_sync_from_d3d12_handle(pyrowave_device device, HANDLE handle)
{
	pyrowave_sync_object_create_info info = {};
	info.device = device;
	info.external_handle = (pyrowave_os_handle)handle;
	// D3D11 fence is aliased with D3D12 fence, it's the same thing in Windows 10+.
	info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
	// Technically it's BINARY in the spec since this sharing API predated timeline, but it gets fixed up by Granite as needed.
	info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
	
	pyrowave_sync_object sync;
	CHECKED(pyrowave_sync_object_create(&info, &sync));
	return sync;
}

static void upload_mirror_image(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
	UINT subresource, ComPtr<ID3D12Resource> &staging,
	int dx0, int dy0, int dx1, int dy1)
{
	auto desc = resource->GetDesc();
	UINT64 total_bytes = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
	ASSERT_THAT(total_bytes != UINT64_MAX);

	D3D12_RESOURCE_DESC staging_desc = {};
	staging_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	staging_desc.Width = total_bytes;
	staging_desc.Height = 1;
	staging_desc.DepthOrArraySize = 1;
	staging_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	staging_desc.MipLevels = 1;
	staging_desc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
	CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &staging_desc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_ID3D12Resource, staging.ppv()));

	uint8_t *ptr;
	CHECK_HRESULT(staging->Map(0, nullptr, reinterpret_cast<void **>(&ptr)));

	for (int y = 0; y < int(footprint.Footprint.Height); y++)
	{
		for (int x = 0; x < int(footprint.Footprint.Width); x++)
		{
			if (subresource == 0)
			{
				ptr[x] = mirror(y * dy0 + x * dx0);
			}
			else
			{
				ptr[2 * x + 0] = mirror(y * dy0 + x * dx0);
				ptr[2 * x + 1] = mirror(y * dy1 + x * dx1);
			}
		}

		ptr += footprint.Footprint.RowPitch;
	}

	staging->Unmap(0, nullptr);

	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = resource;
	dst.SubresourceIndex = subresource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	src.PlacedFootprint = footprint;
	src.pResource = staging.get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

static ComPtr<ID3D12Resource> readback_image(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12Resource *resource, UINT subresource, UINT64 *row_pitch)
{
	auto desc = resource->GetDesc();
	UINT64 total_bytes = 0;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
	ASSERT_THAT(total_bytes != UINT64_MAX);

	D3D12_RESOURCE_DESC staging_desc = {};
	staging_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	staging_desc.Width = total_bytes;
	staging_desc.Height = 1;
	staging_desc.DepthOrArraySize = 1;
	staging_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	staging_desc.MipLevels = 1;
	staging_desc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_READBACK;

	ComPtr<ID3D12Resource> staging;
	CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &staging_desc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource, staging.ppv()));

	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	src.pResource = resource;
	src.SubresourceIndex = subresource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	dst.PlacedFootprint = footprint;
	dst.pResource = staging.get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	*row_pitch = footprint.Footprint.RowPitch;

	return staging;
}

static void validate_d3d12_image(ID3D12Resource *readback, int width, int height, int row_stride, int dx, int dy)
{
	uint8_t *ptr;
	CHECK_HRESULT(readback->Map(0, nullptr, reinterpret_cast<void **>(&ptr)));
	validate_mirror_buffer(ptr, width, height, row_stride, dx, dy);
	readback->Unmap(0, nullptr);
}

static void test_nv12_interop()
{
	// NV12 layout on NVIDIA is bizarre and requires us to hack around it when we import it in Vulkan.
	// Smoke-test for debugging any interop issues.
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12CommandAllocator> allocator;
	uint64_t timeline = 0;

	CHECK_HRESULT(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device.ppv()));
	CHECK_HRESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, allocator.ppv()));
	CHECK_HRESULT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_ID3D12GraphicsCommandList, list.ppv()));
	// Base API create command list starts a new command list, which we usually need to close right away ...
	CHECK_HRESULT(list->Close());
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK_HRESULT(device->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue, queue.ppv()));

	CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.ppv()));

	LUID luid = device->GetAdapterLuid();
	static_assert(sizeof(luid) == sizeof(pyrowave_luid), "LUID struct size does not match.");

	Context context;
	Device dev;

	ASSERT_THAT(Context::init_loader(nullptr));
	context.set_num_thread_indices(1);
	context.set_system_handles({});

	// Just enable video extensions so that we can use video image usage, but don't bother creating queues for it, etc.
	ASSERT_THAT(context.init_instance(nullptr, 0, CONTEXT_CREATION_ENABLE_VIDEO_FEATURE_ONLY_BIT));
	uint32_t count;
	ASSERT_THAT(vkEnumeratePhysicalDevices(context.get_instance(), &count, nullptr) == VK_SUCCESS);
	std::vector<VkPhysicalDevice> gpus(count);
	ASSERT_THAT(vkEnumeratePhysicalDevices(context.get_instance(), &count, gpus.data()) == VK_SUCCESS);

	VkPhysicalDevice selected_gpu = VK_NULL_HANDLE;

	for (auto &gpu : gpus)
	{
		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(gpu, &props);
		// Is this even possible these days?
		if (props.apiVersion < VK_API_VERSION_1_2)
			continue;

		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
		vkGetPhysicalDeviceProperties2(gpu, &props2);

		if (!ids.deviceLUIDValid)
			continue;
		if (memcmp(&luid, ids.deviceLUID, VK_LUID_SIZE) != 0)
			continue;

		ASSERT_THAT(context.init_device(gpu, VK_NULL_HANDLE, nullptr, 0, CONTEXT_CREATION_ENABLE_VIDEO_FEATURE_ONLY_BIT));
		selected_gpu = gpu;
	}

	ASSERT_THAT(selected_gpu);
	dev.set_context(context);

	// NV Windows is quite broken here and no matter what we do,
	// it will only work for very specific resource sizes it seems, even with the video usage hacks ...
	constexpr uint32_t Width = 1024;
	constexpr uint32_t Height = 1024;

	D3D12_RESOURCE_DESC resource_desc = {};
	resource_desc.Width = Width;
	resource_desc.Height = Height;
	resource_desc.DepthOrArraySize = 1;
	resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resource_desc.SampleDesc.Count = 1;
	resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resource_desc.MipLevels = 1;

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	ComPtr<ID3D12Resource> nv12;
	resource_desc.Format = DXGI_FORMAT_NV12;
	CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, nv12.ppv()));

	// Upload frame to NV12 image async.
	list->Reset(allocator.get(), nullptr);
	ComPtr<ID3D12Resource> staging_buffers[2]; // Verify that sync works somewhat so defer destroy these.
	upload_mirror_image(device.get(), list.get(), nv12.get(), 0, staging_buffers[0], 5, 3, 0, 0);
	upload_mirror_image(device.get(), list.get(), nv12.get(), 1, staging_buffers[1], 3, 1, 5, 7);
	list->Close();

	ID3D12CommandList *lists[] = { list.get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.get(), ++timeline);
	fence->SetEventOnCompletion(timeline, nullptr);

	allocator->Reset();
	list->Reset(allocator.get(), nullptr);
	ComPtr<ID3D12Resource> readback_buffer;
	UINT64 row_pitch = {};
	readback_buffer = readback_image(device.get(), list.get(), nv12.get(), 1, &row_pitch);
	list->Close();
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.get(), ++timeline);
	fence->SetEventOnCompletion(timeline, nullptr);

	auto img_info = ImageCreateInfo::immutable_2d_image(Width, Height, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR;
	img_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_VIDEO_PROFILE_INDEPENDENT_BIT_KHR;
	img_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	img_info.misc = IMAGE_MISC_EXTERNAL_MEMORY_BIT | IMAGE_MISC_NO_DEFAULT_VIEWS_BIT;
	CHECK_HRESULT(device->CreateSharedHandle(nv12.get(), nullptr, GENERIC_ALL, nullptr, &img_info.external.handle));
	img_info.external.memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
	auto img = dev.create_image(img_info);
	ASSERT_THAT(img);

	BufferCreateInfo bufinfo = {};
	bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufinfo.domain = BufferDomain::CachedHost;
	bufinfo.size = Width * Height / 2;
	auto buffer = dev.create_buffer(bufinfo);

	auto cmd = dev.request_command_buffer();
	cmd->acquire_image_barrier(*img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	cmd->copy_image_to_buffer(*buffer, *img, 0, {}, { Width / 2, Height / 2, 1 }, Width / 2, Height / 2, { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 });
	cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	Fence sync;
	dev.submit(cmd, &sync);
	sync->wait();

	uint8_t *d3d12_ptr;
	CHECK_HRESULT(readback_buffer->Map(0, nullptr, (void **)&d3d12_ptr));

	const auto *ptr = static_cast<uint8_t *>(dev.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));

	for (uint32_t y = 0; y < Height / 2; y++)
	{
		for (uint32_t x = 0; x < Width / 2; x++)
		{
			uint32_t pix = y * Width / 2 + x;
			ASSERT_THAT(ptr[2 * pix + 0] == d3d12_ptr[y * row_pitch + 2 * x + 0]);
			ASSERT_THAT(ptr[2 * pix + 1] == d3d12_ptr[y * row_pitch + 2 * x + 1]);
		}
	}

	readback_buffer->Unmap(0, nullptr);
}

static void test_d3d12_interop_allocation_stress(ID3D12Device *device, pyrowave_device pyro_device)
{
	D3D12_RESOURCE_DESC resource_desc = {};
	resource_desc.Width = 4096;
	resource_desc.Height = 4096;
	resource_desc.DepthOrArraySize = 1;
	resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resource_desc.SampleDesc.Count = 1;
	resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resource_desc.MipLevels = 1;

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	for (int i = 0; i < 10000; i++)
	{
		ComPtr<ID3D12Resource> img;
		resource_desc.Format = DXGI_FORMAT_R8_UNORM;
		CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, img.ppv()));
		pyrowave_image pyro_img = create_pyrowave_image_from_d3d12(pyro_device, device, img.get(), true);

		// Check to see if destruction order matters.

		if (i % 2)
		{
			pyrowave_image_destroy(pyro_img);
			img = {};
		}
		else
		{
			img = {};
			pyrowave_image_destroy(pyro_img);
		}
	}

	for (int i = 0; i < 10000; i++)
	{
		// Sharing D3D12 to Vulkan is well supported. Other way around, not so much.
		ComPtr<ID3D12Fence> fence;
		CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_ID3D12Fence, fence.ppv()));
		HANDLE fence_handle;
		CHECK_HRESULT(device->CreateSharedHandle(fence.get(), nullptr, GENERIC_ALL, nullptr, &fence_handle));

		pyrowave_sync_object pyro_sync = create_pyrowave_sync_from_d3d12_handle(pyro_device, fence_handle);

		// Check to see if destruction order matters.
		if (i % 2)
		{
			pyrowave_sync_object_destroy(pyro_sync);
			fence = {};
		}
		else
		{
			fence = {};
			pyrowave_sync_object_destroy(pyro_sync);
		}
	}
}

static void test_d3d11_interop()
{
	uint32_t index = 0;
	if (const char *env = getenv("ADAPTER"))
		index = strtoul(env, nullptr, 0);

	bool validate = false;
	if (const char *env = getenv("VALIDATE"))
		validate = strtoul(env, nullptr, 0) != 0;

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11DeviceContext4> context4;
	ComPtr<ID3D11Fence> fence;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(index, (IDXGIAdapter **)adapter.ppv()));

	HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, validate ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION,
		(ID3D11Device **)device.ppv(), nullptr, (ID3D11DeviceContext **)context.ppv());
	ASSERT_THAT(SUCCEEDED(hr));
	CHECK_HRESULT(device->QueryInterface(IID_ID3D11Device5, device5.ppv()));

	DXGI_ADAPTER_DESC adapter_desc;
	CHECK_HRESULT(adapter->GetDesc(&adapter_desc));
	LUID luid = adapter_desc.AdapterLuid;
	static_assert(sizeof(luid) == sizeof(pyrowave_luid), "LUID struct size does not match.");

	CHECK_HRESULT(context->QueryInterface(IID_ID3D11DeviceContext4, context4.ppv()));

	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr,
		reinterpret_cast<pyrowave_luid *>(&luid), &pyro_device));

	for (int i = 0; i < 10000; i++)
	{
		ComPtr<ID3D11Texture2D> tex;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		desc.SampleDesc.Count = 1;
		desc.Width = 4096;
		desc.Height = 4096;
		desc.ArraySize = 1;
		desc.MipLevels = 1;
		CHECK_HRESULT(device5->CreateTexture2D(&desc, nullptr, (ID3D11Texture2D **)tex.ppv()));

		pyrowave_image pyro_img = create_pyrowave_image_from_d3d11(pyro_device, tex.get(), false);

		// Check to see if destruction order matters.
		if (i % 2)
		{
			pyrowave_image_destroy(pyro_img);
			fence = {};
		}
		else
		{
			fence = {};
			pyrowave_image_destroy(pyro_img);
		}

		// Flushing seems to be load bearing here or GPU memory usage baloons out of control.
		context->Flush();
	}

	for (int i = 0; i < 10000; i++)
	{
		ComPtr<ID3D11Texture2D> tex;
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
		desc.SampleDesc.Count = 1;
		desc.Width = 4096;
		desc.Height = 4096;
		desc.ArraySize = 1;
		desc.MipLevels = 1;
		CHECK_HRESULT(device5->CreateTexture2D(&desc, nullptr, (ID3D11Texture2D **)tex.ppv()));

		pyrowave_image pyro_img = create_pyrowave_image_from_d3d11(pyro_device, tex.get(), true);

		// Check to see if destruction order matters.
		if (i % 2)
		{
			pyrowave_image_destroy(pyro_img);
			fence = {};
		}
		else
		{
			fence = {};
			pyrowave_image_destroy(pyro_img);
		}

		// Flushing seems to be load bearing here or GPU memory usage baloons out of control.
		context->Flush();
	}

	for (int i = 0; i < 10000; i++)
	{
		// Sharing D3D11 to Vulkan is well supported. Other way around, not so much.
		ComPtr<ID3D11Fence> share_fence;
		CHECK_HRESULT(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, share_fence.ppv()));
		HANDLE fence_handle;
		CHECK_HRESULT(share_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle));

		context4->Signal(share_fence.get(), 1);
		share_fence = {};

		pyrowave_sync_object pyro_sync = create_pyrowave_sync_from_d3d12_handle(pyro_device, fence_handle);

		// Check to see if destruction order matters.
		if (i % 2)
		{
			pyrowave_sync_object_destroy(pyro_sync);
			fence = {};
		}
		else
		{
			fence = {};
			pyrowave_sync_object_destroy(pyro_sync);
		}

		context->Flush();
	}

	pyrowave_device_destroy(pyro_device);
}

static void test_d3d11_texture_layout()
{
	uint32_t index = 0;
	if (const char *env = getenv("ADAPTER"))
		index = strtoul(env, nullptr, 0);

	bool validate = false;
	if (const char *env = getenv("VALIDATE"))
		validate = strtoul(env, nullptr, 0) != 0;

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11DeviceContext4> context4;
	ComPtr<ID3D11Fence> fence;
	uint64_t timeline = 0;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(index, (IDXGIAdapter **)adapter.ppv()));

	HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, validate ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION,
		(ID3D11Device **)device.ppv(), nullptr, (ID3D11DeviceContext **)context.ppv());
	ASSERT_THAT(SUCCEEDED(hr));
	CHECK_HRESULT(device->QueryInterface(IID_ID3D11Device5, device5.ppv()));

	DXGI_ADAPTER_DESC adapter_desc;
	CHECK_HRESULT(adapter->GetDesc(&adapter_desc));
	LUID luid = adapter_desc.AdapterLuid;

	CHECK_HRESULT(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_ID3D11Fence, fence.ppv()));
	CHECK_HRESULT(context->QueryInterface(IID_ID3D11DeviceContext4, context4.ppv()));

	ASSERT_THAT(Context::init_loader(nullptr));
	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});
	ASSERT_THAT(ctx.init_instance(nullptr, 0));

	VkPhysicalDevice gpus[64];
	uint32_t gpu_count = 64;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	vkEnumeratePhysicalDevices(ctx.get_instance(), &gpu_count, gpus);
	for (uint32_t i = 0; i < gpu_count; i++)
	{
		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
		vkGetPhysicalDeviceProperties2(gpus[i], &props2);
		if (memcmp(ids.deviceLUID, &luid, sizeof(luid)) == 0)
		{
			gpu = gpus[i];
			break;
		}
	}

	ASSERT_THAT(gpu);
	ASSERT_THAT(ctx.init_device(gpu, VK_NULL_HANDLE, nullptr, 0));
	Device vk_device;
	vk_device.set_context(ctx);

	bool has_failure = false;

	for (uint32_t height = 32; height < 2048; height += 31)
	{
		for (uint32_t width = 32; width < 2048; width += 31)
		{
			D3D11_TEXTURE2D_DESC resource_desc = {};
			resource_desc.Width = width;
			resource_desc.Height = height;
			resource_desc.ArraySize = 1;
			resource_desc.Format = DXGI_FORMAT_R8_UNORM;
			resource_desc.SampleDesc.Count = 1;
			resource_desc.MipLevels = 1;
			resource_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			resource_desc.Usage = D3D11_USAGE_DEFAULT;
			resource_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

			ComPtr<ID3D11Texture2D> img;

			CHECK_HRESULT(device->CreateTexture2D(&resource_desc, nullptr, (ID3D11Texture2D **)img.ppv()));

			HANDLE img_handle;
			ComPtr<IDXGIResource1> res;
			img->QueryInterface(IID_IDXGIResource1, res.ppv());
			CHECK_HRESULT(res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &img_handle));

			std::unique_ptr<uint8_t[]> ptr(new uint8_t[width * height]);
			for (uint32_t i = 0; i < width * height; i++)
				ptr[i] = uint8_t(i * 15);
			D3D11_BOX box = {};
			box.right = width;
			box.bottom = height;
			box.back = 1;

			context->UpdateSubresource(img.get(), 0, &box, ptr.get(), width, width * height);

			auto info = ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8_UNORM);
			info.misc |= IMAGE_MISC_EXTERNAL_MEMORY_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			info.external.memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
			info.external.handle = img_handle;
			auto vk_img = vk_device.create_image(info);

			context4->Signal(fence.get(), ++timeline);
			fence->SetEventOnCompletion(timeline, nullptr);

			BufferHandle readback_vk;

			{
				auto cmd = vk_device.request_command_buffer();
				cmd->acquire_image_barrier(*vk_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

				BufferCreateInfo bufinfo = {};
				bufinfo.size = width * height;
				bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				bufinfo.domain = BufferDomain::CachedHost;
				readback_vk = vk_device.create_buffer(bufinfo);
				cmd->copy_image_to_buffer(*readback_vk, *vk_img, 0, {}, { width, height, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
				cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
				Fence fence;
				vk_device.submit(cmd, &fence);
				vk_device.next_frame_context();
				fence->wait();
			}

			const uint8_t *d3d11_ptr = ptr.get();
			const uint8_t *vk_ptr;
			vk_ptr = static_cast<const uint8_t *>(vk_device.map_host_buffer(*readback_vk, MEMORY_ACCESS_READ_BIT));

			bool success = true;

			for (uint32_t y = 0; y < height && success; y++)
			{
				if (memcmp(vk_ptr + y * width, d3d11_ptr + y * width, width) != 0)
					success = false;
			}

			if (success)
				printf("Succeeded %u x %u R8 test (D3D11)\n", width, height);
			else
			{
				printf("Failed %u x %u R8 test (D3D11)\n", width, height);
				has_failure = true;
			}
		}
	}

	ASSERT_THAT(!has_failure);
}

static void test_d3d12_texture_layout()
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12CommandAllocator> allocator;
	uint64_t timeline = 0;

	uint32_t index = 0;
	if (const char *env = getenv("ADAPTER"))
		index = strtoul(env, nullptr, 0);

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(index, (IDXGIAdapter **)adapter.ppv()));

	CHECK_HRESULT(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device.ppv()));
	CHECK_HRESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, allocator.ppv()));
	CHECK_HRESULT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_ID3D12GraphicsCommandList, list.ppv()));
	// Base API create command list starts a new command list, which we usually need to close right away ...
	CHECK_HRESULT(list->Close());
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK_HRESULT(device->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue, queue.ppv()));

	CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.ppv()));
	auto luid = device->GetAdapterLuid();

	ASSERT_THAT(Context::init_loader(nullptr));
	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});
	ASSERT_THAT(ctx.init_instance(nullptr, 0));

	VkPhysicalDevice gpus[64];
	uint32_t gpu_count = 64;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	vkEnumeratePhysicalDevices(ctx.get_instance(), &gpu_count, gpus);
	for (uint32_t i = 0; i < gpu_count; i++)
	{
		VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids };
		vkGetPhysicalDeviceProperties2(gpus[i], &props2);
		if (memcmp(ids.deviceLUID, &luid, sizeof(luid)) == 0)
		{
			gpu = gpus[i];
			break;
		}
	}

	ASSERT_THAT(gpu);
	ASSERT_THAT(ctx.init_device(gpu, VK_NULL_HANDLE, nullptr, 0));
	Device vk_device;
	vk_device.set_context(ctx);

	bool has_failure = false;

	for (uint32_t height = 32; height < 2048; height += 31)
	{
		for (uint32_t width = 32; width < 2048; width += 31)
		{
			D3D12_RESOURCE_DESC resource_desc = {};
			resource_desc.Width = width;
			resource_desc.Height = height;
			resource_desc.DepthOrArraySize = 1;
			resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			resource_desc.SampleDesc.Count = 1;
			resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			resource_desc.MipLevels = 1;
			resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			D3D12_HEAP_PROPERTIES heap_props = {};
			heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

			ComPtr<ID3D12Resource> img;
			resource_desc.Format = DXGI_FORMAT_R8_UNORM;
			CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, img.ppv()));

			HANDLE img_handle;
			CHECK_HRESULT(device->CreateSharedHandle(img.get(), NULL, GENERIC_ALL, nullptr, &img_handle));

			auto info = ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8_UNORM);
			info.misc |= IMAGE_MISC_EXTERNAL_MEMORY_BIT;
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			info.external.memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
			info.external.handle = img_handle;
			auto vk_img = vk_device.create_image(info);

			// Upload frame to NV12 image async.
			list->Reset(allocator.get(), nullptr);
			ComPtr<ID3D12Resource> staging_buffer; // Verify that sync works somewhat so defer destroy these.
			upload_mirror_image(device.get(), list.get(), img.get(), 0, staging_buffer, 1, 3, 0, 0);
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = img.get();
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			list->ResourceBarrier(1, &barrier);
			list->Close();

			ID3D12CommandList *lists[] = { list.get() };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence.get(), ++timeline);
			CHECK_HRESULT(fence->SetEventOnCompletion(timeline, nullptr));

			BufferHandle readback_vk;

			{
				auto cmd = vk_device.request_command_buffer();
				cmd->acquire_image_barrier(*vk_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

				BufferCreateInfo bufinfo = {};
				bufinfo.size = width * height;
				bufinfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				bufinfo.domain = BufferDomain::CachedHost;
				readback_vk = vk_device.create_buffer(bufinfo);
				cmd->copy_image_to_buffer(*readback_vk, *vk_img, 0, {}, { width, height, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
				cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
				Fence fence;
				vk_device.submit(cmd, &fence);
				vk_device.next_frame_context();
				fence->wait();
			}

			allocator->Reset();
			list->Reset(allocator.get(), nullptr);
			ComPtr<ID3D12Resource> readback_buffer;
			UINT64 row_pitch = 0;
			readback_buffer = readback_image(device.get(), list.get(), img.get(), 0, &row_pitch);
			CHECK_HRESULT(list->Close());
			queue->ExecuteCommandLists(1, lists);

			// Wait for device to go idle.
			queue->Signal(fence.get(), ++timeline);
			// null event handle blocks on CPU directly.
			fence->SetEventOnCompletion(timeline, nullptr);

			const uint8_t *d3d12_ptr;
			const uint8_t *vk_ptr;
			CHECK_HRESULT(readback_buffer->Map(0, nullptr, (void **)&d3d12_ptr));
			vk_ptr = static_cast<const uint8_t *>(vk_device.map_host_buffer(*readback_vk, MEMORY_ACCESS_READ_BIT));

			bool success = true;

			for (uint32_t y = 0; y < height && success; y++)
			{
				if (memcmp(vk_ptr + y * width, d3d12_ptr + y * row_pitch, width) != 0)
					success = false;
			}

			readback_buffer->Unmap(0, nullptr);

			if (success)
				printf("Succeeded %u x %u R8 test (D3D12)\n", width, height);
			else
			{
				printf("Failed %u x %u R8 test (D3D12)\n", width, height);
				has_failure = true;
			}
		}
	}

	ASSERT_THAT(!has_failure);
}

static void test_d3d12_interop()
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12CommandAllocator> allocator;
	uint64_t timeline = 0;

	uint32_t index = 0;
	if (const char *env = getenv("ADAPTER"))
		index = strtoul(env, nullptr, 0);

	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(index, (IDXGIAdapter **)adapter.ppv()));

	CHECK_HRESULT(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device.ppv()));
	CHECK_HRESULT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, allocator.ppv()));
	CHECK_HRESULT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_ID3D12GraphicsCommandList, list.ppv()));
	// Base API create command list starts a new command list, which we usually need to close right away ...
	CHECK_HRESULT(list->Close());
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK_HRESULT(device->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue, queue.ppv()));

	// Sharing D3D12 to Vulkan is well supported. Other way around, not so much.
	CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_ID3D12Fence, fence.ppv()));

	LUID luid = device->GetAdapterLuid();
	static_assert(sizeof(luid) == sizeof(pyrowave_luid), "LUID struct size does not match.");

	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr,
		reinterpret_cast<pyrowave_luid *>(&luid), &pyro_device));

	test_d3d12_interop_allocation_stress(device.get(), pyro_device);

	// NV Windows is quite broken here and no matter what we do, it will only work for very specific resource sizes it seems ...
	constexpr uint32_t Width = 1024;
	constexpr uint32_t Height = 1024;

	D3D12_RESOURCE_DESC resource_desc = {};
	resource_desc.Width = Width;
	resource_desc.Height = Height;
	resource_desc.DepthOrArraySize = 1;
	resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resource_desc.SampleDesc.Count = 1;
	resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resource_desc.MipLevels = 1;

	D3D12_HEAP_PROPERTIES heap_props = {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	ComPtr<ID3D12Resource> nv12, yuv420p[3];
	resource_desc.Format = DXGI_FORMAT_NV12;
	CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, nv12.ppv()));

	// DXGI doesn't have proper 3-plane format. Just test the path with 3x R8 images.
	// Also adds some test coverage to that path.
	resource_desc.Format = DXGI_FORMAT_R8_UNORM;
	CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, yuv420p[0].ppv()));
	resource_desc.Width /= 2;
	resource_desc.Height /= 2;
	for (int plane = 1; plane < 3; plane++)
	{
		CHECK_HRESULT(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED, &resource_desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, yuv420p[plane].ppv()));
	}

	pyrowave_image pyro_nv12 = create_pyrowave_image_from_d3d12(pyro_device, device.get(), nv12.get(), false);
	pyrowave_image pyro_yuv420p[3] = {};
	for (int plane = 0; plane < 3; plane++)
		pyro_yuv420p[plane] = create_pyrowave_image_from_d3d12(pyro_device, device.get(), yuv420p[plane].get(), true);

	HANDLE fence_handle;
	CHECK_HRESULT(device->CreateSharedHandle(fence.get(), nullptr, GENERIC_ALL, nullptr, &fence_handle));
	pyrowave_sync_object pyro_sync = create_pyrowave_sync_from_d3d12_handle(pyro_device, fence_handle);

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = pyro_device;
	encoder_info.width = Width;
	encoder_info.height = Height;
	pyrowave_encoder encoder;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.width = Width;
	decoder_info.height = Height;
	pyrowave_decoder decoder;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	// Upload frame to NV12 image async.
	list->Reset(allocator.get(), nullptr);
	ComPtr<ID3D12Resource> staging_buffers[2]; // Verify that sync works somewhat so defer destroy these.
	upload_mirror_image(device.get(), list.get(), nv12.get(), 0, staging_buffers[0], 5, 3, 0, 0);
	upload_mirror_image(device.get(), list.get(), nv12.get(), 1, staging_buffers[1], 3, 1, 5, 7);
	list->Close();

	// Submit to D3D12 queue and signal the shared timeline.
	ID3D12CommandList *lists[] = { list.get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.get(), ++timeline);

	send_image_to_encoder(pyro_nv12, pyro_sync, timeline, pyro_sync, timeline + 1, encoder);
	timeline++;

	// This is a blocking call since we read back the payload on CPU (hopefully on a different machine).
	send_payload_to_decoder(encoder, decoder);

	// Decode and send image back to D3D12.
	decode_image(pyro_yuv420p, false, pyro_sync, ++timeline, decoder);

	queue->Wait(fence.get(), timeline);

	allocator->Reset();
	list->Reset(allocator.get(), nullptr);
	ComPtr<ID3D12Resource> readback_buffers[3];
	UINT64 row_pitch[3] = {};
	for (int plane = 0; plane < 3; plane++)
		readback_buffers[plane] = readback_image(device.get(), list.get(), yuv420p[plane].get(), 0, &row_pitch[plane]);
	CHECK_HRESULT(list->Close());
	queue->ExecuteCommandLists(1, lists);

	// Wait for device to go idle.
	queue->Signal(fence.get(), ++timeline);
	// null event handle blocks on CPU directly.
	fence->SetEventOnCompletion(timeline, nullptr);

	validate_d3d12_image(readback_buffers[0].get(), Width, Height, row_pitch[0], 5, 3);
	validate_d3d12_image(readback_buffers[1].get(), Width / 2, Height / 2, row_pitch[1], 3, 1);
	validate_d3d12_image(readback_buffers[2].get(), Width / 2, Height / 2, row_pitch[2], 5, 7);

	pyrowave_image_destroy(pyro_nv12);
	for (auto &img : pyro_yuv420p)
		pyrowave_image_destroy(img);
	pyrowave_sync_object_destroy(pyro_sync);
	pyrowave_decoder_destroy(decoder);
	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(pyro_device);
}

struct SharedBlock
{
	HANDLE texture[3];
	HANDLE fence;
	HANDLE sem;
	LUID luid;
	uint32_t width, height;
	uint8_t payload[1024 * 1024];
	uint32_t payload_size;
	uint64_t wait_value;
	uint64_t signal_value;
	bool dead;
};

static void test_d3d11_cross_process_encode()
{
	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11DeviceContext4> context4;
	ComPtr<ID3D11Fence> fence;

	uint32_t index = 0;
	if (const char *env = getenv("ADAPTER"))
		index = strtoul(env, nullptr, 0);

	CHECK_HRESULT(CreateDXGIFactory1(IID_IDXGIFactory, factory.ppv()));
	CHECK_HRESULT(factory->EnumAdapters(index, (IDXGIAdapter **)adapter.ppv()));

	HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
		(ID3D11Device **)device.ppv(), nullptr, (ID3D11DeviceContext **)context.ppv());
	ASSERT_THAT(SUCCEEDED(hr));
	CHECK_HRESULT(device->QueryInterface(IID_ID3D11Device5, device5.ppv()));

	DXGI_ADAPTER_DESC adapter_desc;
	CHECK_HRESULT(adapter->GetDesc(&adapter_desc));
	LUID luid = adapter_desc.AdapterLuid;
	static_assert(sizeof(luid) == sizeof(pyrowave_luid), "LUID struct size does not match.");

	HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlock), "PyroFlingTestDummy");
	ASSERT_THAT(mapping != INVALID_HANDLE_VALUE && mapping != nullptr);
	auto *shared = static_cast<SharedBlock *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
	ASSERT_THAT(shared);

	memset(shared, 0, sizeof(*shared));

	shared->luid = luid;
	shared->width = 1280;
	shared->height = 720;

	CHECK_HRESULT(context->QueryInterface(IID_ID3D11DeviceContext4, context4.ppv()));

	char self[4096];
	DWORD ret = GetModuleFileNameA(GetModuleHandle(nullptr), self, sizeof(self));
	self[ret] = '\0';

	strcat(self, " --child");

	HANDLE job_handle = CreateJobObjectA(nullptr, nullptr);
	ASSERT_THAT(job_handle);

	// Kill all child processes if the parent dies.
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
	jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	ASSERT_THAT(SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)));

	STARTUPINFOA si = {};
	si.cb = sizeof(STARTUPINFOA);
	PROCESS_INFORMATION pi;
	ASSERT_THAT(CreateProcessA(nullptr, self, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
		nullptr, nullptr, &si, &pi));

	ASSERT_THAT(AssignProcessToJobObject(job_handle, pi.hProcess));

	HANDLE semaphore = CreateSemaphoreA(nullptr, 0, 1, "PyroFlingSemDummy");
	ASSERT_THAT(semaphore);

	shared->wait_value = 1;
	shared->signal_value = 2;

	ResumeThread(pi.hThread);

	pyrowave_device pyro_device;
	CHECKED(pyrowave_create_default_device(&pyro_device));
	pyrowave_decoder decoder;
	pyrowave_decoder_create_info decoder_info = {};
	decoder_info.device = pyro_device;
	decoder_info.width = 1280;
	decoder_info.height = 720;
	CHECKED(pyrowave_decoder_create(&decoder_info, &decoder));

	std::unique_ptr<uint8_t[]> y_data(new uint8_t[1280 * 720]);
	std::unique_ptr<uint8_t[]> c_data(new uint8_t[640 * 360]);

	std::unique_ptr<uint8_t[]> decoded_y_data(new uint8_t[1280 * 720]);
	std::unique_ptr<uint8_t[]> decoded_cb_data(new uint8_t[640 * 360]);
	std::unique_ptr<uint8_t[]> decoded_cr_data(new uint8_t[640 * 360]);

	ComPtr<ID3D11Texture2D> tex[3];

	// Do encoding work.
	for (int i = 0; i < 1000; i++)
	{
		// Create new handles every so often.
		if (i % 10 == 0)
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			desc.SampleDesc.Count = 1;
			desc.Width = 1280;
			desc.Height = 720;
			desc.ArraySize = 1;
			desc.MipLevels = 1;
			CHECK_HRESULT(device5->CreateTexture2D(&desc, nullptr, (ID3D11Texture2D **)tex[0].ppv()));
			desc.Width /= 2;
			desc.Height /= 2;
			CHECK_HRESULT(device5->CreateTexture2D(&desc, nullptr, (ID3D11Texture2D **)tex[1].ppv()));
			CHECK_HRESULT(device5->CreateTexture2D(&desc, nullptr, (ID3D11Texture2D **)tex[2].ppv()));

			for (int j = 0; j < 3; j++)
			{
				ComPtr<IDXGIResource1> res;
				tex[j]->QueryInterface(IID_IDXGIResource1, res.ppv());
				HANDLE shared_handle;
				CHECK_HRESULT(res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_handle));
				ASSERT_THAT(DuplicateHandle(GetCurrentProcess(), shared_handle, pi.hProcess,
					&shared->texture[j], GENERIC_ALL, FALSE, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS));
			}

			CHECK_HRESULT(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, fence.ppv()));
			HANDLE shared_handle;
			CHECK_HRESULT(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared_handle));
			ASSERT_THAT(DuplicateHandle(GetCurrentProcess(), shared_handle, pi.hProcess,
				&shared->fence, GENERIC_ALL, FALSE, DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS));
		}

		D3D11_BOX box = {};
		box.right = 1280;
		box.bottom = 720;
		box.back = 1;
		memset(y_data.get(), i & 0xff, 1280 * 720);
		context->UpdateSubresource(tex[0].get(), 0, &box, y_data.get(), 1280, 1280 * 720);
		box.right = 640;
		box.bottom = 360;
		memset(c_data.get(), (i + 1) & 0xff, 640 * 360);
		context->UpdateSubresource(tex[1].get(), 0, &box, c_data.get(), 640, 640 * 360);
		memset(c_data.get(), (i + 2) & 0xff, 640 * 360);
		context->UpdateSubresource(tex[2].get(), 0, &box, c_data.get(), 640, 640 * 360);

		context4->Signal(fence.get(), shared->wait_value);
		ASSERT_THAT(ReleaseSemaphore(semaphore, 1, nullptr));

		// Wait until encoder is done.
		CHECK_HRESULT(fence->SetEventOnCompletion(shared->signal_value, nullptr));

		ASSERT_THAT(shared->payload_size);
		pyrowave_decoder_clear(decoder);
		CHECKED(pyrowave_decoder_push_packet(decoder, shared->payload, shared->payload_size));
		ASSERT_THAT(pyrowave_decoder_decode_is_ready(decoder, false));

		pyrowave_cpu_buffer cpu_buffers = {};
		cpu_buffers.data[0] = decoded_y_data.get();
		cpu_buffers.data[1] = decoded_cb_data.get();
		cpu_buffers.data[2] = decoded_cr_data.get();
		cpu_buffers.width = 1280;
		cpu_buffers.height = 720;
		cpu_buffers.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
		cpu_buffers.plane_size_in_bytes[0] = 1280 * 720;
		cpu_buffers.plane_size_in_bytes[1] = 640 * 360;
		cpu_buffers.plane_size_in_bytes[2] = 640 * 360;
		cpu_buffers.row_stride_in_bytes[0] = 1280;
		cpu_buffers.row_stride_in_bytes[1] = 640;
		cpu_buffers.row_stride_in_bytes[2] = 640;

		CHECKED(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpu_buffers));

		for (int pix = 0; pix < 1280 * 720; pix++)
			ASSERT_THAT(decoded_y_data[pix] == (i & 0xff));

		for (int pix = 0; pix < 640 * 360; pix++)
			ASSERT_THAT(decoded_cb_data[pix] == ((i + 1) & 0xff));

		for (int pix = 0; pix < 640 * 360; pix++)
			ASSERT_THAT(decoded_cr_data[pix] == ((i + 2) & 0xff));

		shared->wait_value += 2;
		shared->signal_value += 2;

		LOGI("Running cross-process frame %u / 1000 ...\n", i);
	}

	CloseHandle(pi.hThread);

	shared->dead = true;
	ASSERT_THAT(ReleaseSemaphore(semaphore, 1, nullptr));

	ASSERT_THAT(WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0);
	ASSERT_THAT(!shared->dead);
	CloseHandle(pi.hProcess);
	CloseHandle(semaphore);
	CloseHandle(job_handle);

	pyrowave_decoder_destroy(decoder);
	pyrowave_device_destroy(pyro_device);
}

static void test_child_interop()
{
	//__debugbreak();

	// Open shared handles.
	HANDLE semaphore = CreateSemaphoreA(nullptr, 0, 1, "PyroFlingSemDummy");
	ASSERT_THAT(semaphore);

	HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlock), "PyroFlingTestDummy");
	ASSERT_THAT(mapping != INVALID_HANDLE_VALUE && mapping != nullptr);
	auto *shared = static_cast<SharedBlock *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
	ASSERT_THAT(shared);

	pyrowave_device device;
	CHECKED(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, reinterpret_cast<const pyrowave_luid *>(&shared->luid), &device));

	pyrowave_image img[3] = {};
	pyrowave_sync_object sync = {};
	pyrowave_encoder encoder = {};

	pyrowave_encoder_create_info encoder_info = {};
	encoder_info.device = device;
	encoder_info.width = shared->width;
	encoder_info.height = shared->height;
	encoder_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	CHECKED(pyrowave_encoder_create(&encoder_info, &encoder));

	pyrowave_gpu_external_reference refs[3];

	// Encode loop

	for (;;)
	{
		WaitForSingleObject(semaphore, INFINITE);
		if (shared->dead)
			break;

		for (int i = 0; i < 3; i++)
		{
			if (shared->texture[i])
			{
				if (img[i])
					pyrowave_image_destroy(img[i]);

				VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
				image_info.extent = { shared->width / (i ? 2 : 1), shared->height / (i ? 2 : 1), 1 };
				image_info.format = VK_FORMAT_R8_UNORM;
				image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				image_info.samples = VK_SAMPLE_COUNT_1_BIT;
				image_info.arrayLayers = 1;
				image_info.mipLevels = 1;
				image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
				image_info.imageType = VK_IMAGE_TYPE_2D;

				pyrowave_image_create_info info = {};
				info.device = device;
				info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
				info.external_handle = (pyrowave_os_handle)shared->texture[i];
				info.image_create_info = &image_info;

				CHECKED(pyrowave_image_create(&info, &img[i]));

				refs[i] = { img[i], VK_QUEUE_FAMILY_EXTERNAL };
				shared->texture[i] = nullptr;
			}
		}

		if (shared->fence)
		{
			if (sync)
				pyrowave_sync_object_destroy(sync);

			pyrowave_sync_object_create_info sync_info = {};
			sync_info.device = device;
			sync_info.external_handle = (pyrowave_os_handle)shared->fence;
			sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
			sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
			CHECKED(pyrowave_sync_object_create(&sync_info, &sync));

			shared->fence = nullptr;
		}

		pyrowave_rate_control rate_control = { sizeof(shared->payload) };
		pyrowave_gpu_buffers buffers = {};
		pyrowave_gpu_sync_operation acquire = {};
		pyrowave_gpu_sync_operation release = {};
		acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(sync);
		acquire.sync.value = shared->wait_value;

		acquire.num_images = 3;
		acquire.images = refs;
		release.num_images = 3;
		release.images = refs;

		for (int i = 0; i < 3; i++)
			CHECKED(pyrowave_image_get_image_view(img[i], VkImageAspectFlagBits(VK_IMAGE_ASPECT_PLANE_0_BIT << i), VK_IMAGE_USAGE_SAMPLED_BIT, &buffers.planes[i]));
		CHECKED(pyrowave_encoder_encode_gpu_synchronous(encoder, &acquire, &release, &buffers, &rate_control));

		pyrowave_packet packet;
		size_t out_packets = 1;
		CHECKED(pyrowave_encoder_packetize(encoder, &packet, sizeof(shared->payload), &out_packets, shared->payload, sizeof(shared->payload)));
		shared->payload_size = packet.size;

		// API sanity check.
		CHECKED(pyrowave_sync_object_cpu_wait(sync, shared->wait_value, UINT64_MAX));

		// Signal on CPU.
		CHECKED(pyrowave_sync_object_cpu_signal(sync, shared->signal_value));
	}

	//__debugbreak();

	shared->dead = false;

	pyrowave_sync_object_destroy(sync);
	for (auto &i : img)
		pyrowave_image_destroy(i);
	pyrowave_encoder_destroy(encoder);
	pyrowave_device_destroy(device);
}
#endif

static void test_extended_ycbcr_interop()
{
	ASSERT_THAT(Context::init_loader(nullptr));

	Context ctx;
	ctx.set_num_thread_indices(1);
	ctx.set_system_handles({});
	ASSERT_THAT(ctx.init_instance_and_device(nullptr, 0, nullptr, 0));

	Device device;
	device.set_context(ctx);

	// Tests that we can successfully write to YCbCr as storage image for various formats in decoder.

	static const struct
	{
		const char *name;
		VkFormat image_format;
		VkFormat planar_format;
		bool subsampled;
	} configs[] = {
		{ "yuv420p", VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, VK_FORMAT_R8_UNORM, true },
		{ "yuv444p", VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM, VK_FORMAT_R8_UNORM, false },
		{ "yuv420p16", VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM, VK_FORMAT_R16_UNORM, true },
		{ "yuv444p16", VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM, VK_FORMAT_R16_UNORM, false },
		{ "p010", VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16, VK_FORMAT_R10X6_UNORM_PACK16, true },
		{ "p410", VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16, VK_FORMAT_R10X6_UNORM_PACK16, false },
	};

	for (auto &config : configs)
	{
		VkFormatProperties3 props = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
		device.get_format_properties(config.image_format, &props);

		const VkFormatFeatureFlags2 required_image =
				VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
				VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
				VK_FORMAT_FEATURE_2_COSITED_CHROMA_SAMPLES_BIT |
				VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT |
				(config.subsampled ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT : 0);

		if ((props.optimalTilingFeatures & required_image) != required_image)
		{
			fprintf(stderr, "Format %s not supported for YCbCr sampling, skipping. optimalFeatures = #%llx\n",
			        config.name, static_cast<unsigned long long>(props.optimalTilingFeatures));
			continue;
		}

		const VkFormatFeatureFlags2 required_planar =
				VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
				VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

		device.get_format_properties(config.planar_format, &props);
		if ((props.optimalTilingFeatures & required_planar) != required_planar)
		{
			fprintf(stderr,
			        "Planar format %s not supported for storage image without format, skipping. optimalFeatures = #%llx\n",
			        config.name, static_cast<unsigned long long>(props.optimalTilingFeatures));
			continue;
		}

		ResourceLayout layout;
		ShaderBank::Shaders<> shaders(device, layout, 0);

		VkSamplerYcbcrConversionCreateInfo ycbcr_info = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO };
		ycbcr_info.chromaFilter = VK_FILTER_NEAREST;
		ycbcr_info.format = config.image_format;
		ycbcr_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
		ycbcr_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
		ycbcr_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
		ycbcr_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
		auto *ycbcr_conv = device.request_immutable_ycbcr_conversion(ycbcr_info);

		SamplerCreateInfo sampler_info = {};
		sampler_info.address_mode_u = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.address_mode_v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler_info.min_filter = VK_FILTER_NEAREST;
		sampler_info.mag_filter = VK_FILTER_NEAREST;
		auto *sampler = device.request_immutable_sampler(sampler_info, ycbcr_conv);

		auto image_info = ImageCreateInfo::immutable_2d_image(64, 64, config.image_format);
		image_info.ycbcr_conversion = ycbcr_conv;
		image_info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		image_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		auto image = device.create_image(image_info);

		ImageViewHandle planar_views[3];
		for (int i = 0; i < 3; i++)
		{
			ImageViewCreateInfo view_info = {};
			view_info.format = config.planar_format;
			view_info.image = image.get();
			view_info.aspect = VK_IMAGE_ASPECT_PLANE_0_BIT << i;
			view_info.view_type = VK_IMAGE_VIEW_TYPE_2D;
			planar_views[i] = device.create_image_view(view_info);
		}

		BufferCreateInfo bufinfo = {};
		bufinfo.size = 64 * 64 * sizeof(uint32_t);
		bufinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		bufinfo.domain = BufferDomain::CachedHost;
		auto readback = device.create_buffer(bufinfo);

		auto cmd = device.request_command_buffer();

		auto *shader = shaders.test_read_ycbcr->get_shader(ShaderStage::Compute);
		ImmutableSamplerBank sampler_bank = {};
		sampler_bank.samplers[0][0] = sampler;
		auto *read_program = device.request_program(shader, &sampler_bank);

		cmd->set_program(shaders.test_write_ycbcr);
		cmd->image_barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                   0, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		for (int i = 0; i < 3; i++)
			cmd->set_storage_texture(0, i, *planar_views[i]);
		cmd->dispatch(64 / 8, 64 / 8, 1);

		cmd->image_barrier(*image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
						   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
						   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

		cmd->set_program(read_program);
		cmd->set_texture(0, 0, image->get_view());
		cmd->set_storage_buffer(0, 1, *readback);
		cmd->dispatch(64 / 8, 64 / 8, 1);
		cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		             VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

		Fence fence;
		device.submit(cmd, &fence);
		fence->wait();

		auto *ptr = static_cast<const uint32_t *>(device.map_host_buffer(*readback, MEMORY_ACCESS_READ_BIT));
		for (int y = 0; y < 64; y++)
		{
			for (int x = 0; x < 64; x++)
			{
				uint32_t pix = ptr[y * 64 + x];
				int r = int(pix >>  0) & 0xff;
				int g = int(pix >>  8) & 0xff;
				int b = int(pix >> 16) & 0xff;

				int chroma_x = x >> int(config.subsampled);
				int chroma_y = y >> int(config.subsampled);

				float chroma_shift = config.planar_format == VK_FORMAT_R8_UNORM ? 128.0f / 255.0f : 512.0f / 1023.0f;

				float Y = float(128 + ((x & 3) ^ (y & 7))) / 255.0f;
				float Cb = float(128 + ((chroma_x & 7) ^ (chroma_y & 3))) / 255.0f - chroma_shift;
				float Cr = float(128 + ((chroma_x & 7) ^ (chroma_y & 7))) / 255.0f - chroma_shift;

				float R = Y + 1.5748f * Cr;
				float G = Y - 0.13397432f / 0.7152f * Cb - 0.33480248f / 0.7152f * Cr;
				float B = Y + 1.8556f * Cb;

				auto intR = std::lround(float(R) * 255.0f);
				auto intG = std::lround(float(G) * 255.0f);
				auto intB = std::lround(float(B) * 255.0f);

				// Allow maximum 1 ULP error in reconstruction.
				ASSERT_THAT(std::abs(r - intR) <= 1);
				ASSERT_THAT(std::abs(g - intG) <= 1);
				ASSERT_THAT(std::abs(b - intB) <= 1);
			}
		}

		fprintf(stderr, "Planar storage image test for %s succeeded!\n", config.name);
	}
}

int main(int argc, char **argv)
{
#ifdef _WIN32
	if (getenv("VALIDATE"))
	{
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_ID3D12Debug, debug.ppv())))
			debug->EnableDebugLayer();
	}

	if (argc == 2 && strcmp(argv[1], "--child") == 0)
	{
		test_child_interop();
		return EXIT_SUCCESS;
	}

	printf("Running D3D12 texture layout test ...\n");
	test_d3d11_texture_layout();

	printf("Running D3D12 texture layout test ...\n");
	test_d3d12_texture_layout();

	test_d3d11_cross_process_encode();

	printf("Running NV12 interop test ...\n");
	test_nv12_interop();

	printf("Running D3D11 interop test ...\n");
	test_d3d11_interop();

	printf("Running D3D12 interop test ...\n");
	test_d3d12_interop();
#else
	(void)argc;
	(void)argv;
#endif

	test_extended_ycbcr_interop();

	printf("Running Vulkan <-> Vulkan interop test with direct device share ...\n");
	test_direct_interop();
	test_direct_interop_scaling();

	printf("Running opaque Vulkan <-> Vulkan interop test ...\n");
	test_opaque_interop(false);

#ifdef _WIN32
	printf("Running opaque Vulkan <-> Vulkan interop test (KMT handles) ...\n");
	test_opaque_interop(true);
#endif

	printf("Running drm modifier Vulkan <-> Vulkan interop test ...\n");
	test_drm_modifier_interop();

	printf("Interop tests passed!\n");
}
