/**
 * @file src/nvenc/nvenc_cuda.cpp
 * @brief Native CUDA NVENC encoder used by the experimental Linux backend.
 */
#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)

  #include "nvenc_cuda.h"

  #include <dlfcn.h>

  #include <memory>

  #include "nvenc_api.h"
  #include "src/logging.h"

namespace nvenc {

  namespace {
    constexpr auto nvenc_library_name = "libnvidia-encode.so.1";

    class cuda_context_guard_t {
    public:
      explicit cuda_context_guard_t(nvenc_cuda &encoder):
          encoder(encoder),
          active(encoder.push_context()) {
      }

      ~cuda_context_guard_t() {
        if (active) {
          encoder.pop_context();
        }
      }

      explicit operator bool() const noexcept {
        return active;
      }

    private:
      nvenc_cuda &encoder;
      bool active;
    };
  }  // namespace

  nvenc_cuda::nvenc_cuda(CUcontext cuda_context, CudaFunctions *cuda_functions, CUstream conversion_stream):
      nvenc_base(NV_ENC_DEVICE_TYPE_CUDA),
      cuda_context(cuda_context),
      cuda_functions(cuda_functions),
      conversion_stream(conversion_stream) {
    device = cuda_context;
  }

  nvenc_cuda::~nvenc_cuda() {
    if (encoder) {
      destroy_encoder();
    }

    if (cuda_input_buffer) {
      cuda_context_guard_t context_guard {*this};
      if (context_guard && !registered_input_buffer && !encoder) {
        release_input_buffer();
      } else {
        BOOST_LOG(error) << "NvEnc: leaking the CUDA input surface because native resource teardown did not complete safely";
      }
    }

    if (library_handle && !encoder) {
      dlclose(library_handle);
      library_handle = nullptr;
    } else if (library_handle) {
      BOOST_LOG(error) << "NvEnc: leaving the native library loaded because the encoder could not be destroyed";
    }
  }

  CUdeviceptr nvenc_cuda::input_buffer() const noexcept {
    return cuda_input_buffer;
  }

  std::size_t nvenc_cuda::input_pitch() const noexcept {
    return cuda_input_pitch;
  }

  void nvenc_cuda::enable_io_streams() {
    cuda_context_guard_t context_guard {*this};
    if (!context_guard || !encoder || !nvenc->nvEncSetIOCudaStreams) {
      return;
    }

    if (nvenc_failed(nvenc->nvEncSetIOCudaStreams(
          encoder,
          &conversion_stream,
          &conversion_stream
        ))) {
      BOOST_LOG(warning) << "NvEnc: couldn't associate the CUDA conversion stream; using a CPU synchronization fallback: "
                         << last_nvenc_error_string;
      return;
    }

    io_streams_enabled = true;
    BOOST_LOG(info) << "NvEnc: CUDA input/output stream synchronization enabled";
  }

  bool nvenc_cuda::prepare_to_destroy() {
    // Conversion may have queued a copy before a later plane failed, without
    // ever submitting the frame to NVENC. Destroying the encoder alone does
    // not wait for that copy. Keep the complete device quarantined if CUDA
    // cannot confirm completion, so its input buffer cannot be freed in use.
    if (conversion_stream) {
      cuda_context_guard_t context_guard {*this};
      if (!context_guard ||
          cuda_functions->cuStreamSynchronize(conversion_stream) != CUDA_SUCCESS) {
        BOOST_LOG(error) << "NvEnc: couldn't finish CUDA conversion before teardown";
        return false;
      }
    }
    return !encoder || destroy_encoder();
  }

  bool nvenc_cuda::push_context() {
    if (!cuda_context || !cuda_functions) {
      return false;
    }

    if (cuda_functions->cuCtxPushCurrent(cuda_context) != CUDA_SUCCESS) {
      BOOST_LOG(error) << "NvEnc: couldn't make the CUDA context current";
      return false;
    }
    return true;
  }

  bool nvenc_cuda::enter_context() {
    return push_context();
  }

  void nvenc_cuda::leave_context() {
    pop_context();
  }

  bool nvenc_cuda::preserve_encoder_on_destroy_failure() const noexcept {
    return true;
  }

  void nvenc_cuda::pop_context() {
    CUcontext popped_context {};
    if (cuda_functions->cuCtxPopCurrent(&popped_context) != CUDA_SUCCESS) {
      BOOST_LOG(error) << "NvEnc: couldn't restore the previous CUDA context";
    }
  }

  bool nvenc_cuda::init_library(uint32_t api_version) {
    if (library_handle && nvenc && function_list_api_version == api_version) {
      return true;
    }

    if (!library_handle) {
      library_handle = dlopen(nvenc_library_name, RTLD_LAZY | RTLD_LOCAL);
    }
    if (!library_handle) {
      BOOST_LOG(error) << "NvEnc: couldn't load " << nvenc_library_name << ": " << dlerror();
      return false;
    }

    if (!max_driver_api_version) {
      auto get_max_version = reinterpret_cast<decltype(NvEncodeAPIGetMaxSupportedVersion) *>(
        dlsym(library_handle, "NvEncodeAPIGetMaxSupportedVersion")
      );
      if (!get_max_version) {
        BOOST_LOG(error) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() is missing from " << nvenc_library_name;
        return false;
      }

      uint32_t packed_driver_version = 0;
      if (get_max_version(&packed_driver_version) != NV_ENC_SUCCESS) {
        BOOST_LOG(error) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() failed";
        return false;
      }
      max_driver_api_version = api::driver_max_to_api_version(packed_driver_version);
      BOOST_LOG(info) << "NvEnc: driver supports up to API "
                      << api::version_string(max_driver_api_version)
                      << ", compiled SDK " << NVENCAPI_MAJOR_VERSION << '.' << NVENCAPI_MINOR_VERSION;
    }

    if (!api::driver_supports_api_version(max_driver_api_version, api_version)) {
      last_nvenc_status = NV_ENC_ERR_INVALID_VERSION;
      last_nvenc_error_string = "driver max API " + api::version_string(max_driver_api_version);
      return false;
    }

    auto create_instance = reinterpret_cast<decltype(NvEncodeAPICreateInstance) *>(
      dlsym(library_handle, "NvEncodeAPICreateInstance")
    );
    if (!create_instance) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() is missing from " << nvenc_library_name;
      return false;
    }

    auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
    new_nvenc->version = api::function_list_version(api_version);
    if (nvenc_failed(create_instance(new_nvenc.get()))) {
      if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
        BOOST_LOG(debug) << "NvEnc: NvEncodeAPICreateInstance() rejected API " << api::version_string(api_version);
      } else {
        BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      }
      return false;
    }

    nvenc = std::move(new_nvenc);
    function_list_api_version = api_version;
    return true;
  }

  bool nvenc_cuda::create_and_register_input_buffer() {
    cuda_context_guard_t context_guard {*this};
    if (!context_guard) {
      return false;
    }

    std::size_t bytes_per_sample;
    switch (encoder_params.buffer_format) {
      case NV_ENC_BUFFER_FORMAT_NV12:
        bytes_per_sample = 1;
        break;
      case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        bytes_per_sample = 2;
        break;
      default:
        BOOST_LOG(error) << "NvEnc: native CUDA input format is not supported: "
                         << static_cast<int>(encoder_params.buffer_format);
        return false;
    }

    if (!cuda_input_buffer) {
      const auto allocation_height = encoder_params.height + (encoder_params.height / 2);
      if (cuda_functions->cuMemAllocPitch(
            &cuda_input_buffer,
            &cuda_input_pitch,
            static_cast<std::size_t>(encoder_params.width) * bytes_per_sample,
            allocation_height,
            16
          ) != CUDA_SUCCESS) {
        BOOST_LOG(error) << "NvEnc: couldn't allocate the CUDA input surface";
        cuda_input_buffer = {};
        cuda_input_pitch = {};
        return false;
      }
    }

    NV_ENC_REGISTER_RESOURCE register_resource = {api::register_resource_version(selected_api_version)};
    register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    register_resource.width = encoder_params.width;
    register_resource.height = encoder_params.height;
    register_resource.pitch = static_cast<uint32_t>(cuda_input_pitch);
    register_resource.resourceToRegister = reinterpret_cast<void *>(cuda_input_buffer);
    register_resource.bufferFormat = encoder_params.buffer_format;
    register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &register_resource))) {
      BOOST_LOG(error) << "NvEnc: NvEncRegisterResource(CUDA) failed: " << last_nvenc_error_string;
      return false;
    }

    registered_input_buffer = register_resource.registeredResource;
    return true;
  }

  bool nvenc_cuda::synchronize_input_buffer() {
    if (io_streams_enabled) {
      return true;
    }

    cuda_context_guard_t context_guard {*this};
    if (!context_guard) {
      return false;
    }

    if (cuda_functions->cuStreamSynchronize(conversion_stream) != CUDA_SUCCESS) {
      BOOST_LOG(error) << "NvEnc: CUDA color-conversion stream synchronization failed";
      return false;
    }
    return true;
  }

  void nvenc_cuda::release_input_buffer() {
    if (!cuda_input_buffer || !cuda_functions) {
      return;
    }

    if (cuda_functions->cuMemFree(cuda_input_buffer) != CUDA_SUCCESS) {
      BOOST_LOG(error) << "NvEnc: couldn't free the CUDA input surface";
    }
    cuda_input_buffer = {};
    cuda_input_pitch = {};
  }

}  // namespace nvenc

#endif
