/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <cstdlib>
#include <src/video.h>
#ifdef _WIN32
  #include "src/platform/windows/misc.h"
#endif

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    const char *run_encoders = std::getenv("SUNSHINE_RUN_ENCODER_TESTS");
    if (!run_encoders || std::string_view(run_encoders).empty() || std::string_view(run_encoders) == "0") {
      GTEST_SKIP() << "Encoder tests disabled (set SUNSHINE_RUN_ENCODER_TESTS=1 to enable)";
    }

    if (encoder.name != "software") {
      const char *run_hardware = std::getenv("SUNSHINE_RUN_HARDWARE_ENCODER_TESTS");
      if (!run_hardware || std::string_view(run_hardware).empty() || std::string_view(run_hardware) == "0") {
        GTEST_SKIP() << "Hardware encoder tests disabled (set SUNSHINE_RUN_HARDWARE_ENCODER_TESTS=1 to enable)";
      }
    }

#ifdef _WIN32
    if (encoder.name == "nvenc" && !platf::has_nvidia_gpu()) {
      GTEST_SKIP() << "NVIDIA GPU not detected";
    }
#endif
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}
