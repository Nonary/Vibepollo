#include "src/platform/linux/cuda_render_device.h"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {
  class CudaRenderDevice: public testing::Test {
  protected:
    std::filesystem::path root;
    int opened_fd {-1};

    void SetUp() override {
      char path[] = "/tmp/vibeshine-cuda-render-XXXXXX";
      const auto created = mkdtemp(path);
      ASSERT_NE(created, nullptr);
      root = created;
      std::filesystem::create_directories(root / "pci");
      std::filesystem::create_directories(root / "dri");
    }

    void TearDown() override {
      if (opened_fd >= 0) {
        close(opened_fd);
      }
      if (!root.empty()) {
        std::filesystem::remove_all(root);
      }
    }

    void node(const char *pci, const char *name, const char *contents = "renderer") {
      std::filesystem::create_directories(root / "pci" / pci / "drm" / name);
      std::ofstream(root / "dri" / name) << contents;
    }

    int open_device(const char *pci) {
      opened_fd = cuda::open_render_node_for_pci_device(pci, root / "pci", root / "dri");
      return opened_fd;
    }
  };
}  // namespace

TEST_F(CudaRenderDevice, OpensOnlyTheCudaDevicesRendererAndDoesNotInheritTheDescriptor) {
  node("0000:01:00.0", "renderD128", "other");
  node("0000:ab:01.0", "card2", "primary");
  node("0000:ab:01.0", "renderD129", "selected");

  ASSERT_GE(open_device("0000:AB:01.0"), 0);
  char contents[8] {};
  ASSERT_EQ(read(opened_fd, contents, sizeof(contents)), static_cast<ssize_t>(sizeof(contents)));
  EXPECT_EQ(std::string_view(contents, sizeof(contents)), "selected");
  EXPECT_EQ(fcntl(opened_fd, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
  EXPECT_EQ(fcntl(opened_fd, F_GETFL) & O_ACCMODE, O_RDWR);
}

TEST_F(CudaRenderDevice, MissingRendererDoesNotFallBackToPrimaryOrAnotherGpu) {
  node("0000:01:00.0", "card0");
  node("0000:02:00.0", "renderD128");
  EXPECT_EQ(open_device("0000:01:00.0"), -1);
  EXPECT_EQ(errno, ENODEV);
}

TEST_F(CudaRenderDevice, MissingDeviceFileDoesNotFallBackToPrimary) {
  node("0000:01:00.0", "card0");
  node("0000:01:00.0", "renderD128");
  std::filesystem::remove(root / "dri/renderD128");
  EXPECT_EQ(open_device("0000:01:00.0"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(CudaRenderDevice, RejectsMalformedAndAmbiguousRenderNodeNames) {
  node("0000:01:00.0", "renderD");
  node("0000:01:00.0", "renderD128-DP-1");
  node("0000:01:00.0", "renderDabc");
  EXPECT_EQ(open_device("0000:01:00.0"), -1);
  EXPECT_EQ(errno, ENODEV);

  node("0000:01:00.0", "renderD128");
  node("0000:01:00.0", "renderD129");
  EXPECT_EQ(open_device("0000:01:00.0"), -1);
  EXPECT_EQ(errno, ENODEV);
}

TEST_F(CudaRenderDevice, RejectsInvalidPciIdentitiesAndUnavailableTopology) {
  EXPECT_EQ(open_device("../0000:01:00.0"), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(open_device("0000:01:0g.0"), -1);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(open_device("0000:01:00.0"), -1);
  EXPECT_EQ(errno, ENOENT);
}
