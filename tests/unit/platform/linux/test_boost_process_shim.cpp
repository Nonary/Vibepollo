/**
 * @file tests/unit/platform/linux/test_boost_process_shim.cpp
 * @brief Regression tests for Linux child-process environment ownership.
 */
#include "../../../tests_common.h"

#include <src/boost_process_shim.h>
#include <src/platform/linux/private_vaapi_environment.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

TEST(BoostProcessShim, ConvertedEnvironmentOwnsItsEntries) {
  auto environment = boost_process_shim::environment::current();
  environment["VIBESHINE_ENV_OWNERSHIP_TEST"] = "preserved";

  auto process_environment = environment.to_process_environment();

  const auto owned_entry = std::find_if(
    process_environment.env_buffer.cbegin(),
    process_environment.env_buffer.cend(),
    [](const auto &entry) {
      return entry.native() == "VIBESHINE_ENV_OWNERSHIP_TEST=preserved";
    }
  );

  ASSERT_NE(owned_entry, process_environment.env_buffer.cend());
}

TEST(BoostProcessShim, PrivateVaapiDriverIsAbsentFromLaunchedApplications) {
  boost_process_shim::environment environment;
  environment["VIBEPOLLO_PRIVATE_VAAPI"] = "1";
  environment["LIBVA_DRIVERS_PATH"] = "/opt/vibepollo/private-driver";
  environment["LIBVA_DRIVER_NAME"] = "radeonsi";
  environment["GAME_OPTION"] = "preserved";
  auto child_environment = platf::linux_private_vaapi::child_environment(
    environment, "1", "/opt/vibepollo/private-driver", "radeonsi"
  ).to_process_environment();

  const auto close_file = [](FILE *file) { std::fclose(file); };
  std::unique_ptr<FILE, decltype(close_file)> output {std::tmpfile(), close_file};
  ASSERT_NE(output, nullptr);
  boost::process::v2::process_stdio stdio {};
  stdio.out = output.get();
  boost::process::v2::process child(
    boost::asio::system_executor(), "/usr/bin/env", std::vector<std::string> {}, stdio, child_environment
  );
  EXPECT_EQ(child.wait(), 0);
  std::rewind(output.get());
  std::string text;
  char line[256];
  while (std::fgets(line, sizeof(line), output.get())) {
    text += line;
  }
  EXPECT_EQ(text, "GAME_OPTION=preserved\n");
  // Constructing the child environment leaves the daemon's source unchanged.
  EXPECT_EQ(environment["LIBVA_DRIVER_NAME"].to_string(), "radeonsi");
}

TEST(BoostProcessShim, PrivateVaapiFilteringPreservesApplicationOverridesAndRequiresExactMarker) {
  boost_process_shim::environment environment;
  environment["VIBEPOLLO_PRIVATE_VAAPI"] = "1";
  environment["LIBVA_DRIVERS_PATH"] = "/game/drivers";
  environment["LIBVA_DRIVER_NAME"] = "game-driver";
  const auto child_environment = platf::linux_private_vaapi::child_environment(
    environment, "1", "/opt/vibepollo/private-driver", "radeonsi"
  );
  ASSERT_EQ(std::distance(child_environment.begin(), child_environment.end()), 2);
  for (const auto &entry : child_environment) {
    EXPECT_TRUE(entry.to_string() == "/game/drivers" || entry.to_string() == "game-driver");
  }

  for (const auto *marker : {static_cast<const char *>(nullptr), "", "0", "10"}) {
    const auto unchanged = platf::linux_private_vaapi::child_environment(
      environment, marker, "/game/drivers", "game-driver"
    );
    EXPECT_EQ(std::distance(unchanged.begin(), unchanged.end()), 3);
  }
}
