/**
 * @file tests/unit/test_log_export_sanitizer.cpp
 * @brief Preserve support-export redaction behavior when changing regex engines.
 */
#include "src/log_export.h"

#include <gtest/gtest.h>

namespace {
  struct redaction_case_t {
    const char *name;
    std::string input;
    std::string expected;
  };

  class LogExportSanitizerTest: public testing::TestWithParam<redaction_case_t> {};

  TEST_P(LogExportSanitizerTest, PreservesExistingRedaction) {
    log_export::export_log_sanitizer_t sanitizer;
    EXPECT_EQ(sanitizer.sanitize(GetParam().input), GetParam().expected);
  }

  // Expected text was captured from the previous std::regex implementation.
  // Check IDs as well as boundaries: overlapping candidates must keep their
  // existing precedence and repeated values must share a placeholder.
  INSTANTIATE_TEST_SUITE_P(Compatibility, LogExportSanitizerTest, testing::Values(
    redaction_case_t {"WindowsPaths", R"(C:\Users\Alice Smith\game.log C:/uSeRs/Bob/game.log)", "C:\\Users\\<USER_1>\\game.log C:/uSeRs/<USER_2>/game.log"},
    redaction_case_t {"PosixPathsAndBoundaries", R"(/home/alice/game /Users/Bob/game _/home/keep/game a/home/keep/game)", "/home/<USER_1>/game /Users/<USER_2>/game _/home/keep/game a/home/keep/game"},
    redaction_case_t {"UserFields", "username=alice user_name:'Bob Smith' USER = \"carol\" notuser=dave", "username=<USER_3> user_name:'<USER_1>' USER = \"<USER_2>\" notuser=dave"},
    redaction_case_t {"ServiceIdentities", "user=SYSTEM username=LocalSystem user=root user=daemon user=unknown user=nobody user=NetworkService user=LocalService user=www-data", "user=SYSTEM username=LocalSystem user=root user=daemon user=unknown user=nobody user=NetworkService user=LocalService user=www-data"},
    redaction_case_t {"QuotedServiceIdentities", "user='Local System' user=alice user='root'", "user='Local System' user=<USER_1> user='root'"},
    redaction_case_t {"DelimitedIPv4", "192.168.1.2,10.0.0.1;8.8.8.8 255.255.255.255 0.0.0.0", "<IP_1>,<IP_2>;<IP_3> <IP_4> <IP_5>"},
    redaction_case_t {"InvalidIPv4", "256.1.2.3 01.2.3.4 x192.168.1.2 192.168.1.2x 1.2.3 1234.2.3.4", "256.1.2.3 01.2.3.4 x192.168.1.2 192.168.1.2x 1.2.3 1234.2.3.4"},
    redaction_case_t {"IPv6AndMappedIPv4", "[2001:db8::1],::1 fe80::1234%12 2001:db8:0:0:0:0:0:2 ::ffff:192.168.1.2", "[<IP_2>],<IP_3> <IP_4>%12 <IP_5> <IP_6>"},
    redaction_case_t {"TimestampsAndHexTokens", "12:34:56 2026-10-22 21:08:17.123 abcdef 1.2.3.4.5 dead:beef not::ipv6", "12:34:56 2026-10-22 21:08:17.123 abcdef <IP_1>.5 dead:beef not::ipv6"},
    redaction_case_t {"MacBoundariesAndCase", "aa:bb:cc:dd:ee:ff,01-23-45-67-89-ab AA:BB:CC:DD:EE:FF zaa:bb:cc:dd:ee:ff aa:bb:cc:dd:ee:ffz", "<MAC_1>,<MAC_2> <MAC_1> zaa:bb:cc:dd:ee:ff aa:bb:cc:dd:ee:ffz"},
    redaction_case_t {"EmailBoundariesAndCase", "first.last+tag@example.com A@EXAMPLE.COM a@example.com a@b a@-bad.com alice@example.com.", "<EMAIL_1> <EMAIL_2> <EMAIL_2> a@b a@-bad.com <EMAIL_3>."},
    redaction_case_t {"OverlappingUserAndAddressMatches", R"(user="alice@example.com" C:\Users\alice@example.com\file user=192.168.1.2)", "user=\"<USER_1>\" C:\\Users\\<USER_1>\\file user=<USER_2>"},
    redaction_case_t {"LineEndings", "a\n/home/alice/game\n192.168.1.2\r\n::1\n", "a\n/home/<USER_1>/game\n<IP_2>\r\n<IP_3>\n"},
    redaction_case_t {"MultilineFields", "user\n=\n'alice'\nuser=Bob\r\n", "user\n=\n'<USER_1>'\nuser=<USER_2>\r\n"},
    redaction_case_t {"EmbeddedNul", std::string("before\0", 7) + "user=alice 1.2.3.4 after", std::string("before\0", 7) + "user=<USER_1> <IP_2> after"}
  ), [](const testing::TestParamInfo<redaction_case_t> &info) {
    return info.param.name;
  });

  TEST(LogExportSanitizerTest, KeepsPlaceholderIdentityAcrossFiles) {
    log_export::export_log_sanitizer_t sanitizer;
    const auto first = sanitizer.sanitize("user=alice 1.2.3.4 aa:bb:cc:dd:ee:ff a@example.com");
    const auto second = sanitizer.sanitize("user=alice 1.2.3.4 AA:BB:CC:DD:EE:FF A@EXAMPLE.COM");
    EXPECT_EQ(first, "user=<USER_1> <IP_2> <MAC_3> <EMAIL_4>");
    EXPECT_EQ(second, first);
  }

  TEST(LogExportSanitizerTest, LeavesIncompleteCandidatesAndEmptyInputUnchanged) {
    log_export::export_log_sanitizer_t sanitizer;
    for (const std::string input : {"", "user=", "user='unterminated", "/home/", "C:\\Users\\", "a@", "::::", "--"}) {
      EXPECT_EQ(sanitizer.sanitize(input), input);
    }
  }
}  // namespace
