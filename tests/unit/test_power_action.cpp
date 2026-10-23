/** Links production authorization, power admission, XML and HTTP adapters.
 * Only socket writes and the actual OS suspend syscall are replaced. */
#include <array>
#include <boost/property_tree/xml_parser.hpp>
#include <future>
#include <gtest/gtest.h>
#include <src/power_action_http.h>
#include <src/tls_client_identity.h>
#include <sstream>
#include <thread>

namespace {
  namespace power = nvhttp::power_action;
  namespace tls = nvhttp::tls_identity;
  using namespace std::chrono_literals;
  constexpr auto first_uuid = "715e52ea-1111-4444-8888-000000000001";
  constexpr auto second_uuid = "715e52ea-2222-4444-8888-000000000002";

  const crypto::creds_t &credentials() {
    static const auto value = crypto::gen_creds("Power test", 2048);
    return value;
  }

  // Real request and TLS connection context; no listening socket is needed
  // to exercise certificate binding and live paired-registry revocation.
  class transport_t: public SimpleWeb::ServerBase<nvhttp::SunshineHTTPS> {
  public:
    transport_t():
        ServerBase(0),
        connection(create_connection(io, context)),
        session(std::make_shared<Session>(1024, connection)) {}

    tls::request_t request() {
      return session->request;
    }

  private:
    void accept() override {}

    boost::asio::io_context io;
    boost::asio::ssl::context context {boost::asio::ssl::context::tls_server};
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Session> session;
  };

  struct response_t {
    bool close_connection_after_response {false};
    SimpleWeb::StatusCode status {};
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string body;
    bool flushed {false};
    std::function<void(std::function<void(const SimpleWeb::error_code &)>)> flush;

    void write(SimpleWeb::StatusCode code, const std::string &xml, const SimpleWeb::CaseInsensitiveMultimap &values) {
      status = code;
      body = xml;
      headers = values;
    }

    void send(std::function<void(const SimpleWeb::error_code &)> done) {
      if (flush) {
        flush(std::move(done));
      } else {
        flushed = true;
        done({});
      }
    }

    boost::property_tree::ptree xml() const {
      std::istringstream input(body);
      boost::property_tree::ptree tree;
      boost::property_tree::read_xml(input, tree);
      return tree;
    }
  };

  class PowerAction: public testing::Test {
  protected:
    void SetUp() override {
      clients.push_back({first_uuid, "Power test", credentials().x509, true, true});
      auto native_certificate = std::make_shared<crypto::named_cert_t>();
      native_certificate->cert = credentials().x509;
      chain.add(native_certificate);
      auto certificate = crypto::x509(credentials().x509);
      ASSERT_TRUE(store.authenticate_certificate(transport.request(), certificate, chain, clients).identity);
      hooks = {
        .authority = [&] {
          const auto identity = store.resolve(transport.request(), clients);
          return identity ? power::authority_t {identity->uuid, identity->allow_sleep} : power::authority_t {};
        },
        .supported = [&] {
          return supported;
        },
        .activity = [&] {
          return activity;
        },
        .suspend_if_authorized = [&] {
          const auto identity = store.resolve(transport.request(), clients);
          if (!identity || !identity->allow_sleep) {
            return false;
          }
          EXPECT_TRUE(response->flushed);
          ++suspends;
          return native_success;
        },
        .respond = [&](const auto &value) {
          return power::write_response(response, value);
        },
      };
    }

    power::request_t request() {
      const auto caps = service.capabilities(hooks, now);
      EXPECT_TRUE(caps.available);
      EXPECT_EQ(caps.token.size(), 64);
      return {"POST", caps.token, true};
    }

    int status() const {
      return response->xml().get<int>("root.<xmlattr>.status_code");
    }

    transport_t transport;
    tls::store_t store;
    crypto::cert_chain_t chain;
    std::vector<tls::client_record_view_t> clients;
    std::mutex lifecycle;
    power::service_t service {lifecycle};
    power::hooks_t hooks;
    power::activity_t activity;
    power::service_t::clock::time_point now {power::service_t::clock::now()};
    std::shared_ptr<response_t> response = std::make_shared<response_t>();
    bool supported {true};
    bool native_success {true};
    int suspends {0};
  };

  TEST_F(PowerAction, DefaultPairingDoesNotGrantPowerControl) {
    tls::client_record_view_t legacy {first_uuid, "Power test", credentials().x509, true};
    clients[0] = legacy;
    const auto caps = service.capabilities(hooks, now);
    EXPECT_TRUE(caps.supported);
    EXPECT_FALSE(caps.allowed);
    EXPECT_FALSE(caps.available);
    EXPECT_TRUE(caps.token.empty());
    EXPECT_EQ(service.sleep({"POST", std::string(64, 'a'), true}, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 403);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, MissingDisabledAndReplacedCertificatesCannotExecute) {
    const auto req = request();
    store.reset(transport.request());
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 401);
    auto certificate = crypto::x509(credentials().x509);
    ASSERT_TRUE(store.authenticate_certificate(transport.request(), certificate, chain, clients).identity);
    clients[0].enabled = false;
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 401);
    clients[0].enabled = true;
    ASSERT_TRUE(store.authenticate_certificate(transport.request(), certificate, chain, clients).identity);
    const auto replacement = crypto::gen_creds("Replacement", 2048);
    clients[0].certificate = replacement.x509;
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 401);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, UnsupportedPlatformNeverOffersOrExecutesAnAction) {
    const auto req = request();
    supported = false;
    const auto caps = service.capabilities(hooks, now);
    EXPECT_FALSE(caps.supported);
    EXPECT_FALSE(caps.available);
    EXPECT_TRUE(caps.token.empty());
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 501);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, EveryActiveOrRetainedSessionDimensionRefusesSleep) {
    const std::array fields {&power::activity_t::app, &power::activity_t::pending, &power::activity_t::rtsp, &power::activity_t::running, &power::activity_t::teardown, &power::activity_t::webrtc, &power::activity_t::capture, &power::activity_t::remote_owner};
    for (const auto field : fields) {
      activity = {};
      const auto req = request();
      activity.*field = true;
      EXPECT_FALSE(service.capabilities(hooks, now).available);
      EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
      EXPECT_EQ(status(), 409);
    }
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, BusyLifecycleGateRefusesWithoutQueueing) {
    const auto req = request();
    std::promise<void> acquired, release;
    auto done = release.get_future();
    std::jthread other([&] {
      std::lock_guard lock(lifecycle);
      acquired.set_value();
      done.wait();
    });
    acquired.get_future().wait();
    EXPECT_FALSE(service.capabilities(hooks, now).available);
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 409);
    release.set_value();
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, AcceptanceIsFlushedBeforeNativeCallAndReplaysAreRefused) {
    const auto req = request();
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::requested);
    EXPECT_EQ(status(), 202);
    EXPECT_EQ(response->status, SimpleWeb::StatusCode::success_accepted);
    EXPECT_EQ(response->xml().get<int>("root.SleepAccepted"), 1);
    EXPECT_EQ(response->xml().get<std::string>("root.SleepActiveSessionPolicy"), "idle-only");
    EXPECT_EQ(response->headers.find("Cache-Control")->second, "no-store");
    EXPECT_TRUE(response->close_connection_after_response);
    EXPECT_EQ(suspends, 1);
    EXPECT_EQ(service.sleep(req, hooks, now + 10s), power::result_e::refused);
    EXPECT_EQ(status(), 409);
    EXPECT_EQ(suspends, 1);
  }

  TEST_F(PowerAction, ExpiredAndSupersededTokensCannotExecute) {
    const auto old = request();
    const auto fresh = request();
    EXPECT_NE(old.token, fresh.token);
    EXPECT_EQ(service.sleep(old, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 409);
    EXPECT_EQ(service.sleep(fresh, hooks, now + power::service_t::token_lifetime), power::result_e::refused);
    EXPECT_EQ(status(), 409);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, TokenCannotBeTransferredToAnotherPairedIdentity) {
    const auto req = request();
    // Changing a paired UUID invalidates transport identity. Authenticate the
    // same certificate again under its new explicit registry identity.
    clients[0].uuid = second_uuid;
    auto certificate = crypto::x509(credentials().x509);
    ASSERT_TRUE(store.authenticate_certificate(transport.request(), certificate, chain, clients).identity);
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 409);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, FailedFlushConsumesTokenAndNeverSuspends) {
    const auto req = request();
    response->flush = [](auto done) {
      done(make_error_code(boost::system::errc::connection_reset));
    };
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::response_failed);
    EXPECT_EQ(suspends, 0);
    response->flush = {};
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 409);
  }

  TEST_F(PowerAction, FlushTimeoutCannotTriggerDelayedSleep) {
    const auto req = request();
    std::function<void(const SimpleWeb::error_code &)> finish;
    response->flush = [&](auto done) {
      finish = std::move(done);
    };
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::response_failed);
    ASSERT_TRUE(finish);
    finish({});
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, RevokingGrantWhileResponseFlushesPreventsNativeCall) {
    const auto req = request();
    response->flush = [&](auto done) {
      clients[0].allow_sleep = false;
      response->flushed = true;
      done({});
    };
    EXPECT_EQ(service.sleep(req, hooks, now), power::result_e::native_failed);
    EXPECT_EQ(status(), 202);  // Acceptance deliberately does not claim completion.
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, AdmissionGateSpansResponseFlushAndNativeCall) {
    const auto req = request();
    std::promise<void> sending, release;
    auto released = release.get_future();
    response->flush = [&](auto done) {
      sending.set_value();
      released.wait();
      response->flushed = true;
      done({});
    };
    auto operation = std::async(std::launch::async, [&] {
      return service.sleep(req, hooks, now);
    });
    sending.get_future().wait();
    const bool acquired = lifecycle.try_lock();
    if (acquired) {
      lifecycle.unlock();
    }
    EXPECT_FALSE(acquired);
    release.set_value();
    EXPECT_EQ(operation.get(), power::result_e::requested);
    EXPECT_EQ(suspends, 1);
  }

  TEST_F(PowerAction, QueryShapeAndMethodRejectCommandsAndBrowserRequests) {
    const auto good = request();
    auto req = transport.request();
    req->method = "POST";
    req->query_string = "token=" + good.token;
    EXPECT_TRUE(power::parse_request(*req).valid_shape);
    for (const auto suffix : {"&token=duplicate", "&command=shutdown", "&action=hibernate"}) {
      req->query_string = "token=" + good.token + suffix;
      EXPECT_FALSE(power::parse_request(*req).valid_shape);
      EXPECT_EQ(service.sleep(power::parse_request(*req), hooks, now), power::result_e::refused);
      EXPECT_EQ(status(), 400);
    }
    req->query_string = "token=" + good.token;
    req->header.emplace("Origin", "https://example.invalid");
    EXPECT_FALSE(power::parse_request(*req).valid_shape);
    req->header.clear();
    ASSERT_EQ(req->content.rdbuf()->sputn("command", 7), 7);
    EXPECT_FALSE(power::parse_request(*req).valid_shape);
    EXPECT_EQ(service.sleep(power::parse_request(*req), hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 400);
    req->method = "GET";
    EXPECT_EQ(service.sleep(power::parse_request(*req), hooks, now), power::result_e::refused);
    EXPECT_EQ(status(), 405);
    EXPECT_EQ(suspends, 0);
  }

  TEST_F(PowerAction, EveryResponseHasExactByteLengthWhileConnectionIsRetained) {
    for (const int code : {200, 202, 400, 403, 409, 501}) {
      ASSERT_TRUE(power::write_response(response, {code, "Sleep test"}));
      EXPECT_TRUE(response->close_connection_after_response);
      ASSERT_EQ(response->headers.count("Content-Length"), 1);
      EXPECT_EQ(response->headers.find("Content-Length")->second, std::to_string(response->body.size()));
    }
  }

  TEST_F(PowerAction, NativeFailureIsNotReportedAsCompletedSleep) {
    native_success = false;
    EXPECT_EQ(service.sleep(request(), hooks, now), power::result_e::native_failed);
    EXPECT_EQ(status(), 202);
    EXPECT_NE(response->xml().get<std::string>("root.<xmlattr>.status_message").find("not confirmed"), std::string::npos);
  }
}  // namespace
