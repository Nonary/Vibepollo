/**
 * @file src/nvhttp_server.h
 * @brief GameStream HTTPS transport and certificate-verification dispatch.
 */
#pragma once

#include "nvhttp_pending_peer.h"

#include <Simple-Web-Server/server_https.hpp>

#if defined(__linux__) && !defined(_WIN32)
  #include <array>
  #include <boost/asio/bind_executor.hpp>
  #include <mutex>
#endif

namespace nvhttp {

  class SunshineHTTPS: public SimpleWeb::HTTPS {
  public:
    SunshineHTTPS(boost::asio::io_context &io_context, boost::asio::ssl::context &ctx):
        SimpleWeb::HTTPS(io_context, ctx) {
    }

    virtual ~SunshineHTTPS() {
      // Try to send close_notify without waiting for the peer. This destructor
      // can run on the only HTTPS I/O thread after a response has been sent.
      SimpleWeb::error_code ec;
#if defined(__linux__) && !defined(_WIN32)
      lowest_layer().non_blocking(true, ec);
#endif
      if (!ec) shutdown(ec);
    }
  };

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<bool(std::shared_ptr<Request>, SSL *, pending_peer_factory_t)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

#if defined(__linux__) && !defined(_WIN32)
    // A request handler owns the response while waiting, so the HTTP server has
    // no read outstanding. Probe the TLS stream in nonblocking mode: checking
    // only the TCP fd misses close_notify buffered by the TLS layer. In
    // particular, do not terminal-cancel an SSL read and then reuse the stream.
    class pending_peer_watch_t final: public pending_peer_t, public std::enable_shared_from_this<pending_peer_watch_t> {
    public:
      explicit pending_peer_watch_t(std::shared_ptr<Connection> connection):
          connection_(std::move(connection)),
          timer_(connection_->socket->get_executor()) {}

      void start(std::chrono::steady_clock::time_point deadline, std::function<void(pending_peer_end_e)> on_end) override {
        auto self = shared_from_this();
        boost::asio::post(connection_->write_strand, [self, deadline, on_end = std::move(on_end)]() mutable {
          std::function<void(pending_peer_end_e)> ended;
          {
            std::lock_guard lock(self->mutex_);
            if (self->stopped_) return;
            self->on_end_ = std::move(on_end);
            self->deadline_ = deadline;
            auto &socket = self->connection_->socket->lowest_layer();
            self->previous_nonblocking_ = socket.non_blocking();
            SimpleWeb::error_code ec;
            socket.non_blocking(true, ec);
            if (ec) ended = self->end_locked();
            else self->nonblocking_changed_ = true;
          }
          if (ended) ended(pending_peer_end_e::disconnected);
          else self->poll_peer();
        });
      }

      void prepare(std::function<void(bool)> on_ready) override {
        auto self = shared_from_this();
        boost::asio::post(connection_->write_strand, [self, on_ready = std::move(on_ready)]() mutable {
          std::function<void(pending_peer_end_e)> ended;
          auto reason = pending_peer_end_e::disconnected;
          bool ready = false;
          {
            std::lock_guard lock(self->mutex_);
            if (!self->stopped_ && !self->claimed_) {
              if (std::chrono::steady_clock::now() >= self->deadline_) {
                reason = pending_peer_end_e::expired;
                ended = self->end_locked();
              } else if (!self->probe()) {
                ended = self->end_locked();
              } else {
                ready = true;
              }
            }
          }
          if (ended) ended(reason);
          on_ready(ready);
        });
      }

      bool connected() override {
        // Serialize the worker's final TLS probe with the I/O thread. A
        // successful claim prevents further probes before the response writes.
        std::lock_guard lock(mutex_);
        if (stopped_ || claimed_ || std::chrono::steady_clock::now() >= deadline_ || !probe()) return false;
        claimed_ = true;
        return restore_blocking() && std::chrono::steady_clock::now() < deadline_;
      }

      void stop() override {
        auto self = shared_from_this();
        boost::asio::post(connection_->write_strand, [self] {
          std::lock_guard lock(self->mutex_);
          self->end_locked();
        });
      }

    private:
      bool probe() {
        SimpleWeb::error_code ec;
        connection_->socket->read_some(boost::asio::buffer(read_buffer_), ec);
        // The request is still open only when a nonblocking TLS read needs
        // more input. EOF, close_notify, errors, and extra data fail closed.
        return ec == boost::asio::error::would_block || ec == boost::asio::error::try_again;
      }

      void poll_peer() {
        std::function<void(pending_peer_end_e)> ended;
        auto reason = pending_peer_end_e::disconnected;
        {
          std::lock_guard lock(mutex_);
          if (stopped_ || claimed_) return;
          if (std::chrono::steady_clock::now() >= deadline_) {
            reason = pending_peer_end_e::expired;
            ended = end_locked();
          } else if (!probe()) {
            ended = end_locked();
          } else {
            timer_.expires_at(std::min(deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(25)));
            auto self = shared_from_this();
            timer_.async_wait(boost::asio::bind_executor(connection_->write_strand, [self](const auto &ec) {
              if (!ec) self->poll_peer();
            }));
          }
        }
        if (ended) ended(reason);
      }

      bool restore_blocking() {
        if (!nonblocking_changed_) return true;
        SimpleWeb::error_code ec;
        connection_->socket->lowest_layer().non_blocking(previous_nonblocking_, ec);
        nonblocking_changed_ = false;
        return !ec;
      }

      // Called on the I/O strand with mutex_ held. The callback is invoked
      // after unlocking so it cannot invert the admission/transport lock order.
      std::function<void(pending_peer_end_e)> end_locked() {
        if (stopped_) return {};
        stopped_ = true;
        timer_.cancel();
        restore_blocking();
        return std::move(on_end_);
      }

      std::shared_ptr<Connection> connection_;
      boost::asio::steady_timer timer_;
      std::array<char, 1> read_buffer_ {};
      std::function<void(pending_peer_end_e)> on_end_;
      std::chrono::steady_clock::time_point deadline_;
      std::mutex mutex_;
      bool previous_nonblocking_ = false;
      bool nonblocking_changed_ = false;
      bool stopped_ = false;
      bool claimed_ = false;
    };
#endif

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              SimpleWeb::error_code remote_endpoint_ec;
              session->connection->socket->lowest_layer().remote_endpoint(remote_endpoint_ec);
              if (remote_endpoint_ec) {
                if (this->on_error) {
                  this->on_error(session->request, remote_endpoint_ec);
                }
                return;
              }

              pending_peer_factory_t make_pending_peer;
#if defined(__linux__) && !defined(_WIN32)
              make_pending_peer = [weak = std::weak_ptr<Connection>(session->connection)]() -> std::shared_ptr<pending_peer_t> {
                if (auto connection = weak.lock()) return std::make_shared<pending_peer_watch_t>(std::move(connection));
                return {};
              };
#endif
              if (verify && !verify(session->request, session->connection->socket->native_handle(), std::move(make_pending_peer))) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

}  // namespace nvhttp
