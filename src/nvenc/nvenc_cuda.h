/**
 * @file src/nvenc/nvenc_cuda.h
 * @brief Native CUDA NVENC encoder used by the experimental Linux backend.
 */
#pragma once

#if defined(__linux__) && defined(SUNSHINE_BUILD_CUDA)

  #include <cstddef>
  #include <cstdint>

  #include <ffnvcodec/dynlink_cuda.h>
  #include <ffnvcodec/dynlink_loader.h>

  #include "nvenc_base.h"

namespace nvenc {

  /**
   * Native NVENC session backed by a pitched CUDA device allocation.
   *
   * The CUDA context, function table, and conversion stream are owned by the
   * platform encode device and must outlive this object.
   */
  class nvenc_cuda final: public nvenc_base {
  public:
    nvenc_cuda(CUcontext cuda_context, CudaFunctions *cuda_functions, CUstream conversion_stream);
    ~nvenc_cuda() override;

    CUdeviceptr input_buffer() const noexcept;
    std::size_t input_pitch() const noexcept;
    void enable_io_streams();
    bool prepare_to_destroy();

    // Used by the small RAII guard in the implementation. Kept public because
    // the guard is deliberately independent of platform headers in nvenc_base.
    bool push_context();
    void pop_context();

  private:
    bool enter_context() override;
    void leave_context() override;
    bool preserve_encoder_on_destroy_failure() const noexcept override;
    bool init_library(uint32_t api_version) override;
    bool create_and_register_input_buffer() override;
    bool synchronize_input_buffer() override;

    void release_input_buffer();

    CUcontext cuda_context {};
    CudaFunctions *cuda_functions {};  ///< Borrowed from CUDA's process-stable function table.
    CUstream conversion_stream {};
    CUdeviceptr cuda_input_buffer {};
    std::size_t cuda_input_pitch {};

    void *library_handle {};
    uint32_t function_list_api_version {};
    uint32_t max_driver_api_version {};
    bool io_streams_enabled {false};
  };

}  // namespace nvenc

#endif
