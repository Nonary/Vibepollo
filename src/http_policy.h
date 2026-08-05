#pragma once

#include <string>
#include <string_view>

namespace http::policy {
  enum class creds_state {
    missing_file,
    missing_fields,
    unreadable,
    malformed,
    configured
  };

  creds_state inspect_credentials(std::string_view json);
  std::string percent_encode(std::string_view value);
  std::string percent_decode(std::string_view value);
  std::string host_from_url(std::string_view value);

  struct response_t {
    int status_code = 0;
    std::string body;
  };

  class client_t {
  public:
    virtual ~client_t() = default;
    virtual response_t get(std::string_view url, long ssl_version, int redirect_limit) = 0;
  };

  class file_sink_t {
  public:
    virtual ~file_sink_t() = default;
    virtual bool replace(std::string_view path, std::string_view bytes) = 0;
  };

  bool download(client_t &client, file_sink_t &sink, std::string_view url, std::string_view path, long ssl_version);
}  // namespace http::policy
