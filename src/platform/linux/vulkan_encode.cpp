/**
 * @file src/platform/linux/vulkan_encode.cpp
 * @brief Vulkan-native encoder: DMA-BUF -> Vulkan compute (RGB->YUV) -> Vulkan Video encode.
 *        No EGL/GL dependency — all GPU work stays in a single Vulkan queue.
 */
#include <array>
#include <cstring>
#include <cstdint>
#include <drm_fourcc.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#if defined(__FreeBSD__)
  #include <sys/types.h>
#else
  #include <sys/sysmacros.h>
#endif
#include <unistd.h>
#include <vector>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/video_colorspace.h"
#include "vulkan_encode.h"
#include "vulkan_encode_policy.h"

// SPIR-V data generated at build time
static const std::vector<uint32_t> rgb2yuv_comp_spv_data
#include "shaders/rgb2yuv.spv.inc"
  ;
static const size_t rgb2yuv_comp_spv_size = rgb2yuv_comp_spv_data.size() * sizeof(uint32_t);

using namespace std::literals;

namespace vk {

  struct resolved_vulkan_device_t {
    std::string index;
    std::array<std::uint8_t, VK_UUID_SIZE> uuid {};
  };

  // Match a DRI primary/render node to a Vulkan device and retain a stable UUID
  // so the independently created FFmpeg device can be verified before import.
  static std::optional<resolved_vulkan_device_t> find_vulkan_device_for_drm_node(const char *device_path) {
    struct stat node_stat;
    if (stat(device_path, &node_stat) < 0 || !S_ISCHR(node_stat.st_mode)) {
      return std::nullopt;
    }

    auto target_major = major(node_stat.st_rdev);
    auto target_minor = minor(node_stat.st_rdev);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;

    static const std::array<const char *, 1> instance_exts = {VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = instance_exts.size();
    ci.ppEnabledExtensionNames = instance_exts.data();
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) {
      return std::nullopt;
    }
    auto destroy_instance = util::fail_guard([&]() {
      vkDestroyInstance(inst, nullptr);
    });

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(inst, &count, nullptr) != VK_SUCCESS || count == 0) {
      return std::nullopt;
    }
    std::vector<VkPhysicalDevice> devs(count);
    if (vkEnumeratePhysicalDevices(inst, &count, devs.data()) != VK_SUCCESS) {
      return std::nullopt;
    }

    for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceDrmPropertiesEXT drm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
      VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      drm.pNext = &id;
      VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &drm;
      vkGetPhysicalDeviceProperties2(devs[i], &props2);
      const bool render_match = drm.hasRender && drm.renderMajor == (int64_t) target_major && drm.renderMinor == (int64_t) target_minor;
      const bool primary_match = drm.hasPrimary && drm.primaryMajor == (int64_t) target_major && drm.primaryMinor == (int64_t) target_minor;
      if (render_match || primary_match) {
        resolved_vulkan_device_t result {.index = std::to_string(i)};
        std::copy_n(id.deviceUUID, result.uuid.size(), result.uuid.begin());
        return result;
      }
    }
    return std::nullopt;
  }

  static bool vulkan_device_matches_uuid(AVBufferRef *hw_device_buf, const std::array<std::uint8_t, VK_UUID_SIZE> &expected_uuid) {
    if (!hw_device_buf || !hw_device_buf->data) {
      return false;
    }
    auto *device_ctx = reinterpret_cast<AVHWDeviceContext *>(hw_device_buf->data);
    auto *vulkan_ctx = reinterpret_cast<AVVulkanDeviceContext *>(device_ctx->hwctx);
    if (!vulkan_ctx || vulkan_ctx->phys_dev == VK_NULL_HANDLE) {
      return false;
    }

    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &id;
    vkGetPhysicalDeviceProperties2(vulkan_ctx->phys_dev, &props);
    return std::equal(expected_uuid.begin(), expected_uuid.end(), std::begin(id.deviceUUID));
  }

  static int create_vulkan_hwdevice(AVBufferRef **hw_device_buf, std::optional<std::string_view> exact_capture_device = std::nullopt) {
    if (!hw_device_buf) {
      return -1;
    }
    *hw_device_buf = nullptr;

    const bool exact_capture_device_required = exact_capture_device && !exact_capture_device->empty();
    const std::string requested_device = exact_capture_device_required ?
                                           std::string {*exact_capture_device} :
                                           platf::resolve_render_device();

    if (requested_device.starts_with("/")) {
      if (auto resolved = find_vulkan_device_for_drm_node(requested_device.c_str())) {
        if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, resolved->index.c_str(), nullptr, 0) >= 0) {
          if (vulkan_device_matches_uuid(*hw_device_buf, resolved->uuid)) {
            return 0;
          }
          BOOST_LOG(error) << "FFmpeg selected a Vulkan device that does not match capture device " << requested_device;
          av_buffer_unref(hw_device_buf);
        }
      } else if (exact_capture_device_required) {
        BOOST_LOG(error) << "Could not resolve exact Vulkan capture device " << requested_device;
      }
    } else if (!requested_device.empty()) {
      // Non-path: treat as device name substring or numeric index
      if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, requested_device.c_str(), nullptr, 0) >= 0) {
        return 0;
      }
    }

    if (!policy::may_fallback_to_default_device(exact_capture_device_required)) {
      return -1;
    }

    // Only paths without a capture-owned device identity may use the default.
    if (av_hwdevice_ctx_create(hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) >= 0) {
      return 0;
    }
    return -1;
  }

  struct PushConstants {
    std::array<float, 4> color_vec_y;
    std::array<float, 4> color_vec_u;
    std::array<float, 4> color_vec_v;
    std::array<float, 2> range_y;
    std::array<float, 2> range_uv;
    std::array<int32_t, 2> src_offset;
    std::array<int32_t, 2> src_size;
    std::array<int32_t, 2> dst_size;
    std::array<int32_t, 2> cursor_pos;
    std::array<int32_t, 2> cursor_size;
    int32_t y_invert;
  };

// Helper to check VkResult
#define VK_CHECK(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return -1; \
    } \
  } while (0)
#define VK_CHECK_BOOL(expr) \
  do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) { \
      BOOST_LOG(error) << #expr << " failed: " << _r; \
      return false; \
    } \
  } while (0)

  class vk_vram_t: public platf::avcodec_encode_device_t {
    struct src_image_t;
    struct cursor_image_t;

  public:
    ~vk_vram_t() override {
      cleanup_pipeline();
    }

    int init(
      int in_width,
      int in_height,
      int in_offset_x = 0,
      int in_offset_y = 0,
      std::optional<std::string> in_capture_device_path = std::nullopt
    ) {
      width = in_width;
      height = in_height;
      offset_x = in_offset_x;
      offset_y = in_offset_y;
      capture_device_path = std::move(in_capture_device_path);
      this->data = (void *) &init_hw_device;
      return 0;
    }

    void init_codec_options(AVCodecContext *ctx, AVDictionary **options) override {
      // When VBR mode is selected (rc_mode=4), don't pin rc_min_rate to the target bitrate.
      // Having rc_min_rate == rc_max_rate == bit_rate in VBR mode prevents the encoder from
      // undershooting on simple frames, which builds up headroom that causes large overshoots
      // on complex frames.
      if (config::video.vk.rc_mode == 4) {
        ctx->rc_min_rate = 0;
      }
    }

    int set_frame(AVFrame *new_frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(new_frame);
      this->frame = new_frame;
      this->hw_frames_ctx = hw_frames_ctx_buf;

      auto *frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      auto *dev_ctx = (AVHWDeviceContext *) frames_ctx->device_ref->data;
      ffmpeg_frames_ctx = frames_ctx;
      ffmpeg_device_ctx = dev_ctx;
      vk_dev.ctx = (AVVulkanDeviceContext *) dev_ctx->hwctx;
      vk_dev.dev = vk_dev.ctx->act_dev;
      vk_dev.phys_dev = vk_dev.ctx->phys_dev;
      is_10bit = (frames_ctx->sw_format == AV_PIX_FMT_P010);

      {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(vk_dev.phys_dev, &p);
        BOOST_LOG(info) << "Vulkan encode using GPU: " << p.deviceName;
      }

      // Find a compute-capable queue family from FFmpeg's context
      vk_dev.compute_qf = -1;
      for (int i = 0; i < vk_dev.ctx->nb_qf; i++) {
        if (vk_dev.ctx->qf[i].num > 0 && (vk_dev.ctx->qf[i].flags & VK_QUEUE_COMPUTE_BIT)) {
          vk_dev.compute_qf = vk_dev.ctx->qf[i].idx;
          break;
        }
      }
      if (vk_dev.compute_qf < 0) {
        BOOST_LOG(error) << "No compute queue family in Vulkan device"sv;
        return -1;
      }

      vkGetDeviceQueue(vk_dev.dev, vk_dev.compute_qf, 0, &vk_dev.compute_queue);

      // Load extension functions
      vk_dev.getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(vk_dev.dev, "vkGetMemoryFdPropertiesKHR");
      if (!vk_dev.getMemoryFdProperties) {
        BOOST_LOG(error) << "Vulkan device does not expose vkGetMemoryFdPropertiesKHR";
        return -1;
      }

      if (!create_compute_pipeline()) {
        return -1;
      }
      if (!create_command_resources()) {
        return -1;
      }

      return 0;
    }

    void apply_colorspace() override {
      auto *colors = video::color_vectors_from_colorspace(colorspace, true);
      if (colors) {
        memcpy(push.color_vec_y.data(), colors->color_vec_y, sizeof(push.color_vec_y));
        memcpy(push.color_vec_u.data(), colors->color_vec_u, sizeof(push.color_vec_u));
        memcpy(push.color_vec_v.data(), colors->color_vec_v, sizeof(push.color_vec_v));
        memcpy(push.range_y.data(), colors->range_y, sizeof(push.range_y));
        memcpy(push.range_uv.data(), colors->range_uv, sizeof(push.range_uv));
      }
    }

    void init_hwframes(AVHWFramesContext *frames) override {
      frames->initial_pool_size = 4;
      auto *vk_frames = (AVVulkanFramesContext *) frames->hwctx;
      vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
      vk_frames->usage = (VkImageUsageFlagBits) (VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
    }

    int convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;

      // Get encoder target frame
      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0) < 0) {
          BOOST_LOG(error) << "Failed to get Vulkan frame"sv;
          return -1;
        }
      }

      if (descriptor.sequence == 0) {
        // Dummy frame — clear the target
        return 0;
      }

      const auto submission_slot = static_cast<std::size_t>(cmd.ring_idx);
      cmd.ring_idx = (cmd.ring_idx + 1) % CMD_RING_SIZE;
      if (!prepare_submission_slot(submission_slot)) {
        return -1;
      }

      if (!import_dmabuf(descriptor.sd, cmd.sources[submission_slot])) {
        BOOST_LOG(error) << "Failed to import DMA-BUF"sv;
        return -1;
      }

      if (descriptor.data && descriptor.serial != cursor_serial) {
        if (!replace_cursor_image(descriptor.src_w, descriptor.src_h, descriptor.data)) {
          return -1;
        }
        cursor_serial = descriptor.serial;
      }

      auto *vk_frame = reinterpret_cast<AVVkFrame *>(frame->data[0]);
      auto *vk_frames_ctx = ffmpeg_frames_ctx ?
                              reinterpret_cast<AVVulkanFramesContext *>(ffmpeg_frames_ctx->hwctx) :
                              nullptr;
      if (!vk_frame || !vk_frames_ctx || !vk_frames_ctx->lock_frame || !vk_frames_ctx->unlock_frame) {
        BOOST_LOG(error) << "FFmpeg Vulkan frame synchronization hooks are unavailable"sv;
        return -1;
      }

      vk_frames_ctx->lock_frame(ffmpeg_frames_ctx, vk_frame);
      auto unlock_frame = util::fail_guard([&]() {
        vk_frames_ctx->unlock_frame(ffmpeg_frames_ctx, vk_frame);
      });

      // Setup Y/UV image views for the encoder target while its metadata is locked.
      if (!target.views_created) {
        if (!create_target_views(vk_frame)) {
          return -1;
        }
        target.views_created = true;
      }

      update_descriptors(submission_slot);

      // Fill push constants
      push.src_offset[0] = offset_x;
      push.src_offset[1] = offset_y;
      push.src_size[0] = width;
      push.src_size[1] = height;
      push.dst_size[0] = frame->width;
      push.dst_size[1] = frame->height;
      push.y_invert = descriptor.y_invert ? 1 : 0;

      if (descriptor.data) {
        float scale_x = (float) frame->width / width;
        float scale_y = (float) frame->height / height;
        push.cursor_pos[0] = (int32_t) ((descriptor.x - offset_x) * scale_x);
        push.cursor_pos[1] = (int32_t) ((descriptor.y - offset_y) * scale_y);
        push.cursor_size[0] = (int32_t) (descriptor.width * scale_x);
        push.cursor_size[1] = (int32_t) (descriptor.height * scale_y);
      } else {
        push.cursor_size[0] = 0;
      }

      // Record and submit compute dispatch
      return dispatch_compute(submission_slot, vk_frame);
    }

  private:
    bool create_compute_pipeline() {
      // Shader module
      VkShaderModuleCreateInfo shader_ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shader_ci.codeSize = rgb2yuv_comp_spv_size;
      shader_ci.pCode = rgb2yuv_comp_spv_data.data();
      VK_CHECK_BOOL(vkCreateShaderModule(vk_dev.dev, &shader_ci, nullptr, &compute.shader_module));

      // Descriptor set layout: binding 0=sampler, 1=Y storage, 2=UV storage, 3=cursor sampler
      std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
      bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

      VkDescriptorSetLayoutCreateInfo ds_layout_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      ds_layout_ci.bindingCount = bindings.size();
      ds_layout_ci.pBindings = bindings.data();
      VK_CHECK_BOOL(vkCreateDescriptorSetLayout(vk_dev.dev, &ds_layout_ci, nullptr, &compute.ds_layout));

      // Push constant range
      VkPushConstantRange pc_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};

      VkPipelineLayoutCreateInfo pl_ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl_ci.setLayoutCount = 1;
      pl_ci.pSetLayouts = &compute.ds_layout;
      pl_ci.pushConstantRangeCount = 1;
      pl_ci.pPushConstantRanges = &pc_range;
      VK_CHECK_BOOL(vkCreatePipelineLayout(vk_dev.dev, &pl_ci, nullptr, &compute.pipeline_layout));

      // Compute pipeline
      VkComputePipelineCreateInfo comp_ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      comp_ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      comp_ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      comp_ci.stage.module = compute.shader_module;
      comp_ci.stage.pName = "main";
      comp_ci.layout = compute.pipeline_layout;
      VK_CHECK_BOOL(vkCreateComputePipelines(vk_dev.dev, VK_NULL_HANDLE, 1, &comp_ci, nullptr, &compute.pipeline));

      // Descriptor pool
      std::array<VkDescriptorPoolSize, 2> pool_sizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * CMD_RING_SIZE},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * CMD_RING_SIZE},
      }};
      VkDescriptorPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pool_ci.maxSets = CMD_RING_SIZE;
      pool_ci.poolSizeCount = pool_sizes.size();
      pool_ci.pPoolSizes = pool_sizes.data();
      VK_CHECK_BOOL(vkCreateDescriptorPool(vk_dev.dev, &pool_ci, nullptr, &compute.desc_pool));

      std::array<VkDescriptorSetLayout, CMD_RING_SIZE> descriptor_layouts;
      descriptor_layouts.fill(compute.ds_layout);
      VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc_info.descriptorPool = compute.desc_pool;
      alloc_info.descriptorSetCount = descriptor_layouts.size();
      alloc_info.pSetLayouts = descriptor_layouts.data();
      VK_CHECK_BOOL(vkAllocateDescriptorSets(vk_dev.dev, &alloc_info, compute.desc_sets.data()));

      // Sampler for source image
      VkSamplerCreateInfo sampler_ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler_ci.magFilter = VK_FILTER_LINEAR;
      sampler_ci.minFilter = VK_FILTER_LINEAR;
      sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      VK_CHECK_BOOL(vkCreateSampler(vk_dev.dev, &sampler_ci, nullptr, &compute.sampler));

      if (!create_cursor_image(cursor, 1, 1, nullptr)) {
        return false;
      }

      return true;
    }

    bool create_command_resources() {
      VkCommandPoolCreateInfo pool_ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_ci.queueFamilyIndex = vk_dev.compute_qf;
      pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      VK_CHECK_BOOL(vkCreateCommandPool(vk_dev.dev, &pool_ci, nullptr, &cmd.pool));

      VkCommandBufferAllocateInfo alloc_ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc_ci.commandPool = cmd.pool;
      alloc_ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc_ci.commandBufferCount = CMD_RING_SIZE;
      VK_CHECK_BOOL(vkAllocateCommandBuffers(vk_dev.dev, &alloc_ci, cmd.ring.data()));

      VkFenceCreateInfo fence_ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      for (auto &fence : cmd.fences) {
        VK_CHECK_BOOL(vkCreateFence(vk_dev.dev, &fence_ci, nullptr, &fence));
      }

      return true;
    }

    struct drm_format_info {
      VkFormat format;
      VkComponentMapping swizzle;
    };

    static drm_format_info drm_fourcc_to_vk_format(uint32_t fourcc) {
      static constexpr VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
      };
      static constexpr VkComponentMapping bgr_swap = {
        VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_R,
        VK_COMPONENT_SWIZZLE_A,
      };

      switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
          return {VK_FORMAT_R8G8B8A8_UNORM, identity};
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
          return {VK_FORMAT_A2R10G10B10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
          return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, identity};
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_ABGR16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, identity};
        case DRM_FORMAT_XRGB16161616:
        case DRM_FORMAT_ARGB16161616:
          return {VK_FORMAT_R16G16B16A16_UNORM, bgr_swap};
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, identity};
        case DRM_FORMAT_XRGB16161616F:
        case DRM_FORMAT_ARGB16161616F:
          return {VK_FORMAT_R16G16B16A16_SFLOAT, bgr_swap};
        default:
          BOOST_LOG(warning) << "Unknown DRM fourcc 0x" << std::hex << fourcc << std::dec << ", assuming B8G8R8A8";
          return {VK_FORMAT_B8G8R8A8_UNORM, identity};
      }
    }

    /**
     * @brief Query the driver-expected plane count for a format+modifier pair.
     * @return Expected plane count, or 0 if unknown.
     */
    int query_modifier_plane_count(VkFormat format, uint64_t modifier) {
      VkDrmFormatModifierPropertiesListEXT mod_list = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 fmt_props2 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      fmt_props2.pNext = &mod_list;
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      std::vector<VkDrmFormatModifierPropertiesEXT> mod_props(mod_list.drmFormatModifierCount);
      mod_list.pDrmFormatModifierProperties = mod_props.data();
      vkGetPhysicalDeviceFormatProperties2(vk_dev.phys_dev, format, &fmt_props2);
      for (const auto &mp : mod_props) {
        if (mp.drmFormatModifier == modifier) {
          return mp.drmFormatModifierPlaneCount;
        }
      }
      return 0;
    }

    bool prepare_submission_slot(std::size_t slot) {
      if (cmd.in_flight[slot]) {
        const auto wait_result = vkWaitForFences(
          vk_dev.dev,
          1,
          &cmd.fences[slot],
          VK_TRUE,
          std::numeric_limits<std::uint64_t>::max()
        );
        if (wait_result != VK_SUCCESS) {
          BOOST_LOG(error) << "vkWaitForFences failed: " << wait_result;
          return false;
        }
        cmd.in_flight[slot] = false;
      }

      const auto reset_fence_result = vkResetFences(vk_dev.dev, 1, &cmd.fences[slot]);
      if (reset_fence_result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkResetFences failed: " << reset_fence_result;
        return false;
      }
      const auto reset_command_result = vkResetCommandBuffer(cmd.ring[slot], 0);
      if (reset_command_result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkResetCommandBuffer failed: " << reset_command_result;
        return false;
      }
      destroy_src_image(cmd.sources[slot]);
      return true;
    }

    bool wait_for_all_submissions() {
      for (std::size_t slot = 0; slot < CMD_RING_SIZE; ++slot) {
        if (!cmd.in_flight[slot]) {
          continue;
        }
        const auto result = vkWaitForFences(
          vk_dev.dev,
          1,
          &cmd.fences[slot],
          VK_TRUE,
          std::numeric_limits<std::uint64_t>::max()
        );
        if (result != VK_SUCCESS) {
          BOOST_LOG(error) << "vkWaitForFences failed while replacing cursor: " << result;
          return false;
        }
        cmd.in_flight[slot] = false;
      }
      return true;
    }

    bool import_dmabuf(const egl::surface_descriptor_t &sd, src_image_t &output) {
      if (sd.fds[0] < 0 || sd.width <= 0 || sd.height <= 0 || sd.pitches[0] == 0) {
        BOOST_LOG(error) << "Invalid DMA-BUF descriptor"sv;
        return false;
      }

      src_image_t candidate;
      int fd = dup(sd.fds[0]);
      if (fd < 0) {
        BOOST_LOG(error) << "Failed to duplicate DMA-BUF descriptor: " << strerror(errno);
        return false;
      }
      auto cleanup = util::fail_guard([&]() {
        if (fd >= 0) {
          close(fd);
        }
        destroy_src_image(candidate);
      });

      VkMemoryFdPropertiesKHR fd_props = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      const auto fd_properties_result = vk_dev.getMemoryFdProperties(
        vk_dev.dev,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        fd,
        &fd_props
      );
      if (fd_properties_result != VK_SUCCESS || fd_props.memoryTypeBits == 0) {
        BOOST_LOG(error) << "vkGetMemoryFdPropertiesKHR failed: " << fd_properties_result;
        return false;
      }

      // Create VkImage for the DMA-BUF
      VkExternalMemoryImageCreateInfo ext_ci = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      ext_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

      std::array<VkSubresourceLayout, 4> drm_layouts = {};
      VkImageDrmFormatModifierExplicitCreateInfoEXT drm_ci = {
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT
      };
      VkImageTiling tiling;

      auto [vk_format, vk_swizzle] = drm_fourcc_to_vk_format(sd.fourcc);

      if (sd.modifier != DRM_FORMAT_MOD_INVALID) {
        int dmabuf_planes = 0;
        for (int i = 0; i < 4 && sd.fds[i] >= 0; ++i) {
          dmabuf_planes++;
        }

        // Query driver for the expected plane count for this format+modifier.
        // DMA-BUF exports may include extra metadata planes (e.g. AMD DCC).
        const int expected = query_modifier_plane_count(vk_format, sd.modifier);
        if (expected <= 0 || expected > dmabuf_planes || expected > static_cast<int>(drm_layouts.size())) {
          BOOST_LOG(error) << "Unsupported DMA-BUF modifier plane layout: expected=" << expected
                           << ", available=" << dmabuf_planes;
          return false;
        }
        const int plane_count = expected;

        for (int i = 0; i < plane_count; ++i) {
          drm_layouts[i].offset = sd.offsets[i];
          drm_layouts[i].rowPitch = sd.pitches[i];
        }
        drm_ci.drmFormatModifier = sd.modifier;
        drm_ci.drmFormatModifierPlaneCount = plane_count;
        drm_ci.pPlaneLayouts = drm_layouts.data();
        ext_ci.pNext = &drm_ci;
        tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      } else {
        tiling = VK_IMAGE_TILING_LINEAR;
      }

      VkImageCreateInfo img_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.pNext = &ext_ci;
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = vk_format;
      img_ci.extent = {(uint32_t) sd.width, (uint32_t) sd.height, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = tiling;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      auto res = vkCreateImage(vk_dev.dev, &img_ci, nullptr, &candidate.image);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImage for DMA-BUF failed: " << res
                         << " (modifier=0x" << std::hex << sd.modifier << std::dec
                         << ", pitch=" << sd.pitches[0] << ", offset=" << sd.offsets[0] << ")";
        return false;
      }

      // Bind imported DMA-BUF memory
      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, candidate.image, &mem_req);

      VkImportMemoryFdInfoKHR import_fd = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      import_fd.fd = fd;  // Vulkan takes ownership

      VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.pNext = &import_fd;
      alloc_info.allocationSize = mem_req.size;
      const auto memory_type = find_memory_type(
        fd_props.memoryTypeBits & mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        true
      );
      if (!memory_type) {
        BOOST_LOG(error) << "No compatible Vulkan memory type for DMA-BUF import"sv;
        return false;
      }
      alloc_info.memoryTypeIndex = *memory_type;

      res = vkAllocateMemory(vk_dev.dev, &alloc_info, nullptr, &candidate.mem);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkAllocateMemory for DMA-BUF failed: " << res;
        return false;
      }
      // Successful import transfers descriptor ownership to Vulkan.
      fd = -1;

      res = vkBindImageMemory(vk_dev.dev, candidate.image, candidate.mem, 0);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkBindImageMemory for DMA-BUF failed: " << res;
        return false;
      }

      // Create image view
      VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = candidate.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = vk_format;
      view_ci.components = vk_swizzle;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      res = vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &candidate.view);
      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImageView for DMA-BUF failed: " << res;
        return false;
      }

      output = candidate;
      candidate = {};
      return true;
    }

    bool create_cursor_image(cursor_image_t &output, int w, int h, const uint8_t *pixels) {
      if (w <= 0 || h <= 0 || static_cast<std::size_t>(w) > std::numeric_limits<std::size_t>::max() / 4) {
        return false;
      }

      cursor_image_t candidate;
      auto cleanup = util::fail_guard([&]() {
        destroy_cursor_image(candidate);
      });

      VkImageCreateInfo img_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      img_ci.imageType = VK_IMAGE_TYPE_2D;
      img_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      img_ci.extent = {(uint32_t) w, (uint32_t) h, 1};
      img_ci.mipLevels = 1;
      img_ci.arrayLayers = 1;
      img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
      img_ci.tiling = VK_IMAGE_TILING_LINEAR;
      img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      img_ci.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
      auto result = vkCreateImage(vk_dev.dev, &img_ci, nullptr, &candidate.image);
      if (result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImage for cursor failed: " << result;
        return false;
      }

      VkMemoryRequirements mem_req;
      vkGetImageMemoryRequirements(vk_dev.dev, candidate.image, &mem_req);
      VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.allocationSize = mem_req.size;
      const auto memory_type = find_memory_type(
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        false
      );
      if (!memory_type) {
        BOOST_LOG(error) << "No host-visible coherent Vulkan memory type for cursor"sv;
        return false;
      }
      alloc.memoryTypeIndex = *memory_type;
      result = vkAllocateMemory(vk_dev.dev, &alloc, nullptr, &candidate.mem);
      if (result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkAllocateMemory for cursor failed: " << result;
        return false;
      }
      result = vkBindImageMemory(vk_dev.dev, candidate.image, candidate.mem, 0);
      if (result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkBindImageMemory for cursor failed: " << result;
        return false;
      }

      if (pixels) {
        void *mapped = nullptr;
        result = vkMapMemory(vk_dev.dev, candidate.mem, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS) {
          BOOST_LOG(error) << "vkMapMemory for cursor failed: " << result;
          return false;
        }
        auto unmap = util::fail_guard([&]() {
          vkUnmapMemory(vk_dev.dev, candidate.mem);
        });
        VkImageSubresource subres = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout layout;
        vkGetImageSubresourceLayout(vk_dev.dev, candidate.image, &subres, &layout);
        const auto row_bytes = static_cast<VkDeviceSize>(w) * 4;
        const auto source_row_bytes = static_cast<std::size_t>(w) * 4;
        const auto row_count = static_cast<VkDeviceSize>(h);
        if (layout.rowPitch < row_bytes || layout.offset > mem_req.size ||
            row_bytes > mem_req.size - layout.offset ||
            (row_count > 1 && (row_count - 1) > (mem_req.size - layout.offset - row_bytes) / layout.rowPitch)) {
          BOOST_LOG(error) << "Cursor image layout exceeds allocated memory"sv;
          return false;
        }
        for (int y = 0; y < h; y++) {
          memcpy(
            static_cast<std::uint8_t *>(mapped) + layout.offset + static_cast<VkDeviceSize>(y) * layout.rowPitch,
            pixels + static_cast<std::size_t>(y) * source_row_bytes,
            source_row_bytes
          );
        }
      }

      VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_ci.image = candidate.image;
      view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
      view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      result = vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &candidate.view);
      if (result != VK_SUCCESS) {
        BOOST_LOG(error) << "vkCreateImageView for cursor failed: " << result;
        return false;
      }

      candidate.needs_transition = true;
      output = candidate;
      candidate = {};
      return true;
    }

    bool replace_cursor_image(int w, int h, const uint8_t *pixels) {
      if (!wait_for_all_submissions()) {
        return false;
      }
      cursor_image_t replacement;
      if (!create_cursor_image(replacement, w, h, pixels)) {
        return false;
      }
      destroy_cursor_image(cursor);
      cursor = replacement;
      return true;
    }

    void destroy_cursor_image(cursor_image_t &image) {
      if (image.view) {
        vkDestroyImageView(vk_dev.dev, image.view, nullptr);
      }
      if (image.image) {
        vkDestroyImage(vk_dev.dev, image.image, nullptr);
      }
      if (image.mem) {
        vkFreeMemory(vk_dev.dev, image.mem, nullptr);
      }
      image = {};
    }

    bool create_target_views(AVVkFrame *vk_frame) {
      if (!vk_frame) {
        return false;
      }

      auto y_fmt = is_10bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
      auto uv_fmt = is_10bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;

      // Detect multiplane vs multi-image layout
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }
      if (num_imgs != 1 && num_imgs != 2) {
        BOOST_LOG(error) << "Unsupported FFmpeg Vulkan frame image layout"sv;
        return false;
      }

      if (num_imgs == 1) {
        // Single multiplane image — create plane views
        VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image = vk_frame->img[0];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;

        // Y plane
        view_ci.format = y_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        // UV plane
        view_ci.format = uv_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      } else {
        // Separate images per plane
        VkImageViewCreateInfo view_ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        view_ci.image = vk_frame->img[0];
        view_ci.format = y_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.y_view));

        view_ci.image = vk_frame->img[1];
        view_ci.format = uv_fmt;
        VK_CHECK_BOOL(vkCreateImageView(vk_dev.dev, &view_ci, nullptr, &target.uv_view));
      }
      return true;
    }

    void update_descriptors(std::size_t slot) {
      VkDescriptorImageInfo src_info = {compute.sampler, cmd.sources[slot].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo y_info = {VK_NULL_HANDLE, target.y_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo uv_info = {VK_NULL_HANDLE, target.uv_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo cursor_info = {compute.sampler, cursor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

      std::array<VkWriteDescriptorSet, 4> writes = {};
      writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_sets[slot], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src_info, nullptr, nullptr};
      writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_sets[slot], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &y_info, nullptr, nullptr};
      writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_sets[slot], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &uv_info, nullptr, nullptr};
      writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, compute.desc_sets[slot], 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cursor_info, nullptr, nullptr};
      vkUpdateDescriptorSets(vk_dev.dev, writes.size(), writes.data(), 0, nullptr);
    }

    int dispatch_compute(std::size_t slot, AVVkFrame *vk_frame) {
      int num_imgs = 0;
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        num_imgs++;
      }
      if (num_imgs != 1 && num_imgs != 2) {
        BOOST_LOG(error) << "Unsupported FFmpeg Vulkan frame image layout"sv;
        return -1;
      }

      auto cmd_buf = cmd.ring[slot];

      VkCommandBufferBeginInfo begin_ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin_ci.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      VK_CHECK(vkBeginCommandBuffer(cmd_buf, &begin_ci));

      // Transition source image to SHADER_READ_ONLY
      VkImageMemoryBarrier2 src_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      src_barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      src_barrier.srcAccessMask = VK_ACCESS_2_NONE;
      src_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      src_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      src_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      src_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      src_barrier.image = cmd.sources[slot].image;
      src_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      src_barrier.dstQueueFamilyIndex = vk_dev.compute_qf;
      VkDependencyInfo source_dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      source_dependency.imageMemoryBarrierCount = 1;
      source_dependency.pImageMemoryBarriers = &src_barrier;
      vkCmdPipelineBarrier2(cmd_buf, &source_dependency);

      // Transition cursor image if needed
      if (cursor.needs_transition) {
        VkImageMemoryBarrier2 cursor_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        cursor_barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        cursor_barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        cursor_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        cursor_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        cursor_barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        cursor_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cursor_barrier.image = cursor.image;
        cursor_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cursor_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cursor_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        VkDependencyInfo cursor_dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        cursor_dependency.imageMemoryBarrierCount = 1;
        cursor_dependency.pImageMemoryBarriers = &cursor_barrier;
        vkCmdPipelineBarrier2(cmd_buf, &cursor_dependency);
      }

      // Transition target planes to GENERAL for storage writes
      std::array<VkImageMemoryBarrier2, 2> dst_barriers = {};
      int num_dst_barriers = (num_imgs == 1) ? 1 : 2;
      for (int i = 0; i < num_dst_barriers; i++) {
        const int image_index = num_imgs == 1 ? 0 : i;
        dst_barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        dst_barriers[i].srcStageMask = vk_frame->access[image_index] ?
                                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT :
                                         VK_PIPELINE_STAGE_2_NONE;
        dst_barriers[i].srcAccessMask = vk_frame->access[image_index];
        dst_barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        dst_barriers[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        dst_barriers[i].oldLayout = vk_frame->layout[image_index];
        dst_barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst_barriers[i].image = vk_frame->img[image_index];
        dst_barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dst_barriers[i].srcQueueFamilyIndex = vk_frame->queue_family[image_index];
        dst_barriers[i].dstQueueFamilyIndex = vk_frame->queue_family[image_index] == VK_QUEUE_FAMILY_IGNORED ?
                                                VK_QUEUE_FAMILY_IGNORED :
                                                static_cast<std::uint32_t>(vk_dev.compute_qf);
      }

      VkDependencyInfo target_dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      target_dependency.imageMemoryBarrierCount = num_dst_barriers;
      target_dependency.pImageMemoryBarriers = dst_barriers.data();
      vkCmdPipelineBarrier2(cmd_buf, &target_dependency);

      // Bind pipeline and dispatch
      vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline);
      vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline_layout, 0, 1, &compute.desc_sets[slot], 0, nullptr);
      vkCmdPushConstants(cmd_buf, compute.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &push);

      uint32_t gx = (frame->width + 15) / 16;
      uint32_t gy = (frame->height + 15) / 16;
      vkCmdDispatch(cmd_buf, gx, gy, 1);

      VK_CHECK(vkEndCommandBuffer(cmd_buf));

      // Submit with timeline semaphore signaling for FFmpeg
      VkTimelineSemaphoreSubmitInfo timeline_info = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> wait_sems = {};
      std::array<VkSemaphore, AV_NUM_DATA_POINTERS> signal_sems = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> wait_vals = {};
      std::array<uint64_t, AV_NUM_DATA_POINTERS> signal_vals = {};
      std::array<VkPipelineStageFlags, AV_NUM_DATA_POINTERS> wait_stages = {};
      int sem_count = 0;

      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->sem[i]; i++) {
        wait_sems[sem_count] = vk_frame->sem[i];
        wait_vals[sem_count] = vk_frame->sem_value[i];
        wait_stages[sem_count] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        signal_sems[sem_count] = vk_frame->sem[i];
        signal_vals[sem_count] = vk_frame->sem_value[i] + 1;
        sem_count++;
      }
      if (sem_count != num_imgs) {
        BOOST_LOG(error) << "FFmpeg Vulkan frame is missing timeline semaphores"sv;
        return -1;
      }

      timeline_info.waitSemaphoreValueCount = sem_count;
      timeline_info.pWaitSemaphoreValues = wait_vals.data();
      timeline_info.signalSemaphoreValueCount = sem_count;
      timeline_info.pSignalSemaphoreValues = signal_vals.data();

      VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.pNext = &timeline_info;
      submit.waitSemaphoreCount = sem_count;
      submit.pWaitSemaphores = wait_sems.data();
      submit.pWaitDstStageMask = wait_stages.data();
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd_buf;
      submit.signalSemaphoreCount = sem_count;
      submit.pSignalSemaphores = signal_sems.data();

#if FF_API_VULKAN_SYNC_QUEUES
      // Keep the compatibility callbacks until internally synchronized queues are
      // guaranteed across supported FFmpeg and Vulkan driver combinations.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
      if (!vk_dev.ctx->lock_queue || !vk_dev.ctx->unlock_queue || !ffmpeg_device_ctx) {
        BOOST_LOG(error) << "FFmpeg Vulkan queue synchronization hooks are unavailable"sv;
        return -1;
      }
      vk_dev.ctx->lock_queue(ffmpeg_device_ctx, vk_dev.compute_qf, 0);
      auto unlock_queue = util::fail_guard([&]() {
        vk_dev.ctx->unlock_queue(ffmpeg_device_ctx, vk_dev.compute_qf, 0);
      });
  #pragma GCC diagnostic pop
#endif

      auto res = vkQueueSubmit(vk_dev.compute_queue, 1, &submit, cmd.fences[slot]);

      if (res != VK_SUCCESS) {
        BOOST_LOG(error) << "vkQueueSubmit failed: " << res;
        return -1;
      }
      cmd.in_flight[slot] = true;
      cursor.needs_transition = false;

      // Update frame layouts for FFmpeg
      for (int i = 0; i < AV_NUM_DATA_POINTERS && vk_frame->img[i]; i++) {
        vk_frame->sem_value[i]++;
        vk_frame->layout[i] = VK_IMAGE_LAYOUT_GENERAL;
#if LIBAVUTIL_VERSION_MAJOR >= 61
        vk_frame->access[i] = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
#else
        vk_frame->access[i] = VK_ACCESS_SHADER_WRITE_BIT;
#endif
        vk_frame->queue_family[i] = vk_dev.compute_qf;
      }

      return 0;
    }

    std::optional<std::uint32_t> find_memory_type(
      std::uint32_t type_bits,
      VkMemoryPropertyFlags props,
      bool allow_compatible_fallback
    ) {
      VkPhysicalDeviceMemoryProperties mem_props;
      vkGetPhysicalDeviceMemoryProperties(vk_dev.phys_dev, &mem_props);
      std::array<std::uint32_t, VK_MAX_MEMORY_TYPES> property_flags {};
      for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        property_flags[i] = mem_props.memoryTypes[i].propertyFlags;
      }
      return policy::select_memory_type(
        type_bits,
        std::span<const std::uint32_t> {property_flags.data(), mem_props.memoryTypeCount},
        props,
        allow_compatible_fallback
      );
    }

    void destroy_src_image(src_image_t &image) {
      if (image.view) {
        vkDestroyImageView(vk_dev.dev, image.view, nullptr);
      }
      if (image.image) {
        vkDestroyImage(vk_dev.dev, image.image, nullptr);
      }
      if (image.mem) {
        vkFreeMemory(vk_dev.dev, image.mem, nullptr);
      }
      image = {};
    }

    void cleanup_pipeline() {
      if (!vk_dev.dev) {
        return;
      }
      vkDeviceWaitIdle(vk_dev.dev);
      for (auto &source : cmd.sources) {
        destroy_src_image(source);
      }
      if (target.y_view) {
        vkDestroyImageView(vk_dev.dev, target.y_view, nullptr);
      }
      if (target.uv_view) {
        vkDestroyImageView(vk_dev.dev, target.uv_view, nullptr);
      }
      destroy_cursor_image(cursor);
      for (auto &fence : cmd.fences) {
        if (fence) {
          vkDestroyFence(vk_dev.dev, fence, nullptr);
          fence = VK_NULL_HANDLE;
        }
      }
      if (cmd.pool) {
        vkDestroyCommandPool(vk_dev.dev, cmd.pool, nullptr);
        cmd.pool = VK_NULL_HANDLE;
      }
      if (compute.sampler) {
        vkDestroySampler(vk_dev.dev, compute.sampler, nullptr);
      }
      if (compute.desc_pool) {
        vkDestroyDescriptorPool(vk_dev.dev, compute.desc_pool, nullptr);
      }
      if (compute.pipeline) {
        vkDestroyPipeline(vk_dev.dev, compute.pipeline, nullptr);
      }
      if (compute.pipeline_layout) {
        vkDestroyPipelineLayout(vk_dev.dev, compute.pipeline_layout, nullptr);
      }
      if (compute.ds_layout) {
        vkDestroyDescriptorSetLayout(vk_dev.dev, compute.ds_layout, nullptr);
      }
      if (compute.shader_module) {
        vkDestroyShaderModule(vk_dev.dev, compute.shader_module, nullptr);
      }
    }

    static int init_hw_device(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf) {
      const auto *self = static_cast<vk_vram_t *>(encode_device);
      const std::optional<std::string_view> exact_capture_device =
        self && self->capture_device_path ?
          std::optional<std::string_view> {*self->capture_device_path} :
          std::nullopt;
      return create_vulkan_hwdevice(hw_device_buf, exact_capture_device);
    }

    // Dimensions
    int width = 0;
    int height = 0;
    int offset_x = 0;
    int offset_y = 0;
    bool is_10bit = false;
    AVBufferRef *hw_frames_ctx = nullptr;
    frame_t hwframe;

    // Vulkan device (from FFmpeg)
    struct vk_device_t {
      VkDevice dev = VK_NULL_HANDLE;
      VkPhysicalDevice phys_dev = VK_NULL_HANDLE;
      AVVulkanDeviceContext *ctx = nullptr;
      int compute_qf = -1;
      VkQueue compute_queue = VK_NULL_HANDLE;
      PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
    };

    vk_device_t vk_dev = {};

    static constexpr int CMD_RING_SIZE = 3;

    struct src_image_t {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
    };

    struct cursor_image_t {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
      bool needs_transition = false;
    };

    // Compute pipeline
    struct compute_pipeline_t {
      VkShaderModule shader_module = VK_NULL_HANDLE;
      VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
      VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
      VkDescriptorPool desc_pool = VK_NULL_HANDLE;
      std::array<VkDescriptorSet, CMD_RING_SIZE> desc_sets {};
      VkSampler sampler = VK_NULL_HANDLE;
    };

    compute_pipeline_t compute = {};

    struct cmd_submission_t {
      VkCommandPool pool = VK_NULL_HANDLE;
      std::array<VkCommandBuffer, CMD_RING_SIZE> ring = {};
      std::array<VkFence, CMD_RING_SIZE> fences = {};
      std::array<bool, CMD_RING_SIZE> in_flight = {};
      std::array<src_image_t, CMD_RING_SIZE> sources = {};
      int ring_idx = 0;
    };

    cmd_submission_t cmd = {};

    // Target NV12 plane views
    struct target_state_t {
      VkImageView y_view = VK_NULL_HANDLE;
      VkImageView uv_view = VK_NULL_HANDLE;
      bool views_created = false;
    };

    target_state_t target = {};

    cursor_image_t cursor = {};

    unsigned long cursor_serial = 0;

    // Push constants (color matrix)
    PushConstants push = {};

    AVHWFramesContext *ffmpeg_frames_ctx = nullptr;
    AVHWDeviceContext *ffmpeg_device_ctx = nullptr;
    std::optional<std::string> capture_device_path;
  };

  // Free functions

  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *, AVBufferRef **hw_device_buf) {
    return create_vulkan_hwdevice(hw_device_buf);
  }

  bool validate() {
    if (!avcodec_find_encoder_by_name("h264_vulkan") && !avcodec_find_encoder_by_name("hevc_vulkan")) {
      return false;
    }
    AVBufferRef *dev = nullptr;
    if (create_vulkan_hwdevice(&dev) < 0) {
      return false;
    }
    av_buffer_unref(&dev);
    return true;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(
    int w,
    int h,
    int offset_x,
    int offset_y,
    std::optional<std::string> capture_device_path
  ) {
    auto dev = std::make_unique<vk_vram_t>();
    if (dev->init(w, h, offset_x, offset_y, std::move(capture_device_path)) < 0) {
      return nullptr;
    }
    return dev;
  }

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(int, int) {
    return nullptr;
  }

}  // namespace vk
