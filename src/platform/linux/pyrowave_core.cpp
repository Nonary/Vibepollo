/** @file src/platform/linux/pyrowave_core.cpp
 * @brief GPU capture conversion and synchronous wavelet encoding.
 */
#include "pyrowave_core.h"

#include "command_buffer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "pyrowave_encoder.hpp"
#include "pyrowave_import_diagnostics.h"
#include "pyrowave_instance.h"
#include "pyrowave_linear_upload.h"
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
  #include "pyrowave_truehdr.h"
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <mutex>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace pyrowave::linux_gpu {
  namespace {
    const std::vector<std::uint32_t> shader
#include "shaders/pyrowave.spv.inc"
      ;
    // Volk reloads global instance entry points during context initialization.
    // Physical-device queries during encode and Granite teardown also use those
    // pointers, so they must not overlap a reload. Encoders may run in parallel.
    dispatch_gate_t loader_gate;
    instance_lifetime_t process_instances;

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
      int subsampled, invert_y, use_lut, transfer, output_hdr, truehdr_stage;
      float white_nits;
    };
  }  // namespace

  struct core_t::impl_t {
    // Destruction order matters: every GPU resource must die before the device.
    // Keep the shared instance alive until this context releases its device,
    // even if the process's factory lease is released first during shutdown.
    std::shared_ptr<instance_factory_t> instance_factory;
    Vulkan::Context context;
    Vulkan::Device device;
    PyroWave::Encoder encoder;
    config_t config;
    import_diagnostics::device_t diagnostic_device;
    Vulkan::Program *program = nullptr;
    Vulkan::ImageHandle planes[3];
    Vulkan::BufferHandle meta_gpu, bits_gpu, meta_cpu, bits_cpu;
    bool initialized = false;
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    std::unique_ptr<truehdr_t> truehdr;
#endif

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
      auto lock = loader_gate.lock_exclusive();
      // Closing process admission is terminal, including failed/retried probes.
      if (process_instances.is_closed()) {
        error = "PyroWave is shutting down";
        return false;
      }
      if (!Vulkan::Context::init_loader(nullptr)) {
        error = "Vulkan loader unavailable";
        return false;
      }
      // Per-stream vkDestroyInstance can stall inside the graphics driver even
      // after device cleanup has completed. Retain one process-owned instance
      // across probes, reconnects and capture reinitializations instead. Device
      // and image destruction remains synchronous and ordered below.
      instance_factory = process_instances.acquire(vkCreateInstance, Vulkan::Context::get_instance_proc_addr());
      context.set_instance_factory(instance_factory.get());
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
      // Only loader/instance initialization rewrites global dispatch. Device
      // creation and shader setup use a per-device table, so existing encoders
      // can keep running while this session prepares its GPU resources.
      lock.unlock();
      auto device_lock = loader_gate.lock_shared();
      if (process_instances.is_closed()) {
        error = "PyroWave is shutting down";
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
        if ((!c.truehdr_runtime.empty() && properties.properties.vendorID != 0x10de) || properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
          continue;
        }
        const char *extensions[] = {VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
        if (context.init_device(gpu, VK_NULL_HANDLE, extensions, 1)) {
          diagnostic_device.configured_device = c.render_device;
          diagnostic_device.gpu_name.assign(properties.properties.deviceName, strnlen(properties.properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE));
          diagnostic_device.vendor_id = properties.properties.vendorID;
          diagnostic_device.device_id = properties.properties.deviceID;
          diagnostic_device.has_render = drm.hasRender;
          diagnostic_device.render_major = drm.renderMajor;
          diagnostic_device.render_minor = drm.renderMinor;
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
      layout.sets[0].storage_buffer_mask = (1u << 7) | (1u << 8);
      layout.sets[0].fp_mask = 0x3f;
      for (int i = 0; i < 9; ++i) {
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
      if (!c.truehdr_runtime.empty()) {
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
        if (!c.hdr || !c.ten_bit) {
          error = "PyroWave RTX HDR requires a 10-bit HDR stream";
          return false;
        }
        try {
          truehdr = std::make_unique<truehdr_t>();
          truehdr->init(device, c);
        } catch (const std::exception &e) {
          error = e.what();
          return false;
        }
#else
        error = "PyroWave RTX HDR is unavailable in this build";
        return false;
#endif
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

    Vulkan::ImageHandle upload_linear_surface(const egl::surface_descriptor_t &s, VkFormat format, std::string &error) {
      if (!linear_upload::can_upload_surface(s)) {
        error = "capture surface is not a supported linear CPU upload";
        return {};
      }
      std::size_t mapped_size = 0;
      if (!linear_upload::required_mapped_size(s, mapped_size)) {
        error = "linear DMA-BUF layout size overflow";
        return {};
      }
      if (!linear_upload::backing_extent_matches(s.fds[0], mapped_size)) {
        error = "linear DMA-BUF backing extent is unavailable or smaller than its layout";
        return {};
      }
      void *mapped = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, s.fds[0], 0);
      if (mapped == MAP_FAILED) {
        error = "mapping linear capture DMA-BUF failed";
        return {};
      }

      struct mapping_guard_t {
        void *address;
        std::size_t size;

        ~mapping_guard_t() {
          munmap(address, size);
        }
      } mapping {mapped, mapped_size};

      dma_buf_sync sync {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
      const auto sync_result = linear_upload::retry_sync_call([&] {
        return ioctl(s.fds[0], DMA_BUF_IOCTL_SYNC, &sync);
      });
      if (sync_result < 0) {
        error = "synchronizing linear capture DMA-BUF for CPU read failed";
        return {};
      }

      struct sync_guard_t {
        int fd;
        bool active = true;

        ~sync_guard_t() {
          if (active) {
            dma_buf_sync end {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
            linear_upload::retry_sync_call([&] {
              return ioctl(fd, DMA_BUF_IOCTL_SYNC, &end);
            });
          }
        }

        bool finish() {
          dma_buf_sync end {.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
          const auto result = linear_upload::retry_sync_call([&] {
            return ioctl(fd, DMA_BUF_IOCTL_SYNC, &end);
          });
          active = false;
          return result == 0;
        }
      } cpu_access {s.fds[0]};

      std::vector<std::uint8_t> pixels;
      if (!linear_upload::copy_mapped_rows(s, std::span(static_cast<const std::uint8_t *>(mapped), mapped_size), pixels)) {
        error = "copying linear capture DMA-BUF pixels failed";
        return {};
      }
      if (!cpu_access.finish()) {
        error = "synchronizing linear capture DMA-BUF after CPU read failed";
        return {};
      }
      auto image = upload(pixels.data(), s.width, s.height, s.width * 4, format);
      if (!image) {
        error = "uploading linear capture pixels to Vulkan failed";
      }
      return image;
    }

    Vulkan::ImageHandle import(const egl::surface_descriptor_t &s, std::string &error, bool &producer_ready) {
      producer_ready = false;
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
      producer_ready = true;
      auto format = format_for(s.fourcc);
      if (format == VK_FORMAT_UNDEFINED || s.modifier == DRM_FORMAT_MOD_INVALID || s.fds[0] < 0) {
        error = import_diagnostics::describe(s, format, import_diagnostics::classify(s, format, {}), diagnostic_device);
        return {};
      }
      VkDrmFormatModifierPropertiesListEXT mods {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      VkFormatProperties2 props {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &mods};
      vkGetPhysicalDeviceFormatProperties2(context.get_gpu(), format, &props);
      std::vector<VkDrmFormatModifierPropertiesEXT> list(mods.drmFormatModifierCount);
      mods.pDrmFormatModifierProperties = list.data();
      vkGetPhysicalDeviceFormatProperties2(context.get_gpu(), format, &props);
      list.resize(std::min(list.size(), std::size_t(mods.drmFormatModifierCount)));
      const auto check = import_diagnostics::classify(s, format, list);
      if (check.rejection != import_diagnostics::rejection_e::none) {
        error = import_diagnostics::describe(s, format, check, diagnostic_device);
        return {};
      }
      std::array<VkSubresourceLayout, 4> layouts {};
      struct stat base {};
      if (fstat(s.fds[0], &base)) {
        return {};
      }
      for (unsigned i = 0; i < check.plane_count; ++i) {
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
      drm.drmFormatModifierPlaneCount = check.plane_count;
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

  core_t::~core_t() {
    // The lock must span member destruction, including Device and Context.
    auto lock = loader_gate.lock_shared();
    impl.reset();
  }

  void core_t::shutdown() {
    std::shared_ptr<instance_factory_t> retired;
    {
      auto lock = loader_gate.lock_exclusive();
      retired = process_instances.close();
    }
    // All stream/probe cores have joined before the host calls this. Keep a
    // possible vendor instance-destruction wait outside the dispatch lock and
    // inside main's still-armed shutdown deadline, rather than static teardown.
    retired.reset();
  }

  std::unique_ptr<core_t> core_t::create(const config_t &c, std::string &error) {
    auto core = std::unique_ptr<core_t>(new core_t);
    if (!core->impl->init(c, error)) {
      return {};
    }
    return core;
  }

  bool core_t::encode(const source_t &s, std::size_t budget, std::size_t boundary, std::vector<std::uint8_t> &bitstream, std::vector<packet_t> &packets, std::string &error) {
    auto lock = loader_gate.lock_shared();
    auto &d = *impl;
    budget = std::min<std::size_t>(budget, UINT32_MAX) & ~std::size_t(3);
    if (!budget || !boundary || s.width <= 0 || s.height <= 0) {
      error = "invalid frame dimensions or budget";
      return false;
    }
    d.device.next_frame_context();
    bool producer_ready = false;
    auto source = s.surface ? d.import(*s.surface, error, producer_ready) :
                              (s.pixels && s.stride >= s.width * 4 ? d.upload(s.pixels, s.width, s.height, s.stride, VK_FORMAT_B8G8R8A8_UNORM) : Vulkan::ImageHandle {});
    bool used_linear_upload_fallback = false;
    if (!source && s.surface && linear_upload::should_attempt_linear_upload(*s.surface, producer_ready)) {
      const auto import_error = error;
      std::string fallback_error;
      source = d.upload_linear_surface(*s.surface, format_for(s.surface->fourcc), fallback_error);
      if (source) {
        used_linear_upload_fallback = true;
        error.clear();
      } else {
        error = import_error;
        if (!error.empty()) {
          error += "; ";
        }
        error += "linear CPU upload fallback failed: " + fallback_error;
      }
    }
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
    const bool source_is_external = linear_upload::requires_external_image_barriers(s.surface != nullptr, used_linear_upload_fallback);
    if (source_is_external) {
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
    u.white_nits = 100.0f + s.sdr_brightness;
    u.crop = {s.offset_x, s.offset_y, s.width, s.height};
    u.output = {d.config.width, d.config.height};
    u.subsampled = !d.config.yuv444;
    u.invert_y = s.y_invert;
    u.use_lut = s.lut && !s.lut->empty();
    bool linear = source->get_create_info().format == VK_FORMAT_R16G16B16A16_SFLOAT;
    u.transfer = linear ? (d.config.hdr ? 2 : 1) : 0;
    // KMS HDR pixels are already PQ even when scanout uses an 8-bit buffer.
    // Only expand an SDR source; precision alone does not identify its transfer.
    if (!linear && !s.hdr10 && d.config.hdr && (!s.surface || s.surface->fourcc == DRM_FORMAT_XRGB8888 || s.surface->fourcc == DRM_FORMAT_ARGB8888 || s.surface->fourcc == DRM_FORMAT_XBGR8888 || s.surface->fourcc == DRM_FORMAT_ABGR8888)) {
      u.transfer = 3;
    }
    if (s.cursor) {
      u.cursor = {(s.cursor_x - s.offset_x) * d.config.width / s.width, (s.cursor_y - s.offset_y) * d.config.height / s.height, s.cursor_dst_width * d.config.width / s.width, s.cursor_dst_height * d.config.height / s.height};
    }
    Vulkan::Buffer *hdr_input = d.meta_gpu.get(), *hdr_output = d.meta_gpu.get();
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    if (d.truehdr) {
      if (s.hdr10) {
        d.device.submit_discard(cmd);
        error = "PyroWave RTX HDR requires an SDR capture source";
        return false;
      }
      // Foreground bypass remains SDR-to-PQ at every framebuffer precision.
      u.transfer = 3;
      hdr_input = d.truehdr->input.get();
      hdr_output = d.truehdr->output.get();
      if (s.truehdr_enabled) {
        // Compose/crop/scale/LUT in SDR before the SDK sees RGBA8 pixels.
        auto prepare = u;
        prepare.output_hdr = 0;
        prepare.transfer = 0;
        prepare.truehdr_stage = 1;
        cmd->set_storage_buffer(0, 7, *hdr_input);
        cmd->set_storage_buffer(0, 8, *hdr_output);
        *cmd->allocate_typed_constant_data<uniform_t>(0, 6, 1) = prepare;
        cmd->dispatch((d.config.width + 15) / 16, (d.config.height + 15) / 16, 1);
        cmd->release_buffer_barrier(*hdr_input, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        cmd->release_buffer_barrier(*hdr_output, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        Vulkan::Fence ready;
        d.device.submit(cmd, &ready);
        if (!ready->wait_timeout(UINT64_MAX)) {
          error = "PyroWave RTX HDR: preprocessing wait failed";
          return false;
        }
        try {
          d.truehdr->run(s.truehdr_settings, d.config.width, d.config.height);
        } catch (const std::exception &e) {
          error = e.what();
          return false;
        }
        cmd = d.device.request_command_buffer(Vulkan::CommandBuffer::Type::AsyncCompute);
        cmd->acquire_buffer_barrier(*hdr_input, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        cmd->acquire_buffer_barrier(*hdr_output, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        cmd->set_program(d.program);
        cmd->set_texture(0, 0, source->get_view(), Vulkan::StockSampler::LinearClamp);
        for (int i = 0; i < 3; ++i) {
          cmd->set_storage_texture(0, 1 + i, d.planes[i]->get_view());
        }
        cmd->set_texture(0, 4, cursor->get_view(), Vulkan::StockSampler::LinearClamp);
        cmd->set_texture(0, 5, gamma->get_view(), Vulkan::StockSampler::LinearClamp);
        u.truehdr_stage = 2;
      }
    }
#endif
    cmd->set_storage_buffer(0, 7, *hdr_input);
    cmd->set_storage_buffer(0, 8, *hdr_output);
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
    if (source_is_external) {
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
