#include "src/platform/linux/audio_sink_status.h"

#include <gtest/gtest.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <thread>
#include <unistd.h>

namespace {
  const auto inventory = R"([
    {"name":"speakers","mute":false,"volume":{"front-left":{"value":65536},"front-right":{"value":65536}}},
    {"name":"sink-sunshine-stereo","mute":true,"volume":{"front-left":{"value":19661},"front-right":{"value":19661}}},
    {"name":"zero","mute":false,"volume":{"front-left":{"value":0},"front-right":{"value":0}}},
    {"name":"one-channel","mute":false,"volume":{"front-left":{"value":0},"front-right":{"value":1}}}
  ])";
}

TEST(LinuxAudioSinkStatus, RecognizesMutedAndZeroVolume) {
  const auto muted = platf::audio_sink_status::from_inventory(inventory, "sink-sunshine-stereo");
  ASSERT_TRUE(muted);
  EXPECT_TRUE(muted->muted);
  EXPECT_FALSE(muted->zero_volume);
  const auto zero = platf::audio_sink_status::from_inventory(inventory, "zero");
  ASSERT_TRUE(zero);
  EXPECT_FALSE(zero->muted);
  EXPECT_TRUE(zero->zero_volume);
}

TEST(LinuxAudioSinkStatus, SessionStartupQueriesOnlyReadOnlyStateAndReportsTheCaptureSink) {
  struct result_t { bool success; std::string stdout_text; };
  std::vector<std::vector<std::string>> calls;
  int warnings = 0;
  platf::audio_sink_status::inspect_session_capture(
    [&](const std::vector<std::string> &command, auto) {
      calls.push_back(command);
      return result_t {true, command[0] == "audio-get-default" ? "sink-sunshine-stereo\n" : inventory};
    }, [&](const std::string &sink, bool muted, bool zero) {
      ++warnings;
      EXPECT_EQ(sink, "sink-sunshine-stereo");
      EXPECT_TRUE(muted);
      EXPECT_FALSE(zero);
    });
  EXPECT_EQ(warnings, 1);
  EXPECT_EQ(calls, (std::vector<std::vector<std::string>> {
    {"audio-get-default"}, {"audio-list-sinks", "details"}}));
}

TEST(LinuxAudioSinkStatus, UnavailableSessionStateDoesNotReportFalseSilence) {
  struct result_t { bool success; std::string stdout_text; };
  for (const bool default_available : {false, true}) {
    int warnings = 0;
    platf::audio_sink_status::inspect_session_capture(
      [&](const std::vector<std::string> &command, auto) {
        return result_t {default_available && command[0] == "audio-get-default", "speakers"};
      }, [&](const auto &, bool, bool) { ++warnings; });
    EXPECT_EQ(warnings, 0);
  }
}

TEST(LinuxAudioSinkStatus, HungQueryTimesOutThenCaptureCanProceedWithoutLeakingTheChild) {
  using namespace std::chrono_literals;
  char pid_path[] = "/tmp/vibeshine-audio-query-XXXXXX";
  const auto descriptor = mkstemp(pid_path);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  const std::string script = "printf '%s' $$ > " + std::string {pid_path} + "; exec sleep 30";
  int calls = 0, warnings = 0;
  const auto start = std::chrono::steady_clock::now();
  platf::audio_sink_status::inspect_session_capture(
    [&](const auto &, auto deadline) {
      ++calls;
      return platf::audio_sink_status::query_until({"/bin/sh", "-c", script}, deadline);
    }, [&](const auto &, bool, bool) { ++warnings; }, 50ms);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(warnings, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  pid_t child = 0;
  std::ifstream(pid_path) >> child;
  std::filesystem::remove(pid_path);
  ASSERT_GT(child, 0);
  const auto cleanup_deadline = std::chrono::steady_clock::now() + 1s;
  while (kill(child, 0) == 0 && std::chrono::steady_clock::now() < cleanup_deadline) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(kill(child, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(LinuxAudioSinkStatus, BothQueriesShareOneDeadlineAndFailedStatusDoesNotWarn) {
  using namespace std::chrono_literals;
  int calls = 0, warnings = 0;
  std::optional<std::chrono::steady_clock::time_point> first_deadline;
  const auto start = std::chrono::steady_clock::now();
  platf::audio_sink_status::inspect_session_capture(
    [&](const auto &, auto deadline) {
      ++calls;
      if (!first_deadline) first_deadline = deadline;
      EXPECT_EQ(deadline, *first_deadline);
      return platf::audio_sink_status::query_until({"/bin/sh", "-c",
        calls == 1 ? "printf sink-sunshine-stereo" : "exec sleep 30"}, deadline);
    }, [&](const auto &, bool, bool) { ++warnings; }, 50ms);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(warnings, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

TEST(LinuxAudioSinkStatus, ExpiredBudgetDoesNotStartAQueryProcess) {
  char marker_path[] = "/tmp/vibeshine-audio-expired-XXXXXX";
  const auto descriptor = mkstemp(marker_path);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  std::filesystem::remove(marker_path);
  const std::string script = "touch " + std::string {marker_path};
  const auto result = platf::audio_sink_status::query_until({"/bin/sh", "-c", script},
    std::chrono::steady_clock::now() - std::chrono::milliseconds {1});
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.stdout_text.empty());
  EXPECT_FALSE(std::filesystem::exists(marker_path));
}

TEST(LinuxAudioSinkStatus, UnknownBrokerCompletionRemainsOptionalDiagnosticData) {
  int calls = 0, warnings = 0;
  platf::audio_sink_status::inspect_session_capture(
    [&](const auto &, auto deadline) {
      ++calls;
      auto result = platf::audio_sink_status::query_until({"/bin/sh", "-c", "exit 125"}, deadline);
      EXPECT_TRUE(result.completion_unknown);
      return result;
    }, [&](const auto &, bool, bool) { ++warnings; });
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(warnings, 0);
}

TEST(LinuxAudioSinkStatus, ResolvesTheSelectedSinkAndKeepsNonzeroChannelsAudible) {
  for (const auto *sink : {"speakers", "one-channel"}) {
    const auto status = platf::audio_sink_status::from_inventory(inventory, sink);
    ASSERT_TRUE(status);
    EXPECT_FALSE(status->muted);
    EXPECT_FALSE(status->zero_volume);
  }
  EXPECT_FALSE(platf::audio_sink_status::from_inventory(inventory, "missing"));
}

TEST(LinuxAudioSinkStatus, MalformedOrUnavailableStateRemainsUnknown) {
  for (const auto *bad : {
         "", "not JSON", "{}", "null", "[null]",
         R"([{"name":"sink","mute":true}])",
         R"([{"name":"sink","mute":"yes","volume":{"mono":{"value":0}}}])",
         R"([{"name":"sink","mute":false,"volume":{}}])",
         R"([{"name":"sink","mute":false,"volume":{"mono":{"value":-1}}}])",
         R"([{"name":"sink","mute":false,"volume":{"mono":{"value":"0"}}}])",
         R"([{"name":"sink","mute":false,"volume":{"mono":{"value":0.0}}}])",
       }) {
    EXPECT_FALSE(platf::audio_sink_status::from_inventory(bad, "sink")) << bad;
  }
}
