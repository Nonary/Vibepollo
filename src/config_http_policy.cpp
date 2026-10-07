#include "config_http_policy.h"

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/system/error_code.hpp>
#include <stdexcept>

namespace confighttp::policy {
  bool is_token_route_eligible(std::string_view path) {
    return path.rfind("/api/", 0) == 0 && path.rfind("/api/auth/", 0) != 0;
  }

  std::vector<std::string> ordered_methods_for_catalog(const std::set<std::string, std::less<>> &methods) {
    static constexpr std::array<std::string_view, 5> preferred_order = {
      "GET", "POST", "PUT", "PATCH", "DELETE"
    };

    std::vector<std::string> ordered;
    ordered.reserve(methods.size());
    for (const auto method : preferred_order) {
      if (methods.contains(std::string(method))) {
        ordered.emplace_back(method);
      }
    }
    for (const auto &method : methods) {
      if (std::find(preferred_order.begin(), preferred_order.end(), method) == preferred_order.end()) {
        ordered.push_back(method);
      }
    }
    return ordered;
  }

  std::string get_web_ui_host_for_local_open(std::string_view bind_address, bool prefer_ipv4_loopback) {
    const auto first = bind_address.find_first_not_of(" \t\r\n");
    const auto trimmed = first == std::string_view::npos ? std::string_view {} :
      bind_address.substr(first, bind_address.find_last_not_of(" \t\r\n") - first + 1);
    if (trimmed.empty()) {
      return prefer_ipv4_loopback ? "127.0.0.1" : "localhost";
    }

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(std::string(trimmed), ec);
    if (ec) {
      return std::string(trimmed);
    }
    if (address.is_unspecified()) {
      return address.is_v4() ? "127.0.0.1" : "localhost";
    }
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
      return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6()).to_string();
    }
    auto host = address.to_string();
    if (address.is_v6()) {
      if (const auto scope_separator = host.find('%'); scope_separator != std::string::npos) {
        host.replace(scope_separator, 1, "%25");
      }
      return "[" + host + "]";
    }
    return host;
  }

  std::string make_web_ui_url(std::string_view host, std::uint16_t port, std::string_view path) {
    return "https://" + std::string(host) + ":" + std::to_string(port) + std::string(path);
  }

  TokenScope scope_from_string(std::string_view scope) {
    if (scope == "Read" || scope == "read") {
      return TokenScope::Read;
    }
    if (scope == "Write" || scope == "write") {
      return TokenScope::Write;
    }
    throw std::invalid_argument("Unknown TokenScope: " + std::string(scope));
  }

  std::string scope_to_string(TokenScope scope) {
    switch (scope) {
      case TokenScope::Read:
        return "Read";
      case TokenScope::Write:
        return "Write";
    }
    throw std::invalid_argument("Unknown TokenScope enum value");
  }
}  // namespace confighttp::policy
