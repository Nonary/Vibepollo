#include "../tests_common.h"

#include "src/command_policy.h"

namespace {
  class FakeRunner: public command_policy::runner_t {
  public:
    command_policy::result_t next {0, "ok"};
    std::string command;
    std::string directory;
    command_policy::result_t run(std::string_view value, std::string_view cwd) override {
      command = value;
      directory = cwd;
      return next;
    }
  };
}

TEST(ExternalCommandPolicy, SuccessfulCommandUsesInjectedRunnerAndWorkingDirectory) {
  FakeRunner runner;
  EXPECT_TRUE(command_policy::has_expected_outcome(runner, "verify rules", "fixture-root", true));
  EXPECT_EQ(runner.command, "verify rules");
  EXPECT_EQ(runner.directory, "fixture-root");
}

TEST(ExternalCommandPolicy, FailedCommandSatisfiesNegativeExpectation) {
  FakeRunner runner;
  runner.next = {127, "not found"};
  EXPECT_TRUE(command_policy::has_expected_outcome(runner, "missing-command", {}, false));
}

TEST(ExternalCommandPolicy, UnexpectedExitStatusFailsContract) {
  FakeRunner runner;
  runner.next = {1, "failed"};
  EXPECT_FALSE(command_policy::has_expected_outcome(runner, "verify rules", {}, true));
}
