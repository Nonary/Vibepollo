/**
 * @file tests/unit/test_https_listener_diagnostics.cpp
 * @brief Exercise the production diagnostic callback on real HTTPS connections.
 */
#include "src/http_auth_request_policy.h"
#include "src/https_listener_diagnostics.h"

#include <atomic>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using Server = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using Clock = confighttp::HttpsListenerDiagnostics::Clock;

  struct Credentials {
    std::filesystem::path directory;

    Credentials() {
      static std::atomic_uint sequence {0};
      directory = std::filesystem::temp_directory_path() /
                  ("https-diagnostics-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
      if (!std::filesystem::create_directory(directory)) {
        throw std::runtime_error("Could not create isolated TLS test directory");
      }
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
      try {
        const auto require = [](bool ok) {
          if (!ok) {
            throw std::runtime_error("Could not generate TLS test credentials");
          }
        };
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        require(context && EVP_PKEY_keygen_init(context.get()) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
        EVP_PKEY *raw_key = nullptr;
        require(EVP_PKEY_keygen(context.get(), &raw_key) == 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
        require(certificate && X509_set_version(certificate.get(), 2) == 1);
        require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
        require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) && X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600));
        require(X509_set_pubkey(certificate.get(), key.get()) == 1);
        const auto subject = X509_get_subject_name(certificate.get());
        require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("temporary-local-test"), -1, -1, 0) == 1);
        require(X509_set_issuer_name(certificate.get(), subject) == 1 && X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
        std::unique_ptr<BIO, decltype(&BIO_free)> cert_file(BIO_new_file((directory / "cert.pem").string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_file(BIO_new_file((directory / "key.pem").string().c_str(), "w"), BIO_free);
        require(cert_file && key_file && PEM_write_bio_X509(cert_file.get(), certificate.get()) == 1 && PEM_write_bio_PrivateKey(key_file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
      } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
      }
    }

    ~Credentials() {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  };

  class LiveListener {
  public:
    explicit LiveListener(bool diagnostics, bool throwing_sink = false):
        server((credentials.directory / "cert.pem").string(), (credentials.directory / "key.pem").string()) {
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.timeout_request = 2;
      server.config.timeout_content = 2;
      confighttp::policy::RequestAuthDependencies dependencies;
      dependencies.remote_allowed = [](const std::string &) {
        return false;
      };
      dependencies.https_port = [] {
        return std::uint16_t {47990};
      };
      policy = std::make_unique<confighttp::policy::RequestAuthPolicy>(std::move(dependencies));
      server.default_resource["GET"] = [this](auto response, auto request) {
        ++handler_calls;
        // Exercise the actual policy: /welcome is public; other paths receive
        // its real HTTP 403 result. No authentication algorithm is rewritten.
        const auto result = policy->check("127.0.0.1", "", request->path, request->method);
        response->write(result.ok ? SimpleWeb::StatusCode::success_ok : result.code, result.ok ? "ok" : result.body, result.headers);
      };
      server.resource["^/throwing-route-private$"]["GET"] = [this](auto, auto) {
        ++handler_calls;
        throw std::runtime_error("handler-exception-secret");
      };
      if (diagnostics) {
        confighttp::configure_https_listener_diagnostics(server, [this, throwing_sink](std::string_view message) {
          if (throwing_sink) {
            throw std::runtime_error("test logging failure");
          }
          std::lock_guard lock(messages_mutex);
          messages.emplace_back(message);
        });
        auto production_callback = server.on_error;
        server.on_error = [this, production_callback](auto request, const auto &ec) {
          ++callback_calls;
          if (ec == boost::system::errc::make_error_code(boost::system::errc::operation_canceled)) {
            ++generic_cancellation_calls;
          }
          production_callback(request, ec);
        };
      }
      auto ready = port_promise.get_future();
      worker = std::thread([this] {
        try {
          server.start([this](unsigned short value) {
            port_promise.set_value(value);
          });
        } catch (...) {
          try {
            port_promise.set_exception(std::current_exception());
          } catch (...) {}
        }
      });
      if (ready.wait_for(5s) != std::future_status::ready) {
        server.stop();
        worker.join();
        throw std::runtime_error("TLS test listener did not start");
      }
      try {
        port = ready.get();
      } catch (...) {
        worker.join();
        throw;
      }
    }

    ~LiveListener() {
      server.stop();
      worker.join();
    }

    std::string tls_request(const std::string &request) {
      boost::asio::io_context io;
      boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
      context.load_verify_file((credentials.directory / "cert.pem").string());
      context.set_verify_mode(boost::asio::ssl::verify_peer);
      SimpleWeb::HTTPS socket(io, context);
      socket.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port});
      socket.handshake(boost::asio::ssl::stream_base::client);
      boost::asio::write(socket, boost::asio::buffer(request));
      boost::asio::streambuf response;
      boost::system::error_code ec;
      boost::asio::read(socket, response, ec);
      EXPECT_TRUE(ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated) << ec.message();
      return {boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    }

    std::string raw_request(const std::string &request) {
      boost::asio::io_context io;
      boost::asio::ip::tcp::socket socket(io);
      socket.connect({boost::asio::ip::make_address("127.0.0.1"), port});
      boost::asio::write(socket, boost::asio::buffer(request));
      boost::asio::streambuf response;
      boost::system::error_code ec;
      boost::asio::read(socket, response, ec);
      EXPECT_TRUE(ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) << ec.message();
      return {boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data())};
    }

    std::string plain_request() {
      return raw_request("GET /private-path-secret?token=query-secret HTTP/1.1\r\nHost: private-host-secret\r\nAuthorization: Bearer token-secret\r\nCookie: session=cookie-secret\r\nConnection: close\r\n\r\n");
    }

    std::vector<std::string> diagnostics() {
      std::lock_guard lock(messages_mutex);
      return messages;
    }

    std::atomic_uint handler_calls {0};
    std::atomic_uint callback_calls {0};
    std::atomic_uint generic_cancellation_calls {0};

  private:
    Credentials credentials;
    Server server;
    std::unique_ptr<confighttp::policy::RequestAuthPolicy> policy;
    std::mutex messages_mutex;
    std::vector<std::string> messages;
    std::promise<unsigned short> port_promise;
    std::thread worker;
    unsigned short port {};
  };

  constexpr auto welcome = "GET /welcome HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  constexpr auto denied = "GET /api/private?token=query-secret HTTP/1.1\r\nHost: private-host-secret\r\nAuthorization: Bearer token-secret\r\nCookie: session=cookie-secret\r\nConnection: close\r\n\r\n";

  TEST(HttpsListenerDiagnostics, BeforeAndAfterPreserveResponsesButDiagnosePlainHttp) {
    for (bool enabled : {false, true}) {
      SCOPED_TRACE(enabled);
      LiveListener listener(enabled);
      const auto welcome_response = listener.tls_request(welcome);
      const auto denied_response = listener.tls_request(denied);
      EXPECT_TRUE(welcome_response.starts_with("HTTP/1.1 200"));
      EXPECT_TRUE(denied_response.starts_with("HTTP/1.1 403"));
      EXPECT_EQ(listener.handler_calls, 2u);
      EXPECT_TRUE(listener.diagnostics().empty());
      const auto plain_response = listener.plain_request();
      EXPECT_TRUE(plain_response.empty());
      EXPECT_EQ(listener.handler_calls, 2u);
      const auto messages = listener.diagnostics();
      const std::string prefix = enabled ? "after_" : "before_";
      RecordProperty(prefix + "https_status", welcome_response.substr(0, welcome_response.find("\r\n")));
      RecordProperty(prefix + "policy_status", denied_response.substr(0, denied_response.find("\r\n")));
      RecordProperty(prefix + "plain_http_response_bytes", std::to_string(plain_response.size()));
      RecordProperty(prefix + "diagnostic_count", std::to_string(messages.size()));
      if (!messages.empty()) {
        RecordProperty(prefix + "diagnostic", messages.front());
      }
      if (enabled) {
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_NE(messages[0].find("plain HTTP received on the TLS port"), std::string::npos);
        EXPECT_NE(messages[0].find("https://"), std::string::npos);
        EXPECT_GE(listener.callback_calls, 1u);
      } else {
        EXPECT_TRUE(messages.empty());
      }
      EXPECT_TRUE(listener.tls_request(welcome).starts_with("HTTP/1.1 200"));
    }
  }

  TEST(HttpsListenerDiagnostics, RealProtocolFailuresDoNotDiscloseRequestDataAndAreBounded) {
    LiveListener listener(true);
    for (unsigned n = 0; n < 12; ++n) {
      EXPECT_TRUE(listener.plain_request().empty());
    }
    const auto messages = listener.diagnostics();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_GE(listener.callback_calls, 12u);
    EXPECT_EQ(listener.handler_calls, 0u);
    EXPECT_EQ(messages[0].find("secret"), std::string::npos);
    EXPECT_EQ(messages[0].find("127.0.0.1"), std::string::npos);
    EXPECT_TRUE(listener.tls_request(denied).starts_with("HTTP/1.1 403"));
    EXPECT_EQ(listener.diagnostics().size(), 1u);
  }

  TEST(HttpsListenerDiagnostics, MalformedHttpAfterSuccessfulTlsIsSeparateFromPolicyRejection) {
    LiveListener listener(true);
    EXPECT_TRUE(listener.tls_request("malformed-secret\r\n\r\n").empty());
    const auto messages = listener.diagnostics();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_NE(messages[0].find("HTTP protocol"), std::string::npos);
    EXPECT_EQ(messages[0].find("TLS handshake"), std::string::npos);
    EXPECT_EQ(messages[0].find("secret"), std::string::npos);
    EXPECT_EQ(listener.handler_calls, 0u);
  }

  TEST(HttpsListenerDiagnostics, InvalidTlsProtocolUsesTheActualErrorCallbackWithoutRequestData) {
    LiveListener listener(true);
    EXPECT_TRUE(listener.raw_request("not-tls-protocol-secret\r\n\r\n").empty());
    const auto messages = listener.diagnostics();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_NE(messages[0].find("TLS handshake or protocol error"), std::string::npos);
    EXPECT_EQ(messages[0].find("secret"), std::string::npos);
    EXPECT_EQ(listener.handler_calls, 0u);
    EXPECT_GE(listener.callback_calls, 1u);
    EXPECT_TRUE(listener.tls_request(welcome).starts_with("HTTP/1.1 200"));
  }

  TEST(HttpsListenerDiagnostics, LoggingFailureDoesNotStopTheActualListener) {
    LiveListener listener(true, true);
    EXPECT_TRUE(listener.plain_request().empty());
    EXPECT_TRUE(listener.tls_request(welcome).starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(listener.tls_request(denied).starts_with("HTTP/1.1 403"));
  }

  TEST(HttpsListenerDiagnostics, ThrowingRouteUsesAccurateFixedTextWithoutChangingResponseBehavior) {
    const std::string request = "GET /throwing-route-private?token=query-secret HTTP/1.1\r\nHost: private-host-secret\r\nAuthorization: Bearer token-secret\r\nCookie: session=cookie-secret\r\nConnection: close\r\n\r\n";
    for (bool enabled : {false, true}) {
      SCOPED_TRACE(enabled);
      LiveListener listener(enabled);
      const auto response = listener.tls_request(request);
      EXPECT_TRUE(response.empty());
      EXPECT_EQ(listener.handler_calls, 1u);
      const auto messages = listener.diagnostics();
      const std::string prefix = enabled ? "after_" : "before_";
      RecordProperty(prefix + "throwing_route_response_bytes", std::to_string(response.size()));
      RecordProperty(prefix + "throwing_route_diagnostic_count", std::to_string(messages.size()));
      if (enabled) {
        EXPECT_EQ(listener.generic_cancellation_calls, 1u);
        ASSERT_EQ(messages.size(), 1u);
        RecordProperty("after_throwing_route_diagnostic", messages[0]);
        EXPECT_EQ(messages[0], "Configuration HTTPS listener: connection, HTTP protocol, or request handling error; this is separate from an HTTP access-policy rejection.");
        EXPECT_EQ(messages[0].find("secret"), std::string::npos);
        EXPECT_EQ(messages[0].find("throwing-route-private"), std::string::npos);
        EXPECT_EQ(messages[0].find("127.0.0.1"), std::string::npos);
      } else {
        EXPECT_TRUE(messages.empty());
      }
      EXPECT_TRUE(listener.tls_request(welcome).starts_with("HTTP/1.1 200"));
      EXPECT_TRUE(listener.tls_request(denied).starts_with("HTTP/1.1 403"));
    }
  }

  TEST(HttpsListenerDiagnostics, NormalDisconnectsAreQuietAndTheGlobalLimitExpires) {
    std::vector<std::string> messages;
    confighttp::HttpsListenerDiagnostics reporter([&messages](std::string_view message) {
      messages.emplace_back(message);
    });
    const auto start = Clock::time_point {};
    for (const SimpleWeb::error_code &ec : std::vector<SimpleWeb::error_code> {
           {},
           boost::asio::error::operation_aborted,
           boost::asio::error::eof,
           boost::asio::ssl::error::stream_truncated
         }) {
      reporter.report(ec, start);
    }
    EXPECT_TRUE(messages.empty());
    reporter.report(boost::asio::error::connection_reset, start);
    reporter.report(boost::system::errc::make_error_code(boost::system::errc::protocol_error), start + 29s);
    EXPECT_EQ(messages.size(), 1u);
    reporter.report(boost::system::errc::make_error_code(boost::system::errc::protocol_error), start + 30s);
    EXPECT_EQ(messages.size(), 2u);
  }
}  // namespace
