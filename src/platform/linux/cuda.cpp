/**
 * @file src/platform/linux/cuda.cpp
 * @brief Definitions for CUDA encoding.
 */
// standard includes
#include <algorithm>
#include <bitset>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>

// lib includes
#include <ffnvcodec/dynlink_loader.h>
#include <NvFBC.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/imgutils.h>
}

// local includes
#include "cuda.h"
#include "cuda_interop.h"
#include "graphics.h"
#include "src/logging.h"
#include "src/nvenc/nvenc_cuda.h"
#include "src/nvenc/nvenc_utils.h"
#include "src/utility.h"
#include "src/video.h"
#include "wayland.h"

#define SUNSHINE_STRINGVIEW_HELPER(x) x##sv
#define SUNSHINE_STRINGVIEW(x) SUNSHINE_STRINGVIEW_HELPER(x)

#define CU_CHECK(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
  return -1

#define CU_CHECK_IGNORE(x, y) \
  check((x), SUNSHINE_STRINGVIEW(y ": "))

namespace fs = std::filesystem;

using namespace std::literals;

namespace cuda {
  constexpr auto cudaDevAttrMaxThreadsPerBlock = (CUdevice_attribute) 1;
  constexpr auto cudaDevAttrMaxThreadsPerMultiProcessor = (CUdevice_attribute) 39;

  void pass_error(const std::string_view &sv, const char *name, const char *description) {
    BOOST_LOG(error) << sv << name << ':' << description;
  }

  void cff(CudaFunctions *cf) {
    cuda_free_functions(&cf);
  }

  using cdf_t = util::safe_ptr<CudaFunctions, cff>;

  // cuda_load_functions() frees any table passed to it before loading a new
  // one. Publish exactly one successful table so active devices may safely
  // retain borrowed pointers to it for their complete lifetime.
  static cdf_t cdf;
  static std::mutex cdf_init_mutex;

  bool ensure_egl_loader() {
    return egl::ensure_loader();
  }

  inline static int check(CUresult result, const std::string_view &sv) {
    if (result != CUDA_SUCCESS) {
      const char *name;
      const char *description;

      cdf->cuGetErrorName(result, &name);
      cdf->cuGetErrorString(result, &description);

      BOOST_LOG(error) << sv << name << ':' << description;
      return -1;
    }

    return 0;
  }

  void freeStream(CUstream stream) {
    CU_CHECK_IGNORE(cdf->cuStreamDestroy(stream), "Couldn't destroy cuda stream");
  }

  void unregisterResource(CUgraphicsResource resource) {
    CU_CHECK_IGNORE(cdf->cuGraphicsUnregisterResource(resource), "Couldn't unregister resource");
  }

  using registered_resource_t = util::safe_ptr<CUgraphicsResource_st, unregisterResource>;

  class context_guard_t {
  public:
    explicit context_guard_t(CUcontext context) {
      if (context && cdf->cuCtxPushCurrent(context) == CUDA_SUCCESS) {
        active_ = true;
      } else {
        BOOST_LOG(error) << "Couldn't make CUDA interop context current.";
      }
    }

    context_guard_t(const context_guard_t &) = delete;
    context_guard_t &operator=(const context_guard_t &) = delete;

    ~context_guard_t() {
      if (!active_) {
        return;
      }
      CUcontext popped {};
      CU_CHECK_IGNORE(cdf->cuCtxPopCurrent(&popped), "Couldn't restore CUDA context");
    }

    explicit operator bool() const noexcept {
      return active_;
    }

  private:
    bool active_ {false};
  };

  class img_t: public platf::img_t {
  public:
    tex_t tex;
  };

  int init() {
    std::lock_guard lock {cdf_init_mutex};
    if (cdf) {
      return 0;
    }

    auto status = cuda_load_functions(&cdf, nullptr);
    if (status) {
      BOOST_LOG(error) << "Couldn't load cuda: "sv << status;

      return -1;
    }

    if (check(cdf->cuInit(0), "Couldn't initialize cuda: "sv)) {
      cdf.reset();
      return -1;
    }

    return 0;
  }

  class cuda_t: public platf::avcodec_encode_device_t {
  public:
    int init(int in_width, int in_height) {
      if (!cdf) {
        BOOST_LOG(warning) << "cuda not initialized"sv;
        return -1;
      }

      data = (void *) 0x1;

      width = in_width;
      height = in_height;

      return 0;
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      auto hwframe_ctx = (AVHWFramesContext *) hw_frames_ctx->data;
      if (hwframe_ctx->sw_format != AV_PIX_FMT_NV12) {
        BOOST_LOG(error) << "cuda::cuda_t doesn't support any format other than AV_PIX_FMT_NV12"sv;
        return -1;
      }

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for NVENC"sv;
          return -1;
        }
      }

      auto cuda_ctx = (AVCUDADeviceContext *) hwframe_ctx->device_ctx->hwctx;

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      auto sws_opt = sws_t::make(width, height, frame->width, frame->height, width * 4);
      if (!sws_opt) {
        return -1;
      }

      sws = std::move(*sws_opt);

      linear_interpolation = width != frame->width || height != frame->height;

      return 0;
    }

    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);

      auto tex = tex_t::make(height, width * 4);
      if (!tex) {
        return;
      }

      // The default green color is ugly.
      // Update the background color
      platf::img_t img;
      img.width = width;
      img.height = height;
      img.pixel_pitch = 4;
      img.row_pitch = img.width * img.pixel_pitch;

      std::vector<std::uint8_t> image_data;
      image_data.resize(img.row_pitch * img.height);

      img.data = image_data.data();

      if (sws.load_ram(img, tex->array)) {
        return;
      }

      sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex->texture.linear, stream.get(), {frame->width, frame->height, 0, 0});
    }

    cudaTextureObject_t tex_obj(const tex_t &tex) const {
      return linear_interpolation ? tex.texture.linear : tex.texture.point;
    }

    stream_t stream;
    frame_t hwframe;

    int height;
    int width;

    // When height and width don't change, it's not necessary to use linear interpolation
    bool linear_interpolation;

    sws_t sws;
  };

  class cuda_ram_t: public cuda_t {
  public:
    int convert(platf::img_t &img) override {
      return sws.load_ram(img, tex.array) || sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(tex), stream.get());
    }

    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      if (cuda_t::set_frame(frame, hw_frames_ctx)) {
        return -1;
      }

      auto tex_opt = tex_t::make(height, width * 4);
      if (!tex_opt) {
        return -1;
      }

      tex = std::move(*tex_opt);

      return 0;
    }

    tex_t tex;
  };

  class cuda_vram_t: public cuda_t {
  public:
    int convert(platf::img_t &img) override {
      return sws.convert(frame->data[0], frame->data[1], frame->linesize[0], frame->linesize[1], tex_obj(((img_t *) &img)->tex), stream.get());
    }
  };

  /**
   * @brief Opens the DRM device associated with the CUDA device index.
   * @param index CUDA device index to open.
   * @return File descriptor or -1 on failure.
   */
  file_t open_drm_fd_for_cuda_device(int index) {
    CUdevice device;
    CU_CHECK(cdf->cuDeviceGet(&device, index), "Couldn't get CUDA device");

    // There's no way to directly go from CUDA to a DRM device, so we'll
    // use sysfs to look up the DRM device name from the PCI ID.
    std::array<char, 13> pci_bus_id;
    CU_CHECK(cdf->cuDeviceGetPCIBusId(pci_bus_id.data(), pci_bus_id.size(), device), "Couldn't get CUDA device PCI bus ID");
    BOOST_LOG(debug) << "Found CUDA device with PCI bus ID: "sv << pci_bus_id.data();

    // Linux uses lowercase hexadecimal while CUDA uses uppercase
    std::transform(pci_bus_id.begin(), pci_bus_id.end(), pci_bus_id.begin(), [](char c) {
      return std::tolower(c);
    });

    // Look for the name of the primary node in sysfs
    try {
      char sysfs_path[PATH_MAX];
      std::snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/pci/devices/%s/drm", pci_bus_id.data());
      fs::path sysfs_dir {sysfs_path};
      for (auto &entry : fs::directory_iterator {sysfs_dir}) {
        auto file = entry.path().filename();
        auto filestring = file.generic_string();
        if (std::string_view {filestring}.substr(0, 4) != "card"sv) {
          continue;
        }

        BOOST_LOG(debug) << "Found DRM primary node: "sv << filestring;

        fs::path dri_path {"/dev/dri"sv};
        auto device_path = dri_path / file;
        return open(device_path.c_str(), O_RDWR);
      }
    } catch (const std::filesystem::filesystem_error &err) {
      BOOST_LOG(error) << "Failed to read sysfs: "sv << err.what();
    }

    BOOST_LOG(error) << "Unable to find DRM device with PCI bus ID: "sv << pci_bus_id.data();
    return -1;
  }

  class gl_cuda_vram_t: public platf::avcodec_encode_device_t {
  public:
    ~gl_cuda_vram_t() override {
      if (!cuda_context) {
        return;
      }

      // hwframe owns the FFmpeg reference keeping cuda_context alive. Drain
      // and destroy our stream before that reference is released, with the
      // owning context current even on partial initialization paths.
      context_guard_t guard {cuda_context};
      if (guard && stream) {
        CU_CHECK_IGNORE(cdf->cuStreamSynchronize(stream.get()), "Couldn't finish CUDA conversion during teardown");
      }

      const auto raw_context = std::get<1>(ctx.el);
      if (!guard || eglGetCurrentContext() != raw_context) {
        // Encoder probes can destroy this object from a different thread while
        // the worker still owns the GL context. EGL forbids stealing it; the
        // CUDA/EGL context teardown will reclaim its registered resources.
        (void) y_res.release();
        (void) uv_res.release();
      } else {
        y_res.reset();
        uv_res.reset();
      }

      if (guard) {
        // FFmpeg borrows this stream; clear its pointer before freeing it.
        if (stream && hwframe && hwframe->hw_frames_ctx) {
          auto *frames = reinterpret_cast<AVHWFramesContext *>(hwframe->hw_frames_ctx->data);
          auto *device = reinterpret_cast<AVCUDADeviceContext *>(frames->device_ctx->hwctx);
          if (device->stream == stream.get()) {
            device->stream = nullptr;
          }
        }
        stream.reset();
      } else {
        // Do not pass a stream from an unavailable context to another one.
        (void) stream.release();
      }
    }

    /**
     * @brief Initialize the GL->CUDA encoding device.
     * @param in_width Width of captured frames.
     * @param in_height Height of captured frames.
     * @param offset_x Offset of content in captured frame.
     * @param offset_y Offset of content in captured frame.
     * @return 0 on success or -1 on failure.
     */
    int init(int in_width, int in_height, int offset_x, int offset_y) {
      // This must be non-zero to tell the video core that it's a hardware encoding device.
      data = (void *) 0x1;

      // TODO: Support more than one CUDA device
      file = std::move(open_drm_fd_for_cuda_device(0));
      if (file.el < 0) {
        char string[1024];
        BOOST_LOG(error) << "Couldn't open DRM FD for CUDA device: "sv << strerror_r(errno, string, sizeof(string));
        return -1;
      }

      if (!ensure_egl_loader()) {
        return -1;
      }

      if (!gbm::create_device || !gbm::device_destroy) {
        BOOST_LOG(error) << "Couldn't initialize GBM symbols for CUDA/GL interop"sv;
        return -1;
      }
      gbm.reset(gbm::create_device(file.el));
      if (!gbm) {
        BOOST_LOG(error) << "Couldn't create GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
        return -1;
      }

      display = egl::make_display(gbm.get());
      if (!display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      width = in_width;
      height = in_height;

      sequence = 0;

      this->offset_x = offset_x;
      this->offset_y = offset_y;

      return 0;
    }

    /**
     * @brief Initialize color conversion into target CUDA frame.
     * @param frame Destination CUDA frame to write into.
     * @param hw_frames_ctx_buf FFmpeg hardware frame context.
     * @return 0 on success or -1 on failure.
     */
    int set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx_buf) override {
      this->hwframe.reset(frame);
      this->frame = frame;

      if (!frame->buf[0]) {
        if (av_hwframe_get_buffer(hw_frames_ctx_buf, frame, 0)) {
          BOOST_LOG(error) << "Couldn't get hwframe for VAAPI"sv;
          return -1;
        }
      }

      auto hw_frames_ctx = (AVHWFramesContext *) hw_frames_ctx_buf->data;
      sw_format = hw_frames_ctx->sw_format;

      auto nv12_opt = egl::create_target(frame->width, frame->height, sw_format);
      if (!nv12_opt) {
        return -1;
      }

      auto sws_opt = egl::sws_t::make(width, height, frame->width, frame->height, sw_format);
      if (!sws_opt) {
        return -1;
      }

      this->sws = std::move(*sws_opt);
      this->nv12 = std::move(*nv12_opt);

      auto cuda_ctx = (AVCUDADeviceContext *) hw_frames_ctx->device_ctx->hwctx;
      cuda_context = cuda_ctx->cuda_ctx;
      context_guard_t context_guard {cuda_context};
      if (!context_guard) {
        return -1;
      }

      stream = make_stream();
      if (!stream) {
        return -1;
      }

      cuda_ctx->stream = stream.get();

      CU_CHECK(cdf->cuGraphicsGLRegisterImage(&y_res, nv12->tex[0], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "Couldn't register Y plane texture");
      CU_CHECK(cdf->cuGraphicsGLRegisterImage(&uv_res, nv12->tex[1], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "Couldn't register UV plane texture");

      return 0;
    }

    /**
     * @brief Convert the captured image into the target CUDA frame.
     * @param img Captured screen image.
     * @return 0 on success or -1 on failure.
     */
    int convert(platf::img_t &img) override {
      context_guard_t context_guard {cuda_context};
      if (!context_guard) {
        return -1;
      }

      auto &descriptor = (egl::img_descriptor_t &) img;

      if (descriptor.sequence == 0) {
        // For dummy images, use a blank RGB texture instead of importing a DMA-BUF
        rgb = egl::create_blank(img);
      } else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;

        rgb = egl::rgb_t {};

        auto rgb_opt = egl::import_source(display.get(), descriptor.sd);

        if (!rgb_opt) {
          rgb_opt = egl::upload_source(display.get(), descriptor.sd);
          if (!rgb_opt) {
            return -1;
          }
        }

        rgb = std::move(*rgb_opt);
      }

      // Perform the color conversion and scaling in GL
      sws.load_vram(descriptor, offset_x, offset_y, rgb->tex[0]);
      sws.apply_output_lut(descriptor.crtc_gamma_lut, descriptor.crtc_gamma_lut_serial);
      if (sws.convert(nv12->buf)) {
        return -1;
      }

      auto fmt_desc = av_pix_fmt_desc_get(sw_format);

      // Map the GL textures to read for CUDA
      CUgraphicsResource resources[2] = {y_res.get(), uv_res.get()};
      graphics_mapping_t mapping {*cdf, resources, 2, stream.get()};
      CU_CHECK(mapping.map(), "Couldn't map GL textures in CUDA");

      // Copy from the GL textures to the target CUDA frame
      for (int i = 0; i < 2; i++) {
        CUDA_MEMCPY2D cpy = {};
        cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        CU_CHECK(cdf->cuGraphicsSubResourceGetMappedArray(&cpy.srcArray, resources[i], 0, 0), "Couldn't get mapped plane array");

        cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy.dstDevice = (CUdeviceptr) frame->data[i];
        cpy.dstPitch = frame->linesize[i];
        cpy.WidthInBytes = (frame->width * fmt_desc->comp[i].step) >> (i ? fmt_desc->log2_chroma_w : 0);
        cpy.Height = frame->height >> (i ? fmt_desc->log2_chroma_h : 0);

        CU_CHECK(cdf->cuMemcpy2DAsync(&cpy, stream.get()), "Couldn't copy texture to CUDA frame");
      }

      // Unmap the textures to allow modification from GL again
      CU_CHECK(mapping.unmap(), "Couldn't unmap GL textures from CUDA");
      return 0;
    }

    /**
     * @brief Configures shader parameters for the specified colorspace.
     */
    void apply_colorspace() override {
      sws.apply_colorspace(colorspace);
    }

    file_t file;
    gbm::gbm_t gbm;
    egl::display_t display;
    egl::ctx_t ctx;

    // This must be destroyed before display_t
    stream_t stream;
    frame_t hwframe;

    egl::sws_t sws;
    egl::nv12_t nv12;
    AVPixelFormat sw_format;

    int height;
    int width;

    std::uint64_t sequence;
    egl::rgb_t rgb;

    registered_resource_t y_res;
    registered_resource_t uv_res;
    CUcontext cuda_context {};

    int offset_x;
    int offset_y;
  };

  class gl_cuda_nvenc_t: public platf::nvenc_encode_device_t {
  public:
    ~gl_cuda_nvenc_t() override {
      const auto owned_gl_context = std::get<1>(ctx.el);
      const bool owns_gl_context = eglGetCurrentContext &&
                                   owned_gl_context != EGL_NO_CONTEXT &&
                                   eglGetCurrentContext() == owned_gl_context;
      {
        context_guard_t guard {cuda_context};

        // The NVENC session must release its registered CUDA pointer before the
        // primary context and conversion stream disappear.
        nvenc = nullptr;
        native_encoder.reset();

        if (owns_gl_context && guard) {
          y_res.reset();
          uv_res.reset();
        } else {
          // Encoder probes may tear down on a worker that cannot steal EGL's
          // context. Releasing ownership lets CUDA/EGL reclaim registrations
          // when their contexts are destroyed.
          (void) y_res.release();
          (void) uv_res.release();
        }

        if (stream && guard) {
          if (cdf->cuStreamDestroy(stream) != CUDA_SUCCESS) {
            BOOST_LOG(error) << "Couldn't destroy native NVENC CUDA stream"sv;
          }
          stream = nullptr;
        }
      }

      if (primary_context_retained && cdf) {
        if (cdf->cuDevicePrimaryCtxRelease(cuda_device) != CUDA_SUCCESS) {
          BOOST_LOG(error) << "Couldn't release native NVENC CUDA context"sv;
        }
      }
    }

    int init(int in_width, int in_height, int in_offset_x, int in_offset_y, platf::pix_fmt_e pix_fmt) {
      buffer_format = nvenc::nvenc_format_from_sunshine_format(pix_fmt);
      switch (buffer_format) {
        case NV_ENC_BUFFER_FORMAT_NV12:
          sw_format = AV_PIX_FMT_NV12;
          break;
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
          sw_format = AV_PIX_FMT_P010;
          break;
        default:
          BOOST_LOG(error) << "Unexpected native NVENC pixel format ["sv << platf::from_pix_fmt(pix_fmt) << ']';
          return -1;
      }

      file = std::move(open_drm_fd_for_cuda_device(0));
      if (file.el < 0) {
        BOOST_LOG(error) << "Couldn't open DRM FD for native NVENC CUDA device: "sv << strerror(errno);
        return -1;
      }

      if (!ensure_egl_loader()) {
        return -1;
      }

      if (!gbm::create_device || !gbm::device_destroy) {
        BOOST_LOG(error) << "Couldn't initialize GBM symbols for native NVENC CUDA/GL interop"sv;
        return -1;
      }
      gbm.reset(gbm::create_device(file.el));
      if (!gbm) {
        BOOST_LOG(error) << "Couldn't create native NVENC GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
        return -1;
      }

      display = egl::make_display(gbm.get());
      if (!display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(display.get());
      if (!ctx_opt) {
        return -1;
      }
      ctx = std::move(*ctx_opt);

      CU_CHECK(cdf->cuDeviceGet(&cuda_device, 0), "Couldn't get native NVENC CUDA device");
      CU_CHECK(cdf->cuDevicePrimaryCtxRetain(&cuda_context, cuda_device), "Couldn't retain native NVENC CUDA context");
      primary_context_retained = true;

      context_guard_t guard {cuda_context};
      if (!guard) {
        return -1;
      }
      CU_CHECK(cdf->cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "Couldn't create native NVENC CUDA stream");

      width = in_width;
      height = in_height;
      offset_x = in_offset_x;
      offset_y = in_offset_y;
      return 0;
    }

    bool init_encoder(const ::video::config_t &client_config, const ::video::sunshine_colorspace_t &colorspace) override {
      context_guard_t guard {cuda_context};
      if (!guard) {
        return false;
      }

      native_encoder = std::make_unique<nvenc::nvenc_cuda>(cuda_context, cdf.get(), stream);
      nvenc = native_encoder.get();
      output_width = client_config.width;
      output_height = client_config.height;

      const auto nvenc_colorspace = nvenc::nvenc_colorspace_from_sunshine_colorspace(colorspace);
      if (!native_encoder->create_encoder(
            config::video.nv,
            client_config,
            nvenc_colorspace,
            buffer_format,
            hdr_metadata_valid ? &hdr_metadata : nullptr
          )) {
        nvenc = nullptr;
        // Keep the encoder object owned until the outer device teardown has
        // checked whether the driver session was actually destroyed.  If
        // NvEncDestroyEncoder failed, releasing only this object would let the
        // platform device tear down CUDA/GL state still referenced by NVENC.
        return false;
      }
      native_encoder->enable_io_streams();

      auto target = egl::create_target(client_config.width, client_config.height, sw_format);
      if (!target) {
        return false;
      }
      nv12 = std::move(*target);

      auto scaler = egl::sws_t::make(width, height, client_config.width, client_config.height, sw_format);
      if (!scaler) {
        return false;
      }
      sws = std::move(*scaler);
      sws.apply_colorspace(colorspace);

      if (check(
            cdf->cuGraphicsGLRegisterImage(
              &y_res,
              nv12->tex[0],
              GL_TEXTURE_2D,
              CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY
            ),
            "Couldn't register native NVENC Y texture: "sv
          )) {
        return false;
      }
      if (check(
            cdf->cuGraphicsGLRegisterImage(
              &uv_res,
              nv12->tex[1],
              GL_TEXTURE_2D,
              CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY
            ),
            "Couldn't register native NVENC UV texture: "sv
          )) {
        return false;
      }

      BOOST_LOG(info) << "NvEnc: experimental native CUDA backend initialized"sv;
      return true;
    }

    bool prepare_to_destroy() override {
      return !native_encoder || native_encoder->prepare_to_destroy();
    }

    int convert(platf::img_t &img) override {
      if (!native_encoder) {
        return -1;
      }
      context_guard_t guard {cuda_context};
      if (!guard) {
        return -1;
      }

      auto &descriptor = static_cast<egl::img_descriptor_t &>(img);
      if (descriptor.sequence == 0) {
        rgb = egl::create_blank(img);
      } else if (descriptor.sequence > sequence) {
        sequence = descriptor.sequence;
        rgb = {};

        auto imported = egl::import_source(display.get(), descriptor.sd);
        if (!imported) {
          imported = egl::upload_source(display.get(), descriptor.sd);
          if (!imported) {
            return -1;
          }
        }
        rgb = std::move(*imported);
      }

      sws.load_vram(descriptor, offset_x, offset_y, rgb->tex[0]);
      sws.apply_output_lut(descriptor.crtc_gamma_lut, descriptor.crtc_gamma_lut_serial);
      if (sws.convert(nv12->buf)) {
        return -1;
      }

      CUgraphicsResource resources[2] = {y_res.get(), uv_res.get()};
      graphics_mapping_t mapping {*cdf, resources, 2, stream};
      CU_CHECK(mapping.map(), "Couldn't map native NVENC GL textures in CUDA");

      const auto *format = av_pix_fmt_desc_get(sw_format);
      const auto input_buffer = native_encoder->input_buffer();
      const auto input_pitch = native_encoder->input_pitch();

      for (int plane = 0; plane < 2; ++plane) {
        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        CU_CHECK(cdf->cuGraphicsSubResourceGetMappedArray(
                   &copy.srcArray,
                   resources[plane],
                   0,
                   0
                 ),
                 "Couldn't get native NVENC mapped plane");

        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = input_buffer + (plane == 0 ? 0 : input_pitch * native_height());
        copy.dstPitch = input_pitch;
        copy.WidthInBytes = (native_width() * format->comp[plane].step) >>
                            (plane ? format->log2_chroma_w : 0);
        copy.Height = native_height() >> (plane ? format->log2_chroma_h : 0);

        CU_CHECK(cdf->cuMemcpy2DAsync(&copy, stream), "Couldn't copy converted frame into native NVENC input");
      }

      CU_CHECK(mapping.unmap(), "Couldn't unmap native NVENC GL textures from CUDA");
      return 0;
    }

  private:
    int native_width() const noexcept {
      return output_width;
    }

    int native_height() const noexcept {
      return output_height;
    }

    file_t file;
    gbm::gbm_t gbm;
    egl::display_t display;
    egl::ctx_t ctx;

    CUdevice cuda_device {};
    CUcontext cuda_context {};
    CUstream stream {};
    bool primary_context_retained {false};

    int width {};
    int height {};
    int offset_x {};
    int offset_y {};
    int output_width {};
    int output_height {};
    std::uint64_t sequence {};

    AVPixelFormat sw_format {AV_PIX_FMT_NONE};
    NV_ENC_BUFFER_FORMAT buffer_format {NV_ENC_BUFFER_FORMAT_UNDEFINED};
    egl::sws_t sws;
    egl::nv12_t nv12;
    egl::rgb_t rgb;
    registered_resource_t y_res;
    registered_resource_t uv_res;

    std::unique_ptr<nvenc::nvenc_cuda> native_encoder;
  };

  class synthetic_cuda_display_t final: public platf::display_t {
  public:
    explicit synthetic_cuda_display_t(const video::config_t &config, const bool capture_frames):
        capture_frames(capture_frames),
        frame_rate(std::max(1, config.framerate)),
        hdr(config.dynamicRange > 0 && !config.force_sdr) {
      width = std::max(1, config.width);
      height = std::max(1, config.height);
      logical_width = width;
      logical_height = height;
      env_width = width;
      env_height = height;
      env_logical_width = width;
      env_logical_height = height;
    }

    platf::capture_e capture(
      const push_captured_image_cb_t &push,
      const pull_free_image_cb_t &pull,
      bool *
    ) override {
      return capture_frames ? capture_synthetic_black(push, pull, frame_rate) : platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();
      img->width = width;
      img->height = height;
      img->data = nullptr;
      img->pixel_pitch = 4;
      img->row_pitch = width * img->pixel_pitch;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->sequence = 0;
      img->sd = {};
      img->sd.width = width;
      img->sd.height = height;
      std::fill_n(img->sd.fds, 4, -1);
      return img;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e) override {
      return make_avcodec_gl_encode_device(width, height, 0, 0);
    }

    std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_encode_device(platf::pix_fmt_e pix_fmt) override {
      return make_nvenc_gl_encode_device(width, height, 0, 0, pix_fmt);
    }

    bool is_hdr() override {
      return hdr;
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!hdr) {
        metadata = {};
        return false;
      }

      metadata = {};
      metadata.displayPrimaries[0] = {35400, 14600};
      metadata.displayPrimaries[1] = {8500, 39850};
      metadata.displayPrimaries[2] = {6550, 2300};
      metadata.whitePoint = {15635, 16450};
      metadata.maxDisplayLuminance = 1000;
      metadata.minDisplayLuminance = 1;
      metadata.maxContentLightLevel = 1000;
      metadata.maxFrameAverageLightLevel = 250;
      metadata.maxFullFrameLuminance = 400;
      return true;
    }

  private:
    bool capture_frames;
    int frame_rate;
    bool hdr;
  };

  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(int width, int height, bool vram) {
    if (init()) {
      return nullptr;
    }

    std::unique_ptr<cuda_t> cuda;

    if (vram) {
      cuda = std::make_unique<cuda_vram_t>();
    } else {
      cuda = std::make_unique<cuda_ram_t>();
    }

    if (cuda->init(width, height)) {
      return nullptr;
    }

    return cuda;
  }

  /**
   * @brief Create a GL->CUDA encoding device for consuming captured dmabufs.
   * @param width Width of captured frames.
   * @param height Height of captured frames.
   * @param offset_x Offset of content in captured frame.
   * @param offset_y Offset of content in captured frame.
   * @return FFmpeg encoding device context.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_gl_encode_device(int width, int height, int offset_x, int offset_y) {
    if (init()) {
      return nullptr;
    }

    auto cuda = std::make_unique<gl_cuda_vram_t>();

    if (cuda->init(width, height, offset_x, offset_y)) {
      return nullptr;
    }

    return cuda;
  }

  std::unique_ptr<platf::nvenc_encode_device_t> make_nvenc_gl_encode_device(
    int width,
    int height,
    int offset_x,
    int offset_y,
    platf::pix_fmt_e pix_fmt
  ) {
    if (init()) {
      return nullptr;
    }

    auto cuda = std::make_unique<gl_cuda_nvenc_t>();
    if (cuda->init(width, height, offset_x, offset_y, pix_fmt)) {
      return nullptr;
    }
    return cuda;
  }

  std::shared_ptr<platf::display_t> make_nvenc_probe_display(const video::config_t &config) {
    return std::make_shared<synthetic_cuda_display_t>(config, false);
  }

  std::shared_ptr<platf::display_t> make_black_display(const video::config_t &config) {
    return std::make_shared<synthetic_cuda_display_t>(config, true);
  }

  namespace nvfbc {
    static PNVFBCCREATEINSTANCE createInstance {};
    static NVFBC_API_FUNCTION_LIST func {NVFBC_VERSION};

    static constexpr inline NVFBC_BOOL nv_bool(bool b) {
      return b ? NVFBC_TRUE : NVFBC_FALSE;
    }

    static void *handle {nullptr};

    int init() {
      static bool funcs_loaded = false;

      if (funcs_loaded) {
        return 0;
      }

      if (!handle) {
        handle = dyn::handle({"libnvidia-fbc.so.1", "libnvidia-fbc.so"});
        if (!handle) {
          return -1;
        }
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &createInstance, "NvFBCCreateInstance"},
      };

      if (dyn::load(handle, funcs)) {
        dlclose(handle);
        handle = nullptr;

        return -1;
      }

      auto status = cuda::nvfbc::createInstance(&cuda::nvfbc::func);
      if (status) {
        BOOST_LOG(error) << "Unable to create NvFBC instance"sv;

        dlclose(handle);
        handle = nullptr;
        return -1;
      }

      funcs_loaded = true;
      return 0;
    }

    class ctx_t {
    public:
      ctx_t(NVFBC_SESSION_HANDLE handle) {
        NVFBC_BIND_CONTEXT_PARAMS params {NVFBC_BIND_CONTEXT_PARAMS_VER};

        if (func.nvFBCBindContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't bind NvFBC context to current thread: " << func.nvFBCGetLastErrorStr(handle);
        }

        this->handle = handle;
      }

      ~ctx_t() {
        NVFBC_RELEASE_CONTEXT_PARAMS params {NVFBC_RELEASE_CONTEXT_PARAMS_VER};
        if (func.nvFBCReleaseContext(handle, &params)) {
          BOOST_LOG(error) << "Couldn't release NvFBC context from current thread: " << func.nvFBCGetLastErrorStr(handle);
        }
      }

      NVFBC_SESSION_HANDLE handle;
    };

    class handle_t {
      enum flag_e {
        SESSION_HANDLE,
        SESSION_CAPTURE,
        MAX_FLAGS,
      };

    public:
      handle_t() = default;

      handle_t(handle_t &&other):
          handle_flags {other.handle_flags},
          handle {other.handle} {
        other.handle_flags.reset();
      }

      handle_t &operator=(handle_t &&other) {
        std::swap(handle_flags, other.handle_flags);
        std::swap(handle, other.handle);

        return *this;
      }

      static std::optional<handle_t> make() {
        NVFBC_CREATE_HANDLE_PARAMS params {NVFBC_CREATE_HANDLE_PARAMS_VER};

        // Set privateData to allow NvFBC on consumer NVIDIA GPUs.
        // Based on https://github.com/keylase/nvidia-patch/blob/3193b4b1cea91527bf09ea9b8db5aade6a3f3c0a/win/nvfbcwrp/nvfbcwrp_main.cpp#L23-L25 .
        const unsigned int MAGIC_PRIVATE_DATA[4] = {0xAEF57AC5, 0x401D1A39, 0x1B856BBE, 0x9ED0CEBA};
        params.privateData = MAGIC_PRIVATE_DATA;
        params.privateDataSize = sizeof(MAGIC_PRIVATE_DATA);

        handle_t handle;
        auto status = func.nvFBCCreateHandle(&handle.handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to create session: "sv << handle.last_error();

          return std::nullopt;
        }

        handle.handle_flags[SESSION_HANDLE] = true;

        return handle;
      }

      const char *last_error() {
        return func.nvFBCGetLastErrorStr(handle);
      }

      std::optional<NVFBC_GET_STATUS_PARAMS> status() {
        NVFBC_GET_STATUS_PARAMS params {NVFBC_GET_STATUS_PARAMS_VER};

        auto status = func.nvFBCGetStatus(handle, &params);
        if (status) {
          BOOST_LOG(error) << "Failed to get NvFBC status: "sv << last_error();

          return std::nullopt;
        }

        return params;
      }

      int capture(NVFBC_CREATE_CAPTURE_SESSION_PARAMS &capture_params) {
        if (func.nvFBCCreateCaptureSession(handle, &capture_params)) {
          BOOST_LOG(error) << "Failed to start capture session: "sv << last_error();
          return -1;
        }

        handle_flags[SESSION_CAPTURE] = true;

        NVFBC_TOCUDA_SETUP_PARAMS setup_params {
          NVFBC_TOCUDA_SETUP_PARAMS_VER,
          NVFBC_BUFFER_FORMAT_BGRA,
        };

        if (func.nvFBCToCudaSetUp(handle, &setup_params)) {
          BOOST_LOG(error) << "Failed to setup cuda interop with nvFBC: "sv << last_error();
          return -1;
        }
        return 0;
      }

      int stop() {
        if (!handle_flags[SESSION_CAPTURE]) {
          return 0;
        }

        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS params {NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER};

        if (func.nvFBCDestroyCaptureSession(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy capture session: "sv << last_error();

          return -1;
        }

        handle_flags[SESSION_CAPTURE] = false;

        return 0;
      }

      int reset() {
        if (!handle_flags[SESSION_HANDLE]) {
          return 0;
        }

        stop();

        NVFBC_DESTROY_HANDLE_PARAMS params {NVFBC_DESTROY_HANDLE_PARAMS_VER};

        ctx_t ctx {handle};
        if (func.nvFBCDestroyHandle(handle, &params)) {
          BOOST_LOG(error) << "Couldn't destroy session handle: "sv << func.nvFBCGetLastErrorStr(handle);
        }

        handle_flags[SESSION_HANDLE] = false;

        return 0;
      }

      ~handle_t() {
        reset();
      }

      std::bitset<MAX_FLAGS> handle_flags;

      NVFBC_SESSION_HANDLE handle;
    };

    class display_t: public platf::display_t {
    public:
      int init(const std::string_view &display_name, const ::video::config_t &config) {
        auto handle = handle_t::make();
        if (!handle) {
          return -1;
        }

        ctx_t ctx {handle->handle};

        auto status_params = handle->status();
        if (!status_params) {
          return -1;
        }

        int streamedMonitor = -1;
        if (!display_name.empty()) {
          if (status_params->bXRandRAvailable) {
            auto monitor_nr = util::from_view(display_name);

            if (monitor_nr < 0 || monitor_nr >= status_params->dwOutputNum) {
              BOOST_LOG(warning) << "Can't stream monitor ["sv << monitor_nr << "], it needs to be between [0] and ["sv << status_params->dwOutputNum - 1 << "], defaulting to virtual desktop"sv;
            } else {
              streamedMonitor = monitor_nr;
            }
          } else {
            BOOST_LOG(warning) << "XrandR not available, streaming entire virtual desktop"sv;
          }
        }

        delay = std::chrono::nanoseconds {1s} / config.framerate;

        capture_params = NVFBC_CREATE_CAPTURE_SESSION_PARAMS {NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER};

        capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        capture_params.bDisableAutoModesetRecovery = nv_bool(true);

        capture_params.dwSamplingRateMs = 1000 /* ms */ / config.framerate;

        if (streamedMonitor != -1) {
          auto &output = status_params->outputs[streamedMonitor];

          width = output.trackedBox.w;
          height = output.trackedBox.h;
          offset_x = output.trackedBox.x;
          offset_y = output.trackedBox.y;

          capture_params.eTrackingType = NVFBC_TRACKING_OUTPUT;
          capture_params.dwOutputId = output.dwId;
        } else {
          capture_params.eTrackingType = NVFBC_TRACKING_SCREEN;

          width = status_params->screenSize.w;
          height = status_params->screenSize.h;
        }

        env_width = status_params->screenSize.w;
        env_height = status_params->screenSize.h;

        this->handle = std::move(*handle);
        return 0;
      }

      platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        {
          // We must create at least one texture on this thread before calling NvFBCToCudaSetUp()
          // Otherwise it fails with "Unable to register an OpenGL buffer to a CUDA resource (result: 201)" message
          std::shared_ptr<platf::img_t> img_dummy;
          pull_free_image_cb(img_dummy);
        }

        // Force display_t::capture to initialize handle_t::capture
        cursor_visible = !*cursor;

        ctx_t ctx {handle.handle};
        auto fg = util::fail_guard([&]() {
          handle.reset();
        });

        sleep_overshoot_logger.reset();

        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
            sleep_overshoot_logger.first_point(next_frame);
            sleep_overshoot_logger.second_point_now_and_log();
          }

          next_frame += delay;
          if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
            next_frame = now + delay;
          }

          std::shared_ptr<platf::img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 150ms, *cursor);
          switch (status) {
            case platf::capture_e::reinit:
            case platf::capture_e::error:
            case platf::capture_e::interrupted:
              return status;
            case platf::capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return platf::capture_e::ok;
              }
              break;
            case platf::capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return platf::capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }

        return platf::capture_e::ok;
      }

      // Reinitialize the capture session.
      platf::capture_e reinit(bool cursor) {
        if (handle.stop()) {
          return platf::capture_e::error;
        }

        cursor_visible = cursor;
        if (cursor) {
          capture_params.bPushModel = nv_bool(false);
          capture_params.bWithCursor = nv_bool(true);
          capture_params.bAllowDirectCapture = nv_bool(false);
        } else {
          capture_params.bPushModel = nv_bool(true);
          capture_params.bWithCursor = nv_bool(false);
          capture_params.bAllowDirectCapture = nv_bool(true);
        }

        if (handle.capture(capture_params)) {
          return platf::capture_e::error;
        }

        // If trying to capture directly, test if it actually does.
        if (capture_params.bAllowDirectCapture) {
          CUdeviceptr device_ptr;
          NVFBC_FRAME_GRAB_INFO info;

          NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
            NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
            NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
            &device_ptr,
            &info,
            0,
          };

          // Direct Capture may fail the first few times, even if it's possible
          for (int x = 0; x < 3; ++x) {
            if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
              if (status == NVFBC_ERR_MUST_RECREATE) {
                return platf::capture_e::reinit;
              }

              BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();

              return platf::capture_e::error;
            }

            if (info.bDirectCapture) {
              break;
            }

            BOOST_LOG(debug) << "Direct capture failed attempt ["sv << x << ']';
          }

          if (!info.bDirectCapture) {
            BOOST_LOG(debug) << "Direct capture failed, trying the extra copy method"sv;
            // Direct capture failed
            capture_params.bPushModel = nv_bool(false);
            capture_params.bWithCursor = nv_bool(false);
            capture_params.bAllowDirectCapture = nv_bool(false);

            if (handle.stop() || handle.capture(capture_params)) {
              return platf::capture_e::error;
            }
          }
        }

        return platf::capture_e::ok;
      }

      platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
        if (cursor != cursor_visible) {
          auto status = reinit(cursor);
          if (status != platf::capture_e::ok) {
            return status;
          }
        }

        CUdeviceptr device_ptr;
        NVFBC_FRAME_GRAB_INFO info;

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab {
          NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER,
          NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT,
          &device_ptr,
          &info,
          (std::uint32_t) timeout.count(),
        };

        if (auto status = func.nvFBCToCudaGrabFrame(handle.handle, &grab)) {
          if (status == NVFBC_ERR_MUST_RECREATE) {
            return platf::capture_e::reinit;
          }

          BOOST_LOG(error) << "Couldn't capture nvFramebuffer: "sv << handle.last_error();
          return platf::capture_e::error;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }
        auto img = (img_t *) img_out.get();

        if (img->tex.copy((std::uint8_t *) device_ptr, img->height, img->row_pitch)) {
          return platf::capture_e::error;
        }

        return platf::capture_e::ok;
      }

      std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
        return ::cuda::make_avcodec_encode_device(width, height, true);
      }

      std::shared_ptr<platf::img_t> alloc_img() override {
        auto img = std::make_shared<cuda::img_t>();

        img->data = nullptr;
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->width * img->pixel_pitch;

        auto tex_opt = tex_t::make(height, width * img->pixel_pitch);
        if (!tex_opt) {
          return nullptr;
        }

        img->tex = std::move(*tex_opt);

        return img;
      };

      int dummy_img(platf::img_t *) override {
        return 0;
      }

      std::chrono::nanoseconds delay;

      bool cursor_visible;
      handle_t handle;

      NVFBC_CREATE_CAPTURE_SESSION_PARAMS capture_params;
    };
  }  // namespace nvfbc
}  // namespace cuda

namespace platf {
  std::shared_ptr<display_t> nvfbc_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize nvfbc display with the given hw device type"sv;
      return nullptr;
    }

    auto display = std::make_shared<cuda::nvfbc::display_t>();

    if (display->init(display_name, config)) {
      return nullptr;
    }

    return display;
  }

  std::vector<std::string> nvfbc_display_names() {
    if (cuda::init() || cuda::nvfbc::init()) {
      return {};
    }

    std::vector<std::string> display_names;

    auto handle = cuda::nvfbc::handle_t::make();
    if (!handle) {
      return {};
    }

    auto status_params = handle->status();
    if (!status_params) {
      return {};
    }

    if (!status_params->bIsCapturePossible) {
      BOOST_LOG(error) << "NVidia driver doesn't support NvFBC screencasting"sv;
    }

    BOOST_LOG(info) << "Found ["sv << status_params->dwOutputNum << "] outputs"sv;
    BOOST_LOG(info) << "Virtual Desktop: "sv << status_params->screenSize.w << 'x' << status_params->screenSize.h;
    BOOST_LOG(info) << "XrandR: "sv << (status_params->bXRandRAvailable ? "available"sv : "unavailable"sv);

    for (auto x = 0; x < status_params->dwOutputNum; ++x) {
      auto &output = status_params->outputs[x];
      BOOST_LOG(info) << "-- Output --"sv;
      BOOST_LOG(debug) << "  ID: "sv << output.dwId;
      BOOST_LOG(debug) << "  Name: "sv << output.name;
      BOOST_LOG(info) << "  Resolution: "sv << output.trackedBox.w << 'x' << output.trackedBox.h;
      BOOST_LOG(info) << "  Offset: "sv << output.trackedBox.x << 'x' << output.trackedBox.y;
      display_names.emplace_back(std::to_string(x));
    }

    return display_names;
  }
}  // namespace platf
