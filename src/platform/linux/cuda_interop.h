/**
 * @file src/platform/linux/cuda_interop.h
 * @brief Scoped CUDA graphics mappings, including failed conversions.
 */
#pragma once

#include <ffnvcodec/dynlink_loader.h>

namespace cuda {
  // The function table, resource array, stream and current CUDA context must
  // outlive this scope. A failed explicit unmap is retried during unwinding.
  class graphics_mapping_t {
  public:
    graphics_mapping_t(CudaFunctions &functions, CUgraphicsResource *resources, unsigned int count, CUstream stream):
        functions(functions),
        resources(resources),
        count(count),
        stream(stream) {
    }

    graphics_mapping_t(const graphics_mapping_t &) = delete;
    graphics_mapping_t &operator=(const graphics_mapping_t &) = delete;

    ~graphics_mapping_t() {
      if (mapped) {
        unmap();
      }
    }

    CUresult map() {
      const auto status = functions.cuGraphicsMapResources(count, resources, stream);
      mapped = status == CUDA_SUCCESS;
      return status;
    }

    CUresult unmap() {
      if (!mapped) {
        return CUDA_SUCCESS;
      }
      const auto status = functions.cuGraphicsUnmapResources(count, resources, stream);
      if (status == CUDA_SUCCESS) {
        mapped = false;
      }
      return status;
    }

  private:
    CudaFunctions &functions;
    CUgraphicsResource *resources;
    unsigned int count;
    CUstream stream;
    bool mapped {false};
  };
}  // namespace cuda
