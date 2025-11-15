/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_helper_integration.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "state_storage.h"
#ifdef _WIN32
  #include "platform/windows/display_helper_request_helpers.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
#endif
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "update.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"

using namespace std::literals;

namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

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

    std::function<int(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

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
              if (verify && !verify(session->connection->socket->native_handle())) {
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

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

#ifdef _WIN32
  namespace {
    bool has_any_active_display() {
      if (VDISPLAY::has_active_physical_display()) {
        return true;
      }
      const auto virtual_displays = VDISPLAY::enumerateSudaVDADisplays();
      return std::any_of(
        virtual_displays.begin(),
        virtual_displays.end(),
        [](const VDISPLAY::SudaVDADisplayInfo &info) {
          return info.is_active;
        }
      );
    }

    bool wait_for_display_activation(std::chrono::steady_clock::duration timeout) {
      if (timeout <= std::chrono::steady_clock::duration::zero()) {
        return has_any_active_display();
      }

      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (has_any_active_display()) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      return has_any_active_display();
    }

    std::atomic<bool> virtual_display_cleanup_pending {false};

    void cleanup_virtual_display_state() {
      VDISPLAY::setWatchdogFeedingEnabled(false);
      VDISPLAY::removeAllVirtualDisplays();
      display_helper_integration::revert();
    }

    void schedule_virtual_display_cleanup() {
      bool expected = false;
      if (!virtual_display_cleanup_pending.compare_exchange_strong(expected, true)) {
        return;
      }

      std::thread([] {
        auto guard = util::fail_guard([]() {
          virtual_display_cleanup_pending.store(false, std::memory_order_release);
        });
        try {
          cleanup_virtual_display_state();
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Virtual display cleanup failed: " << e.what();
        } catch (...) {
          BOOST_LOG(warning) << "Virtual display cleanup failed with an unknown exception.";
        }
      }).detach();
    }
  }  // namespace
#endif

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
  };

  struct client_t {
    std::vector<named_cert_t> named_devices;
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  void save_state() {
    statefile::migrate_recent_state_keys();
    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();

    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    pt::ptree root;

    if (fs::exists(sunshine_path)) {
      try {
        pt::read_json(sunshine_path, root);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();
        return;
      }
    }

    pt::ptree root_node;
    if (auto existing_root = root.get_child_optional("root")) {
      root_node = *existing_root;
    }

    root_node.put("uniqueid", http::unique_id);
    client_t &client = client_root;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root_node.put_child("named_devices", named_cert_nodes);
    root.put_child("root", root_node);

    try {
      pt::write_json(sunshine_path, root);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << sunshine_path << ": "sv << e.what();
      return;
    }

    if (!vibeshine_path.empty()) {
      auto ensure_root = [](pt::ptree &tree) -> pt::ptree & {
        auto it = tree.find("root");
        if (it == tree.not_found()) {
          auto inserted = tree.insert(tree.end(), std::make_pair(std::string("root"), pt::ptree {}));
          return inserted->second;
        }
        return it->second;
      };

      pt::ptree vibeshine_tree;
      if (fs::exists(vibeshine_path)) {
        try {
          pt::read_json(vibeshine_path, vibeshine_tree);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Couldn't read "sv << vibeshine_path << ": "sv << e.what();
          vibeshine_tree = {};
        }
      }

      auto &vibe_root = ensure_root(vibeshine_tree);
      vibe_root.put("last_notified_version", update::state.last_notified_version);

#ifdef _WIN32
      if (!http::shared_virtual_display_guid.empty()) {
        vibe_root.put("shared_virtual_display_guid", http::shared_virtual_display_guid);
      }
#endif

      try {
        pt::write_json(vibeshine_path, vibeshine_tree);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't write "sv << vibeshine_path << ": "sv << e.what();
      }
    }
  }

  void load_state() {
    statefile::migrate_recent_state_keys();
    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();

    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    if (!fs::exists(sunshine_path)) {
      BOOST_LOG(info) << "File "sv << sunshine_path << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      update::state.last_notified_version.clear();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(sunshine_path, tree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    if (!vibeshine_path.empty() && fs::exists(vibeshine_path)) {
      try {
        pt::ptree vibeshine_tree;
        pt::read_json(vibeshine_path, vibeshine_tree);
        update::state.last_notified_version = vibeshine_tree.get("root.last_notified_version", "");
#ifdef _WIN32
        http::shared_virtual_display_guid = vibeshine_tree.get("root.shared_virtual_display_guid", "");
#endif
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Couldn't read "sv << vibeshine_path << " for notification state: "sv << e.what();
        update::state.last_notified_version.clear();
#ifdef _WIN32
        http::shared_virtual_display_guid.clear();
#endif
      }
    } else {
      update::state.last_notified_version.clear();
#ifdef _WIN32
      http::shared_virtual_display_guid.clear();
#endif
    }

    client_t client;

    if (auto root = tree.get_child_optional("root")) {
      // Import from old format
      if (auto device_nodes = root->get_child_optional("devices")) {
        for (auto &[_, device_node] : *device_nodes) {
          auto uniqID = device_node.get<std::string>("uniqueid");

          if (device_node.count("certs")) {
            for (auto &[_, el] : device_node.get_child("certs")) {
              named_cert_t named_cert;
              named_cert.name = ""s;
              named_cert.cert = el.get_value<std::string>();
              named_cert.uuid = uuid_util::uuid_t::generate().string();
              client.named_devices.emplace_back(named_cert);
            }
          }
        }
      }

      if (root->count("named_devices")) {
        for (auto &[_, el] : root->get_child("named_devices")) {
          named_cert_t named_cert;
          named_cert.name = el.get_child("name").get_value<std::string>();
          named_cert.cert = el.get_child("cert").get_value<std::string>();
          named_cert.uuid = el.get_child("uuid").get_value<std::string>();
          client.named_devices.emplace_back(named_cert);
        }
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void add_authorized_client(const std::string &name, std::string &&cert) {
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    client.named_devices.emplace_back(named_cert);

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  // Thread-local storage for peer certificate during SSL verification
  thread_local crypto::x509_t tl_peer_certificate;

  std::string get_client_uuid_from_peer_cert(const crypto::x509_t &client_cert, std::string *client_name_out = nullptr) {
    if (!client_cert) {
      BOOST_LOG(debug) << "No client certificate available";
      return {};
    }

    auto client_cert_signature = crypto::signature(const_cast<X509 *>(client_cert.get()));

    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      auto stored_x509 = crypto::x509(named_cert.cert);
      if (stored_x509) {
        auto stored_signature = crypto::signature(stored_x509.get());
        if (stored_signature == client_cert_signature) {
          BOOST_LOG(debug) << "Found matching client UUID: " << named_cert.uuid << " for client: " << named_cert.name;
          if (client_name_out) {
            *client_name_out = named_cert.name;
          }
          return named_cert.uuid;
        }
      }
    }

    BOOST_LOG(debug) << "No matching client UUID found for certificate";
    return {};
  }

  std::string get_client_uuid_from_request(req_https_t request, std::string *client_name_out = nullptr) {
    // Try to use the peer certificate that was stored during SSL verification
    return get_client_uuid_from_peer_cert(tl_peer_certificate, client_name_out);
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, const args_t &args, req_https_t request = nullptr) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;
    launch_session->gen1_framegen_fix = false;
    launch_session->gen2_framegen_fix = false;
    launch_session->lossless_scaling_framegen = false;
    launch_session->framegen_refresh_rate.reset();
    launch_session->lossless_scaling_target_fps.reset();
    launch_session->lossless_scaling_rtss_limit.reset();
    launch_session->frame_generation_provider = "lossless-scaling";
    launch_session->device_name = config::nvhttp.sunshine_name;
    launch_session->virtual_display = false;
    launch_session->virtual_display_guid_bytes.fill(0);
    launch_session->virtual_display_device_id.clear();
    launch_session->virtual_display_ready_since.reset();
    launch_session->app_metadata.reset();
    launch_session->client_uuid.clear();
    launch_session->client_name.clear();

    if (request) {
      launch_session->client_uuid = get_client_uuid_from_request(request, &launch_session->client_name);
    }

    auto client_name_arg = get_arg(args, "clientName", "");
    if (!client_name_arg.empty()) {
      launch_session->device_name = client_name_arg;
    }

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        launch_session->fps = atoi(segment.c_str());
      }
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = util::from_view(get_arg(args, "appid", "unknown"));
    if (launch_session->appid > 0) {
      try {
        auto apps_snapshot = proc::proc.get_apps();
        const std::string app_id_str = std::to_string(launch_session->appid);
        for (const auto &app_ctx : apps_snapshot) {
          if (app_ctx.id == app_id_str) {
            launch_session->gen1_framegen_fix = app_ctx.gen1_framegen_fix;
            launch_session->gen2_framegen_fix = app_ctx.gen2_framegen_fix;
            launch_session->lossless_scaling_framegen = app_ctx.lossless_scaling_framegen;
            launch_session->lossless_scaling_target_fps = app_ctx.lossless_scaling_target_fps;
            launch_session->lossless_scaling_rtss_limit = app_ctx.lossless_scaling_rtss_limit;
            launch_session->frame_generation_provider = app_ctx.frame_generation_provider;
            rtsp_stream::launch_session_t::app_metadata_t metadata;
            metadata.id = app_ctx.id;
            metadata.name = app_ctx.name;
            metadata.virtual_screen = app_ctx.virtual_screen;
            metadata.has_command = !app_ctx.cmd.empty();
            metadata.has_playnite = !app_ctx.playnite_id.empty();
            launch_session->virtual_display = app_ctx.virtual_screen;
            launch_session->virtual_display_mode_override = app_ctx.virtual_display_mode_override;
            launch_session->virtual_display_layout_override = app_ctx.virtual_display_layout_override;
            launch_session->app_metadata = std::move(metadata);
            break;
          }
        }
      } catch (...) {
      }
    }

    const auto apply_refresh_override = [&](int candidate) {
      if (candidate <= 0) {
        return;
      }
      if (!launch_session->framegen_refresh_rate || candidate > *launch_session->framegen_refresh_rate) {
        launch_session->framegen_refresh_rate = candidate;
      }
    };

    launch_session->framegen_refresh_rate.reset();
    if (launch_session->fps > 0) {
      const auto saturating_double = [](int value) -> int {
        if (value > std::numeric_limits<int>::max() / 2) {
          return std::numeric_limits<int>::max();
        }
        return value * 2;
      };

      if (launch_session->gen1_framegen_fix || launch_session->gen2_framegen_fix) {
        apply_refresh_override(saturating_double(launch_session->fps));
      }
    }
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key,
        false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
    } else {
      tree.put("root.paired", 0);
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template<class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(verbose) << "HTTP "sv << request->method << ' ' << request->path << " tunnel="sv << tunnel<T>::to_string;

    if (!request->header.empty()) {
      BOOST_LOG(verbose) << "Headers:"sv;
      for (auto &[name, val] : request->header) {
        BOOST_LOG(verbose) << name << " -- " << val;
      }
    }

    auto query = request->parse_query_string();
    if (!query.empty()) {
      BOOST_LOG(verbose) << "Query Params:"sv;
      for (auto &[name, val] : query) {
        BOOST_LOG(verbose) << name << " -- " << val;
      }
    }
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template<class T>
  void pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(verbose) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
        } else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin);
    sess.client.name = name;

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    } else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    } else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration {false};
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (revert_display_configuration) {
        display_helper_integration::revert();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    // Prevent interleaving with hot-apply while we prep/start a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    auto launch_session = make_launch_session(host_audio, args, request);
    const bool no_active_sessions = (rtsp_stream::session_count() == 0);

#ifdef _WIN32
    bool config_requests_virtual = config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled;
    BOOST_LOG(debug) << "config_requests_virtual: " << config_requests_virtual;
    const bool session_requests_virtual = launch_session->app_metadata && launch_session->app_metadata->virtual_screen;
    BOOST_LOG(debug) << "session_requests_virtual: " << session_requests_virtual;
    bool request_virtual_display = config_requests_virtual || session_requests_virtual;
    BOOST_LOG(debug) << "request_virtual_display: " << request_virtual_display;

    auto apply_virtual_display_request = [&](bool should_request_virtual_display) {
      if (!should_request_virtual_display) {
        launch_session->virtual_display = false;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
        launch_session->virtual_display_ready_since.reset();
        return;
      }

      if (!no_active_sessions) {
        auto existing_device = VDISPLAY::resolveAnyVirtualDisplayDeviceId();
        if (existing_device) {
          launch_session->virtual_display = true;
          launch_session->virtual_display_device_id = *existing_device;
          launch_session->virtual_display_ready_since = std::chrono::steady_clock::now();
          BOOST_LOG(info) << "Virtual display already active (device_id=" << *existing_device
                          << "). Skipping additional creation because another session is running.";
        } else {
          launch_session->virtual_display = false;
          launch_session->virtual_display_device_id.clear();
          launch_session->virtual_display_ready_since.reset();
          BOOST_LOG(info) << "Skipping virtual display creation because another session is running and no reusable device was found.";
        }
        launch_session->virtual_display_guid_bytes.fill(0);
        return;
      }

      if (proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        proc::initVDisplayDriver();
        if (proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
          BOOST_LOG(warning) << "SudaVDA driver unavailable (status=" << static_cast<int>(proc::vDisplayDriverStatus) << "). Continuing with best-effort virtual display creation.";
        }
      }
      if (!config::video.adapter_name.empty()) {
        (void) VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
      } else {
        (void) VDISPLAY::setRenderAdapterWithMostDedicatedMemory();
      }

      auto parse_uuid = [](const std::string &value) -> std::optional<uuid_util::uuid_t> {
        if (value.empty()) {
          return std::nullopt;
        }
        try {
          return uuid_util::uuid_t::parse(value);
        } catch (...) {
          return std::nullopt;
        }
      };

      auto ensure_shared_guid = [&]() -> uuid_util::uuid_t {
        if (!http::shared_virtual_display_guid.empty()) {
          if (auto parsed = parse_uuid(http::shared_virtual_display_guid)) {
            return *parsed;
          }
        }
        auto generated = VDISPLAY::persistentVirtualDisplayUuid();
        http::shared_virtual_display_guid = generated.string();
        nvhttp::save_state();
        return generated;
      };

      const bool shared_mode = (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::shared);
      uuid_util::uuid_t session_uuid;
      if (shared_mode) {
        session_uuid = ensure_shared_guid();
        launch_session->unique_id = session_uuid.string();
      } else if (auto parsed = parse_uuid(launch_session->unique_id)) {
        session_uuid = *parsed;
      } else {
        session_uuid = VDISPLAY::persistentVirtualDisplayUuid();
        launch_session->unique_id = session_uuid.string();
      }

      std::string display_uuid_source;
      if (!shared_mode && !launch_session->client_uuid.empty()) {
        display_uuid_source = launch_session->client_uuid;
        BOOST_LOG(debug) << "Using client UUID for virtual display: " << display_uuid_source;
      } else {
        display_uuid_source = session_uuid.string();
        BOOST_LOG(debug) << "Using session UUID for virtual display: " << display_uuid_source;
      }

      GUID virtual_display_guid {};
      if (!shared_mode && !launch_session->client_uuid.empty()) {
        if (auto client_uuid_parsed = parse_uuid(launch_session->client_uuid)) {
          std::memcpy(&virtual_display_guid, client_uuid_parsed->b8, sizeof(virtual_display_guid));
          std::copy_n(std::cbegin(client_uuid_parsed->b8), sizeof(client_uuid_parsed->b8), launch_session->virtual_display_guid_bytes.begin());
        } else {
          std::memcpy(&virtual_display_guid, session_uuid.b8, sizeof(virtual_display_guid));
          std::copy_n(std::cbegin(session_uuid.b8), sizeof(session_uuid.b8), launch_session->virtual_display_guid_bytes.begin());
        }
      } else {
        std::memcpy(&virtual_display_guid, session_uuid.b8, sizeof(virtual_display_guid));
        std::copy_n(std::cbegin(session_uuid.b8), sizeof(session_uuid.b8), launch_session->virtual_display_guid_bytes.begin());
      }

      uint32_t vd_width = launch_session->width > 0 ? static_cast<uint32_t>(launch_session->width) : 1920u;
      uint32_t vd_height = launch_session->height > 0 ? static_cast<uint32_t>(launch_session->height) : 1080u;
      uint32_t vd_fps = 0;
      if (launch_session->framegen_refresh_rate && *launch_session->framegen_refresh_rate > 0) {
        vd_fps = static_cast<uint32_t>(*launch_session->framegen_refresh_rate);
      } else if (launch_session->fps > 0) {
        vd_fps = static_cast<uint32_t>(launch_session->fps);
      } else {
        vd_fps = 60000u;
      }
      if (vd_fps < 1000u) {
        vd_fps *= 1000u;
      }

      std::string client_label;
      if (shared_mode) {
        client_label = config::nvhttp.sunshine_name.empty() ? "Sunshine Shared Display" : config::nvhttp.sunshine_name + " Shared";
      } else {
        if (!launch_session->client_name.empty()) {
          client_label = launch_session->client_name;
        } else if (!launch_session->device_name.empty()) {
          client_label = launch_session->device_name;
        } else {
          client_label = config::nvhttp.sunshine_name;
        }
        if (client_label.empty()) {
          client_label = "Sunshine";
        }
      }

      VDISPLAY::setWatchdogFeedingEnabled(true);
      auto display_info = VDISPLAY::createVirtualDisplay(
        display_uuid_source.c_str(),
        client_label.c_str(),
        vd_width,
        vd_height,
        vd_fps,
        virtual_display_guid
      );

      if (display_info) {
        launch_session->virtual_display = true;
        if (display_info->device_id && !display_info->device_id->empty()) {
          launch_session->virtual_display_device_id = *display_info->device_id;
        } else if (auto resolved_device = VDISPLAY::resolveAnyVirtualDisplayDeviceId()) {
          launch_session->virtual_display_device_id = *resolved_device;
        } else {
          launch_session->virtual_display_device_id.clear();
        }
        launch_session->virtual_display_ready_since = display_info->ready_since;
        if (display_info->display_name && !display_info->display_name->empty()) {
          BOOST_LOG(info) << "Virtual display created at " << platf::to_utf8(*display_info->display_name);
        } else {
          BOOST_LOG(info) << "Virtual display created (device name pending enumeration).";
        }

        if (!display_info->reused_existing) {
          VDISPLAY::VirtualDisplayRecoveryParams recovery_params;
          recovery_params.guid = virtual_display_guid;
          recovery_params.width = vd_width;
          recovery_params.height = vd_height;
          recovery_params.fps = vd_fps;
          recovery_params.client_uid = display_uuid_source;
          recovery_params.client_name = client_label;
          recovery_params.display_name = display_info->display_name;
          if (display_info->device_id && !display_info->device_id->empty()) {
            recovery_params.device_id = *display_info->device_id;
          } else if (!launch_session->virtual_display_device_id.empty()) {
            recovery_params.device_id = launch_session->virtual_display_device_id;
          }
          recovery_params.max_attempts = 3;

          std::weak_ptr<rtsp_stream::launch_session_t> session_weak = launch_session;
          recovery_params.should_abort = [session_weak]() {
            return session_weak.expired();
          };
          recovery_params.on_recovery_success = [session_weak](const VDISPLAY::VirtualDisplayCreationResult &info) {
            if (auto session_locked = session_weak.lock()) {
              if (info.device_id && !info.device_id->empty()) {
                session_locked->virtual_display_device_id = *info.device_id;
              }
              session_locked->virtual_display_ready_since = info.ready_since;
            }
          };

          VDISPLAY::schedule_virtual_display_recovery_monitor(recovery_params);
        }
      } else {
        launch_session->virtual_display = false;
        launch_session->virtual_display_guid_bytes.fill(0);
        launch_session->virtual_display_device_id.clear();
        launch_session->virtual_display_ready_since.reset();
        BOOST_LOG(warning) << "Virtual display creation failed.";
      }
    };

    if (!request_virtual_display && VDISPLAY::should_auto_enable_virtual_display()) {
      BOOST_LOG(info) << "No physical monitors detected. Automatically enabling virtual display.";
      request_virtual_display = true;
    }
    apply_virtual_display_request(request_virtual_display);
#endif

    // The display should be restored in case something fails as there are no other sessions.
    if (no_active_sessions) {
      revert_display_configuration = true;

#ifdef _WIN32
      HANDLE user_token = platf::retrieve_users_token(false);
      const bool helper_session_available = (user_token != nullptr);
      if (user_token) {
        CloseHandle(user_token);
      }

      if (helper_session_available) {
        auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
        if (!request) {
          BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
        }

        if (request && !display_helper_integration::apply(*request)) {
          BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
        }
      } else {
        BOOST_LOG(warning) << "Display helper: unable to apply display preferences because there isn't a user signed in currently.";
      }
#else
      display_helper_integration::DisplayApplyBuilder noop_builder;
      noop_builder.set_session(*launch_session);
      if (!display_helper_integration::apply(noop_builder.build())) {
        BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
      }
#endif

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      bool encoder_probe_failed = video::probe_encoders();

#ifdef _WIN32
      if (encoder_probe_failed && !has_any_active_display()) {
        BOOST_LOG(info) << "Encoder probe failed with no active display; waiting for activation before retry.";
        constexpr auto kDisplayActivationTimeout = std::chrono::seconds(5);
        if (wait_for_display_activation(kDisplayActivationTimeout)) {
          BOOST_LOG(info) << "Display became active; retrying encoder probe.";
          encoder_probe_failed = video::probe_encoders();
        } else {
          BOOST_LOG(warning) << "Timed out waiting for a display to become active before retrying encoder probe.";
        }
      }
#endif

      if (encoder_probe_failed) {
        BOOST_LOG(error) << "Failed to initialize video capture/encoding. Is a display connected and turned on?";
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute(appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.gamesession", 1);
    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);

    rtsp_stream::launch_session_raise(launch_session);

    // Stream was started successfully, we will revert the config when the app or session terminates
    revert_display_configuration = false;
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    // Prevent interleaving with hot-apply while we prep/resume a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    const auto launch_session = make_launch_session(host_audio, args, request);

    if (no_active_sessions) {
      const bool should_reapply_display = config::video.dd.config_revert_on_disconnect;
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.

      bool display_apply_attempted = false;
      bool display_apply_failed = false;
#ifdef _WIN32
      if (should_reapply_display) {
        HANDLE user_token = platf::retrieve_users_token(false);
        const bool helper_session_available = (user_token != nullptr);
        if (user_token) {
          CloseHandle(user_token);
        }

        display_apply_attempted = true;
        if (helper_session_available) {
          auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
          if (!request) {
            BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
            display_apply_failed = true;
          } else if (!display_helper_integration::apply(*request)) {
            display_apply_failed = true;
            BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
          }
        } else {
          display_apply_failed = true;
          BOOST_LOG(warning) << "Display helper: unable to apply display preferences because there isn't a user signed in currently.";
        }
      } else {
        BOOST_LOG(debug) << "Display helper: skipping resume re-apply because revert-on-disconnect is disabled.";
      }
#else
      if (should_reapply_display) {
        display_apply_attempted = true;
        display_helper_integration::DisplayApplyBuilder noop_builder;
        noop_builder.set_session(*launch_session);
        if (!display_helper_integration::apply(noop_builder.build())) {
          display_apply_failed = true;
          BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
        }
      } else {
        BOOST_LOG(debug) << "Display helper: skipping resume re-apply because revert-on-disconnect is disabled.";
      }
#endif

      if (display_apply_attempted && display_apply_failed) {
        const bool no_display_available = !has_any_active_display();
        if (no_display_available) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 503);
          tree.put("root.<xmlattr>.status_message", "Failed to apply display configuration before streaming.");
          return;
        }

        BOOST_LOG(warning) << "Display helper: failed to re-apply display configuration on resume; continuing with existing display.";
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      bool encoder_probe_failed = video::probe_encoders();

#ifdef _WIN32
      if (encoder_probe_failed && !has_any_active_display()) {
        BOOST_LOG(info) << "Resume encoder probe failed with no active display; waiting for activation before retry.";
        constexpr auto kDisplayActivationTimeout = std::chrono::seconds(5);
        if (wait_for_display_activation(kDisplayActivationTimeout)) {
          BOOST_LOG(info) << "Display became active; retrying resume encoder probe.";
          encoder_probe_failed = video::probe_encoders();
        } else {
          BOOST_LOG(warning) << "Timed out waiting for a display to become active before retrying resume encoder probe.";
        }
      }
#endif

      if (encoder_probe_failed) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.resume", 1);
    tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }
    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.

#ifdef _WIN32
    schedule_virtual_display_cleanup();
#endif
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [add_cert](SSL *ssl) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };

      // Store peer certificate in thread-local storage for use in request handlers
      if (x509) {
        tl_peer_certificate = std::move(x509);
      }

      // Re-fetch for verification logic
      crypto::x509_t x509_verify {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };

      if (!x509_verify) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509_verify.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(verbose) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(verbose) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      auto err_str = cert_chain.verify(x509_verify.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SunshineHTTPS>(add_cert, resp, req);
    };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) {
      pair<SimpleWeb::HTTP>(add_cert, resp, req);
    };

    http_server.config.reuse_address = true;
    http_server.config.address = net::af_to_any_address_string(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
  }

  // (Windows-only) display_helper_integration is included above

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();
    return removed;
  }
}  // namespace nvhttp
