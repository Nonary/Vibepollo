/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
#include "stream.h"
#include "system_tray.h"
#include "update.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "zwpad.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  using p_named_cert_t = crypto::p_named_cert_t;
  using PERM = crypto::PERM;

  struct client_t {
    std::vector<p_named_cert_t> named_devices;
  };

  struct pair_session_t;

  crypto::cert_chain_t cert_chain;
  static std::shared_ptr<safe::queue_t<crypto::x509_t>> pending_cert_queue =
    std::make_shared<safe::queue_t<crypto::x509_t>>(30);
  static std::string one_time_pin;
  static std::string otp_passphrase;
  static std::string otp_device_name;
  static std::chrono::time_point<std::chrono::steady_clock> otp_creation_time;
  thread_local crypto::x509_t tl_peer_certificate;

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

    std::function<bool(std::shared_ptr<Request>, SSL *)> verify;
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
              if (verify && !verify(session->request, session->connection->socket->native_handle())) {
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
        // If a new session spun up while we were scheduling cleanup, leave displays alone.
        if (rtsp_stream::session_count() > 0) {
          BOOST_LOG(info) << "Skipping virtual display cleanup because a streaming session is active.";
          return;
        }

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

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  // Helper function to extract command entries from a JSON object.
  cmd_list_t extract_command_entries(const nlohmann::json &j, const std::string &key) {
    cmd_list_t commands;

    if (j.contains(key)) {
      try {
        for (const auto &item : j.at(key)) {
          try {
            std::string cmd = item.at("cmd").get<std::string>();
            bool elevated = util::get_non_string_json_value<bool>(item, "elevated", false);
            commands.push_back({cmd, elevated});
          } catch (const std::exception &e) {
            BOOST_LOG(warning) << "Error parsing command entry: " << e.what();
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Error retrieving key \"" << key << "\": " << e.what();
      }
    } else {
      BOOST_LOG(debug) << "Key \"" << key << "\" not found in the JSON.";
    }

    return commands;
  }

  std::optional<config::video_t::virtual_display_mode_e> parse_virtual_display_mode_override(const std::string &value) {
    if (value.empty()) {
      return std::nullopt;
    }
    const auto normalized = boost::algorithm::to_lower_copy(value);
    if (normalized == "disabled") {
      return config::video_t::virtual_display_mode_e::disabled;
    }
    if (normalized == "per_client") {
      return config::video_t::virtual_display_mode_e::per_client;
    }
    if (normalized == "shared") {
      return config::video_t::virtual_display_mode_e::shared;
    }
    return std::nullopt;
  }

  std::optional<config::video_t::virtual_display_layout_e> parse_virtual_display_layout_override(const std::string &value) {
    if (value.empty()) {
      return std::nullopt;
    }
    const auto normalized = boost::algorithm::to_lower_copy(value);
    if (normalized == "exclusive") {
      return config::video_t::virtual_display_layout_e::exclusive;
    }
    if (normalized == "extended") {
      return config::video_t::virtual_display_layout_e::extended;
    }
    if (normalized == "extended_primary") {
      return config::video_t::virtual_display_layout_e::extended_primary;
    }
    if (normalized == "extended_isolated") {
      return config::video_t::virtual_display_layout_e::extended_isolated;
    }
    if (normalized == "extended_primary_isolated") {
      return config::video_t::virtual_display_layout_e::extended_primary_isolated;
    }
    return std::nullopt;
  }

  void save_state() {
    statefile::migrate_recent_state_keys();
    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();
    const bool share_state_file = statefile::share_state_file();

    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    nlohmann::json root = nlohmann::json::object();
    if (fs::exists(sunshine_path)) {
      try {
        std::ifstream in(sunshine_path);
        in >> root;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();
        root = nlohmann::json::object();
      }
    }

    root["root"] = nlohmann::json::object();
    root["root"]["uniqueid"] = http::unique_id;
    if (share_state_file) {
      root["root"]["last_notified_version"] = update::state.last_notified_version;
    }

    client_t &client = client_root;
    nlohmann::json named_cert_nodes = nlohmann::json::array();

    std::unordered_set<std::string> unique_certs;
    std::unordered_map<std::string, int> name_counts;

    for (auto &named_cert_p : client.named_devices) {
      if (unique_certs.insert(named_cert_p->cert).second) {
        nlohmann::json named_cert_node = nlohmann::json::object();
        std::string base_name = named_cert_p->name;
        size_t pos = base_name.find(" (");
        if (pos != std::string::npos) {
          base_name = base_name.substr(0, pos);
        }
        int count = name_counts[base_name]++;
        std::string final_name = base_name;
        if (count > 0) {
          final_name += " (" + std::to_string(count + 1) + ")";
        }
        named_cert_node["name"] = final_name;
        named_cert_node["cert"] = named_cert_p->cert;
        named_cert_node["uuid"] = named_cert_p->uuid;
        named_cert_node["display_mode"] = named_cert_p->display_mode;
        if (!named_cert_p->virtual_display_mode_override.empty()) {
          named_cert_node["virtual_display_mode"] = named_cert_p->virtual_display_mode_override;
        }
        if (!named_cert_p->virtual_display_layout_override.empty()) {
          named_cert_node["virtual_display_layout"] = named_cert_p->virtual_display_layout_override;
        }
        named_cert_node["perm"] = static_cast<uint32_t>(named_cert_p->perm);
        named_cert_node["enable_legacy_ordering"] = named_cert_p->enable_legacy_ordering;
        named_cert_node["allow_client_commands"] = named_cert_p->allow_client_commands;
        named_cert_node["always_use_virtual_display"] = named_cert_p->always_use_virtual_display;

        if (!named_cert_p->do_cmds.empty()) {
          nlohmann::json do_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->do_cmds) {
            do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["do"] = do_cmds_node;
        }

        if (!named_cert_p->undo_cmds.empty()) {
          nlohmann::json undo_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->undo_cmds) {
            undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["undo"] = undo_cmds_node;
        }

        named_cert_nodes.push_back(named_cert_node);
      }
    }

    root["root"]["named_devices"] = named_cert_nodes;

    try {
      auto sunshine_dir = fs::path(sunshine_path).parent_path();
      if (!sunshine_dir.empty() && !fs::exists(sunshine_dir)) {
        fs::create_directories(sunshine_dir);
      }
      std::ofstream out(sunshine_path);
      out << root.dump(4);
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << sunshine_path << ": "sv << e.what();
      return;
    }

    if (!share_state_file) {
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
        } catch (const std::exception &e) {
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
        auto vibe_dir = fs::path(vibeshine_path).parent_path();
        if (!vibe_dir.empty() && !fs::exists(vibe_dir)) {
          fs::create_directories(vibe_dir);
        }
        pt::write_json(vibeshine_path, vibeshine_tree);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Couldn't write "sv << vibeshine_path << ": "sv << e.what();
      }
    }
  }

  void load_state() {
    statefile::migrate_recent_state_keys();
    const auto &sunshine_path = statefile::sunshine_state_path();
    const auto &vibeshine_path = statefile::vibeshine_state_path();
    const bool share_state_file = statefile::share_state_file();

    std::lock_guard<std::mutex> state_lock(statefile::state_mutex());

    if (!fs::exists(sunshine_path)) {
      BOOST_LOG(info) << "File "sv << sunshine_path << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      update::state.last_notified_version.clear();
      return;
    }

    nlohmann::json tree;
    try {
      std::ifstream in(sunshine_path);
      in >> tree;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << sunshine_path << ": "sv << e.what();
      return;
    }

    nlohmann::json root = tree.contains("root") ? tree["root"] : nlohmann::json::object();

    if (share_state_file) {
      update::state.last_notified_version = root.value("last_notified_version", "");
    } else if (fs::exists(vibeshine_path)) {
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

#ifdef _WIN32
    if (share_state_file && !root.contains("shared_virtual_display_guid")) {
      http::shared_virtual_display_guid.clear();
    }
#endif

    if (!root.contains("uniqueid")) {
      http::uuid = uuid_util::uuid_t::generate();
      http::unique_id = http::uuid.string();
      return;
    }

    std::string uid = root["uniqueid"];
    http::uuid = uuid_util::uuid_t::parse(uid);
    http::unique_id = uid;



    client_t client;

    if (root.contains("devices")) {
      for (auto &device_node : root["devices"]) {
        if (device_node.contains("certs")) {
          for (auto &el : device_node["certs"]) {
            auto named_cert_p = std::make_shared<crypto::named_cert_t>();
            named_cert_p->name = "";
            named_cert_p->cert = el.get<std::string>();
            named_cert_p->uuid = uuid_util::uuid_t::generate().string();
            named_cert_p->display_mode = "";
            named_cert_p->perm = PERM::_all;
            named_cert_p->enable_legacy_ordering = true;
            named_cert_p->allow_client_commands = true;
            named_cert_p->always_use_virtual_display = false;
            client.named_devices.emplace_back(named_cert_p);
          }
        }
      }
    }

    if (root.contains("named_devices")) {
      for (auto &el : root["named_devices"]) {
        auto named_cert_p = std::make_shared<crypto::named_cert_t>();
        named_cert_p->name = el.value("name", "");
        named_cert_p->cert = el.value("cert", "");
        named_cert_p->uuid = el.value("uuid", "");
        named_cert_p->display_mode = el.value("display_mode", "");
        named_cert_p->virtual_display_mode_override = el.value("virtual_display_mode", "");
        named_cert_p->virtual_display_layout_override = el.value("virtual_display_layout", "");
        named_cert_p->perm = (PERM) (util::get_non_string_json_value<uint32_t>(el, "perm", (uint32_t) PERM::_all)) & PERM::_all;
        named_cert_p->enable_legacy_ordering = util::get_non_string_json_value<bool>(el, "enable_legacy_ordering", true);
        named_cert_p->allow_client_commands = util::get_non_string_json_value<bool>(el, "allow_client_commands", true);
        named_cert_p->always_use_virtual_display = util::get_non_string_json_value<bool>(el, "always_use_virtual_display", false);
        named_cert_p->do_cmds = extract_command_entries(el, "do");
        named_cert_p->undo_cmds = extract_command_entries(el, "undo");
        client.named_devices.emplace_back(named_cert_p);
      }
    }

    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(named_cert);
    }

    client_root = client;
  }

  void add_authorized_client(const p_named_cert_t &named_cert_p) {
    client_t &client = client_root;
    client.named_devices.push_back(named_cert_p);

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_paired(named_cert_p->name);
#endif

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
      load_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, bool input_only, const args_t &args, const crypto::named_cert_t *named_cert_p) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;
    launch_session->appid = 0;
    launch_session->gen1_framegen_fix = false;
    launch_session->gen2_framegen_fix = false;
    launch_session->lossless_scaling_framegen = false;
    launch_session->framegen_refresh_rate.reset();
    launch_session->lossless_scaling_target_fps.reset();
    launch_session->lossless_scaling_rtss_limit.reset();
    launch_session->frame_generation_provider = "lossless-scaling";
#ifdef _WIN32
#endif
    launch_session->device_name = named_cert_p->name.empty() ? config::nvhttp.sunshine_name : named_cert_p->name;
    launch_session->virtual_display = false;
    launch_session->virtual_display_guid_bytes.fill(0);
    launch_session->virtual_display_device_id.clear();
    launch_session->app_metadata.reset();

    auto client_name_arg = get_arg(args, "clientName", "");
    if (launch_session->device_name.empty() && !client_name_arg.empty()) {
      launch_session->device_name = client_name_arg;
    }

    // If launched from client
    if (named_cert_p->uuid != http::unique_id) {
      auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
      std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

      launch_session->host_audio = host_audio;

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
    }

    std::stringstream mode;
    if (named_cert_p->display_mode.empty()) {
      auto mode_str = get_arg(args, "mode", config::video.fallback_mode.c_str());
      mode = std::stringstream(mode_str);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name << "] requested to ["sv << mode_str << ']';
      launch_session->client_display_mode_override = false;
    } else {
      mode = std::stringstream(named_cert_p->display_mode);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name << "] overriden to ["sv << named_cert_p->display_mode << ']';
      launch_session->client_display_mode_override = true;
    }

    if (!named_cert_p->virtual_display_mode_override.empty()) {
      if (const auto parsed_mode = parse_virtual_display_mode_override(named_cert_p->virtual_display_mode_override)) {
        launch_session->virtual_display_mode_override = *parsed_mode;
      }
    }
    if (!named_cert_p->virtual_display_layout_override.empty()) {
      if (const auto parsed_layout = parse_virtual_display_layout_override(named_cert_p->virtual_display_layout_override)) {
        launch_session->virtual_display_layout_override = *parsed_layout;
      }
    }

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
        auto fps = atof(segment.c_str());
        if (fps < 1000) {
          fps *= 1000;
        };
        launch_session->fps = (int) fps;
        break;
      }
      x++;
    }

    // Parsing have failed or missing components
    if (x != 2) {
      launch_session->width = 1920;
      launch_session->height = 1080;
      launch_session->fps = 60000;  // 60fps * 1000 denominator
    }

    launch_session->device_name = named_cert_p->name.empty() ? "ApolloDisplay"s : named_cert_p->name;
    launch_session->unique_id = named_cert_p->uuid;
    launch_session->perm = named_cert_p->perm;
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
            metadata.virtual_screen = app_ctx.virtual_screen || app_ctx.virtual_display;
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

      if (config::video.double_refreshrate) {
        apply_refresh_override(saturating_double(launch_session->fps));
      }
    }
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    launch_session->virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0")) || named_cert_p->always_use_virtual_display;
    launch_session->scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));

    launch_session->client_do_cmds = named_cert_p->do_cmds;
    launch_session->client_undo_cmds = named_cert_p->undo_cmds;

    launch_session->input_only = input_only;

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
    BOOST_LOG(warning) << "Pair attempt failed due to " << status_msg;
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

  void clientpairingsecret(
    pair_session_t &sess,
    const std::shared_ptr<safe::queue_t<crypto::x509_t>> &pending_certs,
    pt::ptree &tree,
    const std::string &client_pairing_secret
  ) {
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

      auto named_cert_p = std::make_shared<crypto::named_cert_t>();
      named_cert_p->name = client.name;
      for (char &c : named_cert_p->name) {
        if (c == '(') {
          c = '[';
        } else if (c == ')') {
          c = ']';
        }
      }
      named_cert_p->cert = std::move(client.cert);
      named_cert_p->uuid = uuid_util::uuid_t::generate().string();
      // If the device is the first one paired with the server, assign full permission.
      if (client_root.named_devices.empty()) {
        named_cert_p->perm = PERM::_all;
      } else {
        named_cert_p->perm = PERM::_default;
      }

      named_cert_p->enable_legacy_ordering = true;
      named_cert_p->allow_client_commands = true;
      named_cert_p->always_use_virtual_display = false;

      auto it = map_id_sess.find(client.uniqueID);
      map_id_sess.erase(it);

      add_authorized_client(named_cert_p);

      if (pending_certs) {
        pending_certs->raise(crypto::x509(named_cert_p->cert));
      }
    } else {
      tree.put("root.paired", 0);
      BOOST_LOG(warning) << "Pair attempt failed due to same_hash: " << same_hash << ", verify: " << verify;
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    clientpairingsecret(sess, pending_cert_queue, tree, client_pairing_secret);
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

  inline crypto::named_cert_t *get_verified_cert(req_https_t request) {
    return (crypto::named_cert_t *) request->userp.get();
  }

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
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
    response->close_connection_after_response = true;
  }

  template<class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    if (!config::sunshine.enable_pairing) {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Pairing is disabled for this instance");

      return;
    }

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

        auto deviceName {get_arg(args, "devicename")};

        if (deviceName == "roth"sv) {
          deviceName = "Legacy Moonlight Client";
        }

        sess.client.uniqueID = std::move(uniqID);
        sess.client.name = std::move(deviceName);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(verbose) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));

        auto it = args.find("otpauth");
        if (it != std::end(args)) {
          if (one_time_pin.empty() || (std::chrono::steady_clock::now() - otp_creation_time > OTP_EXPIRE_DURATION)) {
            one_time_pin.clear();
            otp_passphrase.clear();
            otp_device_name.clear();
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "OTP auth not available.");
          } else {
            auto hash = util::hex(crypto::hash(one_time_pin + ptr->second.async_insert_pin.salt + otp_passphrase), true);

            if (hash.to_string_view() == it->second) {
              if (!otp_device_name.empty()) {
                ptr->second.client.name = std::move(otp_device_name);
              }

              getservercert(ptr->second, tree, one_time_pin);

              one_time_pin.clear();
              otp_passphrase.clear();
              otp_device_name.clear();
              return;
            }
          }

          // Always return positive, attackers will fail in the next steps.
          getservercert(ptr->second, tree, crypto::rand(16));
          return;
        }

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
      clientpairingsecret(sess_it->second, tree, pairingsecret);
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

    if (!name.empty()) {
      sess.client.name = name;
    }

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

      auto named_cert_p = get_verified_cert(request);
      if (!!(named_cert_p->perm & PERM::server_cmd)) {
        pt::ptree &root_node = tree.get_child("root");

        if (config::sunshine.server_cmds.size() > 0) {
          // Broadcast server_cmds
          for (const auto &cmd : config::sunshine.server_cmds) {
            pt::ptree cmd_node;
            cmd_node.put_value(cmd.cmd_name);
            root_node.push_back(std::make_pair("ServerCommand", cmd_node));
          }
        }
      } else {
        BOOST_LOG(debug) << "Permission Get ServerCommand denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";
      }

      tree.put("root.Permission", std::to_string((uint32_t) named_cert_p->perm));

#ifdef _WIN32
      tree.put("root.VirtualDisplayCapable", true);
      if (!!(named_cert_p->perm & PERM::_all_actions)) {
        tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);
      } else {
        tree.put("root.VirtualDisplayDriverReady", true);
      }
#endif
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
      tree.put("root.Permission", "0");
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

    tree.put("root.PairStatus", pair_status);

    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      int current_appid = proc::proc.running();
      // When input only mode is enabled, the only resume method should be launching the same app again.
      if (config::input.enable_input_only_mode && current_appid != proc::input_only_app_id) {
        current_appid = 0;
      }
      tree.put("root.currentgame", current_appid);
      tree.put("root.currentgameuuid", proc::proc.get_running_app_uuid());
      tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
    } else {
      tree.put("root.currentgame", 0);
      tree.put("root.currentgameuuid", "");
      tree.put("root.state", "SUNSHINE_SERVER_FREE");
    }

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert->name;
      named_cert_node["uuid"] = named_cert->uuid;
      named_cert_node["display_mode"] = named_cert->display_mode;
      named_cert_node["virtual_display_mode"] = named_cert->virtual_display_mode_override;
      named_cert_node["virtual_display_layout"] = named_cert->virtual_display_layout_override;
      named_cert_node["perm"] = static_cast<uint32_t>(named_cert->perm);
      named_cert_node["enable_legacy_ordering"] = named_cert->enable_legacy_ordering;
      named_cert_node["allow_client_commands"] = named_cert->allow_client_commands;
      named_cert_node["always_use_virtual_display"] = named_cert->always_use_virtual_display;

      // Add "do" commands if available
      if (!named_cert->do_cmds.empty()) {
        nlohmann::json do_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->do_cmds) {
          do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["do"] = do_cmds_node;
      }

      // Add "undo" commands if available
      if (!named_cert->undo_cmds.empty()) {
        nlohmann::json undo_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->undo_cmds) {
          undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["undo"] = undo_cmds_node;
      }

      // Determine connection status
      bool connected = false;
      if (connected_uuids.empty()) {
        connected = false;
      } else {
        for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
          if (*it == named_cert->uuid) {
            connected = true;
            connected_uuids.erase(it);
            break;
          }
        }
      }
      named_cert_node["connected"] = connected;

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

    auto named_cert_p = get_verified_cert(request);
    if (!!(named_cert_p->perm & PERM::_all_actions)) {
      auto current_appid = proc::proc.running();
      auto should_hide_inactive_apps = current_appid > 0 && current_appid != proc::input_only_app_id;

      auto app_list = proc::proc.get_apps();

      std::vector<const proc::ctx_t *> visible_apps;
      visible_apps.reserve(app_list.size());

      for (const auto &app : app_list) {
        auto appid = util::from_view(app.id);
        bool include = true;
        if (should_hide_inactive_apps) {
          if (
            appid != current_appid && appid != proc::input_only_app_id && appid != proc::terminate_app_id
          ) {
            include = false;
          }
        } else if (appid == proc::terminate_app_id) {
          include = false;
        }

        if (!include) {
          continue;
        }

        visible_apps.push_back(&app);
      }

      bool enable_legacy_ordering = config::sunshine.legacy_ordering && named_cert_p->enable_legacy_ordering;
      size_t bits = 0;
      if (enable_legacy_ordering && !visible_apps.empty()) {
        bits = zwpad::pad_width_for_count(visible_apps.size());
      }

      for (size_t i = 0; i < visible_apps.size(); ++i) {
        const auto &app = *visible_apps[i];

        std::string app_name;
        if (enable_legacy_ordering && bits > 0) {
          app_name = zwpad::pad_for_ordering(app.name, bits, i);
        } else {
          app_name = app.name;
        }

        pt::ptree app_node;

        app_node.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
        app_node.put("AppTitle"s, app_name);
        app_node.put("UUID", app.uuid);
        app_node.put("IDX", app.idx);
        app_node.put("ID", app.id);

        apps.push_back(std::make_pair("App", std::move(app_node)));
      }
    } else {
      BOOST_LOG(debug) << "Permission ListApp denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      pt::ptree app_node;

      app_node.put("IsHdrSupported"s, 0);
      app_node.put("AppTitle"s, "Permission Denied");
      app_node.put("UUID", "");
      app_node.put("IDX", "0");
      app_node.put("ID", "114514");

      apps.push_back(std::make_pair("App", std::move(app_node)));

      return;
    }
  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    bool revert_display_configuration = false;
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

    auto appid_str = get_arg(args, "appid", "0");
    auto appuuid_str = get_arg(args, "appuuid", "");
    auto appid = util::from_view(appid_str);
    auto current_appid = proc::proc.running();
    auto current_app_uuid = proc::proc.get_running_app_uuid();
    bool is_input_only = config::input.enable_input_only_mode && (appid == proc::input_only_app_id || (appuuid_str == REMOTE_INPUT_UUID));

    auto named_cert_p = get_verified_cert(request);
    auto perm = PERM::launch;

    BOOST_LOG(verbose) << "Launching app [" << appid_str << "] with UUID [" << appuuid_str << "]";
    // BOOST_LOG(verbose) << "QS: " << request->query_string;

    // If we have already launched an app, we should allow clients with view permission to join the input only or current app's session.
    if (
      current_appid > 0 && (appuuid_str != TERMINATE_APP_UUID || appid != proc::terminate_app_id) && (is_input_only || appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid))
    ) {
      perm = PERM::_allow_view;
    }

    if (!(named_cert_p->perm & perm)) {
      BOOST_LOG(debug) << "Permission LaunchApp denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    if (!is_input_only) {
      // Special handling for the "terminate" app
      if (
        (appid == proc::terminate_app_id && proc::terminate_app_id > 0) || appuuid_str == TERMINATE_APP_UUID
      ) {
        proc::proc.terminate();

        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 410);
        tree.put("root.<xmlattr>.status_message", "App terminated.");

        return;
      }

      if (
        current_appid > 0 && current_appid != proc::input_only_app_id && ((appid > 0 && appid != current_appid) || (!appuuid_str.empty() && appuuid_str != current_app_uuid))
      ) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

        return;
      }
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    // Prevent interleaving with hot-apply while we prep/start a session
    auto _hot_apply_gate = config::acquire_apply_read_gate();
    auto launch_session = make_launch_session(host_audio, is_input_only, args, named_cert_p);
    bool no_active_sessions = (rtsp_stream::session_count() == 0);

#ifdef _WIN32
    auto disable_virtual_display_request = [&]() {
      launch_session->virtual_display = false;
      launch_session->virtual_display_guid_bytes.fill(0);
      launch_session->virtual_display_device_id.clear();
    };

    if (is_input_only) {
      disable_virtual_display_request();
    } else {
      bool config_requests_virtual = config::video.virtual_display_mode != config::video_t::virtual_display_mode_e::disabled;
      BOOST_LOG(debug) << "config_requests_virtual: " << config_requests_virtual;
      const bool session_requests_virtual = launch_session->app_metadata && launch_session->app_metadata->virtual_screen;
      BOOST_LOG(debug) << "session_requests_virtual: " << session_requests_virtual;
      bool request_virtual_display = config_requests_virtual || session_requests_virtual;
      BOOST_LOG(debug) << "request_virtual_display: " << request_virtual_display;

      auto apply_virtual_display_request = [&](bool should_request_virtual_display) {
        if (!should_request_virtual_display) {
          disable_virtual_display_request();
          return;
        }

        if (!no_active_sessions) {
          auto existing_device = VDISPLAY::resolveAnyVirtualDisplayDeviceId();
          if (existing_device) {
            launch_session->virtual_display = true;
            launch_session->virtual_display_device_id = *existing_device;
            BOOST_LOG(info) << "Virtual display already active (device_id=" << *existing_device
                            << "). Skipping additional creation because another session is running.";
          } else {
            disable_virtual_display_request();
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

        const auto desired_layout = launch_session->virtual_display_layout_override.value_or(config::video.virtual_display_layout);
        const bool wants_extended_layout = desired_layout != config::video_t::virtual_display_layout_e::exclusive;
        if (wants_extended_layout) {
          auto topology_snapshot = display_helper_integration::capture_current_topology();
          if (topology_snapshot) {
            launch_session->virtual_display_topology_snapshot = *topology_snapshot;
          } else {
            launch_session->virtual_display_topology_snapshot.reset();
          }
        } else {
          launch_session->virtual_display_topology_snapshot.reset();
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
          if (display_info->display_name && !display_info->display_name->empty()) {
            BOOST_LOG(info) << "Virtual display created at " << platf::to_utf8(*display_info->display_name);
          } else {
            BOOST_LOG(info) << "Virtual display created (device name pending enumeration).";
          }
        } else {
          disable_virtual_display_request();
          BOOST_LOG(warning) << "Virtual display creation failed.";
        }
      };

      if (!request_virtual_display && VDISPLAY::should_auto_enable_virtual_display()) {
        BOOST_LOG(info) << "No physical monitors detected. Automatically enabling virtual display.";
        request_virtual_display = true;
      }
      apply_virtual_display_request(request_virtual_display);
    }
#endif

    // The display should be restored in case something fails as there are no other sessions.
    if (no_active_sessions && !launch_session->input_only) {
      revert_display_configuration = true;
      bool display_apply_attempted = false;
      bool display_apply_failed = false;

#ifdef _WIN32
      HANDLE user_token = platf::retrieve_users_token(false);
      const bool helper_session_available = (user_token != nullptr);
      if (user_token) {
        CloseHandle(user_token);
      }

      if (helper_session_available) {
        display_apply_attempted = true;
        (void) display_helper_integration::disarm_pending_restore();
        auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
        if (!request) {
          display_apply_failed = true;
          BOOST_LOG(warning) << "Display helper: failed to build display configuration request; continuing with existing display.";
        }

        if (request && !display_helper_integration::apply(*request)) {
          display_apply_failed = true;
          BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
        }
      } else {
        BOOST_LOG(warning) << "Display helper: unable to apply display preferences because there isn't a user signed in currently.";
      }
#else
      display_helper_integration::DisplayApplyBuilder noop_builder;
      noop_builder.set_session(*launch_session);
      display_apply_attempted = true;
      if (!display_helper_integration::apply(noop_builder.build())) {
        display_apply_failed = true;
        BOOST_LOG(warning) << "Display helper: failed to apply display configuration; continuing with existing display.";
      }
#endif

      if (display_apply_attempted && display_apply_failed) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to apply display configuration before streaming.");
        tree.put("root.gamesession", 0);
        return;
      }

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      bool encoder_probe_failed = video::probe_encoders();

      if (encoder_probe_failed && !is_input_only) {
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

    no_active_sessions = (rtsp_stream::session_count() == 0);

    if (is_input_only) {
      BOOST_LOG(info) << "Launching input only session..."sv;

      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();

      // Still probe encoders once, if input only session is launched first
      // But we're ignoring if it's successful or not
      if (no_active_sessions && !proc::proc.virtual_display) {
        video::probe_encoders();
        if (current_appid == 0) {
          proc::proc.launch_input_only();
        }
      }
    } else if (appid > 0 || !appuuid_str.empty()) {
      if (appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid)) {
        // We're basically resuming the same app

        BOOST_LOG(debug) << "Resuming app [" << proc::proc.get_last_run_app_name() << "] from launch app path...";

        if (!proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        if (current_appid == proc::input_only_app_id) {
          launch_session->input_only = true;
        }

      } else {
        const auto &apps = proc::proc.get_apps();
        auto app_iter = std::find_if(apps.begin(), apps.end(), [&appid_str, &appuuid_str](const auto _app) {
          return _app.id == appid_str || _app.uuid == appuuid_str;
        });

        if (app_iter == apps.end()) {
          BOOST_LOG(error) << "Couldn't find app with ID ["sv << appid_str << "] or UUID ["sv << appuuid_str << ']';
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "Cannot find requested application");
          tree.put("root.gamesession", 0);
          return;
        }

        if (!app_iter->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        auto err = proc::proc.execute(*app_iter, launch_session);
        if (err) {
          tree.put("root.<xmlattr>.status_code", err);
          tree.put(
            "root.<xmlattr>.status_message",
            err == 503 ? "Failed to initialize video capture/encoding. Is a display connected and turned on?" : "Failed to start the specified application"
          );
          tree.put("root.gamesession", 0);

          return;
        }
      }
    } else {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "How did you get here?");
      tree.put("root.gamesession", 0);
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

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::_allow_view)) {
      BOOST_LOG(debug) << "Permission ViewApp denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

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
    auto launch_session = make_launch_session(host_audio, false, args, named_cert_p);

    if (!proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();
    }

    if (config::input.enable_input_only_mode && current_appid == proc::input_only_app_id) {
      launch_session->input_only = true;
    }

    // Apply display configuration early if there are no active sessions
    if (no_active_sessions && !launch_session->input_only) {
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
          (void) display_helper_integration::disarm_pending_restore();
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
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to apply display configuration before streaming.");
        return;
      }
    }

    if (no_active_sessions && !proc::proc.virtual_display && !launch_session->input_only) {
      // We want to prepare display only if there are no active sessions
      // and the current session isn't virtual display at the moment.
      // This should be done before probing encoders as it could change the active displays.
#ifdef _WIN32
      auto request = display_helper_integration::helpers::build_request_from_session(config::video, *launch_session);
      if (request) {
        display_helper_integration::apply(*request);
      }
#endif
      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      // Skip encoder probing failure for input-only mode
      if (video::probe_encoders() && !launch_session->input_only) {
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

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::update_tray_client_connected(named_cert_p->name);
#endif
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

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::launch)) {
      BOOST_LOG(debug) << "Permission CancelApp denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

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

    auto fg = util::fail_guard([&]() {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);

    if (!(named_cert_p->perm & PERM::_all_actions)) {
      BOOST_LOG(debug) << "Permission Get AppAsset denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    fg.disable();

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void getClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view) || !(named_cert_p->perm & PERM::clipboard_read)
    ) {
      BOOST_LOG(debug) << "Permission Read Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client [" << named_cert_p->name << "] trying to get clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = platf::get_clipboard();
    response->write(content);
    return;
  }

  void
    setClipboard(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view) || !(named_cert_p->perm & PERM::clipboard_set)
    ) {
      BOOST_LOG(debug) << "Permission Write Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t) named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client [" << named_cert_p->name << "] trying to set clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = request->content.string();

    bool success = platf::set_clipboard(content);

    if (!success) {
      BOOST_LOG(debug) << "Setting clipboard failed!";

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    }

    response->write();
    return;
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

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](req_https_t req, SSL *ssl) {
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
        return false;
      }

      bool verified = false;
      p_named_cert_t named_cert_p;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509_verify.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(verbose) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      if (pending_cert_queue) {
        while (pending_cert_queue->peek()) {
          auto cert = pending_cert_queue->pop();
          if (!cert) {
            continue;
          }

          char subject_name[256];
          X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));
          BOOST_LOG(verbose) << "Added cert ["sv << subject_name << ']';

          const auto pem = crypto::pem(cert);
          auto named_it = std::find_if(
            client_root.named_devices.begin(),
            client_root.named_devices.end(),
            [&pem](const p_named_cert_t &named_cert) {
              return named_cert && named_cert->cert == pem;
            }
          );

          if (named_it != client_root.named_devices.end()) {
            cert_chain.add(*named_it);
          } else {
            BOOST_LOG(warning) << "Pending certificate not found in client registry: "sv << subject_name;
          }
        }
      }

      auto err_str = cert_chain.verify(x509_verify.get(), named_cert_p);
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;
        return verified;
      }

      verified = true;
      req->userp = named_cert_p;

      return true;
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
    https_server.resource["^/pair$"]["GET"] = pair<SunshineHTTPS>;
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/actions/clipboard$"]["GET"] = getClipboard;
    https_server.resource["^/actions/clipboard$"]["POST"] = setClipboard;

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = pair<SimpleWeb::HTTP>;

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

    map_id_sess.clear();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  std::string request_otp(const std::string &passphrase, const std::string &deviceName) {
    if (passphrase.size() < 4) {
      return "";
    }

    one_time_pin = crypto::rand_alphabet(4, "0123456789"sv);
    otp_passphrase = passphrase;
    otp_device_name = deviceName;
    otp_creation_time = std::chrono::steady_clock::now();

    return one_time_pin;
  }

  void
    erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
    load_state();
  }

  void stop_session(stream::session_t &session, bool graceful) {
    if (graceful) {
      stream::session::graceful_stop(session);
    } else {
      stream::session::stop(session);
    }
  }

  bool find_and_stop_session(const std::string &uuid, bool graceful) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      stop_session(*session, graceful);
      return true;
    }
    return false;
  }

  void update_session_info(stream::session_t &session, const std::string &name, const crypto::PERM newPerm) {
    stream::session::update_device_info(session, name, newPerm);
  }

  bool find_and_udpate_session_info(const std::string &uuid, const std::string &name, const crypto::PERM newPerm) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      update_session_info(*session, name, newPerm);
      return true;
    }
    return false;
  }

  bool update_device_info(
    const std::string &uuid,
    const std::string &name,
    const std::string &display_mode,
    const cmd_list_t &do_cmds,
    const cmd_list_t &undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display,
    const std::string &virtual_display_mode,
    const std::string &virtual_display_layout
  ) {
    find_and_udpate_session_info(uuid, name, newPerm);

    client_t &client = client_root;
    auto it = client.named_devices.begin();
    for (; it != client.named_devices.end(); ++it) {
      auto named_cert_p = *it;
      if (named_cert_p->uuid == uuid) {
        named_cert_p->name = name;
        named_cert_p->display_mode = display_mode;
        named_cert_p->perm = newPerm;
        named_cert_p->do_cmds = do_cmds;
        named_cert_p->undo_cmds = undo_cmds;
        named_cert_p->enable_legacy_ordering = enable_legacy_ordering;
        named_cert_p->allow_client_commands = allow_client_commands;
        named_cert_p->always_use_virtual_display = always_use_virtual_display;
        named_cert_p->virtual_display_mode_override = virtual_display_mode;
        named_cert_p->virtual_display_layout_override = virtual_display_layout;
        save_state();
        return true;
      }
    }

    return false;
  }

  // (Windows-only) display_helper_integration is included above

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it)->uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();

    if (removed) {
      auto session = rtsp_stream::find_session(uuid);
      if (session) {
        stop_session(*session, true);
      }

      if (client.named_devices.empty()) {
        proc::proc.terminate();
      }
    }

    return removed;
  }
}  // namespace nvhttp
