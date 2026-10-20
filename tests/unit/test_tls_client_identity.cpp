/**
 * @file tests/unit/test_tls_client_identity.cpp
 * @brief Direct production authorization and HTTPS transport behavior.
 *
 * Links tls_client_identity.cpp, crypto.cpp and http_pairing_policy.cpp. The
 * registry consists of real input records; no authentication or revocation
 * algorithm is substituted. All sockets use ephemeral localhost listeners.
 */
#include <atomic>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <src/tls_client_identity_native.h>
#include <thread>

namespace {
  using namespace nvhttp;
  using request_t = tls_identity::request_t;

  constexpr auto first_uuid = "715e52ea-1111-4444-8888-000000000001";
  constexpr auto second_uuid = "715e52ea-2222-4444-8888-000000000002";

  const crypto::creds_t &first_credentials() {
    static const auto value = crypto::gen_creds("First client", 2048);
    return value;
  }

  const crypto::creds_t &second_credentials() {
    static const auto value = crypto::gen_creds("Second client", 2048);
    return value;
  }

  const crypto::creds_t &unknown_credentials() {
    static const auto value = crypto::gen_creds("Unknown client", 2048);
    return value;
  }

  void add_native_certificate(crypto::cert_chain_t &chain, const std::string &pem) {
    auto record = std::make_shared<crypto::named_cert_t>();
    record->cert = pem;
    chain.add(record);
  }

  // Only socket acceptance is controlled here. The objects below are the real
  // SimpleWeb Connection/Session/Request types used by SunshineHTTPSServer.
  class transport_factory_t: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    using connection_t = SimpleWeb::ServerBase<SunshineHTTPS>::Connection;

    struct transport_t {
      std::shared_ptr<connection_t> connection;
      std::unique_ptr<boost::asio::ip::tcp::socket> peer;
    };

    transport_factory_t():
        ServerBase(0),
        listener(io, {boost::asio::ip::make_address("127.0.0.1"), 0}) {}

    transport_t connect() {
      transport_t transport {create_connection(io, context), std::make_unique<boost::asio::ip::tcp::socket>(io)};
      transport.peer->connect(listener.local_endpoint());
      listener.accept(transport.connection->socket->lowest_layer());
      return transport;
    }

    request_t request(const transport_t &transport) {
      return std::make_shared<Session>(1024, transport.connection)->request;
    }

    request_t keepalive(const transport_t &transport) {
      return std::make_shared<Session>(1024, transport.connection, std::make_unique<SimpleWeb::asio::streambuf>(1024))->request;
    }

  private:
    void accept() override {}

    boost::asio::io_context io;
    boost::asio::ssl::context context {boost::asio::ssl::context::tls_server};
    boost::asio::ip::tcp::acceptor listener;
  };

  class TlsClientIdentity: public testing::Test {
  protected:
    void SetUp() override {
      clients = {
        {first_uuid, "First client", first_credentials().x509, true},
        {second_uuid, "Second client", second_credentials().x509, true},
      };
      add_native_certificate(chain, first_credentials().x509);
      add_native_certificate(chain, second_credentials().x509);
    }

    tls_identity::authentication_result_t authenticate(const request_t &request, const crypto::creds_t &credentials = first_credentials()) {
      std::lock_guard lock(client_mutex);
      auto certificate = crypto::x509(credentials.x509);
      return store.authenticate_certificate(request, certificate, chain, clients);
    }

    std::optional<tls_identity::identity_t> resolve(const request_t &request) {
      std::lock_guard lock(client_mutex);
      return store.resolve(request, clients);
    }

    transport_factory_t factory;
    tls_identity::store_t store;
    crypto::cert_chain_t chain;
    std::vector<tls_identity::client_record_view_t> clients;
    std::mutex client_mutex;
  };

  TEST_F(TlsClientIdentity, ExactEnabledCertificatesAreRequired) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    const auto accepted = authenticate(request);
    ASSERT_TRUE(accepted.identity);
    EXPECT_EQ(accepted.identity->uuid, first_uuid);
    EXPECT_FALSE(authenticate(request, unknown_credentials()).identity);
    EXPECT_FALSE(resolve(request));
    clients[0].enabled = false;
    EXPECT_FALSE(authenticate(request).identity);
    EXPECT_FALSE(resolve(request));
    clients[0].enabled = true;
    clients[1].uuid = first_uuid;
    EXPECT_FALSE(authenticate(request).identity);
    EXPECT_FALSE(resolve(request));
  }

  TEST_F(TlsClientIdentity, TrustedChainDoesNotReplaceExactCertificateMatching) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    auto changed = crypto::x509(first_credentials().x509);
    auto key = crypto::pkey(first_credentials().pkey);
    ASSERT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(changed.get()), 42), 1);
    ASSERT_GT(X509_sign(changed.get(), key.get(), EVP_sha256()), 0);
    // A stale entry in the chain store is not a current pairing record. Even
    // a trusted certificate with the same key/name needs the exact DER match.
    add_native_certificate(chain, crypto::pem(changed));
    crypto::p_named_cert_t native_chain_result;
    ASSERT_EQ(chain.verify(changed.get(), native_chain_result), nullptr);
    ASSERT_TRUE(native_chain_result);
    EXPECT_FALSE(store.authenticate_certificate(request, changed, chain, clients).identity);
    EXPECT_FALSE(resolve(request));
  }

  TEST_F(TlsClientIdentity, MissingCertificateCannotRetainPreviousAuthority) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    EXPECT_FALSE(store.authenticate(request, nullptr, chain, clients).identity);
    EXPECT_FALSE(resolve(request));
  }

  TEST_F(TlsClientIdentity, KeepaliveUsesTheSameTransportAuthority) {
    auto transport = factory.connect();
    const auto first = factory.request(transport);
    const auto next = factory.keepalive(transport);
    ASSERT_NE(first, next);
    ASSERT_TRUE(authenticate(first).identity);
    ASSERT_TRUE(resolve(next));
    EXPECT_EQ(resolve(next)->uuid, first_uuid);
  }

  TEST_F(TlsClientIdentity, ChurnCannotEvictAnyLiveAuthenticatedTransport) {
    std::vector<transport_factory_t::transport_t> transports;
    std::vector<request_t> requests;
    const auto count = pairing_policy::max_paired_clients * 8 + 1;
    transports.reserve(count);
    requests.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      transports.push_back(factory.connect());
      requests.push_back(factory.request(transports.back()));
      ASSERT_TRUE(authenticate(requests.back()).identity);
    }
    std::size_t recognized = 0;
    for (const auto &request : requests) {
      recognized += resolve(request).has_value();
    }
    EXPECT_EQ(recognized, count) << "An unrelated handshake discarded live certificate authority";
  }

  TEST_F(TlsClientIdentity, CompletedConnectionChurnPreservesKeepaliveAuthority) {
    auto retained = factory.connect();
    const auto request = factory.request(retained);
    ASSERT_TRUE(authenticate(request).identity);
    for (std::size_t i = 0; i < 4096; ++i) {
      auto completed = factory.connect();
      ASSERT_TRUE(authenticate(factory.request(completed)).identity);
    }
    ASSERT_TRUE(resolve(factory.keepalive(retained)));
    EXPECT_EQ(resolve(request)->uuid, first_uuid);
  }

  TEST_F(TlsClientIdentity, RevocationIsImmediateAndDoesNotRevokeAnotherClient) {
    auto first = factory.connect();
    auto second = factory.connect();
    const auto first_request = factory.request(first);
    const auto second_request = factory.request(second);
    ASSERT_TRUE(authenticate(first_request).identity);
    ASSERT_TRUE(authenticate(second_request, second_credentials()).identity);
    store.revoke(first_uuid);
    EXPECT_FALSE(resolve(first_request));
    EXPECT_FALSE(resolve(factory.keepalive(first)));
    ASSERT_TRUE(resolve(second_request));
    EXPECT_EQ(resolve(second_request)->uuid, second_uuid);
    store.clear();
    EXPECT_FALSE(resolve(second_request));
  }

  TEST_F(TlsClientIdentity, DisabledAndReenabledClientNeedsNewAuthentication) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    clients[0].enabled = false;
    EXPECT_FALSE(resolve(request));
    clients[0].enabled = true;
    EXPECT_FALSE(resolve(request));
    ASSERT_TRUE(authenticate(request).identity);
    clients[0].enabled = false;
    store.revoke(first_uuid);  // The application also revokes on disable.
    clients[0].enabled = true;
    EXPECT_FALSE(resolve(request));
    EXPECT_TRUE(authenticate(request).identity);
  }

  TEST_F(TlsClientIdentity, LiveRecordControlsAuthorizationAndName) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    clients[0].name = "Renamed client";
    ASSERT_TRUE(resolve(request));
    EXPECT_EQ(resolve(request)->name, "Renamed client");
    clients[0].enabled = false;
    EXPECT_FALSE(resolve(factory.keepalive(transport)));
  }

  TEST_F(TlsClientIdentity, ReplacedCertificateCannotReviveAnOldTransport) {
    auto old_transport = factory.connect();
    const auto old_request = factory.request(old_transport);
    ASSERT_TRUE(authenticate(old_request).identity);
    clients[0].certificate = unknown_credentials().x509;
    add_native_certificate(chain, unknown_credentials().x509);
    EXPECT_FALSE(resolve(old_request));
    clients[0].certificate = first_credentials().x509;
    EXPECT_FALSE(resolve(old_request));
    clients[0].certificate = unknown_credentials().x509;
    auto replacement = factory.connect();
    EXPECT_FALSE(authenticate(factory.request(replacement)).identity);
    EXPECT_TRUE(authenticate(factory.request(replacement), unknown_credentials()).identity);
    store.revoke(first_uuid);
    clients.erase(clients.begin());
    EXPECT_FALSE(resolve(factory.keepalive(replacement)));
  }

  TEST_F(TlsClientIdentity, EquivalentPemFormattingPreservesExactCertificateAuthority) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    const auto reformatted = first_credentials().x509 + "\n";
    clients[0].certificate = reformatted;
    ASSERT_TRUE(resolve(request));
    EXPECT_EQ(resolve(request)->uuid, first_uuid);
  }

  TEST_F(TlsClientIdentity, SocketClosurePreservesQueuedIdentityWhileTransportLives) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    ASSERT_NE(request->remote_endpoint().port(), 0);
    transport.connection->socket->lowest_layer().close();
    ASSERT_EQ(request->remote_endpoint().port(), 0);
    ASSERT_TRUE(resolve(request));
    EXPECT_EQ(resolve(request)->uuid, first_uuid);
  }

  TEST_F(TlsClientIdentity, ConnectionDestructionReleasesAuthorityDespiteRetainedRequest) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    std::weak_ptr<void> authority = request->get_connection_context();
    ASSERT_FALSE(authority.expired());
    transport.connection.reset();
    EXPECT_TRUE(authority.expired());
    EXPECT_FALSE(resolve(request));
  }

  TEST_F(TlsClientIdentity, ConcurrentAuthenticationLookupAndRevocation) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    const auto next = factory.keepalive(transport);
    ASSERT_TRUE(authenticate(request).identity);
    std::atomic_bool invalid {false};
    std::thread reader([&] {
      for (int i = 0; i < 3000; ++i) {
        auto identity = resolve(next);
        if (identity && identity->uuid != first_uuid) {
          invalid = true;
        }
      }
    });
    std::thread revoker([&] {
      for (int i = 0; i < 1000; ++i) {
        store.revoke(first_uuid);
        if (i % 5 == 0) {
          store.clear();
        }
      }
    });
    for (int i = 0; i < 200; ++i) {
      if (!authenticate(request).identity) {
        invalid = true;
      }
    }
    reader.join();
    revoker.join();
    EXPECT_FALSE(invalid);
    store.revoke(first_uuid);
    EXPECT_FALSE(resolve(next));
  }

  TEST_F(TlsClientIdentity, ConcurrentDestructionCannotRetainOrPublishAuthority) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(authenticate(request).identity);
    std::atomic_bool reading {false};
    std::atomic_bool wrong_identity {false};
    std::thread reader([&] {
      reading = true;
      for (int i = 0; i < 3000; ++i) {
        auto identity = resolve(request);
        if (identity && identity->uuid != first_uuid) {
          wrong_identity = true;
        }
      }
    });
    while (!reading) {
      std::this_thread::yield();
    }
    transport.connection.reset();
    reader.join();
    EXPECT_FALSE(wrong_identity);
    EXPECT_FALSE(resolve(request));
    EXPECT_FALSE(authenticate(request).identity);
  }

  class temporary_directory_t {
  public:
    temporary_directory_t() {
      static std::atomic_uint sequence {0};
      for (;;) {
        path = std::filesystem::temp_directory_path() / ("tls-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        if (std::filesystem::create_directory(path)) {
          break;
        }
      }
    }

    ~temporary_directory_t() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }

    std::filesystem::path path;
  };

  struct http_reply_t {
    int status;
    std::string body;
  };

  http_reply_t read_reply(SimpleWeb::HTTPS &client) {
    boost::asio::streambuf buffer;
    boost::asio::read_until(client, buffer, "\r\n\r\n");
    std::istream stream(&buffer);
    std::string version;
    int status = 0;
    stream >> version >> status;
    std::string line;
    std::getline(stream, line);
    std::size_t size = 0;
    while (std::getline(stream, line) && line != "\r") {
      if (line.starts_with("Content-Length:")) {
        size = std::stoul(line.substr(15));
      }
    }
    if (buffer.size() < size) {
      boost::asio::read(client, buffer, boost::asio::transfer_exactly(size - buffer.size()));
    }
    std::string body(size, '\0');
    stream.read(body.data(), static_cast<std::streamsize>(body.size()));
    return {status, std::move(body)};
  }

  http_reply_t get_identity(SimpleWeb::HTTPS &client) {
    const std::string request = "GET /identity HTTP/1.1\r\nHost: localhost\r\n\r\n";
    boost::asio::write(client, boost::asio::buffer(request));
    return read_reply(client);
  }

  TEST_F(TlsClientIdentity, RealTlsHandshakeAndHttpKeepaliveUseProductionServer) {
    temporary_directory_t scratch;
    const auto &host = unknown_credentials();
    const auto cert_path = scratch.path / "host.crt";
    const auto key_path = scratch.path / "host.key";
    std::ofstream(cert_path) << host.x509;
    std::ofstream(key_path) << host.pkey;
    SunshineHTTPSServer server(cert_path.string(), key_path.string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 3;
    std::atomic_uint handshakes {0};
    std::vector<request_t> route_requests;
    server.verify = [&](request_t request, SSL *ssl) {
      ++handshakes;
      std::lock_guard lock(client_mutex);
      return store.authenticate(request, ssl, chain, clients).identity.has_value();
    };
    server.on_verify_failed = [](auto response, auto) {
      response->close_connection_after_response = true;
      response->write(SimpleWeb::StatusCode::client_error_unauthorized, "certificate rejected");
    };
    server.resource["^/identity$"]["GET"] = [&](auto response, auto request) {
      route_requests.push_back(request);
      auto identity = resolve(request);
      response->write(identity ? identity->uuid : "unauthorized");
    };
    std::promise<unsigned short> ready;
    auto ready_future = ready.get_future();
    std::thread worker([&] {
      server.start([&](auto port) {
        ready.set_value(port);
      });
    });

    struct stop_server_t {
      SunshineHTTPSServer &server;
      std::thread &worker;

      ~stop_server_t() {
        server.stop();
        if (worker.joinable()) {
          worker.join();
        }
      }
    } stop {server, worker};

    ASSERT_EQ(ready_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto port = ready_future.get();
    boost::asio::io_context io;
    boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
    context.use_certificate_chain(boost::asio::buffer(first_credentials().x509));
    context.use_private_key(boost::asio::buffer(first_credentials().pkey), boost::asio::ssl::context::pem);
    context.load_verify_file(cert_path.string());
    context.set_verify_mode(boost::asio::ssl::verify_peer);
    SimpleWeb::HTTPS client(io, context);
    client.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port});
    client.handshake(boost::asio::ssl::stream_base::client);
    EXPECT_EQ(get_identity(client).body, first_uuid);
    EXPECT_EQ(get_identity(client).body, first_uuid);
    {
      std::lock_guard lock(client_mutex);
      clients[0].enabled = false;
    }
    store.revoke(first_uuid);
    EXPECT_EQ(get_identity(client).body, "unauthorized");
    {
      std::lock_guard lock(client_mutex);
      clients[0].enabled = true;
    }
    EXPECT_EQ(get_identity(client).body, "unauthorized");
    client.lowest_layer().close();
    boost::asio::ssl::context unknown_context(boost::asio::ssl::context::tls_client);
    unknown_context.use_certificate_chain(boost::asio::buffer(unknown_credentials().x509));
    unknown_context.use_private_key(boost::asio::buffer(unknown_credentials().pkey), boost::asio::ssl::context::pem);
    unknown_context.load_verify_file(cert_path.string());
    unknown_context.set_verify_mode(boost::asio::ssl::verify_peer);
    SimpleWeb::HTTPS unknown_client(io, unknown_context);
    unknown_client.lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port});
    unknown_client.handshake(boost::asio::ssl::stream_base::client);
    EXPECT_EQ(read_reply(unknown_client).status, 401);
    unknown_client.lowest_layer().close();
    server.stop();
    worker.join();
    ASSERT_EQ(handshakes, 2u);
    ASSERT_EQ(route_requests.size(), 4u);
    EXPECT_NE(route_requests[0], route_requests[1]);
  }

  class NativeTlsClientIdentity: public TlsClientIdentity {
  protected:
    void SetUp() override {
      TlsClientIdentity::SetUp();
      auto record = std::make_shared<crypto::named_cert_t>();
      record->uuid = first_uuid;
      record->name = "First native client";
      record->cert = first_credentials().x509;
      record->perm = crypto::PERM::view;
      records.push_back(record);
      chain.clear();
      chain.add(records.front());
    }

    tls_identity::authentication_result_t native_authenticate(const request_t &request, const crypto::creds_t &credentials = first_credentials()) {
      std::lock_guard lock(client_mutex);
      auto certificate = crypto::x509(credentials.x509);
      return native_store.authenticate_certificate(request, certificate, chain, records);
    }

    std::optional<crypto::named_cert_t> native_resolve(const request_t &request) {
      std::lock_guard lock(client_mutex);
      return native_store.resolve_snapshot(request, records);
    }

    tls_identity::native_store_t native_store;
    std::vector<crypto::p_named_cert_t> records;
  };

  TEST_F(NativeTlsClientIdentity, NativeConversionRecognizesOnlyCurrentPermissionBits) {
    for (const auto permission : {crypto::PERM::_no, crypto::PERM::_reserved, static_cast<crypto::PERM>(0x80000000u)}) {
      records[0]->perm = permission;
      auto views = tls_identity::native_client_views(records);
      ASSERT_TRUE(views);
      ASSERT_EQ(views->size(), 1u);
      EXPECT_FALSE(views->front().enabled);
      auto transport = factory.connect();
      EXPECT_FALSE(native_authenticate(factory.request(transport)).identity);
    }
    for (const auto permission : {crypto::PERM::view, crypto::PERM::list, crypto::PERM::input_kbd, crypto::PERM::server_cmd, crypto::PERM::_all}) {
      records[0]->perm = permission;
      auto views = tls_identity::native_client_views(records);
      ASSERT_TRUE(views);
      EXPECT_TRUE(views->front().enabled);
      EXPECT_EQ(views->front().uuid, first_uuid);
      EXPECT_EQ(views->front().certificate, first_credentials().x509);
      auto transport = factory.connect();
      const auto request = factory.request(transport);
      ASSERT_TRUE(native_authenticate(request).identity);
      ASSERT_TRUE(native_resolve(request));
      EXPECT_EQ(native_resolve(request)->perm, permission);
    }
  }

  TEST_F(NativeTlsClientIdentity, NullNativeRegistryFailsClosedAndInvalidatesExistingAuthority) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(native_authenticate(request).identity);
    records.push_back(nullptr);
    EXPECT_FALSE(tls_identity::native_client_views(records));
    EXPECT_FALSE(native_resolve(request));
    EXPECT_FALSE(tls_identity::current_native_permissions_match(records, first_uuid, crypto::PERM::view));
    EXPECT_FALSE(native_authenticate(request).identity);
    records.pop_back();
    EXPECT_FALSE(native_resolve(request));
    EXPECT_TRUE(native_authenticate(request).identity);
  }

  TEST_F(NativeTlsClientIdentity, SnapshotCopiesAllCurrentNativePreferencesAndCommands) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    auto &record = *records[0];
    record.display_mode = "3840x2160x119.88";
    record.hdr_profile = "native-hdr-profile";
    record.output_name_override = "native-output";
    record.virtual_display_mode_override = "per_client";
    record.virtual_display_layout_override = "extended";
    record.do_cmds = {{"native-do-command", true}};
    record.undo_cmds = {{"native-undo-command", false}};
    record.enable_legacy_ordering = true;
    record.allow_client_commands = true;
    record.always_use_virtual_display = true;
    record.prefer_10bit_sdr = true;
    record.last_seen = 123456789;
    record.config_overrides = {{"fps", "119.88"}, {"hdr", "true"}};
    ASSERT_TRUE(native_authenticate(request).identity);
    const auto snapshot = native_resolve(request);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->uuid, first_uuid);
    EXPECT_EQ(snapshot->name, record.name);
    EXPECT_EQ(snapshot->cert, record.cert);
    EXPECT_EQ(snapshot->perm, crypto::PERM::view);
    EXPECT_EQ(snapshot->display_mode, "3840x2160x119.88");
    EXPECT_EQ(snapshot->hdr_profile, "native-hdr-profile");
    EXPECT_EQ(snapshot->output_name_override, "native-output");
    EXPECT_EQ(snapshot->virtual_display_mode_override, "per_client");
    EXPECT_EQ(snapshot->virtual_display_layout_override, "extended");
    ASSERT_EQ(snapshot->do_cmds.size(), 1u);
    EXPECT_EQ(snapshot->do_cmds.front().cmd, "native-do-command");
    EXPECT_TRUE(snapshot->do_cmds.front().elevated);
    ASSERT_EQ(snapshot->undo_cmds.size(), 1u);
    EXPECT_EQ(snapshot->undo_cmds.front().cmd, "native-undo-command");
    EXPECT_FALSE(snapshot->undo_cmds.front().elevated);
    EXPECT_TRUE(snapshot->enable_legacy_ordering);
    EXPECT_TRUE(snapshot->allow_client_commands);
    EXPECT_TRUE(snapshot->always_use_virtual_display);
    EXPECT_TRUE(snapshot->prefer_10bit_sdr);
    EXPECT_EQ(snapshot->last_seen, 123456789);
    EXPECT_EQ(snapshot->config_overrides, record.config_overrides);

    record.name = "Changed native client";
    record.perm = crypto::PERM::list;
    record.display_mode = "2560x1440x59.94";
    record.hdr_profile = "changed-hdr";
    record.output_name_override = "changed-output";
    record.virtual_display_mode_override = "disabled";
    record.virtual_display_layout_override = "primary";
    record.do_cmds = {{"changed-do", false}};
    record.undo_cmds.clear();
    record.enable_legacy_ordering = false;
    record.allow_client_commands = false;
    record.always_use_virtual_display = false;
    record.prefer_10bit_sdr = false;
    record.last_seen.reset();
    record.config_overrides = {{"fps", "59.94"}};
    const auto current = native_resolve(factory.keepalive(transport));
    ASSERT_TRUE(current);
    EXPECT_EQ(current->name, record.name);
    EXPECT_EQ(current->perm, crypto::PERM::list);
    EXPECT_EQ(current->display_mode, "2560x1440x59.94");
    EXPECT_EQ(current->hdr_profile, "changed-hdr");
    EXPECT_EQ(current->output_name_override, "changed-output");
    EXPECT_EQ(current->virtual_display_mode_override, "disabled");
    EXPECT_EQ(current->virtual_display_layout_override, "primary");
    ASSERT_EQ(current->do_cmds.size(), 1u);
    EXPECT_EQ(current->do_cmds.front().cmd, "changed-do");
    EXPECT_FALSE(current->do_cmds.front().elevated);
    EXPECT_TRUE(current->undo_cmds.empty());
    EXPECT_FALSE(current->enable_legacy_ordering);
    EXPECT_FALSE(current->allow_client_commands);
    EXPECT_FALSE(current->always_use_virtual_display);
    EXPECT_FALSE(current->prefer_10bit_sdr);
    EXPECT_FALSE(current->last_seen);
    EXPECT_EQ(current->config_overrides, record.config_overrides);
    // These are independent native value snapshots, not mutable aliases.
    EXPECT_EQ(snapshot->perm, crypto::PERM::view);
    EXPECT_EQ(snapshot->config_overrides.at("fps"), "119.88");
  }

  TEST_F(NativeTlsClientIdentity, FinalPermissionCheckRequiresTheExactCurrentMask) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(native_authenticate(request).identity);
    const auto snapshot = native_resolve(request);
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(tls_identity::current_native_permissions_match(records, snapshot->uuid, snapshot->perm));
    records[0]->perm = crypto::PERM::list;
    EXPECT_FALSE(tls_identity::current_native_permissions_match(records, snapshot->uuid, snapshot->perm));
    EXPECT_TRUE(tls_identity::current_native_permissions_match(records, snapshot->uuid, crypto::PERM::list));
    EXPECT_FALSE(tls_identity::current_native_permissions_match(records, second_uuid, crypto::PERM::list));
    records[0]->perm = crypto::PERM::_no;
    EXPECT_FALSE(tls_identity::current_native_permissions_match(records, snapshot->uuid, crypto::PERM::_no));
  }

  TEST_F(NativeTlsClientIdentity, NativeDisableReenableAndRevocationDenyExistingTransport) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(native_authenticate(request).identity);
    records[0]->perm = crypto::PERM::_no;
    EXPECT_FALSE(native_resolve(request));
    records[0]->perm = crypto::PERM::view;
    EXPECT_FALSE(native_resolve(request));
    ASSERT_TRUE(native_authenticate(request).identity);
    native_store.revoke(first_uuid);
    EXPECT_FALSE(native_resolve(request));
    ASSERT_TRUE(native_authenticate(request).identity);
    native_store.clear();
    EXPECT_FALSE(native_resolve(request));
  }

  TEST_F(NativeTlsClientIdentity, ReplacedNativePointerCannotAuthorizeStaleChainOrOldConnection) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    ASSERT_TRUE(native_authenticate(request).identity);
    const auto original = records[0];
    auto replacement = std::make_shared<crypto::named_cert_t>(*original);
    replacement->cert = unknown_credentials().x509;
    records[0] = replacement;
    chain.add(records[0]);
    crypto::p_named_cert_t trusted_output;
    auto old_certificate = crypto::x509(first_credentials().x509);
    ASSERT_EQ(chain.verify(old_certificate.get(), trusted_output), nullptr);
    ASSERT_EQ(trusted_output, original);  // actual native chain returns stale record
    EXPECT_FALSE(native_resolve(request));
    EXPECT_FALSE(native_authenticate(request).identity);
    EXPECT_TRUE(native_authenticate(request, unknown_credentials()).identity);
    records[0] = original;
    EXPECT_FALSE(native_resolve(request));
    EXPECT_FALSE(native_authenticate(request, unknown_credentials()).identity);
    EXPECT_TRUE(native_authenticate(request).identity);
    native_store.revoke(first_uuid);
    records.clear();
    EXPECT_FALSE(native_resolve(request));
  }

  TEST_F(NativeTlsClientIdentity, RequestSuppliedUuidDoesNotGrantAuthority) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    request->query_string = std::string("uniqueid=") + first_uuid;
    EXPECT_FALSE(native_resolve(request));
    EXPECT_FALSE(native_authenticate(request, unknown_credentials()).identity);
    EXPECT_FALSE(native_resolve(request));
    ASSERT_TRUE(native_authenticate(request).identity);
    request->query_string = std::string("uniqueid=") + second_uuid;
    ASSERT_TRUE(native_resolve(request));
    EXPECT_EQ(native_resolve(request)->uuid, first_uuid);
    transport.connection.reset();
    EXPECT_FALSE(native_resolve(request));
  }

  TEST_F(NativeTlsClientIdentity, ConcurrentNativeUpdatesCopyCoherentCurrentRecords) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    records[0]->name = "A";
    records[0]->display_mode = "A";
    records[0]->config_overrides = {{"generation", "A"}};
    ASSERT_TRUE(native_authenticate(request).identity);
    std::atomic_bool inconsistent {false};
    std::thread reader([&] {
      for (int i = 0; i < 3000; ++i) {
        const auto snapshot = native_resolve(request);
        if (snapshot && (snapshot->name != snapshot->display_mode ||
                         snapshot->name != snapshot->config_overrides.at("generation") ||
                         snapshot->perm != crypto::PERM::view)) {
          inconsistent = true;
        }
      }
    });
    std::thread writer([&] {
      for (int i = 0; i < 1000; ++i) {
        std::lock_guard lock(client_mutex);
        const auto value = i % 2 == 0 ? "A" : "B";
        records[0]->name = value;
        records[0]->display_mode = value;
        records[0]->config_overrides["generation"] = value;
      }
    });
    std::thread revoker([&] {
      for (int i = 0; i < 1000; ++i) native_store.revoke(first_uuid);
    });
    reader.join();
    writer.join();
    revoker.join();
    EXPECT_FALSE(inconsistent);
    EXPECT_FALSE(native_resolve(request));
  }

  TEST_F(NativeTlsClientIdentity, SleepGrantDefaultsOffAndTracksLiveNativeRecord) {
    auto transport = factory.connect();
    const auto request = factory.request(transport);
    records[0]->perm = crypto::PERM::_all;
    ASSERT_TRUE(native_authenticate(request).identity);
    ASSERT_TRUE(native_resolve(request));
    EXPECT_FALSE(native_resolve(request)->allow_sleep);
    records[0]->allow_sleep = true;
    EXPECT_TRUE(native_resolve(request)->allow_sleep);
    const auto views = nvhttp::tls_identity::native_client_views(records);
    ASSERT_TRUE(views);
    EXPECT_TRUE((*views)[0].allow_sleep);
    records[0]->allow_sleep = false;
    EXPECT_FALSE(native_resolve(request)->allow_sleep);
    records[0]->allow_sleep = true;
    records[0]->perm = crypto::PERM::_no;
    EXPECT_FALSE(native_resolve(request));
  }
}  // namespace
