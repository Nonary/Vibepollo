#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <vector>

#include "src/config_http_policy.h"

namespace confighttp::policy {
  TEST(ConfigHttpPolicyTest, TokenRoutesExcludeAuthenticationEndpoints) {
    EXPECT_TRUE(is_token_route_eligible("/api/clients/list"));
    EXPECT_TRUE(is_token_route_eligible("/api/token/routes"));
    EXPECT_FALSE(is_token_route_eligible("/api/auth/login"));
    EXPECT_FALSE(is_token_route_eligible("/api/auth/sessions/abc123"));
    EXPECT_FALSE(is_token_route_eligible("/clients"));
  }

  TEST(ConfigHttpPolicyTest, CatalogMethodsUseThePreferredOrder) {
    std::set<std::string, std::less<>> methods {"PATCH", "DELETE", "POST", "GET", "TRACE"};
    EXPECT_EQ(ordered_methods_for_catalog(methods),
              (std::vector<std::string> {"GET", "POST", "PATCH", "DELETE", "TRACE"}));
  }

  TEST(ConfigHttpPolicyTest, TokenScopesRoundTripAndRejectUnknownValues) {
    EXPECT_EQ(scope_to_string(TokenScope::Read), "Read");
    EXPECT_EQ(scope_to_string(TokenScope::Write), "Write");
    EXPECT_EQ(scope_from_string("read"), TokenScope::Read);
    EXPECT_THROW(scope_from_string("admin"), std::invalid_argument);
    EXPECT_THROW(scope_to_string(static_cast<TokenScope>(-1)), std::invalid_argument);
  }

  TEST(ConfigHttpPolicyTest, UsesLocalhostForDualStackWildcard) {
    EXPECT_EQ(get_web_ui_host_for_local_open({}, false), "localhost");
    EXPECT_EQ(make_web_ui_url(get_web_ui_host_for_local_open({}, false), 47990),
              "https://localhost:47990");
  }

  TEST(ConfigHttpPolicyTest, UsesIpv4LoopbackForIpv4Wildcard) {
    EXPECT_EQ(get_web_ui_host_for_local_open({}, true), "127.0.0.1");
    EXPECT_EQ(get_web_ui_host_for_local_open("0.0.0.0", false), "127.0.0.1");
    EXPECT_EQ(make_web_ui_url(get_web_ui_host_for_local_open("0.0.0.0", false), 47990, "/login"),
              "https://127.0.0.1:47990/login");
  }

  TEST(ConfigHttpPolicyTest, UsesConfiguredIpv4AndIpv6Hosts) {
    EXPECT_EQ(get_web_ui_host_for_local_open("192.168.1.154", false), "192.168.1.154");
    EXPECT_EQ(get_web_ui_host_for_local_open("2001:db8::154", false), "[2001:db8::154]");
    EXPECT_EQ(make_web_ui_url("[2001:db8::154]", 47990, "/login"),
              "https://[2001:db8::154]:47990/login");
  }
}  // namespace confighttp::policy
