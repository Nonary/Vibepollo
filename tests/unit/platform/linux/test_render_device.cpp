#include "src/platform/linux/render_device.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <sys/stat.h>

namespace {
  class RenderDevice: public testing::Test {
  protected:
    std::filesystem::path root;

    void SetUp() override {
      char path[] = "/tmp/vibepollo-render-topology-XXXXXX";
      const auto created = mkdtemp(path);
      ASSERT_NE(created, nullptr);
      root = created;
      std::filesystem::create_directories(root / "class");
      std::filesystem::create_directories(root / "dri");
    }

    void TearDown() override {
      if (!root.empty()) {
        std::filesystem::remove_all(root);
      }
    }

    void card(const char *name, const char *status, const char *render = nullptr) {
      std::filesystem::create_directories(root / "class" / name / "device/drm");
      const auto connector = root / "class" / (std::string(name) + "-DP-1");
      std::filesystem::create_directories(connector);
      std::ofstream(connector / "status") << status << '\n';
      if (render) {
        std::filesystem::create_directories(root / "class" / name / "device/drm" / render);
        // A renderer open would block on this FIFO. Discovery may only stat it.
        ASSERT_EQ(mkfifo((root / "dri" / render).c_str(), 0600), 0);
      }
    }

    std::string discover() {
      return platf::drm_topology::find_render_node_with_display(root / "class", root / "dri");
    }
  };
}  // namespace

TEST_F(RenderDevice, SelectsConnectedPhysicalRendererWithoutOpeningDevices) {
  card("card0", "connected");  // Virtual card: deliberately no renderer.
  card("card1", "disconnected", "renderD128");
  card("card2", "connected", "renderD129");
  EXPECT_EQ(discover(), (root / "dri/renderD129").string());
}

TEST_F(RenderDevice, DoesNotConfuseCardPrefixesOrAcceptMalformedNodes) {
  card("card1", "disconnected", "renderD128");
  card("card10", "connected", "renderD129");
  card("card0fake", "connected", "renderD130");
  EXPECT_EQ(discover(), (root / "dri/renderD129").string());
}

TEST_F(RenderDevice, MissingTopologyOrDeviceDoesNotInventARenderer) {
  card("card0", "connected", "renderD128");
  std::filesystem::remove(root / "dri/renderD128");
  EXPECT_TRUE(discover().empty());
  EXPECT_TRUE(platf::drm_topology::find_render_node_with_display(root / "missing", root / "dri").empty());
}
