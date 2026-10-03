/**
 * @file tests/unit/platform/linux/test_boost_process_shim.cpp
 * @brief Regression tests for Linux child-process environment ownership.
 */
#include "../../../tests_common.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <src/boost_process_shim.h>
#include <src/platform/linux/private_vaapi_environment.h>
#include <src/platform/linux/process_group.h>
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

namespace {
  pid_t completed_child(const int exit_code, const bool own_group = false) {
    const auto pid = fork();
    if (pid == 0) {
      if (own_group && setpgid(0, 0)) {
        _exit(99);
      }
      _exit(exit_code);
    }
    if (pid > 0) {
      siginfo_t status {};
      while (waitid(P_PID, pid, &status, WEXITED | WNOWAIT) && errno == EINTR) {}
    }
    return pid;
  }
}  // namespace

TEST(BoostProcessShim, ExternallyReapedChildIsStoppedWithUnknownStatus) {
  const auto pid = completed_child(7);
  ASSERT_GT(pid, 0);
  boost_process_shim::child child {pid};
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_EQ(WEXITSTATUS(status), 7);
  EXPECT_NO_THROW(EXPECT_FALSE(child.running()));
  EXPECT_EQ(child.exit_code(), -1);
  EXPECT_NE(child.native_exit_code(), 0);
  EXPECT_NO_THROW(EXPECT_EQ(child.wait(), -1));
  std::error_code error;
  EXPECT_EQ(child.wait(error), -1);
  EXPECT_EQ(error, std::errc::no_child_process);
  EXPECT_FALSE(child.running(error));
  EXPECT_EQ(error, std::errc::no_child_process);
}

TEST(BoostProcessShim, WaitAlsoToleratesExternalReapingBeforeAnyPoll) {
  const auto pid = completed_child(7);
  ASSERT_GT(pid, 0);
  boost_process_shim::child child {pid};
  ASSERT_EQ(waitpid(pid, nullptr, 0), pid);
  EXPECT_NO_THROW(EXPECT_EQ(child.wait(), -1));
  EXPECT_EQ(child.exit_code(), -1);
}

TEST(BoostProcessShim, NormalWaitRetainsNonzeroExitStatus) {
  const auto pid = completed_child(7);
  ASSERT_GT(pid, 0);
  boost_process_shim::child child {pid};
  EXPECT_EQ(child.wait(), 7);
  EXPECT_EQ(child.exit_code(), 7);
}

TEST(BoostProcessShim, DetachedReaperDoesNotConsumeUnrelatedChildren) {
  const auto pid = completed_child(7);
  ASSERT_GT(pid, 0);
  boost_process_shim::child::reap_detached();
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_EQ(WEXITSTATUS(status), 7);
}

TEST(BoostProcessShim, DetachedChildIsCollectedWithoutReapingItsNeighbor) {
  const auto detached = completed_child(3);
  const auto neighbor = completed_child(7);
  ASSERT_GT(detached, 0);
  ASSERT_GT(neighbor, 0);
  boost_process_shim::child child {detached};
  child.detach();
  boost_process_shim::child::reap_detached();
  errno = 0;
  EXPECT_EQ(waitpid(detached, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
  int status = 0;
  ASSERT_EQ(waitpid(neighbor, &status, 0), neighbor);
  EXPECT_EQ(WEXITSTATUS(status), 7);
}

TEST(LinuxProcessGroup, ObservationNeverReapsExitedLeader) {
  const auto pid = completed_child(7, true);
  ASSERT_GT(pid, 0);
  EXPECT_FALSE(platf::linux_process::group_running(pid));
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_EQ(WEXITSTATUS(status), 7);
}

TEST(LinuxProcessGroup, ObservesLiveGroupAndRejectsInvalidHandle) {
  const auto group = getpgrp();
  EXPECT_TRUE(platf::linux_process::group_running(group));
  EXPECT_FALSE(platf::linux_process::group_running(0));
  EXPECT_FALSE(platf::linux_process::group_running(-1));
}

TEST(LinuxProcessGroup, LeaderExitKeepsLiveMemberWithoutConsumingEitherStatus) {
  int ready[2], leader_release[2], member_release[2];
  ASSERT_EQ(pipe(ready), 0);
  ASSERT_EQ(pipe(leader_release), 0);
  ASSERT_EQ(pipe(member_release), 0);
  const auto spawn = [&](const pid_t group, const int release, const int code) {
    const auto pid = fork();
    if (pid == 0) {
      if (setpgid(0, group)) {
        _exit(99);
      }
      const char byte = 'r';
      if (write(ready[1], &byte, 1) != 1) {
        _exit(99);
      }
      char signal;
      if (read(release, &signal, 1) != 1) {
        _exit(99);
      }
      _exit(code);
    }
    return pid;
  };
  const auto leader = spawn(0, leader_release[0], 7);
  ASSERT_GT(leader, 0);
  char byte;
  ASSERT_EQ(read(ready[0], &byte, 1), 1);
  const auto member = spawn(leader, member_release[0], 11);
  ASSERT_GT(member, 0);
  ASSERT_EQ(read(ready[0], &byte, 1), 1);
  ASSERT_EQ(write(leader_release[1], "x", 1), 1);
  siginfo_t info {};
  ASSERT_EQ(waitid(P_PID, leader, &info, WEXITED | WNOWAIT), 0);
  EXPECT_TRUE(platf::linux_process::group_running(leader));
  ASSERT_EQ(write(member_release[1], "x", 1), 1);
  ASSERT_EQ(waitid(P_PID, member, &info, WEXITED | WNOWAIT), 0);
  EXPECT_FALSE(platf::linux_process::group_running(leader));
  int status = 0;
  ASSERT_EQ(waitpid(leader, &status, 0), leader);
  EXPECT_EQ(WEXITSTATUS(status), 7);
  ASSERT_EQ(waitpid(member, &status, 0), member);
  EXPECT_EQ(WEXITSTATUS(status), 11);
  for (const auto fd : {ready[0], ready[1], leader_release[0], leader_release[1], member_release[0], member_release[1]}) {
    close(fd);
  }
}

TEST(LinuxProcessGroup, PositiveGroupObservationsAreBoundedUnderInputPacketPolling) {
  using namespace std::chrono_literals;
  platf::linux_process::group_observer_t observer;
  const auto start = std::chrono::steady_clock::time_point {};
  int scans = 0;
  bool live = true;
  const auto observe = [&](const pid_t group) {
    EXPECT_EQ(group, 123);
    ++scans;
    return live;
  };
  // A thousand input events within the interval share one non-reaping scan.
  for (int packet = 0; packet < 1000; ++packet) {
    EXPECT_TRUE(observer.running(123, start + packet * 200us, observe));
  }
  EXPECT_EQ(scans, 1);
  live = false;
  EXPECT_TRUE(observer.running(123, start + 249ms, observe));
  EXPECT_FALSE(observer.running(123, start + 250ms, observe));
  EXPECT_EQ(scans, 2);
  // A later group with the same ID is observed immediately after a negative
  // result rather than inheriting an old "not running" observation.
  live = true;
  EXPECT_TRUE(observer.running(123, start + 251ms, observe));
  EXPECT_EQ(scans, 3);
}

TEST(LinuxProcessGroup, CachedObservationDoesNotCarryAcrossGroupsOrInvalidHandles) {
  using namespace std::chrono_literals;
  platf::linux_process::group_observer_t observer;
  const auto start = std::chrono::steady_clock::time_point {};
  int scans = 0;
  const auto observe = [&](const pid_t group) {
    ++scans;
    return group == 123;
  };
  EXPECT_TRUE(observer.running(123, start, observe));
  EXPECT_FALSE(observer.running(456, start + 1ms, observe));
  EXPECT_FALSE(observer.running(0, start + 2ms, observe));
  EXPECT_FALSE(observer.running(-1, start + 3ms, observe));
  EXPECT_EQ(scans, 2);
}
