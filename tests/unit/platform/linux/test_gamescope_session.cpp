/**
 * @file tests/unit/platform/linux/test_gamescope_session.cpp
 * @brief Tests for bounded Gamescope session-environment discovery.
 */
#include "../../../tests_common.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <src/platform/linux/gamescope_hdr_policy.h>
#include <src/platform/linux/gamescope_session.h>
#include <sys/stat.h>
#include <unistd.h>

namespace session = platf::gamescope_session;

TEST(GamescopeHDR, RequiresKnownProfileOnTheCapturedNode) {
  using platf::gamescope_hdr::supported;
  EXPECT_TRUE(supported(1, 42, 42));
  EXPECT_FALSE(supported(0, 42, 42));
  EXPECT_FALSE(supported(2, 42, 42));
  EXPECT_FALSE(supported(1, 43, 42));
  EXPECT_FALSE(supported(1, UINT32_MAX, UINT32_MAX));
}

TEST(GamescopeHDR, RejectsPartialOrDowngradedHDRNegotiation) {
  using platf::gamescope_hdr::negotiated;
  EXPECT_TRUE(negotiated(true, true, true, true, true, true));
  EXPECT_FALSE(negotiated(false, true, true, true, true, true));
  EXPECT_FALSE(negotiated(true, false, true, true, true, true));
  EXPECT_FALSE(negotiated(true, true, false, true, true, true));
  EXPECT_FALSE(negotiated(true, true, true, false, true, true));
  EXPECT_FALSE(negotiated(true, true, true, true, false, true));
  EXPECT_FALSE(negotiated(true, true, true, true, true, false));
}

TEST(GamescopeSession, AcceptsSocketBasenamesOnly) {
  EXPECT_TRUE(session::valid_wayland_display("gamescope-0"));
  EXPECT_TRUE(session::valid_wayland_display("wayland_1.test"));

  EXPECT_FALSE(session::valid_wayland_display(""));
  EXPECT_FALSE(session::valid_wayland_display("../gamescope-0"));
  EXPECT_FALSE(session::valid_wayland_display("/run/user/1000/gamescope-0"));
  EXPECT_FALSE(session::valid_wayland_display("gamescope-0\nDISPLAY=:9"));
  EXPECT_FALSE(session::valid_wayland_display("."));
  EXPECT_FALSE(session::valid_wayland_display(".."));
  EXPECT_FALSE(session::valid_wayland_display(std::string(108, 'a')));
}

TEST(GamescopeSession, ParsesTheSteamOSEnvironmentWithoutEvaluation) {
  const auto parsed = session::parse_environment(
    "SHELL=/bin/bash\n"
    "DISPLAY=:0\n"
    "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n"
    "IGNORED=$(touch /tmp/never-run)\n"
  );

  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->wayland_display);
  ASSERT_TRUE(parsed->x11_display);
  EXPECT_EQ(*parsed->wayland_display, "gamescope-0");
  EXPECT_EQ(*parsed->x11_display, ":0");
}

TEST(GamescopeSession, RejectsAmbiguousOrHostileAssignments) {
  EXPECT_FALSE(session::parse_environment(
    "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n"
    "GAMESCOPE_WAYLAND_DISPLAY=gamescope-1\n"
  ));
  EXPECT_FALSE(session::parse_environment("GAMESCOPE_WAYLAND_DISPLAY=../../tmp/socket\n"));
  std::string embedded_nul = "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0";
  embedded_nul.push_back('\0');
  embedded_nul += "DISPLAY=:0";
  EXPECT_FALSE(session::parse_environment(embedded_nul));
  EXPECT_FALSE(session::parse_environment(std::string(16385, 'a')));
}

namespace {
  class GamescopeDiscovery: public ::testing::Test {
  protected:
    std::optional<std::string> old_runtime;
    std::optional<std::string> old_display;
    std::filesystem::path directory;

    void SetUp() override {
      if (const auto *value = std::getenv("XDG_RUNTIME_DIR")) {
        old_runtime = value;
      }
      if (const auto *value = std::getenv("GAMESCOPE_WAYLAND_DISPLAY")) {
        old_display = value;
      }
      char path[] = "/tmp/vibeshine-gamescope-discovery.XXXXXX";
      ASSERT_NE(mkdtemp(path), nullptr);
      directory = path;
      setenv("XDG_RUNTIME_DIR", path, 1);
      unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
    }

    void TearDown() override {
      if (old_runtime) {
        setenv("XDG_RUNTIME_DIR", old_runtime->c_str(), 1);
      } else {
        unsetenv("XDG_RUNTIME_DIR");
      }
      if (old_display) {
        setenv("GAMESCOPE_WAYLAND_DISPLAY", old_display->c_str(), 1);
      } else {
        unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
      }
      if (!directory.empty()) {
        std::filesystem::remove_all(directory);
      }
    }
  };
}  // namespace

TEST_F(GamescopeDiscovery, ReadsOwnedFileAndHonorsExplicitSocket) {
  EXPECT_FALSE(session::discover_wayland_display());
  std::ofstream(directory / "gamescope-environment") << "export GAMESCOPE_WAYLAND_DISPLAY='gamescope-0'\n";
  EXPECT_EQ(session::discover_wayland_display(), "gamescope-0");
  setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-1", 1);
  EXPECT_EQ(session::discover_wayland_display(), "gamescope-1");
}

TEST_F(GamescopeDiscovery, ImportsOnlyTheMatchingLocalX11Display) {
  const auto *old = std::getenv("DISPLAY");
  const std::optional<std::string> previous = old ? std::optional<std::string> {old} : std::nullopt;
  setenv("DISPLAY", ":77", 1);
  std::ofstream(directory / "gamescope-environment") << "DISPLAY=:2.0\nGAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n";
  setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-other", 1);
  EXPECT_FALSE(session::import_x11_display());
  EXPECT_STREQ(std::getenv("DISPLAY"), ":77");
  unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
  EXPECT_TRUE(session::import_x11_display());
  EXPECT_STREQ(std::getenv("DISPLAY"), ":2.0");
  std::ofstream(directory / "gamescope-environment") << "DISPLAY=remote:0\nGAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n";
  EXPECT_FALSE(session::import_x11_display());
  if (previous) {
    setenv("DISPLAY", previous->c_str(), 1);
  } else {
    unsetenv("DISPLAY");
  }
}

TEST_F(GamescopeDiscovery, RejectsSymlinkOversizeAndFifoWithoutBlocking) {
  const auto file = directory / "gamescope-environment";
  const auto target = directory / "other";
  std::ofstream(target) << "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n";
  std::filesystem::create_symlink(target, file);
  EXPECT_FALSE(session::discover_wayland_display());
  std::filesystem::remove(file);
  std::ofstream(file) << std::string(16385, 'a');
  EXPECT_FALSE(session::discover_wayland_display());
  std::filesystem::remove(file);
  ASSERT_EQ(mkfifo(file.c_str(), 0600), 0);
  EXPECT_FALSE(session::discover_wayland_display());
}
