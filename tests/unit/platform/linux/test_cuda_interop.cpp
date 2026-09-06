/**
 * @file tests/unit/platform/linux/test_cuda_interop.cpp
 * @brief Fault injection for CUDA graphics mapping lifetimes, without a GPU.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/cuda_interop.h>

namespace {
  // ffnvcodec intentionally exposes only a subset of CUDA's error names.
  constexpr auto invalid_value = static_cast<CUresult>(1);
  constexpr auto invalid_context = static_cast<CUresult>(201);

  class CudaInterop: public testing::Test {
  protected:
    static inline CudaInterop *active;
    CudaFunctions functions {};
    CUgraphicsResource resources[2] {};
    CUstream stream = reinterpret_cast<CUstream>(0x100);
    CUresult map_result = CUDA_SUCCESS;
    CUresult unmap_result = CUDA_SUCCESS;
    int maps = 0;
    int unmaps = 0;

    void SetUp() override {
      active = this;
      functions.cuGraphicsMapResources = [](unsigned int count, CUgraphicsResource *resources, CUstream stream) {
        EXPECT_EQ(count, 2u);
        EXPECT_EQ(resources, active->resources);
        EXPECT_EQ(stream, active->stream);
        ++active->maps;
        return active->map_result;
      };
      functions.cuGraphicsUnmapResources = [](unsigned int count, CUgraphicsResource *resources, CUstream stream) {
        EXPECT_EQ(count, 2u);
        EXPECT_EQ(resources, active->resources);
        EXPECT_EQ(stream, active->stream);
        ++active->unmaps;
        return active->unmap_result;
      };
    }
  };

  TEST_F(CudaInterop, UnmapsOnEarlyConversionFailure) {
    const auto convert = [&] {
      cuda::graphics_mapping_t mapping {functions, resources, 2, stream};
      EXPECT_EQ(mapping.map(), CUDA_SUCCESS);
      // A plane lookup or asynchronous copy submission failed after mapping.
      return invalid_value;
    };
    EXPECT_EQ(convert(), invalid_value);
    EXPECT_EQ(maps, 1);
    EXPECT_EQ(unmaps, 1);
  }

  TEST_F(CudaInterop, DoesNotUnmapFailedMap) {
    map_result = invalid_context;
    {
      cuda::graphics_mapping_t mapping {functions, resources, 2, stream};
      EXPECT_EQ(mapping.map(), invalid_context);
    }
    EXPECT_EQ(unmaps, 0);
  }

  TEST_F(CudaInterop, SuccessfulConversionUnmapsExactlyOnce) {
    {
      cuda::graphics_mapping_t mapping {functions, resources, 2, stream};
      ASSERT_EQ(mapping.map(), CUDA_SUCCESS);
      EXPECT_EQ(mapping.unmap(), CUDA_SUCCESS);
    }
    EXPECT_EQ(unmaps, 1);
  }

  TEST_F(CudaInterop, RetriesFailedUnmapDuringCleanup) {
    {
      cuda::graphics_mapping_t mapping {functions, resources, 2, stream};
      ASSERT_EQ(mapping.map(), CUDA_SUCCESS);
      unmap_result = CUDA_ERROR_UNKNOWN;
      EXPECT_EQ(mapping.unmap(), CUDA_ERROR_UNKNOWN);
      unmap_result = CUDA_SUCCESS;
    }
    EXPECT_EQ(unmaps, 2);
  }

  TEST_F(CudaInterop, CleanupDoesNotLoopOnDriverFailure) {
    {
      cuda::graphics_mapping_t mapping {functions, resources, 2, stream};
      ASSERT_EQ(mapping.map(), CUDA_SUCCESS);
      unmap_result = invalid_context;
    }
    EXPECT_EQ(unmaps, 1);
  }
}  // namespace
