/**
 * @file tests/unit/platform/linux/test_pipewire_cuda_policy.cpp
 * @brief Tests for Linux PipeWire CUDA DMA-BUF selection.
 */
#include "../../../tests_common.h"

#include <src/platform/linux/pipewire_cuda_policy.h>

namespace policy = platf::linux_pipewire;

TEST(PipewireCudaPolicy, UsesTheActiveNvidiaEglDevice) {
  EXPECT_TRUE(policy::egl_vendor_supports_cuda_dmabuf("NVIDIA"));
  EXPECT_TRUE(policy::egl_vendor_supports_cuda_dmabuf("NVIDIA Corporation"));
}

TEST(PipewireCudaPolicy, RejectsNonNvidiaOrMissingEglDevices) {
  EXPECT_FALSE(policy::egl_vendor_supports_cuda_dmabuf("Mesa Project"));
  EXPECT_FALSE(policy::egl_vendor_supports_cuda_dmabuf("Intel"));
  EXPECT_FALSE(policy::egl_vendor_supports_cuda_dmabuf(""));
}
