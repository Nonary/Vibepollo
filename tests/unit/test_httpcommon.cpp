/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// lib imports
// standard includes
#include <string>
#include <string_view>

// local imports
#include <src/http_policy.h>

TEST(UserCredsStateTest, MalformedJsonIsNotMissingCredentials) {
  EXPECT_EQ(http::policy::inspect_credentials("{not valid json"), http::policy::creds_state::malformed);
}

TEST(UserCredsStateTest, MissingRequiredFields) {
  EXPECT_EQ(http::policy::inspect_credentials(R"({"username":"admin","password":"hash"})"), http::policy::creds_state::missing_fields);
}

TEST(UserCredsStateTest, Configured) {
  EXPECT_EQ(http::policy::inspect_credentials(R"({"username":"admin","password":"hash","salt":"salt"})"), http::policy::creds_state::configured);
}

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
    ASSERT_EQ(http::policy::percent_encode(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
    ASSERT_EQ(http::policy::host_from_url(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

namespace {
  struct FakeClient: http::policy::client_t {
    http::policy::response_t response {200, "hello!"};
    int redirect_limit = 0;
    http::policy::response_t get(std::string_view, long, int redirects) override {
      redirect_limit = redirects;
      return response;
    }
  };
  struct FakeSink: http::policy::file_sink_t {
    std::string path;
    std::string bytes;
    bool replace(std::string_view destination, std::string_view body) override {
      path = destination;
      bytes = body;
      return true;
    }
  };
}

TEST(DownloadFileTest, WritesSuccessfulResponseThroughInjectedBoundaries) {
  FakeClient client;
  FakeSink sink;
  EXPECT_TRUE(http::policy::download(client, sink, "https://example.test/data", "hello.txt", 6));
  EXPECT_EQ(client.redirect_limit, 5);
  EXPECT_EQ(sink.path, "hello.txt");
  EXPECT_EQ(sink.bytes, "hello!");
}

TEST(DownloadFileTest, RejectsFailedResponseBeforeWriting) {
  FakeClient client;
  client.response.status_code = 503;
  FakeSink sink;
  EXPECT_FALSE(http::policy::download(client, sink, "https://example.test/data", "hello.txt", 6));
  EXPECT_TRUE(sink.path.empty());
}

// Tests for cookie escaping and unescaping
struct CookieEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(CookieEscapeTest, Escape) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::policy::percent_encode(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  CookieEscapeTests,
  CookieEscapeTest,
  testing::Values(
    std::make_tuple("simpleToken123", "simpleToken123"),
    std::make_tuple("token with spaces", "token%20with%20spaces"),
    std::make_tuple("symbols&=%\"", "symbols%26%3D%25%22")
  )
);

struct CookieUnescapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(CookieUnescapeTest, Unescape) {
  const auto &[expected, input] = GetParam();
  ASSERT_EQ(http::policy::percent_decode(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  CookieUnescapeTests,
  CookieUnescapeTest,
  testing::Values(
    std::make_tuple("simpleToken123", "simpleToken123"),
    std::make_tuple("token with spaces", "token%20with%20spaces"),
    std::make_tuple("symbols&=%\"", "symbols%26%3D%25%22")
  )
);
