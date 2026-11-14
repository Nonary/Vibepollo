#pragma once

#include <string>
#include <string_view>

#include <boost/algorithm/string.hpp>

namespace confighttp::policy {
  inline bool is_sensitive_http_field(std::string_view name) {
    constexpr std::string_view exact_names[] = {
      "authorization",
      "proxy-authorization",
      "cookie",
      "set-cookie",
      "x-csrf-token",
      "x-api-key",
    };
    for (const auto candidate : exact_names) {
      if (boost::iequals(name, candidate)) {
        return true;
      }
    }

    std::string lowered(name);
    boost::algorithm::to_lower(lowered);
    return lowered.contains("password") || lowered.contains("secret") ||
           lowered.contains("credential") || lowered.contains("token") ||
           lowered.contains("api_key") || lowered.contains("apikey");
  }
}  // namespace confighttp::policy
