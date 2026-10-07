/** @file src/platform/linux/pyrowave_core.cpp
 * @brief GPU capture conversion and synchronous wavelet encoding.
 */
#include "pyrowave_core.h"

#include "command_buffer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "pyrowave_encoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace pyrowave::linux_gpu {
  namespace {
    const std::vector<std::uint32_t> shader
#include "shaders/pyrowave.spv.inc"
      ;
    std::mutex loader_mutex;

    VkFormat format_for(std::uint32_t fourcc) {
      switch (fourcc) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
          return VK_FORMAT_B8G8R8A8_UNORM;
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ABGR8888:
          return VK_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_ARGB2101010:
          return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_ABGR2101010:
          return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DRM_FORMAT_XBGR16161616F:
        case DRM_FORMAT_ABGR16161616F:
          return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_ABGR16161616:
          return VK_FORMAT_R16G16B16A16_UNORM;
        default:
          return VK_FORMAT_UNDEFINED;
      }
    }

    struct alignas(16) uniform_t {
      std::array<float, 12> matrix;
      std::array<int, 4> crop, cursor;
      std::array<int, 2> output;
      int subsampled, invert_y, use_lut, transfer, output_hdr;
    };
  }  // namespace

  struct core_t::impl_t {
    // Destruction order matters: every GPU resource must die before the device.
    Vulkan::Context context;
    Vulkan::Device device;
    PyroWave::Encoder encoder;
    config_t config;
    Vulkan::Program *program = nullptr;
    Vulkan::ImageHandle planes[3];
    Vulkan::BufferHandle meta_gpu, bits_gpu, meta_cpu, bits_cpu;
    bool initialized = false;

    ~impl_t() {
      if (initialized) {
        device.wait_idle();
      }
    }

    bool init(const config_t &c, std::string &error) {
      config = c;
      if (c.width <= 0 || c.height <= 0 || (!c.yuv444 && ((c.width | c.height) & 1))) {
        error = "invalid output dimensions";
        return false;
      }
      std::lock_guard lock(loader_mutex);
      if (!Vulkan::Context::init_loader(nullptr)) {
        error = "Vulkan loader unavailable";
        return false;
      }
      context.set_num_thread_indices(1);
      context.set_system_handles({});
      VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.apiVersion = VK_API_VERSION_1_3;
      app.pApplicationName = "Vibeshine PyroWave";
      context.set_application_info(&app);
      if (!context.init_instance(nullptr, 0)) {
        error = "Vulkan instance creation failed";
        return false;
      }
      std::uint32_t count = 0;
      if (vkEnumeratePhysicalDevices(context.get_instance(), &count, nullptr) != VK_SUCCESS || !count) {
        error = "no Vulkan devices";
        return false;
      }
      std::vector<VkPhysicalDevice> gpus(count);
      if (vkEnumeratePhysicalDevices(context.get_instance(), &count, gpus.data()) != VK_SUCCESS) {
        return false;
      }
      struct stat node {};
      const bool exact = !c.render_device.empty();
      if (exact && (stat(c.render_device.c_str(), &node) || !S_ISCHR(node.st_mode))) {
        error = "capture render device is not a DRM node: " + c.render_device;
        return false;
      }
      bool found = false;
      for (auto gpu : gpus) {
        VkPhysicalDeviceDrmPropertiesEXT drm {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
        vkGetPhysicalDeviceProperties2(gpu, &properties);
        if (exact && !((drm.hasRender && drm.renderMajor == major(node.st_rdev) && drm.renderMinor == minor(node.st_rdev)) || (drm.hasPrimary && drm.primaryMajor == major(node.st_rdev) && drm.primaryMinor == minor(node.st_rdev)))) {
          continue;
        }
        if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
          continue;
        }
        const char *extensions[] = {VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
        if (context.init_device(gpu, VK_NULL_HANDLE, extensions, 1)) {
          found = true;
          break;
        }
      }
      if (!found) {
        error = "no compatible Vulkan GPU for capture device";
        return false;
      }
      Vulkan::ContextOptions options {};
      options.memory_priorities = false;
      options.lean_memory_mode = true;
      device.set_context(context, options);
      initialized = true;
      if (!encoder.init(&device, c.width, c.height, c.yuv444 ? PyroWave::ChromaSubsampling::Chroma444 : PyroWave::ChromaSubsampling::Chroma420)) {
        error = "GPU does not support the PyroWave encoder";
        return false;
      }
      // The vendored Granite build intentionally disables runtime SPIR-V reflection.
      Vulkan::ResourceLayout layout {};
      layout.sets[0].sampled_image_mask = (1u << 0) | (1u << 4) | (1u << 5);
      layout.sets[0].storage_image_mask = (1u << 1) | (1u << 2) | (1u << 3);
      layout.sets[0].uniform_buffer_mask = 1u << 6;
      layout.sets[0].fp_mask = 0x3f;
      for (int i = 0; i < 7; ++i) {
        layout.sets[0].meta[i].array_size = 1;
      }
      program = device.request_program(shader.data(), shader.size() * sizeof(shader[0]), &layout);
      if (!program) {
        error = "conversion shader creation failed";
        return false;
      }
      for (int i = 0; i < 3; ++i) {
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(c.width, c.height, VK_FORMAT_R16_UNORM);
        if (i && !c.yuv444) {
          info.width /= 2;
          info.height /= 2;
        }
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.layout = Vulkan::ImageLayout::General;
        planes[i] = device.create_image(info);
        if (!planes[i]) {
          error = "plane allocation failed";
          return false;
        }
      }
      return true;
    }

    Vulkan::ImageHandle upload(const void *pixels, int width, int height, int stride, VkFormat format) {
      auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, format);
      Vulkan::ImageInitialData data {};
      data.data = pixels;
      data.row_length = stride / (format == VK_FORMAT_R16G16B16A16_UNORM ? 8 : 4);
      return device.create_image(info, &data);
    }

    Vulkan::ImageHandle import(const egl::surface_descriptor_t &s, std::string &error) {
      // Vulkan does not implicitly wait on the DMA-BUF reservation fence.
      pollfd ready {s.fds[0], POLLIN, 0};
      int waited;
      do {
        waited = poll(&ready, 1, 1000);
      } while (waited < 0 && errno == EINTR);
      if (waited <= 0 || !(ready.revents & POLLIN)) {
        error = "capture DMA-BUF producer fence did not signal";
        return {};
      }
      auto format = format_for(s.fourcc);
      if (format == VK_FORMAT_UNDEFINED || s.modifier == DRM_FORMAT_MOD_INVALID || s.fds[0] < 0) {
        error = "capture format or explicit DMA-BUF modifier unsupported";
        return {};
      }
      VkDrmFormatModifierPropertiesListEXT mods {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 props {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &mods};
      vkGetPhysicalDeviceFormatProperties2(context.get_gpu(), format, &props);
      std::vector<VkDrmFormatModifierPropertiesEXT> list(mods.drmFormatModifierCount);
      mods.pDrmFormatModifierProperties = list.data();
      vkGetPhysicalDeviceFormatProperties2(context.get_gpu(), format, &props);
      auto mod = std::find_if(list.begin(), list.end(), [&](auto &m) {
        return m.drmFormatModifier == s.modifier;
      });
      if (mod == list.end() || !mod->drmFormatModifierPlaneCount || mod->drmFormatModifierPlaneCount > 4) {
        error = "GPU cannot import capture modifier";
        return {};
      }
      std::array<VkSubresourceLayout, 4> layouts {};
      struct stat base {};
      if (fstat(s.fds[0], &base)) {
        return {};
      }
      for (unsigned i = 0; i < mod->drmFormatModifierPlaneCount; ++i) {
        struct stat plane {};
        if (s.fds[i] < 0 || fstat(s.fds[i], &plane) || base.st_ino != plane.st_ino || base.st_dev != plane.st_dev) {
          error = "DMA-BUF planes must share one allocation";
          return {};
        }
        layouts[i].offset = s.offsets[i];
        layouts[i].rowPitch = s.pitches[i];
      }
      VkImageDrmFormatModifierExplicitCreateInfoEXT drm {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
      drm.drmFormatModifier = s.modifier;
      drm.drmFormatModifierPlaneCount = mod->drmFormatModifierPlaneCount;
      drm.pPlaneLayouts = layouts.data();
      auto info = Vulkan::ImageCreateInfo::immutable_2d_image(s.width, s.height, format);
      info.misc = Vulkan::IMAGE_MISC_EXTERNAL_MEMORY_BIT;
      info.external.handle = fcntl(s.fds[0], F_DUPFD_CLOEXEC, 0);
      if (info.external.handle < 0) {
        error = "duplicating DMA-BUF failed";
        return {};
      }
      info.external.memory_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      info.pnext = &drm;
      info.layout = Vulkan::ImageLayout::General;
      info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
      auto image = device.create_image(info);
      // The vendored importer retains the supplied descriptor on failure.
      if (!image) {
        close(info.external.handle);
        error = "Vulkan DMA-BUF import failed";
      }
      return image;
    }

    bool ensure(Vulkan::BufferHandle &buffer, std::size_t size, Vulkan::BufferDomain domain) {
      if (!buffer || buffer->get_create_info().size < size) {
        Vulkan::BufferCreateInfo info {};
        info.size = size;
        info.domain = domain;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer = device.create_buffer(info);
      }
      return bool(buffer);
    }
  };

  core_t::core_t():
      impl(std::make_unique<impl_t>()) {}

  core_t::~core_t() = default;

  std::unique_ptr<core_t> core_t::create(const config_t &c, std::string &error) {
    auto core = std::unique_ptr<core_t>(new core_t);
    if (!core->impl->init(c, error)) {
      return {};
    }
    return core;
  }

  bool core_t::encode(const source_t &s, std::size_t budget, std::size_t boundary, std::vector<std::uint8_t> &bitstream, std::vector<packet_t> &packets, std::string &error) {
    auto &d = *impl;
    budget = std::min<std::size_t>(budget, UINT32_MAX) & ~std::size_t(3);
    if (!budget || !boundary || s.width <= 0 || s.height <= 0) {
      error = "invalid frame dimensions or budget";
      return false;
    }
    d.device.next_frame_context();
    auto source = s.surface ? d.import(*s.surface, error) :
                              (s.pixels && s.stride >= s.width * 4 ? d.upload(s.pixels, s.width, s.height, s.stride, VK_FORMAT_B8G8R8A8_UNORM) : Vulkan::ImageHandle {});
    if (!source) {
      if (error.empty()) {
        error = "no valid capture image";
      }
      return false;
    }
    const std::uint32_t transparent = 0;
    auto cursor = s.cursor ? d.upload(s.cursor, s.cursor_width, s.cursor_height, s.cursor_width * 4, VK_FORMAT_B8G8R8A8_UNORM) : d.upload(&transparent, 1, 1, 4, VK_FORMAT_B8G8R8A8_UNORM);
    std::vector<std::array<std::uint16_t, 4>> lut;
    if (s.lut && !s.lut->empty()) {
      for (auto &v : *s.lut) {
        lut.push_back({v[0], v[1], v[2], 65535});
      }
    } else {
      lut = {{{0, 0, 0, 65535}}, {{65535, 65535, 65535, 65535}}};
    }
    auto gamma = d.upload(lut.data(), lut.size(), 1, lut.size() * 8, VK_FORMAT_R16G16B16A16_UNORM);
    if (!cursor || !gamma) {
      error = "cursor or gamma LUT upload failed";
      return false;
    }
    auto meta_size = d.encoder.get_meta_required_size();
    auto bits_size = budget + meta_size;
    if (!d.ensure(d.meta_gpu, meta_size, Vulkan::BufferDomain::Device) || !d.ensure(d.bits_gpu, bits_size, Vulkan::BufferDomain::Device) || !d.ensure(d.meta_cpu, meta_size, Vulkan::BufferDomain::CachedHost) || !d.ensure(d.bits_cpu, bits_size, Vulkan::BufferDomain::CachedHost)) {
      error = "bitstream allocation failed";
      return false;
    }
    auto cmd = d.device.request_command_buffer(Vulkan::CommandBuffer::Type::AsyncCompute);
    if (s.surface) {
      cmd->acquire_image_barrier(*source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
    for (auto &plane : d.planes) {
      cmd->image_barrier(*plane, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    }
    cmd->set_program(d.program);
    cmd->set_texture(0, 0, source->get_view(), Vulkan::StockSampler::LinearClamp);
    for (int i = 0; i < 3; ++i) {
      cmd->set_storage_texture(0, 1 + i, d.planes[i]->get_view());
    }
    cmd->set_texture(0, 4, cursor->get_view(), Vulkan::StockSampler::LinearClamp);
    cmd->set_texture(0, 5, gamma->get_view(), Vulkan::StockSampler::LinearClamp);
    uniform_t u {};
    u.matrix = d.config.matrix;
    u.output_hdr = d.config.hdr;
    u.crop = {s.offset_x, s.offset_y, s.width, s.height};
    u.output = {d.config.width, d.config.height};
    u.subsampled = !d.config.yuv444;
    u.invert_y = s.y_invert;
    u.use_lut = s.lut && !s.lut->empty();
    bool linear = source->get_create_info().format == VK_FORMAT_R16G16B16A16_SFLOAT;
    u.transfer = linear ? (d.config.hdr ? 2 : 1) : 0;
    // Native 10-bit KMS HDR pixels are already PQ; 8-bit captures are SDR.
    if (!linear && d.config.hdr && (!s.surface || s.surface->fourcc == DRM_FORMAT_XRGB8888 || s.surface->fourcc == DRM_FORMAT_ARGB8888 || s.surface->fourcc == DRM_FORMAT_XBGR8888 || s.surface->fourcc == DRM_FORMAT_ABGR8888)) {
      u.transfer = 3;
    }
    if (s.cursor) {
      u.cursor = {(s.cursor_x - s.offset_x) * d.config.width / s.width, (s.cursor_y - s.offset_y) * d.config.height / s.height, s.cursor_dst_width * d.config.width / s.width, s.cursor_dst_height * d.config.height / s.height};
    }
    auto *data = cmd->allocate_typed_constant_data<uniform_t>(0, 6, 1);
    *data = u;
    cmd->dispatch((d.config.width + 15) / 16, (d.config.height + 15) / 16, 1);
    cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    PyroWave::ViewBuffers views {};
    for (int i = 0; i < 3; ++i) {
      views.planes[i] = &d.planes[i]->get_view();
    }
    PyroWave::Encoder::BitstreamBuffers buffers {};
    buffers.meta.buffer = d.meta_gpu.get();
    buffers.meta.size = d.meta_gpu->get_create_info().size;
    buffers.bitstream.buffer = d.bits_gpu.get();
    buffers.bitstream.size = d.bits_gpu->get_create_info().size;
    buffers.target_size = budget;
    if (!d.encoder.encode(*cmd, views, buffers)) {
      d.device.submit_discard(cmd);
      error = "wavelet encode failed";
      return false;
    }
    if (s.surface) {
      cmd->release_image_barrier(*source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
    cmd->copy_buffer(*d.meta_cpu, *d.meta_gpu);
    cmd->copy_buffer(*d.bits_cpu, *d.bits_gpu);
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    Vulkan::Fence fence;
    d.device.submit(cmd, &fence);
    if (!fence->wait_timeout(UINT64_MAX)) {
      error = "GPU encode wait failed";
      return false;
    }
    auto *meta = d.device.map_host_buffer(*d.meta_cpu, Vulkan::MEMORY_ACCESS_READ_BIT);
    auto *bits = d.device.map_host_buffer(*d.bits_cpu, Vulkan::MEMORY_ACCESS_READ_BIT);
    std::vector<PyroWave::Encoder::Packet> native(d.encoder.compute_num_packets(meta, boundary));
    bitstream.resize(d.bits_cpu->get_create_info().size + 8);
    auto count = d.encoder.packetize(native.data(), boundary, bitstream.data(), bitstream.size(), meta, bits);
    d.device.unmap_host_buffer(*d.meta_cpu, Vulkan::MEMORY_ACCESS_READ_BIT);
    d.device.unmap_host_buffer(*d.bits_cpu, Vulkan::MEMORY_ACCESS_READ_BIT);
    if (!count || count > native.size()) {
      error = "packetization failed";
      return false;
    }
    packets.clear();
    for (std::size_t i = 0; i < count; ++i) {
      packets.push_back({native[i].offset, native[i].size});
    }
    return true;
  }
}  // namespace pyrowave::linux_gpu
