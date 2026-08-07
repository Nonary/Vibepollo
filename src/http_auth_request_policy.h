#pragma once

#include "http_auth.h"

namespace confighttp::policy {
  struct RequestAuthDependencies {
    boost::function<bool(const std::string &)> remote_allowed;
    boost::function<bool()> credentials_configured;
    boost::function<bool(const std::string &, const std::string &)> credentials_valid;
    boost::function<bool(const std::string &, const std::string &, const std::string &)> bearer_valid;
    boost::function<bool(const std::string &)> session_valid;
    boost::function<std::string(const std::string &)> decode_base64;
    boost::function<std::string(const std::string &)> cookie_unescape;
    boost::function<std::uint16_t()> https_port;
  };

  class RequestAuthPolicy {
  public:
    explicit RequestAuthPolicy(RequestAuthDependencies dependencies): _dependencies(std::move(dependencies)) {}
    AuthResult make_error(StatusCode code, const std::string &error) const;
    AuthResult check_bearer(const std::string &raw_auth, const std::string &path, const std::string &method) const;
    AuthResult check_session(const std::string &raw_auth) const;
    AuthResult check(const std::string &remote_address, const std::string &auth_header, const std::string &path, const std::string &method) const;
    bool is_html_request(const std::string &path) const;
    std::string extract_cookie(const SimpleWeb::CaseInsensitiveMultimap &headers, std::string_view name) const;

  private:
    AuthResult basic_error() const;
    AuthResult check_basic(const std::string &raw_auth) const;
    RequestAuthDependencies _dependencies;
  };
}  // namespace confighttp::policy
