/** Real TLS transport and production handoff admission, with a controlled app worker. */
#include "src/nvhttp_dispatch.h"
#include "src/tls_client_identity.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace std::chrono_literals;
namespace {
  using namespace nvhttp;
  constexpr auto client_uuid = "715e52ea-1111-4444-8888-000000000001";

  const crypto::creds_t &client_credentials() {
    static const auto credentials = crypto::gen_creds("Handoff client", 2048);
    return credentials;
  }

  const crypto::creds_t &host_credentials() {
    static const auto credentials = crypto::gen_creds("Handoff host", 2048);
    return credentials;
  }

  class scratch_t {
  public:
    scratch_t() {
      static std::atomic_uint sequence {0};
      do {
        path = std::filesystem::temp_directory_path() / ("handoff-tls-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
      } while (!std::filesystem::create_directory(path));
    }
    ~scratch_t() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    std::filesystem::path path;
  };

  class server_t {
  public:
    server_t(tls_identity::store_t &store, crypto::cert_chain_t &chain,
             std::vector<tls_identity::client_record_view_t> &clients, std::mutex &client_mutex,
             std::chrono::steady_clock::duration timeout = 3s):
        store_(store), chain_(chain), clients_(clients), client_mutex_(client_mutex), timeout_(timeout) {
      cert_path = scratch_.path / "host.crt";
      auto key_path = scratch_.path / "host.key";
      std::ofstream(cert_path) << host_credentials().x509;
      std::ofstream(key_path) << host_credentials().pkey;
      server_ = std::make_unique<SunshineHTTPSServer>(cert_path.string(), key_path.string());
      server_->config.address = "127.0.0.1";
      server_->config.port = 0;
      server_->verify = [this](tls_identity::request_t request, SSL *ssl, pending_peer_factory_t peer) {
        std::lock_guard lock(client_mutex_);
        return store_.authenticate(request, ssl, chain_, clients_, std::move(peer)).identity.has_value();
      };
      server_->resource["^/cancel$"]["GET"] = [this](auto response, auto) {
        auto result = dispatch::run_handoff(admission_, submitter(), response, "cancel", [this, response] {
          ++cancels;
          response->write("canceled");
          response->close_connection_after_response = true;
        });
        record(result);
      };
      server_->resource["^/launch$"]["GET"] = [this](auto response, auto request) {
        launch_response_ = response;
        auto peer = store_.pending_peer(request);
        auto result = dispatch::run_handoff(admission_, submitter(), response, "launch", [this, response, request] {
          std::lock_guard lock(client_mutex_);
          if (store_.resolve(request, clients_)) {
            ++starts;
            response->write("started");
          } else {
            response->write("unauthorized");
          }
          response->close_connection_after_response = true;
        }, std::move(peer), [this](const dispatch::trace_event_t &event) {
          std::lock_guard lock(mutex_);
          events_.push_back(event.stage);
          changed_.notify_all();
        }, timeout_);
        record(result);
      };
      server_->resource["^/ping$"]["GET"] = [](auto response, auto) {
        response->write("ready");
        response->close_connection_after_response = true;
      };
      std::promise<unsigned short> ready;
      auto future = ready.get_future();
      worker_ = std::thread([this, ready = std::move(ready)]() mutable {
        server_->start([&](auto selected_port) { ready.set_value(selected_port); });
      });
      if (future.wait_for(5s) != std::future_status::ready) {
        server_->stop();
        worker_.join();
        throw std::runtime_error("TLS server did not start");
      }
      port = future.get();
    }

    ~server_t() {
      admission_.stop();
      server_->stop();
      if (worker_.joinable()) worker_.join();
    }

    std::function<void(std::function<void()>)> submitter() {
      return [this](auto task) {
        std::lock_guard lock(mutex_);
        tasks_.emplace_back(std::move(task));
        changed_.notify_all();
      };
    }

    bool wait_results(std::size_t count) {
      std::unique_lock lock(mutex_);
      return changed_.wait_for(lock, 3s, [&] { return results_.size() >= count; });
    }
    bool wait_tasks(std::size_t count) {
      std::unique_lock lock(mutex_);
      return changed_.wait_for(lock, 3s, [&] { return tasks_.size() >= count; });
    }
    bool wait_event(dispatch::trace_stage_e event) {
      std::unique_lock lock(mutex_);
      return changed_.wait_for(lock, 3s, [&] { return std::find(events_.begin(), events_.end(), event) != events_.end(); });
    }
    std::string events() {
      std::lock_guard lock(mutex_);
      std::string value;
      for (auto stage : events_) value += std::string(dispatch::trace_stage_name(stage)) + " ";
      return value;
    }
    bool run_one() {
      std::function<void()> task;
      {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) return false;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
      return true;
    }
    std::size_t queued() { std::lock_guard lock(mutex_); return tasks_.size(); }
    handoff::result_e result(std::size_t i) { std::lock_guard lock(mutex_); return results_.at(i); }
    long response_refs() const { return launch_response_.use_count(); }

    std::filesystem::path cert_path;
    unsigned short port = 0;
    std::atomic_int starts {0}, cancels {0};

  private:
    void record(handoff::result_e result) {
      std::lock_guard lock(mutex_);
      results_.push_back(result);
      changed_.notify_all();
    }
    scratch_t scratch_;
    tls_identity::store_t &store_;
    crypto::cert_chain_t &chain_;
    std::vector<tls_identity::client_record_view_t> &clients_;
    std::mutex &client_mutex_;
    std::chrono::steady_clock::duration timeout_;
    handoff::admission_t admission_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    std::vector<handoff::result_e> results_;
    std::vector<dispatch::trace_stage_e> events_;
    std::unique_ptr<SunshineHTTPSServer> server_;
    std::weak_ptr<SunshineHTTPSServer::Response> launch_response_;
    std::thread worker_;
  };

  class client_t {
  public:
    explicit client_t(const server_t &server): context_(boost::asio::ssl::context::tls_client) {
      context_.use_certificate_chain(boost::asio::buffer(client_credentials().x509));
      context_.use_private_key(boost::asio::buffer(client_credentials().pkey), boost::asio::ssl::context::pem);
      context_.load_verify_file(server.cert_path.string());
      context_.set_verify_mode(boost::asio::ssl::verify_peer);
      client_ = std::make_unique<SimpleWeb::HTTPS>(io_, context_);
      client_->lowest_layer().connect({boost::asio::ip::make_address("127.0.0.1"), server.port});
      client_->handshake(boost::asio::ssl::stream_base::client);
    }
    void get(const std::string &path) {
      auto request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
      boost::asio::write(*client_, boost::asio::buffer(request));
    }
    std::optional<std::string> reply() {
      std::promise<std::optional<std::string>> result;
      auto future = result.get_future();
      std::thread reader([this, result = std::move(result)]() mutable {
        try {
          boost::asio::streambuf buffer;
          boost::asio::read_until(*client_, buffer, "\r\n\r\n");
          std::istream stream(&buffer);
          std::string line;
          std::size_t size = 0;
          while (std::getline(stream, line) && line != "\r") {
            if (line.starts_with("Content-Length:")) size = std::stoul(line.substr(15));
          }
          if (buffer.size() < size) boost::asio::read(*client_, buffer, boost::asio::transfer_exactly(size - buffer.size()));
          std::string body(size, '\0');
          stream.read(body.data(), static_cast<std::streamsize>(size));
          result.set_value(std::move(body));
        } catch (...) { result.set_value(std::nullopt); }
      });
      if (future.wait_for(3s) != std::future_status::ready) close();
      auto value = future.get();
      reader.join();
      return value;
    }
    void close() {
      boost::system::error_code ec;
      client_->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      client_->lowest_layer().close(ec);
    }
    void close_notify() {
      client_->async_shutdown([](const auto &) {});
      io_.run_for(200ms);
    }
  private:
    boost::asio::io_context io_;
    boost::asio::ssl::context context_;
    std::unique_ptr<SimpleWeb::HTTPS> client_;
  };

  class NvhttpHandoffTls: public testing::Test {
  protected:
    void SetUp() override {
      clients = {{client_uuid, "client", client_credentials().x509, true}};
      auto native_client = std::make_shared<crypto::named_cert_t>();
      native_client->uuid = client_uuid;
      native_client->cert = client_credentials().x509;
      native_client->perm = crypto::PERM::_all;
      chain.add(native_client);
    }
    tls_identity::store_t store;
    crypto::cert_chain_t chain;
    std::vector<tls_identity::client_record_view_t> clients;
    std::mutex client_mutex;
  };

  TEST_F(NvhttpHandoffTls, StartsAutomaticallyAfterCancel) {
    server_t server(store, chain, clients, client_mutex);
    client_t old_app(server), next_app(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    ASSERT_TRUE(server.run_one());
    ASSERT_TRUE(server.wait_tasks(1)) << server.events();
    ASSERT_TRUE(server.run_one());
    auto reply = next_app.reply();
    ASSERT_TRUE(reply) << "events=" << server.events() << " response_refs=" << server.response_refs();
    EXPECT_EQ(*reply, "started");
    EXPECT_EQ(server.cancels, 1);
    EXPECT_EQ(server.starts, 1);
    old_app.close();
    next_app.close();
  }

  TEST_F(NvhttpHandoffTls, DisconnectAndTlsCloseNotifyRemoveThePendingLaunch) {
    for (bool graceful : {false, true}) {
      server_t server(store, chain, clients, client_mutex);
      client_t old_app(server), next_app(server);
      old_app.get("/cancel");
      ASSERT_TRUE(server.wait_tasks(1));
      ASSERT_TRUE(server.wait_results(1));
      next_app.get("/launch");
      ASSERT_TRUE(server.wait_results(2));
      ASSERT_EQ(server.result(1), handoff::result_e::waiting);
      if (graceful) next_app.close_notify(); else next_app.close();
      ASSERT_TRUE(server.wait_event(dispatch::trace_stage_e::handoff_disconnected)) << server.events();
      ASSERT_TRUE(server.run_one());
      EXPECT_EQ(server.queued(), 0u);
      EXPECT_EQ(server.starts, 0);
      old_app.close();
      next_app.close();
    }
  }

  TEST_F(NvhttpHandoffTls, ServerRemainsResponsiveWhileThePreviousAppIsClosing) {
    server_t server(store, chain, clients, client_mutex);
    client_t old_app(server), next_app(server), observer(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    observer.get("/ping");
    auto reply = observer.reply();
    ASSERT_TRUE(reply);
    EXPECT_EQ(*reply, "ready");
    next_app.close();
    ASSERT_TRUE(server.wait_event(dispatch::trace_stage_e::handoff_disconnected));
    ASSERT_TRUE(server.run_one());
    EXPECT_EQ(server.starts, 0);
    old_app.close();
    observer.close();
  }

  TEST_F(NvhttpHandoffTls, TimeoutReturnsAFramedErrorAndNeverStartsLater) {
    server_t server(store, chain, clients, client_mutex, 100ms);
    client_t old_app(server), next_app(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    ASSERT_TRUE(server.wait_event(dispatch::trace_stage_e::handoff_expired)) << server.events();
    auto reply = next_app.reply();
    ASSERT_TRUE(reply) << "events=" << server.events() << " response_refs=" << server.response_refs();
    EXPECT_NE(reply->find("Timed out waiting"), std::string::npos);
    ASSERT_TRUE(server.run_one());
    EXPECT_EQ(server.queued(), 0u);
    EXPECT_EQ(server.starts, 0);
    old_app.close();
    next_app.close();
  }

  TEST_F(NvhttpHandoffTls, RevokedClientCannotStartAfterWaiting) {
    server_t server(store, chain, clients, client_mutex);
    client_t old_app(server), next_app(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    store.revoke(client_uuid);
    ASSERT_TRUE(server.run_one());
    ASSERT_TRUE(server.wait_tasks(1)) << server.events();
    ASSERT_TRUE(server.run_one());
    auto reply = next_app.reply();
    ASSERT_TRUE(reply);
    EXPECT_EQ(*reply, "unauthorized");
    EXPECT_EQ(server.starts, 0);
    old_app.close();
    next_app.close();
  }

  TEST_F(NvhttpHandoffTls, DisconnectAfterReservationIsRecheckedOnTheWorker) {
    server_t server(store, chain, clients, client_mutex);
    client_t old_app(server), next_app(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    ASSERT_TRUE(server.run_one());
    ASSERT_TRUE(server.wait_tasks(1)) << server.events();
    next_app.close();
    ASSERT_TRUE(server.wait_event(dispatch::trace_stage_e::handoff_disconnected)) << server.events();
    ASSERT_TRUE(server.run_one());
    EXPECT_EQ(server.starts, 0);
    old_app.close();
  }

  TEST_F(NvhttpHandoffTls, DeadlineStillAppliesAfterReservation) {
    server_t server(store, chain, clients, client_mutex, 100ms);
    client_t old_app(server), next_app(server);
    old_app.get("/cancel");
    ASSERT_TRUE(server.wait_tasks(1));
    ASSERT_TRUE(server.wait_results(1));
    next_app.get("/launch");
    ASSERT_TRUE(server.wait_results(2));
    ASSERT_EQ(server.result(1), handoff::result_e::waiting);
    ASSERT_TRUE(server.run_one());
    ASSERT_TRUE(server.wait_tasks(1)) << server.events();
    ASSERT_TRUE(server.wait_event(dispatch::trace_stage_e::handoff_expired)) << server.events();
    auto reply = next_app.reply();
    ASSERT_TRUE(reply);
    EXPECT_NE(reply->find("Timed out waiting"), std::string::npos);
    ASSERT_TRUE(server.run_one());
    EXPECT_EQ(server.starts, 0);
    old_app.close();
    next_app.close();
  }
}  // namespace
