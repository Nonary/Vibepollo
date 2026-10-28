#pragma once

#include "power_action.h"

#include <future>
#include <memory>
#include <Simple-Web-Server/server_http.hpp>

namespace nvhttp::power_action {
  template<class Request>
  request_t parse_request(const Request &request) {
    const auto args = request.parse_query_string();
    const auto token = args.find("token");
    return {
      request.method,
      token == args.end() ? "" : token->second,
      args.size() == 1 && token != args.end() && request.content.size() == 0 && request.header.count("Origin") == 0,
    };
  }

  template<class Response>
  bool write_response(Response response, const response_t &value) {
    SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/xml"},
      {"Cache-Control", "no-store"},
      {"Connection", "close"},
    };
    const auto body = response_xml(value);
    // Response ownership survives the native call. Bound the body explicitly
    // so clients finish on flushed bytes without waiting for connection EOF.
    headers.emplace("Content-Length", std::to_string(body.size()));
    response->close_connection_after_response = true;
    response->write(static_cast<SimpleWeb::StatusCode>(value.status), body, headers);
    if (value.status != 202) {
      return true;
    }
    // The blocking-route worker owns its mutex throughout this wait; the
    // socket callback runs on the HTTPS IO context and only resolves a promise.
    // A timeout/failed write must never schedule a later suspend.
    auto sent = std::make_shared<std::promise<bool>>();
    auto finished = sent->get_future();
    response->send([sent](const SimpleWeb::error_code &error) {
      sent->set_value(!error);
    });
    return finished.wait_for(std::chrono::seconds(2)) == std::future_status::ready && finished.get();
  }
}  // namespace nvhttp::power_action
