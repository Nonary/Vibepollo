// Copyright (c) 2026 Hans-Kristian Arntzen
// SPDX-License-Identifier: MIT

#ifndef PYROWAVE_H_
#define PYROWAVE_H_

#if !defined(VULKAN_CORE_H_)
#error "Must include vulkan headers before including pyrowave.h"
#endif

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

// API and ABI is not considered stable until MAJOR version hits 1!

#define PYROWAVE_API_VERSION_MAJOR 0
#define PYROWAVE_API_VERSION_MINOR 6
#define PYROWAVE_API_VERSION_PATCH 0

#if !defined(PYROWAVE_PUBLIC_API)
#if defined(PYROWAVE_EXPORT_SYMBOLS)
#if defined(__GNUC__)
#define PYROWAVE_PUBLIC_API __attribute__((visibility("default")))
#elif defined(_MSC_VER)
#define PYROWAVE_PUBLIC_API __declspec(dllexport)
#else
#define PYROWAVE_PUBLIC_API
#endif
#else
#define PYROWAVE_PUBLIC_API
#endif
#else
#define PYROWAVE_PUBLIC_API
#endif

typedef void (*pyrowave_message_cb)(void *userdata, const char *msg);

typedef enum pyrowave_result
{
	PYROWAVE_SUCCESS = 0,
	PYROWAVE_TIMEOUT = 1,
	PYROWAVE_ERROR_GENERIC = -1,
	PYROWAVE_ERROR_INVALID_ARGUMENT = -2,
	PYROWAVE_ERROR_OUT_OF_HOST_MEMORY = -3,
	PYROWAVE_ERROR_OUT_OF_DEVICE_MEMORY = -4,
	PYROWAVE_ERROR_NO_VULKAN = -5,
	PYROWAVE_ERROR_NOT_IMPLEMENTED = -6,
	PYROWAVE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE = -7,
	PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE = -8,
	PYROWAVE_ERROR_INT_MAX = 0x7fffffff
} pyrowave_result;

typedef enum pyrowave_chroma_subsampling
{
	PYROWAVE_CHROMA_SUBSAMPLING_420 = 0,
	PYROWAVE_CHROMA_SUBSAMPLING_444 = 1,
	PYROWAVE_CHROMA_SUBSAMPLING_INT_MAX = 0x7fffffff
} pyrowave_chroma_subsampling;

typedef struct pyrowave_encoder_opaque *pyrowave_encoder;
typedef struct pyrowave_decoder_opaque *pyrowave_decoder;
typedef struct pyrowave_device_opaque *pyrowave_device;
typedef struct pyrowave_sync_object_opaque *pyrowave_sync_object;
typedef struct pyrowave_image_opaque *pyrowave_image;

// Used to dynamically detect any API/ABI incompatibility.
// This entry point is stable.
PYROWAVE_PUBLIC_API void pyrowave_get_api_version(uint32_t *major, uint32_t *minor, uint32_t *patch);

// Device API.
PYROWAVE_PUBLIC_API pyrowave_result pyrowave_create_default_device(pyrowave_device *device);

typedef struct pyrowave_device_create_queue_info
{
	VkQueue queue;
	uint32_t familyIndex;
	uint32_t index;
} pyrowave_device_create_queue_info;

// Locks and unlocks submissions to all VkQueues on the VkDevice which the pyrowave_device as access to
// (either via vkGetDeviceQueue or queue_info structs).
typedef void (*pyrowave_queue_lock_cb)(void *userdata);

typedef struct pyrowave_device_create_info
{
	// The vkGetInstanceProcAddr entry point for a valid Vulkan loader.
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr;

	// The Vulkan handles used to create the device.
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;

	// The CreateInfos used to create instance and device.
	// The pointers and all contents inside them must remain valid for the lifetime of the pyrowave_device.
	// device_create_info needs to supply valid queue create infos as well as
	// extensions and pNext needs to contain PDF2 struct.
	// Instance create infos needs valid extensions as well as a compatible pApplicationInfo w.r.t apiVersion.
	// apiVersion should be at least Vulkan 1.3.
	// At least one graphics capable queue must be present.
	const VkInstanceCreateInfo *instance_create_info;
	const VkDeviceCreateInfo *device_create_info;

	// Rather than calling vkGetDeviceQueue to get queues,
	// implementation will look for a valid queue here first.
	// This allows passing only graphics queue #2 for example.
	// These queues should only be used for spurious uploads as needed.
	pyrowave_device_create_queue_info *queue_info;
	uint32_t queue_info_count;

	// Misc callbacks. Can be NULL. If device was created with VK_KHR_implicitly_synchronized_queued, locking
	// callbacks are not needed.
	// pyrowave device will only submit queue commands inside pyrowave device API calls,
	// so that is another way to ensure synchronization.
	pyrowave_queue_lock_cb queue_lock_callback;
	pyrowave_queue_lock_cb queue_unlock_callback;

	// Userdata provided to callbacks.
	void *userdata;
} pyrowave_device_create_info;

typedef struct pyrowave_uuid
{
	uint8_t uuid[VK_UUID_SIZE];
} pyrowave_uuid;

typedef struct pyrowave_luid
{
	uint8_t luid[VK_LUID_SIZE];
} pyrowave_luid;

// Direct API that shares a VkDevice. Avoids needing to use external memory to encode and decode.
// Encoder expects features:
// - Basic subgroup support. ARITHMETIC, SHUFFLE, SHUFFLE_RELATIVE, VOTE, BALLOT, CLUSTERED, BASIC (Vulkan 1.1 core).
// - Subgroup size control (Vulkan 1.3 core).
// - Subgroup size control enough to force wave16, wave32 or wave64.
// - shaderInt16
// - storageBuffer8BitAccess
// This covers all desktop GPUs and most mobile GPUs.
// Optional:
// - shaderFloat16
// Decoder expects features:
// - Basic subgroup support. ARITHMETIC, SHUFFLE, SHUFFLE_RELATIVE, VOTE, BALLOT, BASIC (Vulkan 1.1 core).
// - Subgroup size control (Vulkan 1.3 core).
// - Anything from wave4 to wave128 goes as long as it supports subgroup size control.
// Should cover anything remotely relevant.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_create_device(const pyrowave_device_create_info *info, pyrowave_device *device);

// On Windows, LUID is generally used, but other OS-es may need device_uuid/driver_uuid.
PYROWAVE_PUBLIC_API pyrowave_result pyrowave_create_device_by_compat(
	// If non-zero, needs to match VkPhysicalDeviceProperties::vendorID/deviceID.
	// Risks picking the wrong device if there are multiple ICDs for the same GPU.
	uint32_t vid, uint32_t pid,
	const pyrowave_uuid *device_uuid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::deviceUUID
	const pyrowave_uuid *driver_uuid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::driverUUID
	const pyrowave_luid *device_luid, // If non-NULL, needs to match VkPhysicalDeviceIDProperties::deviceLUID
	pyrowave_device *device);

// For performance debugging, reports GPU timestamps.
PYROWAVE_PUBLIC_API void
pyrowave_device_report_performance_stats(pyrowave_device device, pyrowave_message_cb cb, void *userdata, bool reset);

// Out pointers can be NULL, in which case nothing is written to them.
PYROWAVE_PUBLIC_API void pyrowave_device_get_vk_device_handles(
	pyrowave_device device,
	VkInstance *vk_instance, VkPhysicalDevice *vk_physical_device,
	VkDevice *vk_device);

// If a command buffer is set on the device, any encoder or decoder commands which record Vulkan commands
// will record to cmd instead.
// pyrowave_device will not submit or close the command buffer.
// Any state on the command buffer is assumed to be clobbered.
// When recording is complete, set cmd to VK_NULL_HANDLE.
// pyrowave_device will only record commands directly inside an API entry point.
// The command buffer must be created for a an appropriate queue family based on how its used.
// Encoder: VK_QUEUE_COMPUTE_BIT.
// Decoder: VK_QUEUE_COMPUTE_BIT (if using normal path), VK_QUEUE_GRAPHICS_BIT (if using fragment path).
PYROWAVE_PUBLIC_API void
pyrowave_device_set_command_buffer(pyrowave_device device, VkCommandBuffer cmd);

// When an explicit command buffer is not used, controls which queue family is preferred for encode or decode operations.
// This is typically used to select between graphics or async compute encoding.
// These methods have tradeoffs, but for cross-process encoding, it makes more sense to use async compute encode,
// since the graphics queue may become busy.
// If encoding in-process on QueuePresent or similar, graphics queue might make more sense, since it guarantees lowest possible encoding latency.
// For decoding, graphics queue is natural since the result will immediately be consumed there.
// Valid values for queue_flags are VK_QUEUE_GRAPHICS_BIT or VK_QUEUE_COMPUTE_BIT.
// For borrowed devices, pyrowave must have been provided an async compute queue, or it will fallback to graphics.
// If there are no async compute queues on the device, graphics queue will be used instead.
// When explicit command buffers are used, queue_flags denotes which queue type the command buffer belongs to.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_device_set_queue_type(pyrowave_device device, VkQueueFlagBits queue_flags);

PYROWAVE_PUBLIC_API bool
pyrowave_device_confirm_interop_support(pyrowave_device device);

// All encoders and decoders must have been destroyed before destroying the device.
PYROWAVE_PUBLIC_API void pyrowave_device_destroy(pyrowave_device device);
////

// External sync API
// On Windows, this is a HANDLE reinterpreted as uintptr_t.
// On POSIX, it's a file descriptor int casted to uintptr_t.
typedef uintptr_t pyrowave_os_handle;

typedef struct pyrowave_sync_object_create_info
{
	pyrowave_device device;

	// If this is an invalid handle according to the OS (NULL HANDLE, negative fd),
	// the sync object is created as an exportable handle.
	// If a handle is imported successfully, pyrowave_sync_object takes ownership of the OS handle.
	pyrowave_os_handle external_handle;

	// Must be one of the supported handle types by the device.
	// The implementation will fail the call with an appropriate error if not supported.
	// Recognized types:
	// - OPAQUE_FD
	// - SYNC_FD_BIT
	// - OPAQUE_WIN32_BIT
	// - OPAQUE_WIN32_KMT_BIT
	// - D3D12_FENCE_BIT (D3D11_FENCE_BIT is alias of D3D12_FENCE_BIT)
	// NOTE: When importing NT handles, the implementation will take ownership and close the HANDLE on import.
	// The semaphore holds a reference to the underlying object.
	// It may be a good idea to call DuplicateHandle() and hand that over to the implementation instead.
	// This has been known to workaround some weird bugs in the wild, but the root cause is unknown.
	VkExternalSemaphoreHandleTypeFlagBits handle_type;

	// Binary or Timeline. For D3D11/D3D12 fence import, this must be TIMELINE.
	VkSemaphoreType semaphore_type;

	// Only relevant for importing.
	// For binary semaphores, this must be TEMPORARY for now.
	// This makes the sync object fire and forget and can only be used once.
	// TEMPORARY must not be used for timeline semaphores.
	VkSemaphoreImportFlags import_flags;
} pyrowave_sync_object_create_info;

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_sync_object_create(const pyrowave_sync_object_create_info *info, pyrowave_sync_object *sync);

PYROWAVE_PUBLIC_API VkSemaphore
pyrowave_sync_object_get_semaphore(pyrowave_sync_object sync);

// Called after signaling a semaphore, the sync payload can be exported to a handle.
// For timeline semaphores this can be called at any time if was created exportable.
// The common use case on Windows is to import a D3D12 fence timeline, never export, since
// not all implementations support exporting a timeline semaphore on that platform,
// but all support importing.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_sync_object_export_handle(pyrowave_sync_object sync, pyrowave_os_handle *handle);

// Timeout interpreted as Vulkan API.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_sync_object_cpu_wait(pyrowave_sync_object sync, uint64_t value, uint64_t timeout);

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_sync_object_cpu_signal(pyrowave_sync_object sync, uint64_t value);

PYROWAVE_PUBLIC_API void pyrowave_sync_object_destroy(pyrowave_sync_object sync);
////

// External resource API
typedef struct pyrowave_image_view
{
	VkImage image;
	// Extent of mip0. Must be consistent with width/height used to create the encoder.
	// If the view is taking chroma of a planar image,
	// the width/height is for the luma plane, i.e. the base image.
	uint32_t width;
	uint32_t height;
	// Base format used to create the image.
	VkFormat image_format;

	// Must be UNORM in some way and be supported for sampling and storage.
	// For planar image_format, the image must have been created with
	// VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT to be able to take plane views.
	VkFormat view_format;
	uint32_t mip_level;
	uint32_t layer;
	// If using a planar image format, needs to be e.g. VK_IMAGE_ASPECT_PLANE_*_BIT.
	VkImageAspectFlagBits aspect;
	// For decode path, must be IDENTITY or R.
	VkComponentSwizzle swizzle;
	// Must be VK_IMAGE_LAYOUT_(SHADER_)READ_ONLY_OPTIMAL (encode only) or VK_IMAGE_LAYOUT_GENERAL.
	// For fragment decode path, must be (COLOR_)ATTACHMENT_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL.
	// Pyrowave will not perform any image layout transitions on its own in the GPU buffer paths.
	VkImageLayout layout;
} pyrowave_image_view;

typedef struct pyrowave_image_create_info
{
	pyrowave_device device;

	// Must be a valid external handle.
	// NOTE: When importing NT handles, the implementation will take ownership and close the HANDLE on import.
	// The image holds a reference to the underlying object.
	// It may be a good idea to call DuplicateHandle() and hand that over to the implementation instead.
	// This has been known to workaround some weird bugs in the wild, but the root cause is unknown.
	pyrowave_os_handle external_handle;
	VkExternalMemoryHandleTypeFlagBits handle_type;

	// For OPAQUE handles, the create info must be conformant to spec requirements where the create infos
	// have to match between creator and consumer.
	// (In practice, this can be awkward especially when sharing between e.g. GL and Vulkan,
	// spec calls for enabling "all" flags).
	// For other types, image_create_info has to be compatible enough to make the sharing work.
	//
	// - Tiling must be OPTIMAL or DRM_FORMAT_MODIFIER_EXT.
	// - Sharing mode must be EXCLUSIVE.
	// - If a planar format like NV12 is used, the image must have MUTABLE_BIT image creation set.
	const VkImageCreateInfo *image_create_info;

	// DRM format modifier usage:
	// - Set image_create_info->tiling to VK_IMAGE_TILING_DRM_FORMAT_MODIFIER.
	// - handle_type must be VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT.
	// - VkImageDrmFormatModifierExplicitCreateInfoEXT must be chained into the pNext of image_create_info.

	// External images are always assumed to be in GENERAL layout.
} pyrowave_image_create_info;

// Only intended to be used with external memory. For pyrowave_create_device() path
// application should create its own images and set the image view struct without going through this API.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_image_create(const pyrowave_image_create_info *info, pyrowave_image *image);

PYROWAVE_PUBLIC_API VkImage
pyrowave_image_get_handle(pyrowave_image image);

// Generates an image view from an (imported) image automatically for convenience.
// - Aspect must be VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_PLANE_0_BIT, PLANE_1_BIT or PLANE_2_BIT.
// - For 2-plane YCbCr image formats or two component image formats, image view swizzles are used to synthesize 3 planes.
// - For single component image formats, the aspect is ignored (the image is the plane itself).
// - For 3-component image formats, the aspect selects the component index through image swizzle.
// - If COLOR_BIT aspect is used, the image format is used as-is as the view format, with exception
//   for sRGB. The equivalent UNORM format is used as view format, and image must be created with compatible casting.
// - COLOR_BIT aspect can be used with YCbCr, but it's only supported by the scaling path, and only NV12 format.
//   Very special case for pipewire interop. Don't use unless you know what you're doing.
//
// Some validation rules:
// - For 2-plane YCbCr image formats, usage must not be STORAGE_BIT.
// - For non-YCbCr image formats with more than 1 component, usage must not be STORAGE_BIT.
// - Usage must be VK_IMAGE_USAGE_STORAGE_BIT (for decode) or VK_IMAGE_USAGE_SAMPLED_BIT (for encode).
// - The format of the image must be recognized. Highly unusual formats may be rejected.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_image_get_image_view(pyrowave_image image, VkImageAspectFlagBits aspect,
                              VkImageUsageFlagBits usage, pyrowave_image_view *view);

PYROWAVE_PUBLIC_API void pyrowave_image_destroy(pyrowave_image image);
////

// Encoder API
typedef struct pyrowave_encoder_create_info
{
	pyrowave_device device;
	// For 420 subsampling, must be even.
	int width;
	int height;
	pyrowave_chroma_subsampling chroma;
} pyrowave_encoder_create_info;

typedef struct pyrowave_packet
{
	size_t offset;
	size_t size;
} pyrowave_packet;

typedef struct pyrowave_sync_point
{
	// Can be VK_NULL_HANDLE, in which case it means "no sync".
	VkSemaphore semaphore;

	// If semaphore is a binary semaphore, value must be 0.
	// NOTE: While waiting for a timeline semaphore value of 0 is valid Vulkan,
	// it's a noop and can be replaced with that.
	uint64_t value;
} pyrowave_sync_point;

typedef struct pyrowave_gpu_external_reference
{
	pyrowave_image image;
	// VK_QUEUE_FAMILY_EXTERNAL, _FOREIGN, _IGNORED or a normal queue family index.
	uint32_t queue_family_index;
} pyrowave_gpu_external_reference;

typedef struct pyrowave_gpu_buffers
{
	// All 3 planes must be provided. For NV12 images, pass in the same plane for Cb and Cr, but use swizzle
	// to select R and G planes to fake a YUV420P image.
	// Very slightly less efficient, but should barely be measurable.
	// pyrowave_image_get_image_view() can be used as a helper to fill these in.
	pyrowave_image_view planes[3];
} pyrowave_gpu_buffers;

typedef struct pyrowave_gpu_sync_operation
{
	// If interacting with external images, it's expected that implementation needs to acquire and release the image.
	// Decode only:
	// If acquiring from QUEUE_FAMILY_IGNORED,
	// the image will be transitioned away from VK_IMAGE_LAYOUT_UNDEFINED instead rather than taking ownership.
	// In Vulkan, if content does not have to be preserved (i.e. decoding), it can just be discarded with UNDEFINED.
	const pyrowave_gpu_external_reference *images;
	size_t num_images;
	pyrowave_sync_point sync;
} pyrowave_gpu_sync_operation;

// TODO: Add support for importing external memory as GPU buffers.

// The CPU path is mostly for bringup testing.
typedef enum pyrowave_cpu_buffer_format
{
	PYROWAVE_CPU_BUFFER_FORMAT_NV12 = 0, // 2 planes. Y packed in 8bpp, then CbCr packed in 16bpp. Only supported for encoding.
	PYROWAVE_CPU_BUFFER_FORMAT_YUV420P = 1, // 3 planes. Y, Cb, Cr packed into separate planes. Native format for pyrowave.
	PYROWAVE_CPU_BUFFER_FORMAT_YUV444P = 2, // 3 planes. Y, Cb, Cr packed into separate planes. Native format for pyrowave.
	PYROWAVE_CPU_BUFFER_FORMAT_INT_MAX = 0x7fffffff
} pyrowave_cpu_buffer_format;

typedef struct pyrowave_cpu_buffer
{
	// Written in decoder, read-only in encoder.
	void *data[3];
	// Must be at least width for plane times texel size of the plane.
	size_t row_stride_in_bytes[3];
	// Must be at least row_stride times height of plane.
	size_t plane_size_in_bytes[3];
	// Size of the luma plane. Size of chroma is implied by format.
	// Must be same extent as decoder.
	int width;
	int height;
	pyrowave_cpu_buffer_format format;
} pyrowave_cpu_buffer;

typedef struct pyrowave_rate_control
{
	// Very basic, target bitstream for an image must not exceed this size.
	size_t maximum_bitstream_size;
} pyrowave_rate_control;

// The entry points for encoder are not thread safe. Application must ensure synchronization.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_create(const pyrowave_encoder_create_info *info, pyrowave_encoder *encoder);

// Synchronous encode API. For low-latency use cases, overlapping frames in encode is meaningless
// due to latency and the encoder is so fast anyway. This function will not block, but subsequent functions will.
// Calling an encode operation with synchronous API clobbers any previous encoded frame.
// The encoded stream will contain a small sequence counter that tracks frame ordering.
// acquire and release can be NULL if no sync is required.
// If command buffer is set on pyrowave_device, acquire and release must both be NULL.
// If command buffer is set on pyrowave_device, applications is responsible for submitting that work to GPU
// and waiting for it before calling pyrowave_encoder_compute_num_packets or pyrowave_encoder_packetize.
// If command buffer is set, application must ensure synchronization as:
// - Before, the layouts in buffers must be correct.
//   Memory must be visible to COMPUTE_SHADER / SHADER_SAMPLED_READ.
// - After: Application must add execution barrier on COMPUTE_SHADER stage before writing to images.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_gpu_synchronous(pyrowave_encoder encoder,
                                        const pyrowave_gpu_sync_operation *acquire,
                                        const pyrowave_gpu_sync_operation *release,
                                        const pyrowave_gpu_buffers *buffers,
                                        const pyrowave_rate_control *rate_control);

typedef struct pyrowave_scaled_encode_info
{
	// Input view must be some RGB(A) UNORM format.
	// Alternatively, as a special case, NV12 (G8_B8R8_2PLANE_420) is allowed here with COLOR_ASPECT.
	// This is only intended to be used with pipewire dmabuf screen capture path.
	// If scaling NV12, the crop-rect (if any) must be aligned to 2 pixel offset and extent.
	// The crop rect will be scaled accordingly for chroma plane.
	pyrowave_image_view view;

	// For SDR, use VK_COLOR_SPACE_SRGB_NONLINEAR.
	// SPACE_EXTENDED_SRGB_LINEAR_EXT (80 nits normalized) and HDR10_ST2084 is supported as well.
	VkColorSpaceKHR input_color_space;

	// Use VK_COLOR_SPACE_SRGB_NONLINEAR or HDR10_ST2084.
	// HDR10 is *not* tonemapped.
	VkColorSpaceKHR output_color_space;

	// YCbCr transform is always full-range, center chroma siting.
	// If output color space is HDR10_ST2084, BT.2020 NCL transform is used,
	// otherwise, BT.701 coefficients are used.

	// Intermediate format used for planes. Should be R8_UNORM or R16_UNORM.
	// R16_UNORM is more or less required for HDR10, but can be used for SDR too
	// to avoid some potential banding, especially for 10-bit SDR swapchains.
	// R8_UNORM intermediate format will receive dithering to avoid some banding artifacts.
	// The dither will smooth out nicely when encoding.
	VkFormat intermediate_plane_format;

	// In YCbCr, the center point for chroma may depend on bit depth in some cases.
	// Since Pyrowave is a floating point codec, this is mostly irrelevant for us,
	// but provided here for compatibility. Consumer of the final image is expected
	// to know which encoding for pure gray was used.
	// Common values would be 0.5 (bit-depth agnostic default),
	// 128.0 / 255.0 (8-bit BT) or 512.0 / 1023.0 (10-bit BT).
	float ycbcr_chroma_midpoint;

	// Instead of sinc, force plain LINEAR scaling filter.
	bool force_linear_filtering;

	// Skip any dithering for 8-bit outputs.
	bool skip_dither;

	// Optional.
	const VkRect2D *crop_rect;
} pyrowave_scaled_encode_info;

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_gpu_scaled_synchronous(pyrowave_encoder encoder,
                                               const pyrowave_gpu_sync_operation *acquire,
                                               const pyrowave_gpu_sync_operation *release,
                                               const pyrowave_scaled_encode_info *scaling_info,
                                               const pyrowave_rate_control *rate_control);

// A command buffer must not be set on pyrowave_device.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_encode_cpu_synchronous(pyrowave_encoder encoder, const pyrowave_cpu_buffer *buffers,
                                        const pyrowave_rate_control *rate_control);

// Can only be called after a successful encoding operation and result is only valid for that particular frame.
// Computes the number of network packets required if each packet can consume a provided number of bytes.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_compute_num_packets(pyrowave_encoder encoder, size_t packet_boundary, size_t *num_packets);
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_compute_num_packets_with_padding(
		pyrowave_encoder encoder, size_t packet_boundary, size_t padding_size, size_t *num_packets);
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_compute_num_critical_packets(
		pyrowave_encoder encoder, int bands, size_t packet_boundary, size_t padding_size, size_t *num_packets);

// Number of packets is implied to be greater-than-equal to num_packets as returned earlier.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_packetize(pyrowave_encoder encoder, pyrowave_packet *packets, size_t packet_boundary,
                           size_t *out_packets, void *bitstream, size_t size);
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_packetize_with_padding(
		pyrowave_encoder encoder, pyrowave_packet *packets, size_t packet_boundary, size_t padding_size,
		size_t *out_packets, void *bitstream, size_t size);

// Special purpose for manual packetization.
// Client is expected to understand the pyrowave bitstream format.
// Don't use if you don't know what you're doing.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_get_mapped_raw_bitstream(
		pyrowave_encoder encoder, const void **mapped_bitstream, size_t *mapped_bitstream_size,
		const void **mapped_metadata, size_t *mapped_metadata_size);

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_get_num_active_blocks(pyrowave_encoder encoder, int bands, size_t *num_active_blocks);

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_encoder_compute_block_active_words(pyrowave_encoder encoder,
		int bands, uint32_t *words, size_t word_count);

// Implementation ensures GPU is idle before destroying objects.
PYROWAVE_PUBLIC_API void
pyrowave_encoder_destroy(pyrowave_encoder encoder);
//////

// Decoder
typedef struct pyrowave_decoder_create_info
{
	pyrowave_device device;
	// For 420 subsampling, must be even.
	int width;
	int height;
	pyrowave_chroma_subsampling chroma;
	bool fragment_path;
} pyrowave_decoder_create_info;

// Fragment path is optimized for typical mobile GPUs which have weak compute support.
// iDWT is instead computed entirely in traditional render passes and fragment shaders.
// This path is *not* recommended for desktop-class chips.
PYROWAVE_PUBLIC_API bool
pyrowave_decoder_device_prefers_fragment_path(pyrowave_device device);

PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_decoder_create(const pyrowave_decoder_create_info *info, pyrowave_decoder *decoder);

// Throws away all queued packets.
PYROWAVE_PUBLIC_API void pyrowave_decoder_clear(pyrowave_decoder decoder);

// A frame is potentially split into multiple packets.
// If a packet is pushed for a frame that is deemed to arrive earlier, it is dropped.
// A packet that is pushed for a frame with a higher frame sequence will clear out the old queued frame and start a new frame.
// Packets are pushed into the decoder until decode_is_ready says it's ready.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_decoder_push_packet(pyrowave_decoder decoder, const void *data, size_t size);

// For error correction purposes, it may be okay to decode a frame which dropped some packets.
PYROWAVE_PUBLIC_API bool
pyrowave_decoder_decode_is_ready(pyrowave_decoder decoder, bool allow_partial_frame);

PYROWAVE_PUBLIC_API bool
pyrowave_decoder_decode_is_ready_with_sideband(pyrowave_decoder decoder, bool allow_partial_frame,
		int num_pristine_bands, float minimum_packet_ratio,
		const uint32_t *active_block_mask, size_t word_count);

// Decoding can be done at any time, leading to potentially corrupt/incomplete results if packets are missing.
// Missing wavelet weights are assumed to be 0 which can lead to extra blurring.
// See pyrowave_decoder_decode_is_ready() to determine if the final result is known to be complete.
// acquire and release can be NULL if no sync is required.
// If command buffer is set on pyrowave_device, acquire and release must both be NULL.
// If command buffer is set, application must ensure synchronization as:
// - Before, the layouts in buffers must be correct.
//   If fragment path, memory must be visible to COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE.
//   If compute path, memory must be visible to COMPUTE_SHADER / SHADER_STORAGE_WRITE.
// - After: Application must synchronize against the stages above before it can read or transition away.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_decoder_decode_gpu_buffer(pyrowave_decoder decoder,
                                   const pyrowave_gpu_sync_operation *acquire,
                                   const pyrowave_gpu_sync_operation *release,
                                   const pyrowave_gpu_buffers *buffers);

// A command buffer must not be set on pyrowave_device.
PYROWAVE_PUBLIC_API pyrowave_result
pyrowave_decoder_decode_cpu_buffer_synchronous(pyrowave_decoder decoder, const pyrowave_cpu_buffer *buffers);

// Implementation ensures GPU is idle before destroying objects.
PYROWAVE_PUBLIC_API void pyrowave_decoder_destroy(pyrowave_decoder decoder);
//////

#ifdef __cplusplus
}
#endif

#endif
