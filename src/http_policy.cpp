#include "http_policy.h"

#include <cctype>
#include <sstream>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace http::policy {
  namespace {
    constexpr char hex[] = "0123456789ABCDEF";
    int from_hex(char value) {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      return -1;
    }
  }  // namespace

  creds_state inspect_credentials(std::string_view json) {
    boost::property_tree::ptree tree;
    std::stringstream stream;
    stream << json;
    try {
      boost::property_tree::read_json(stream, tree);
    } catch (const boost::property_tree::json_parser::json_parser_error &) {
      return creds_state::malformed;
    } catch (...) {
      return creds_state::unreadable;
    }
    if (tree.find("username") == tree.not_found() ||
        tree.find("password") == tree.not_found() ||
        tree.find("salt") == tree.not_found()) {
      return creds_state::missing_fields;
    }
    return creds_state::configured;
  }

  std::string percent_encode(std::string_view value) {
    std::string result;
    for (const unsigned char ch : value) {
      if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
        result.push_back(static_cast<char>(ch));
      } else {
        result.push_back('%');
        result.push_back(hex[ch >> 4]);
        result.push_back(hex[ch & 0x0f]);
      }
    }
    return result;
  }

  std::string percent_decode(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '%' && i + 2 < value.size()) {
        const int hi = from_hex(value[i + 1]);
        const int lo = from_hex(value[i + 2]);
        if (hi >= 0 && lo >= 0) {
          result.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      result.push_back(value[i]);
    }
    return result;
  }

  std::string host_from_url(std::string_view value) {
    const auto scheme = value.find("://");
    if (scheme == std::string_view::npos) return {};
    const auto begin = scheme + 3;
    if (begin >= value.size() || value[begin] == '[') return {};
    const auto end = value.find_first_of(":/", begin);
    const auto host = value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
    if (host.empty() || host.find_first_of("{}! ") != std::string_view::npos) return {};
    return std::string(host);
  }

  bool download(client_t &client, file_sink_t &sink, std::string_view url, std::string_view path, long ssl_version) {
    const auto response = client.get(url, ssl_version, 5);
    return response.status_code >= 200 && response.status_code < 300 && sink.replace(path, response.body);
  }
}  // namespace http::policy
