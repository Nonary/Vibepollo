#include "../../../tests_common.h"
#include <src/platform/linux/capture_fallback.h>

#include <memory>
#include <string>
#include <vector>

namespace {
  struct fake_display_t {
    bool hdr;
    bool is_hdr() const { return hdr; }
  };

  const auto all_outputs = [](const std::string &) { return true; };
}  // namespace

TEST(LinuxCaptureFallback, UsesEnumeratedOutputAfterFailedCreation) {
  std::vector<std::string> attempted;
  auto working = std::make_shared<fake_display_t>(false);
  const auto result = platf::linux_capture::try_outputs(
    [] { return std::vector<std::string> {"eDP-1", "HDMI-A-1", "DP-1"}; },
    [&](const std::string &name) {
      attempted.push_back(name);
      return name == "HDMI-A-1" ? working : nullptr;
    }, all_outputs, false
  );
  EXPECT_EQ(result, working);
  EXPECT_EQ(attempted, (std::vector<std::string> {"eDP-1", "HDMI-A-1"}));
}

TEST(LinuxCaptureFallback, SkipsIneligibleOutputs) {
  std::vector<std::string> attempted;
  const auto result = platf::linux_capture::try_outputs(
    [] { return std::vector<std::string> {"managed-output", "eDP-1"}; },
    [&](const std::string &name) {
      attempted.push_back(name);
      return std::make_shared<fake_display_t>(false);
    }, [](const std::string &name) { return name != "managed-output"; }, false
  );
  EXPECT_TRUE(result);
  EXPECT_EQ(attempted, (std::vector<std::string> {"eDP-1"}));
}

TEST(LinuxCaptureFallback, ReleasesRejectedSdrBeforeTryingHdrOutput) {
  std::weak_ptr<fake_display_t> rejected;
  const auto result = platf::linux_capture::try_outputs(
    [] { return std::vector<std::string> {"sdr", "hdr"}; },
    [&](const std::string &name) {
      EXPECT_TRUE(rejected.expired());
      auto display = std::make_shared<fake_display_t>(name == "hdr");
      if (name == "sdr") {
        rejected = display;
      }
      return display;
    }, all_outputs, true
  );
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->is_hdr());
}

TEST(LinuxCaptureFallback, CannotSatisfyHdrWithSdrOnlyOutputs) {
  EXPECT_FALSE(platf::linux_capture::try_outputs(
    [] { return std::vector<std::string> {"sdr"}; },
    [](const std::string &) { return std::make_shared<fake_display_t>(false); }, all_outputs, true
  ));
}

TEST(LinuxCaptureFallback, NoOutputsDoesNotAttemptCapture) {
  EXPECT_FALSE(platf::linux_capture::try_outputs(
    [] { return std::vector<std::string> {}; },
    [](const std::string &) {
      ADD_FAILURE() << "Capture must not run without an enumerated output";
      return std::shared_ptr<fake_display_t> {};
    }, all_outputs, false
  ));
}
