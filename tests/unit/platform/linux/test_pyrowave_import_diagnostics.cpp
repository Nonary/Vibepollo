/** Direct tests of the modifier classification/diagnostics used by the importer. */
#include "../../../tests_common.h"

#include <limits>
#include <src/platform/linux/pyrowave_import_diagnostics.h>

namespace {
  namespace diagnostic = pyrowave::linux_gpu::import_diagnostics;

  constexpr std::uint64_t tiled_modifier = 0x0300000000606013ULL;
  constexpr VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

  class PyrowaveImportDiagnostics: public testing::Test {
  protected:
    egl::surface_descriptor_t surface;
    diagnostic::device_t device {"/dev/dri/renderD129", "NVIDIA GeForce RTX 4080", 0x10de, 0x2704, true, 226, 129};

    void SetUp() override {
      // Classification only inspects metadata; no descriptor ownership or GPU
      // operations are involved in this component.
      surface.fds[0] = 3;
      surface.fourcc = DRM_FORMAT_XRGB8888;
      surface.modifier = tiled_modifier;
    }

    void has(const std::string &detail, const char *field) {
      EXPECT_NE(detail.find(field), std::string::npos) << detail;
    }
  };

  TEST_F(PyrowaveImportDiagnostics, MissingModifierIdentifiesSelectedDeviceAndCapturePair) {
    const std::vector<VkDrmFormatModifierPropertiesEXT> modifiers {{DRM_FORMAT_MOD_LINEAR, 1, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT}};
    const auto check = diagnostic::classify(surface, format, modifiers);
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::modifier_not_advertised);
    const auto detail = diagnostic::describe(surface, format, check, device);
    has(detail, "GPU cannot import capture modifier (reason=modifier-not-advertised");
    has(detail, "configured_device=\"/dev/dri/renderD129\"");
    has(detail, "gpu=\"NVIDIA GeForce RTX 4080\"");
    has(detail, "vendor=0x10de, device=0x2704, drm_render=226:129");
    has(detail, "fourcc=\"XR24\"/0x34325258, VkFormat=44");
    has(detail, "modifier=0x300000000606013, advertised_modifiers=1");
    has(detail, "matching_planes=unavailable, matching_tiling_features=unavailable");
    std::cout << "Observed diagnostic: " << detail << '\n';
  }

  TEST_F(PyrowaveImportDiagnostics, PrimarySelectionPathIsDistinctFromSelectedGpuRenderIdentity) {
    device.configured_device = "/dev/dri/card1";
    device.has_render = true;
    device.render_major = 226;
    device.render_minor = 129;
    const auto detail = diagnostic::describe(surface, format, diagnostic::classify(surface, format, {}), device);
    has(detail, "configured_device=\"/dev/dri/card1\"");
    has(detail, "gpu=\"NVIDIA GeForce RTX 4080\"");
    has(detail, "vendor=0x10de, device=0x2704, drm_render=226:129");
    EXPECT_EQ(detail.find("render_node="), std::string::npos);
    EXPECT_LT(detail.size(), 768u);
    std::cout << "Primary selection diagnostic: " << detail << '\n';
  }

  TEST_F(PyrowaveImportDiagnostics, EmptyQueryDoesNotInventAMatchingLinearModifier) {
    surface.modifier = DRM_FORMAT_MOD_LINEAR;
    const auto check = diagnostic::classify(surface, format, {});
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::modifier_not_advertised);
    EXPECT_FALSE(check.matched);
    has(diagnostic::describe(surface, format, check, device), "modifier=0x0, advertised_modifiers=0, matching_planes=unavailable");
  }

  TEST_F(PyrowaveImportDiagnostics, InvalidMatchingPlaneCountsRemainRejectedAndExposeCapabilities) {
    for (const auto planes : {0u, 5u, std::numeric_limits<std::uint32_t>::max()}) {
      SCOPED_TRACE(planes);
      const auto features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
      const std::vector<VkDrmFormatModifierPropertiesEXT> modifiers {{tiled_modifier, planes, VkFormatFeatureFlags(features)}};
      const auto check = diagnostic::classify(surface, format, modifiers);
      EXPECT_EQ(check.rejection, diagnostic::rejection_e::invalid_plane_count);
      EXPECT_TRUE(check.matched);
      EXPECT_EQ(check.plane_count, planes);
      EXPECT_EQ(check.tiling_features, VkFormatFeatureFlags(features));
      const auto detail = diagnostic::describe(surface, format, check, device);
      has(detail, "reason=invalid-plane-count");
      has(detail, ("matching_planes=" + std::to_string(planes)).c_str());
      has(detail, "matching_tiling_features=0x4001");
    }
  }

  TEST_F(PyrowaveImportDiagnostics, ValidMatchesKeepPlaneCountAndGenerateNoError) {
    for (const auto planes : {1u, 2u, 3u, 4u}) {
      // Zero feature bits must not introduce a new acceptance gate here.
      const std::vector<VkDrmFormatModifierPropertiesEXT> modifiers {{DRM_FORMAT_MOD_LINEAR, 1, 1}, {tiled_modifier, planes, 0}};
      const auto check = diagnostic::classify(surface, format, modifiers);
      EXPECT_EQ(check.rejection, diagnostic::rejection_e::none);
      EXPECT_TRUE(check.matched);
      EXPECT_EQ(check.plane_count, planes);
      EXPECT_EQ(check.advertised_modifiers, 2u);
      EXPECT_TRUE(diagnostic::describe(surface, format, check, device).empty());
    }
    surface.modifier = DRM_FORMAT_MOD_LINEAR;
    const auto check = diagnostic::classify(surface, format, {{DRM_FORMAT_MOD_LINEAR, 1, 1}});
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::none);
  }

  TEST_F(PyrowaveImportDiagnostics, ImplicitLayoutIsNotReportedAsAQueriedCapability) {
    surface.modifier = DRM_FORMAT_MOD_INVALID;
    const auto check = diagnostic::classify(surface, format, {});
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::implicit_modifier);
    const auto detail = diagnostic::describe(surface, format, check, device);
    has(detail, "capture format or explicit DMA-BUF modifier unsupported (reason=implicit-modifier");
    has(detail, "advertised_modifiers=not-queried");
  }

  TEST_F(PyrowaveImportDiagnostics, UnsupportedFormatIsDistinctFromMissingModifier) {
    const auto check = diagnostic::classify(surface, VK_FORMAT_UNDEFINED, {});
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::unsupported_format);
    const auto detail = diagnostic::describe(surface, VK_FORMAT_UNDEFINED, check, device);
    has(detail, "reason=unsupported-format");
    has(detail, "VkFormat=0");
    has(detail, "advertised_modifiers=not-queried");
  }

  TEST_F(PyrowaveImportDiagnostics, InvalidDescriptorHelperClassificationRemainsRejected) {
    // Helper-only coverage: the normal adapter rejects this FD, and the
    // importer's preceding fence poll cannot reach this diagnostic for it.
    surface.fds[0] = -1;
    const auto check = diagnostic::classify(surface, format, {{tiled_modifier, 1, 1}});
    EXPECT_EQ(check.rejection, diagnostic::rejection_e::invalid_fd);
    has(diagnostic::describe(surface, format, check, device), "reason=invalid-fd");
  }

  TEST_F(PyrowaveImportDiagnostics, AutomaticSelectionDoesNotInventAConfiguredPath) {
    device.configured_device.clear();
    device.has_render = false;
    const auto detail = diagnostic::describe(surface, format, diagnostic::classify(surface, format, {}), device);
    has(detail, "configured_device=\"(automatic)\"");
    has(detail, "drm_render=unavailable");
  }

  TEST_F(PyrowaveImportDiagnostics, DeviceTextAndFourccCannotExpandOrInjectLogLines) {
    device.configured_device = std::string(10000, '\n');
    device.gpu_name = std::string(10000, '"');
    device.render_major = std::numeric_limits<std::int64_t>::min();
    device.render_minor = std::numeric_limits<std::int64_t>::min();
    device.vendor_id = device.device_id = std::numeric_limits<std::uint32_t>::max();
    surface.fourcc = 0x5c220a00;
    const auto detail = diagnostic::describe(surface, format, diagnostic::classify(surface, format, {}), device);
    EXPECT_LT(detail.size(), 768u);
    EXPECT_EQ(detail.find('\n'), std::string::npos);
    EXPECT_EQ(detail.find('\\'), std::string::npos);
    has(detail, "fourcc=\"????\"/0x5c220a00");
    has(detail, "modifier=0x300000000606013");
    has(detail, "matching_planes=unavailable");
  }
}  // namespace
