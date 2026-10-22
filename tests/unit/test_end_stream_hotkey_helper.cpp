#include "src/platform/windows/end_stream_hotkey_protocol.h"
#include "windows.h"

#include <gtest/gtest.h>

int sunshine_end_stream_hotkey_main(int, char **);

namespace {
  using namespace platf::hotkey::end_stream;

  int run(const char *key = "81", const char *modifiers = "7") {
    char executable[] = "helper";
    char pipe[] = "private-pipe";
    char host[] = "99";
    char *args[] = {executable, pipe, host, const_cast<char *>(key), const_cast<char *>(modifiers)};
    return sunshine_end_stream_hotkey_main(5, args);
  }

  struct Helper: testing::Test {
    void SetUp() override {
      hotkey_test_api::state = {};
    }
  };
}  // namespace

TEST_F(Helper, DisabledOrUnsafeBindingsNeverRegister) {
  EXPECT_NE(run("0"), 0);
  EXPECT_NE(run("81", "0"), 0);
  EXPECT_NE(run("27"), 0);
  EXPECT_NE(run("bogus"), 0);
  EXPECT_EQ(hotkey_test_api::state.registrations, 0);
  EXPECT_TRUE(hotkey_test_api::state.writes.empty());
}

TEST_F(Helper, NeverRegistersInSessionZeroOrInactiveSession) {
  hotkey_test_api::state.session = 0;
  hotkey_test_api::state.console = 0;
  EXPECT_NE(run(), 0);
  hotkey_test_api::state.session = 2;
  hotkey_test_api::state.console = 3;
  EXPECT_NE(run(), 0);
  EXPECT_EQ(hotkey_test_api::state.registrations, 0);
}

TEST_F(Helper, RequiresInteractiveWindowStation) {
  hotkey_test_api::state.station = L"Service-0x0-3e7$";
  EXPECT_NE(run(), 0);
  EXPECT_EQ(hotkey_test_api::state.registrations, 0);
}

TEST_F(Helper, RejectsAnUnrelatedPipeServer) {
  hotkey_test_api::state.server_pid = 100;
  EXPECT_NE(run(), 0);
  EXPECT_EQ(hotkey_test_api::state.registrations, 0);
  EXPECT_TRUE(hotkey_test_api::state.writes.empty());
}

TEST_F(Helper, ReportsConflictsWithoutPublishingReadyOrTerminate) {
  hotkey_test_api::state.registration_succeeds = false;
  EXPECT_NE(run(), 0);
  EXPECT_EQ(hotkey_test_api::state.writes, (std::vector<std::uint8_t> {static_cast<std::uint8_t>(message::registration_failed)}));
}

TEST_F(Helper, ReportsOnlyItsRegisteredHotkeyAndUnregistersAtExit) {
  auto &s = hotkey_test_api::state;
  s.messages = {{WM_HOTKEY, 2}, {WM_TIMER, 10}, {WM_HOTKEY, 1}};
  EXPECT_EQ(run(), 0);
  EXPECT_EQ(s.registrations, 1);
  EXPECT_EQ(s.key, static_cast<unsigned int>('Q'));
  EXPECT_EQ(s.modifiers, 7U | MOD_NOREPEAT);
  EXPECT_EQ(s.unregistrations, 1);
  EXPECT_EQ(s.writes, (std::vector<std::uint8_t> {static_cast<std::uint8_t>(message::ready), static_cast<std::uint8_t>(message::heartbeat), static_cast<std::uint8_t>(message::terminate)}));
}

TEST_F(Helper, DropsTheShortcutAfterConsoleOwnershipChanges) {
  auto &s = hotkey_test_api::state;
  s.messages = {{WM_HOTKEY, 1}};
  s.switch_console_on_message = true;
  EXPECT_EQ(run(), 0);
  EXPECT_EQ(s.unregistrations, 1);
  EXPECT_EQ(s.writes, (std::vector<std::uint8_t> {static_cast<std::uint8_t>(message::ready)}));
}
