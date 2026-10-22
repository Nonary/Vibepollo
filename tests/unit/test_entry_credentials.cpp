/**
 * @file tests/unit/test_entry_credentials.cpp
 * @brief Credential CLI argument routing contracts.
 */
#include "../tests_common.h"

#include <src/entry_credentials.h>
#include <string>

namespace {
  struct Calls {
    int help = 0;
    int save = 0;
    std::string program_name;
    std::string username;
    std::string password;
  };

  auto make_help(Calls &calls) {
    return [&calls](const char *name) {
      ++calls.help;
      calls.program_name = name;
      return 23;
    };
  }

  auto make_saver(Calls &calls) {
    return [&calls](const char *username, const char *password) {
      ++calls.save;
      calls.username = username;
      calls.password = password;
      return 42;
    };
  }

  TEST(EntryCredentials, MissingOperandsShowHelpWithoutSaving) {
    Calls calls;
    char one_operand[] = "alice";
    char *one_argument[] = {one_operand};
    char password[] = "test-password";
    char *null_username[] = {nullptr, password};
    char *null_password[] = {one_operand, nullptr};

    EXPECT_EQ(
      args::creds("vibeshine", 0, nullptr, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(
      args::creds("vibeshine", 1, one_argument, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(
      args::creds("vibeshine", 2, nullptr, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(
      args::creds("vibeshine", 2, null_username, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(
      args::creds("vibeshine", 2, null_password, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(calls.help, 5);
    EXPECT_EQ(calls.save, 0);
    EXPECT_EQ(calls.program_name, "vibeshine");
  }

  TEST(EntryCredentials, HelpOperandShowsHelpWithoutSaving) {
    Calls calls;
    char help[] = "help";
    char ignored[] = "ignored";
    char alice[] = "alice";
    char *help_username[] = {help, ignored};
    char *help_password[] = {alice, help};

    EXPECT_EQ(
      args::creds("vibeshine", 2, help_username, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(
      args::creds("vibeshine", 2, help_password, make_help(calls), make_saver(calls)),
      23
    );
    EXPECT_EQ(calls.help, 2);
    EXPECT_EQ(calls.save, 0);
    EXPECT_EQ(calls.program_name, "vibeshine");
  }

  TEST(EntryCredentials, ValidOperandsReachSaverUnchanged) {
    Calls calls;
    char alice[] = "alice";
    char password[] = "test-password";
    char *valid_arguments[] = {alice, password};

    EXPECT_EQ(
      args::creds("vibeshine", 2, valid_arguments, make_help(calls), make_saver(calls)),
      42
    );
    EXPECT_EQ(calls.help, 0);
    EXPECT_EQ(calls.save, 1);
    EXPECT_EQ(calls.username, "alice");
    EXPECT_EQ(calls.password, "test-password");
  }

  TEST(EntryCredentials, SaverFailureIsReturnedWithoutFallingBackToHelp) {
    Calls calls;
    char alice[] = "alice";
    char password[] = "test-password";
    char *valid_arguments[] = {alice, password};
    auto failing_saver = [&calls](const char *username, const char *saved_password) {
      ++calls.save;
      calls.username = username;
      calls.password = saved_password;
      return -1;
    };

    EXPECT_EQ(
      args::creds("vibeshine", 2, valid_arguments, make_help(calls), failing_saver),
      -1
    );
    EXPECT_EQ(calls.help, 0);
    EXPECT_EQ(calls.save, 1);
    EXPECT_EQ(calls.username, "alice");
    EXPECT_EQ(calls.password, "test-password");
  }
}  // namespace
