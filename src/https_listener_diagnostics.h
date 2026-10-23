/**
 * @file src/https_listener_diagnostics.h
 * @brief Bounded error diagnostics for the configuration HTTPS listener.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <openssl/err.h>
#include <openssl/sslerr.h>
#include <Simple-Web-Server/server_https.hpp>
#include <string_view>
#include <utility>

namespace confighttp {
  class HttpsListenerDiagnostics {
  public:
    using Clock = std::chrono::steady_clock;
    using Sink = std::function<void(std::string_view)>;

    explicit HttpsListenerDiagnostics(Sink sink):
        _sink(std::move(sink)) {}

    // The clock argument permits deterministic testing of the production rate
    // limit. All error classes share one warning per 30 seconds per listener.
    void report(const SimpleWeb::error_code &ec, Clock::time_point now = Clock::now()) noexcept {
      // Normal disconnects and listener shutdown are not useful warnings.
      if (!ec || ec == SimpleWeb::asio::error::operation_aborted || ec == SimpleWeb::asio::error::eof || ec == SimpleWeb::asio::ssl::error::stream_truncated) {
        return;
      }

      // Use fixed text only: neither request fields nor OpenSSL/system error
      // strings, certificates, peer addresses or credentials reach the sink.
      // This shared callback also receives escaping route exceptions as
      // generic operation_canceled; that code is not limited to transport.
      std::string_view message = "Configuration HTTPS listener: connection, HTTP protocol, or request handling error; this is separate from an HTTP access-policy rejection.";
      if (ec.category() == SimpleWeb::asio::error::get_ssl_category()) {
        message = ERR_GET_REASON(static_cast<unsigned long>(ec.value())) == SSL_R_HTTP_REQUEST ?
                    "Configuration HTTPS listener: plain HTTP received on the TLS port; use an https:// URL." :
                    "Configuration HTTPS listener: TLS handshake or protocol error; this is separate from an HTTP access-policy rejection.";
      }

      try {
        {
          std::lock_guard lock(_mutex);
          if (now < _next_warning) {
            return;
          }
          _next_warning = now + std::chrono::seconds(30);
        }
        _sink(message);
      } catch (...) {
        // Diagnostics must not change the server's error/connection handling.
      }
    }

  private:
    Sink _sink;
    std::mutex _mutex;
    Clock::time_point _next_warning {Clock::time_point::min()};
  };

  inline void configure_https_listener_diagnostics(SimpleWeb::Server<SimpleWeb::HTTPS> &server, HttpsListenerDiagnostics::Sink sink) {
    auto diagnostics = std::make_shared<HttpsListenerDiagnostics>(std::move(sink));
    server.on_error = [diagnostics](const auto &, const SimpleWeb::error_code &ec) noexcept {
      diagnostics->report(ec);
    };
  }
}  // namespace confighttp
