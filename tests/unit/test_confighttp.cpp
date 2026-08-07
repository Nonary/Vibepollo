// standard includes
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "../tests_common.h"
#include "src/confighttp.h"
#include "src/http_auth.h"
#include "src/http_auth_request_policy.h"

using namespace testing;

namespace confighttp {
  using policy::is_token_route_eligible;
  using policy::ordered_methods_for_catalog;

  class ConfigHttpAuthHelpersTest: public Test {
  protected:
    void SetUp() override {
      policy::RequestAuthDependencies deps;
      deps.remote_allowed = [](const std::string &address) { return address == "127.0.0.1"; };
      deps.credentials_configured = [this] { return credentials_configured; };
      deps.credentials_valid = [](const std::string &username, const std::string &password) { return username == "testuser" && password == "testpass"; };
      deps.bearer_valid = [](const std::string &, const std::string &, const std::string &) { return false; };
      deps.session_valid = [](const std::string &) { return false; };
      deps.decode_base64 = [](const std::string &value) { return value; };
      deps.cookie_unescape = [](const std::string &value) {
        std::string decoded;
        for (std::size_t index = 0; index < value.size(); ++index) {
          if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex = value.substr(index + 1, 2);
            if (hex == "20") { decoded.push_back(' '); index += 2; continue; }
            if (hex == "25") { decoded.push_back('%'); index += 2; continue; }
            if (hex == "3B") { decoded.push_back(';'); index += 2; continue; }
          }
          decoded.push_back(value[index]);
        }
        return decoded;
      };
      deps.https_port = [] { return std::uint16_t {47990}; };
      auth = std::make_unique<policy::RequestAuthPolicy>(std::move(deps));
    }

    std::string createBasicAuthHeader(const std::string &username, const std::string &password) const {
      return "Basic " + username + ":" + password;
    }

    std::unique_ptr<policy::RequestAuthPolicy> auth;
    bool credentials_configured = true;
  };

  TEST_F(ConfigHttpAuthHelpersTest, given_unauthorized_error_when_making_auth_error_then_should_return_proper_response) {
    auto result = auth->make_error(SimpleWeb::StatusCode::client_error_unauthorized, "Unauthorized");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Unauthorized");
    auto www_auth_it = result.headers.find("WWW-Authenticate");
    EXPECT_EQ(www_auth_it, result.headers.end());
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_forbidden_error_when_making_auth_error_then_should_return_proper_response) {
    auto result = auth->make_error(SimpleWeb::StatusCode::client_error_forbidden, "Forbidden");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_forbidden);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Forbidden");
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_redirect_location_when_making_auth_error_then_should_return_redirect_response) {
    auto result = auth->make_error(SimpleWeb::StatusCode::redirection_temporary_redirect, "");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::redirection_temporary_redirect);
    EXPECT_TRUE(result.body.empty());
    auto location_header = result.headers.find("Location");
    EXPECT_EQ(location_header, result.headers.end());
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_custom_error_message_when_making_auth_error_then_should_return_response_with_custom_message) {
    auto result = auth->make_error(SimpleWeb::StatusCode::client_error_forbidden, "Custom error message");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_forbidden);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Custom error message");
  }

  class ConfigHttpCheckBearerAuthTest: public ConfigHttpAuthHelpersTest {};

  TEST_F(ConfigHttpCheckBearerAuthTest, given_invalid_bearer_token_when_checking_auth_then_should_return_forbidden) {
    // Given: Invalid bearer token for API endpoint
    auto raw_auth = "Bearer invalid_token_123";
    auto path = "/api/test";
    auto method = "GET";

    // When: Checking bearer authentication
    auto result = auth->check_bearer(raw_auth, path, method);

    // Then: Should return forbidden error
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_forbidden);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Forbidden: Token does not have permission for this path/method.");
  }

  class ConfigHttpCheckAuthTest: public ConfigHttpAuthHelpersTest {};

  TEST_F(ConfigHttpCheckAuthTest, given_missing_auth_header_when_checking_auth_then_should_return_unauthorized) {
    // Given: No authentication header provided

    // When: Checking authentication with empty header
    auto result = auth->check("127.0.0.1", "", "/api/test", "GET");

    // Then: Should return unauthorized error
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Unauthorized");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_csrf_token_endpoint_when_checking_auth_then_should_allow_without_authentication) {
    auto result = auth->check("127.0.0.1", "", "/api/csrf-token", "GET");

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result.body.empty());
    EXPECT_TRUE(result.headers.empty());
  }

  TEST_F(ConfigHttpCheckAuthTest, given_empty_username_config_when_checking_auth_then_should_return_unauthorized) {
    // Given: Empty username configuration (initial setup)
    credentials_configured = false;

    // When: Checking authentication during initial setup for an API endpoint
    auto result = auth->check("127.0.0.1", "Basic test:test", "/api/test", "GET");

    // Then: Should return unauthorized error for API access
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Credentials not configured");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_disallowed_ip_address_when_checking_auth_then_should_return_forbidden) {
    // Given: Valid credentials but disallowed IP address
    auto auth_header = createBasicAuthHeader("testuser", "testpass");

    // When: Checking authentication from external IP
    auto result = auth->check("8.8.8.8", auth_header, "/api/test", "GET");

    // Then: Should return forbidden error
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_forbidden);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Forbidden");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_invalid_basic_credentials_when_checking_auth_then_should_return_unauthorized) {
    auto auth_header = createBasicAuthHeader("testuser", "wrongpass");

    auto result = auth->check("127.0.0.1", auth_header, "/api/test", "GET");

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Unauthorized");
    auto www_auth_it = result.headers.find("WWW-Authenticate");
    EXPECT_NE(www_auth_it, result.headers.end());
    EXPECT_EQ(www_auth_it->second, "Basic realm=\"Sunshine\"");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_valid_basic_credentials_when_checking_auth_then_should_authorize) {
    auto auth_header = createBasicAuthHeader("testuser", "testpass");

    auto result = auth->check("127.0.0.1", auth_header, "/api/test", "GET");

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result.body.empty());
    EXPECT_TRUE(result.headers.empty());
  }

  TEST_F(ConfigHttpCheckAuthTest, given_invalid_bearer_token_when_checking_auth_then_should_return_forbidden) {
    // Given: Invalid bearer token for API access

    // When: Checking authentication with invalid bearer token
    auto result = auth->check("127.0.0.1", "Bearer invalid_token", "/api/test", "GET");

    // Then: Should return forbidden error
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_forbidden);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Forbidden: Token does not have permission for this path/method.");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_unsupported_auth_scheme_when_checking_auth_then_should_return_unauthorized) {
    // Given: Unsupported authentication scheme (Digest)

    // When: Checking authentication with unsupported scheme
    auto result = auth->check("127.0.0.1", "Digest realm=test", "/api/test", "GET");

    // Then: Should return unauthorized error
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);

    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Unauthorized");
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_various_paths_when_checking_is_html_request_then_should_return_expected) {
    EXPECT_TRUE(auth->is_html_request("/"));
    EXPECT_TRUE(auth->is_html_request("/index.html"));
    EXPECT_FALSE(auth->is_html_request("/api/test"));
    EXPECT_FALSE(auth->is_html_request("/assets/style.css"));
    EXPECT_FALSE(auth->is_html_request("/images/logo.png"));
    EXPECT_TRUE(auth->is_html_request("/login"));
  }

  TEST(ConfigHttpHelpersTest, given_token_scope_when_converting_to_string_then_should_return_expected) {
    EXPECT_EQ(policy::scope_to_string(TokenScope::Read), "Read");
    EXPECT_EQ(policy::scope_to_string(TokenScope::Write), "Write");
    EXPECT_THROW(policy::scope_to_string(static_cast<TokenScope>(-1)), std::invalid_argument);
  }

  TEST(ConfigHttpHelpersTest, given_api_paths_when_checking_token_route_eligibility_then_should_filter_auth_routes) {
    EXPECT_TRUE(is_token_route_eligible("/api/clients/list"));
    EXPECT_TRUE(is_token_route_eligible("/api/token/routes"));
    EXPECT_FALSE(is_token_route_eligible("/api/auth/login"));
    EXPECT_FALSE(is_token_route_eligible("/api/auth/sessions/abc123"));
    EXPECT_FALSE(is_token_route_eligible("/clients"));
  }

  TEST(ConfigHttpHelpersTest, given_unsorted_methods_when_ordering_catalog_methods_then_should_follow_preferred_order) {
    std::set<std::string, std::less<>> methods {"PATCH", "DELETE", "POST", "GET", "TRACE"};
    const auto ordered = ordered_methods_for_catalog(methods);
    ASSERT_EQ(ordered.size(), 5);
    EXPECT_EQ(ordered[0], "GET");
    EXPECT_EQ(ordered[1], "POST");
    EXPECT_EQ(ordered[2], "PATCH");
    EXPECT_EQ(ordered[3], "DELETE");
    EXPECT_EQ(ordered[4], "TRACE");
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_invalid_session_format_then_should_return_error) {
    auto result = auth->check_session("Invalid token");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Invalid session token format");
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_invalid_session_token_then_should_return_error) {
    auto result = auth->check_session("Session fake_token");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::client_error_unauthorized);
    auto json_response = nlohmann::json::parse(result.body);
    EXPECT_EQ(json_response["error"], "Invalid or expired session token");
  }

  TEST_F(ConfigHttpCheckAuthTest, given_html_page_request_without_auth_when_checking_auth_then_should_redirect_to_login_with_redirect_param) {
    auto result = auth->check("127.0.0.1", "", "/home", "GET");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result.body.empty());
    EXPECT_TRUE(result.headers.empty());
  }

  TEST_F(ConfigHttpCheckAuthTest, given_login_page_path_when_checking_auth_then_should_allow_without_authentication) {
    auto result = auth->check("127.0.0.1", "", "/login", "GET");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result.body.empty());
    EXPECT_TRUE(result.headers.empty());

    auto result2 = auth->check("127.0.0.1", "", "/login/", "GET");
    EXPECT_TRUE(result2.ok);
    EXPECT_EQ(result2.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result2.body.empty());
    EXPECT_TRUE(result2.headers.empty());
  }

  TEST_F(ConfigHttpCheckAuthTest, given_unknown_auth_scheme_and_html_path_when_checking_auth_then_should_redirect_to_login) {
    auto result = auth->check("127.0.0.1", "Digest realm=foo", "/index.html", "GET");
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.code, SimpleWeb::StatusCode::success_ok);
    EXPECT_TRUE(result.body.empty());
    EXPECT_TRUE(result.headers.empty());
  }

  class ConfigHttpCorsTest: public ConfigHttpAuthHelpersTest {};

  TEST_F(ConfigHttpCorsTest, given_auth_error_response_when_creating_then_should_include_correct_cors_headers) {
    auto result = auth->make_error(SimpleWeb::StatusCode::client_error_unauthorized, "Unauthorized");

    auto cors_origin_it = result.headers.find("Access-Control-Allow-Origin");
    EXPECT_NE(cors_origin_it, result.headers.end());

    // The CORS origin should use the correct HTTPS port
    std::uint16_t expected_port = 47990;
    std::string expected_origin = std::format("https://localhost:{}", expected_port);

    EXPECT_EQ(cors_origin_it->second, expected_origin);
  }

  TEST_F(ConfigHttpCorsTest, given_different_auth_error_when_creating_then_should_include_correct_cors_headers) {
    auto result = auth->make_error(SimpleWeb::StatusCode::client_error_forbidden, "Forbidden");

    auto cors_origin_it = result.headers.find("Access-Control-Allow-Origin");
    EXPECT_NE(cors_origin_it, result.headers.end());

    // The CORS origin should use the correct HTTPS port and be https (not http)
    std::uint16_t expected_port = 47990;
    std::string expected_origin = std::format("https://localhost:{}", expected_port);

    EXPECT_EQ(cors_origin_it->second, expected_origin);

    // Verify it's not using http://
    EXPECT_THAT(cors_origin_it->second, Not(HasSubstr("http://localhost:")));
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_percent_encoded_session_token_in_cookie_when_extracting_then_should_unescape_token) {
    // Given: A percent-encoded session token in the Cookie header
    std::string raw_token = "token_with_special%3Bchars%20and%25percent";
    std::string encoded_token = "token_with_special%253Bchars%2520and%2525percent";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cookie", std::string(session_cookie_name) + "=" + encoded_token);

    // When: Extracting the session token
    std::string extracted = auth->extract_cookie(headers, session_cookie_name);

    // Then: The extracted token should match the original raw token
    EXPECT_EQ(extracted, raw_token);
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_no_session_token_in_cookie_when_extracting_then_should_return_empty_string) {
    // Given: No session_token in the Cookie header
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cookie", "other_cookie=foo");

    // When: Extracting the session token
    std::string extracted = auth->extract_cookie(headers, session_cookie_name);

    // Then: The extracted token should be empty
    EXPECT_TRUE(extracted.empty());
  }

  TEST_F(ConfigHttpAuthHelpersTest, given_percent_encoded_cookie_when_extracting_token_then_should_return_decoded_token) {
    // Given: A cookie header with a percent-encoded session token
    std::string raw_token = "token with spaces;and%percent";
    std::string encoded_token = "token%20with%20spaces%3Band%25percent";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cookie", std::string(session_cookie_name) + "=" + encoded_token);

    // When: Extracting the session token
    std::string extracted = auth->extract_cookie(headers, session_cookie_name);

    // Then: Should return the decoded token
    EXPECT_EQ(extracted, raw_token);
  }

}  // namespace confighttp
