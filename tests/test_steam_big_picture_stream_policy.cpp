#include "src/steam_big_picture_stream_policy.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace {
  using platf::steam::stream_ui::action;
  using platf::steam::stream_ui::session;
  using platf::steam::stream_ui::uri;
}

TEST(SteamBigPictureStream, SendsOpenBeforeGameAndCloseOnTerminalTeardown) {
  session app;
  std::vector<std::string_view> events;
  auto record = [&](action command) { events.push_back(uri(command)); };
  app.start(true, true, record);
  events.push_back("game launch");
  app.finish(record);
  EXPECT_EQ(events, (std::vector<std::string_view> {
    "steam://open/bigpicture", "game launch", "steam://close/bigpicture"
  }));
}

TEST(SteamBigPictureStream, DisabledAndUnrelatedAppsSendNeitherCommand) {
  session disabled, unrelated;
  auto unexpected = [](action) { ADD_FAILURE() << "Unexpected Big Picture command"; };
  disabled.start(false, true, unexpected);
  unrelated.start(true, false, unexpected);
  disabled.finish(unexpected);
  unrelated.finish(unexpected);
}

TEST(SteamBigPictureStream, RepeatedStartupAndTerminalCallsDoNotResendCommands) {
  session app;
  std::vector<action> events;
  auto record = [&](action command) { events.push_back(command); };
  app.start(true, true, record);

  // A resumable connection does not end the app lifetime. If startup is
  // repeated for that same app it must not send another open either.
  app.start(true, true, record);
  EXPECT_EQ(events, (std::vector {action::open}));

  app.finish(record);
  app.finish(record);
  EXPECT_EQ(events, (std::vector {action::open, action::close}));
}

TEST(SteamBigPictureStream, CommandFailuresDoNotBlockGameOrConsumeLaterLifetimes) {
  session app;
  std::vector<std::string_view> events;
  const auto failed_command = [&](action command) {
    events.push_back(uri(command));
    return false;
  };

  app.start(true, true, failed_command);
  events.push_back("game launch");
  app.finish(failed_command);
  app.finish(failed_command);
  app.start(true, true, failed_command);
  EXPECT_EQ(events, (std::vector<std::string_view> {
    "steam://open/bigpicture", "game launch", "steam://close/bigpicture", "steam://open/bigpicture"
  }));
}

TEST(SteamBigPictureStream, MovingTheAppLeavesOnePendingClose) {
  session app;
  std::vector<action> events;
  auto record = [&](action command) { events.push_back(command); };
  app.start(true, true, record);
  session replacement = std::move(app);
  app.finish(record);
  replacement.finish(record);
  EXPECT_EQ(events, (std::vector {action::open, action::close}));
}
