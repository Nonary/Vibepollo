/**
 * @file src/rtsp.cpp
 * @brief Definitions for RTSP streaming.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
#include <moonlight-common-c/src/Rtsp.h>
}

// standard includes
#include <array>
#include <cctype>
#include <format>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// lib includes
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#ifdef _WIN32
  #include <sddl.h>
  #include <windows.h>
#endif

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "rtsp_pending_policy.h"
#include "stream.h"
#include "sync.h"
#include "thread_pool.h"
#include "video.h"

namespace asio = boost::asio;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

#ifdef _WIN32
namespace {
  constexpr wchar_t kVulkanHdrLayerGlobalActiveEventName[] = L"Global\\SunshineVirtualHdrActive";
  constexpr wchar_t kVulkanHdrLayerLocalActiveEventName[] = L"Local\\SunshineVirtualHdrActive";

  std::mutex g_vulkan_hdr_layer_event_mutex;
  HANDLE g_vulkan_hdr_layer_global_event = nullptr;
  HANDLE g_vulkan_hdr_layer_local_event = nullptr;

  HANDLE create_vulkan_hdr_active_event(const wchar_t *name) {
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    SECURITY_ATTRIBUTES security_attributes {};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = FALSE;

    SECURITY_ATTRIBUTES *security_attributes_ptr = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;WD)",
          SDDL_REVISION_1,
          &security_descriptor,
          nullptr
        )) {
      security_attributes.lpSecurityDescriptor = security_descriptor;
      security_attributes_ptr = &security_attributes;
    }

    HANDLE event = CreateEventW(security_attributes_ptr, TRUE, FALSE, name);
    if (security_descriptor) {
      LocalFree(security_descriptor);
    }
    return event;
  }

  void ensure_vulkan_hdr_active_events() {
    if (!g_vulkan_hdr_layer_global_event) {
      g_vulkan_hdr_layer_global_event = create_vulkan_hdr_active_event(kVulkanHdrLayerGlobalActiveEventName);
    }
    if (!g_vulkan_hdr_layer_local_event) {
      g_vulkan_hdr_layer_local_event = create_vulkan_hdr_active_event(kVulkanHdrLayerLocalActiveEventName);
    }
  }

  void set_vulkan_hdr_layer_streaming_active(bool active) {
    std::scoped_lock lock {g_vulkan_hdr_layer_event_mutex};
    ensure_vulkan_hdr_active_events();

    bool signaled = false;
    for (HANDLE event : {g_vulkan_hdr_layer_global_event, g_vulkan_hdr_layer_local_event}) {
      if (!event) {
        continue;
      }
      if (active) {
        signaled = SetEvent(event) || signaled;
      } else {
        signaled = ResetEvent(event) || signaled;
      }
    }

    if (active && !signaled) {
      BOOST_LOG(warning) << "Failed to signal Vulkan HDR layer streaming state.";
    }
  }
}  // namespace
#endif

namespace rtsp_stream {
  void free_msg(PRTSP_MESSAGE msg) {
    freeMessage(msg);

    delete msg;
  }

#pragma pack(push, 1)

  struct encrypted_rtsp_header_t {
    // We set the MSB in encrypted RTSP messages to allow format-agnostic
    // parsing code to be able to tell encrypted from plaintext messages.
    static constexpr std::uint32_t ENCRYPTED_MESSAGE_TYPE_BIT = 0x80000000;

    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint32_t payload_length() {
      return util::endian::big<std::uint32_t>(typeAndLength) & ~ENCRYPTED_MESSAGE_TYPE_BIT;
    }

    bool is_encrypted() {
      return !!(util::endian::big<std::uint32_t>(typeAndLength) & ENCRYPTED_MESSAGE_TYPE_BIT);
    }

    // This field is the length of the payload + ENCRYPTED_MESSAGE_TYPE_BIT in big-endian
    std::uint32_t typeAndLength;

    // This field is the number used to initialize the bottom 4 bytes of the AES IV in big-endian
    std::uint32_t sequenceNumber;

    // This field is the AES GCM authentication tag
    std::uint8_t tag[16];
  };

#pragma pack(pop)

  class rtsp_server_t;
  class socket_t;

  using msg_t = util::safe_ptr<RTSP_MESSAGE, free_msg>;
  using cmd_func_t = std::function<bool(rtsp_server_t *server, std::shared_ptr<socket_t>, std::shared_ptr<launch_session_t>, msg_t &&)>;

  void print_msg(PRTSP_MESSAGE msg);
  bool cmd_not_found(rtsp_server_t *server, std::shared_ptr<socket_t>, std::shared_ptr<launch_session_t>, msg_t &&req);
  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload);

  void apply_rtx_hdr_stream_policy(video::config_t &config) {
    // Keep the stream in a TrueHDR-capable HDR pipeline for the whole session. The
    // per-frame RTX HDR runtime still bypasses conversion while the foreground is
    // desktop or any non-matching app, so we can turn RTX HDR off without changing
    // WGC capture format or reinitializing the encoder.
    config.rtx_hdr_active = config::runtime_config_override_enabled("rtx_hdr") &&
                            config::video.rtx_hdr.enabled &&
                            config.dynamicRange > 0 &&
                            !config.prefer_sdr_10bit &&
                            !config.force_sdr;
    config.rtx_hdr_peak_nits = std::clamp(config::video.rtx_hdr.peak_brightness, 400, 2000);
  }

  bool activates_vulkan_hdr_layer_for_stream(const video::config_t &config) {
    return config.dynamicRange != 0 && !config.prefer_sdr_10bit && !config.force_sdr;
  }

  std::shared_ptr<launch_session_t> make_startup_launch_session_snapshot(const launch_session_t &source) {
    auto snapshot = std::make_shared<launch_session_t>();

    snapshot->id = source.id;
    snapshot->role = source.role;
    snapshot->role_generation = source.role_generation;
    snapshot->remote_capture_output = source.remote_capture_output;
    snapshot->rtsp_source_address = source.rtsp_source_address;
    snapshot->gcm_key = source.gcm_key;
    snapshot->iv = source.iv;
    snapshot->av_ping_payload = source.av_ping_payload;
    snapshot->control_connect_data = source.control_connect_data;
    snapshot->unique_id = source.unique_id;
    snapshot->client_uuid = source.client_uuid;
    snapshot->client_name = source.client_name;
    snapshot->device_name = source.device_name;
    snapshot->client_display_mode_override = source.client_display_mode_override;
    snapshot->client_display_refresh_millihz = source.client_display_refresh_millihz;
    snapshot->enable_hdr = source.enable_hdr;
    snapshot->prefer_sdr_10bit = source.prefer_sdr_10bit;
    snapshot->force_sdr = source.force_sdr;
    snapshot->client_vrr_requested = source.client_vrr_requested;
    snapshot->virtual_display = source.virtual_display;
    snapshot->virtual_display_guid_bytes = source.virtual_display_guid_bytes;
    snapshot->gen1_framegen_fix = source.gen1_framegen_fix;
    snapshot->gen2_framegen_fix = source.gen2_framegen_fix;
    snapshot->frame_generation_enabled = source.frame_generation_enabled;
    snapshot->lossless_scaling_framegen = source.lossless_scaling_framegen;
    snapshot->framegen_refresh_rate = source.framegen_refresh_rate;
    snapshot->framegen_refresh_millihz = source.framegen_refresh_millihz;
    snapshot->framegen_refresh_multiplier = source.framegen_refresh_multiplier;
    snapshot->frame_generation_provider = source.frame_generation_provider;
    snapshot->lossless_scaling_target_fps = source.lossless_scaling_target_fps;
    snapshot->lossless_scaling_rtss_limit = source.lossless_scaling_rtss_limit;
#ifdef _WIN32
    snapshot->display_helper_gate = source.display_helper_gate;
#endif

    return snapshot;
  }

  class socket_t: public std::enable_shared_from_this<socket_t> {
  public:
    socket_t(boost::asio::io_context &io_context, std::function<void(std::shared_ptr<socket_t>, msg_t &&)> &&handle_data_fn):
        handle_data_fn {std::move(handle_data_fn)},
        sock {io_context} {
    }

    /**
     * @brief Queue an asynchronous read to begin the next message.
     */
    void read() {
      if (!session) {
        read_unbound();
        return;
      }
      if (begin == std::end(msg_buf) || (session->rtsp_cipher && begin + sizeof(encrypted_rtsp_header_t) >= std::end(msg_buf))) {
        BOOST_LOG(error) << "RTSP: read(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      if (session->rtsp_cipher) {
        // For encrypted RTSP, we will read the the entire header first
        boost::asio::async_read(sock, boost::asio::buffer(begin, sizeof(encrypted_rtsp_header_t)), boost::bind(&socket_t::handle_read_encrypted_header, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      } else {
        sock.async_read_some(
          boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
          boost::bind(
            &socket_t::handle_read_plaintext,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
          )
        );
      }
    }

    // A mixed NAT cannot bind an address-owned plaintext launch before seeing
    // the framing word: encrypted RTSP marks that word's MSB and must still be
    // authenticated against every encrypted candidate.
    void read_unbound() {
      if (!plaintext_candidate && encrypted_candidates.empty()) {
        boost::system::error_code ec;
        sock.close(ec);
        return;
      }
      boost::asio::async_read(sock, boost::asio::buffer(begin, sizeof(std::uint32_t)), boost::bind(&socket_t::handle_read_unbound_prefix, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    static void handle_read_unbound_prefix(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      if (ec || bytes != sizeof(std::uint32_t)) {
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      const std::array<std::uint8_t, 4> first_word {
        static_cast<std::uint8_t>(socket->begin[0]), static_cast<std::uint8_t>(socket->begin[1]),
        static_cast<std::uint8_t>(socket->begin[2]), static_cast<std::uint8_t>(socket->begin[3]),
      };
      switch (pending_policy::choose_initial_route(static_cast<bool>(socket->plaintext_candidate), !socket->encrypted_candidates.empty(), first_word)) {
        case pending_policy::initial_route_e::plaintext:
          socket->session = std::move(socket->plaintext_candidate);
          socket->begin += bytes;
          socket->read();
          return;
        case pending_policy::initial_route_e::encrypted:
          boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, sizeof(encrypted_rtsp_header_t) - bytes), boost::bind(&socket_t::handle_read_unbound_encrypted_header_after_prefix, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
          return;
        case pending_policy::initial_route_e::reject:
          break;
      }
      boost::system::error_code close_ec;
      socket->sock.close(close_ec);
    }

    static void handle_read_unbound_encrypted_header_after_prefix(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      if (ec || bytes != sizeof(encrypted_rtsp_header_t) - sizeof(std::uint32_t)) {
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      auto header = reinterpret_cast<encrypted_rtsp_header_t *>(socket->begin);
      const auto payload_length = header->payload_length();
      if (!header->is_encrypted() || socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(warning) << "Rejecting unbound RTSP connection without a valid encrypted header.";
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + sizeof(*header), payload_length), boost::bind(&socket_t::handle_read_unbound_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    static void handle_read_unbound_encrypted_header(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      if (ec || bytes < sizeof(encrypted_rtsp_header_t)) {
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      auto header = reinterpret_cast<encrypted_rtsp_header_t *>(socket->begin);
      const auto payload_length = header->payload_length();
      if (!header->is_encrypted() || socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(warning) << "Rejecting unbound RTSP connection without a valid encrypted header.";
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, payload_length), boost::bind(&socket_t::handle_read_unbound_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    static void handle_read_unbound_encrypted_message(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      if (ec) {
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      auto header = reinterpret_cast<encrypted_rtsp_header_t *>(socket->begin);
      const auto payload_length = header->payload_length();
      if (bytes < payload_length) {
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      const auto sequence = util::endian::big<std::uint32_t>(header->sequenceNumber);
      crypto::aes_t iv(12);
      std::copy_n(reinterpret_cast<const std::uint8_t *>(&sequence), sizeof(sequence), std::begin(iv));
      iv[10] = 'C';
      iv[11] = 'R';
      std::vector<std::uint8_t> plaintext;
      std::shared_ptr<launch_session_t> matched;
      for (const auto &candidate : socket->encrypted_candidates) {
        if (!candidate || !candidate->rtsp_cipher) continue;
        std::vector<std::uint8_t> candidate_plaintext;
        if (candidate->rtsp_cipher->decrypt(std::string_view {reinterpret_cast<const char *>(header->tag), sizeof(header->tag) + bytes}, candidate_plaintext, &iv) == 0) {
          if (matched) {
            BOOST_LOG(error) << "Encrypted RTSP matched more than one pending launch; rejecting ambiguous transport.";
            boost::system::error_code close_ec;
            socket->sock.close(close_ec);
            return;
          }
          matched = candidate;
          plaintext = std::move(candidate_plaintext);
        }
      }
      if (!matched) {
        BOOST_LOG(warning) << "Encrypted RTSP authentication did not match a pending launch.";
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      socket->session = std::move(matched);
      msg_t req {new msg_t::element_type {}};
      if (parseRtspMessage(req.get(), reinterpret_cast<char *>(plaintext.data()), static_cast<int>(plaintext.size()))) {
        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        boost::system::error_code close_ec;
        socket->sock.close(close_ec);
        return;
      }
      print_msg(req.get());
      socket->handle_data(std::move(req));
    }

    /**
     * @brief Handle the initial read of the header of an encrypted message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_header(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted_header(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec || bytes < sizeof(encrypted_rtsp_header_t)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      if (!header->is_encrypted()) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Rejecting unencrypted RTSP message"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto payload_length = header->payload_length();

      // Check if we have enough space to read this message
      if (socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Exceeded maximum rtsp packet size: "sv << socket->msg_buf.size();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      // Read the remainder of the header and full encrypted payload
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, payload_length), boost::bind(&socket_t::handle_read_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    /**
     * @brief Handle the final read of the content of an encrypted message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_encrypted_message(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_message(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      auto payload_length = header->payload_length();
      auto seq = util::endian::big<std::uint32_t>(header->sequenceNumber);

      if (ec || bytes < payload_length) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RC' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // received from each client before the IV repeats.
      crypto::aes_t iv(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'C';  // Client originated
      iv[11] = 'R';  // RTSP

      std::vector<uint8_t> plaintext;
      if (socket->session->rtsp_cipher->decrypt(std::string_view {(const char *) header->tag, sizeof(header->tag) + bytes}, plaintext, &iv)) {
        BOOST_LOG(error) << "Failed to verify RTSP message tag"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), (char *) plaintext.data(), (int) plaintext.size())) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      print_msg(req.get());

      socket->handle_data(std::move(req));
    }

    /**
     * @brief Queue an asynchronous read of the payload portion of a plaintext message.
     */
    void read_plaintext_payload() {
      if (begin == std::end(msg_buf)) {
        BOOST_LOG(error) << "RTSP: read_plaintext_payload(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);

        return;
      }

      sock.async_read_some(
        boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
        boost::bind(
          &socket_t::handle_plaintext_payload,
          shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred
        )
      );
    }

    /**
     * @brief Handle the read of the payload portion of a plaintext message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_plaintext_payload(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_plaintext_payload(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't read from tcp socket: "sv << ec.message();

        return;
      }

      auto end = socket->begin + bytes;
      msg_t req {new msg_t::element_type {}};
      if (auto status = parseRtspMessage(req.get(), socket->msg_buf.data(), (int) (end - socket->msg_buf.data()))) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      auto fg = util::fail_guard([&socket]() {
        socket->read_plaintext_payload();
      });

      auto content_length = 0;
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Content-length"sv == option->option) {
          BOOST_LOG(debug) << "Found Content-Length: "sv << option->content << " bytes"sv;

          // If content_length > bytes read, then we need to store current data read,
          // to be appended by the next read.
          std::string_view content {option->content};
          auto begin = std::find_if(std::begin(content), std::end(content), [](auto ch) {
            return (bool) std::isdigit(ch);
          });

          content_length = (int) util::from_chars(begin, std::end(content));
          break;
        }
      }

      if (end - socket->crlf >= content_length) {
        if (end - socket->crlf > content_length) {
          BOOST_LOG(warning) << "(end - socket->crlf) > content_length -- "sv << (std::size_t) (end - socket->crlf) << " > "sv << content_length;
        }

        fg.disable();
        print_msg(req.get());

        socket->handle_data(std::move(req));
      }

      socket->begin = end;
    }

    /**
     * @brief Handle the read of the header portion of a plaintext message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void handle_read_plaintext(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_plaintext(): Handle read of size: "sv << bytes << " bytes"sv;

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't read from tcp socket: "sv << ec.message();

        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't close tcp socket: "sv << ec.message();
        }

        return;
      }

      auto fg = util::fail_guard([&socket]() {
        socket->read();
      });

      auto begin = std::max(socket->msg_buf.data(), socket->begin - 4);
      auto end = socket->begin + bytes;
      auto buf_size = end - begin;

      constexpr auto needle = "\r\n\r\n"sv;

      auto it = std::search(begin, begin + buf_size, std::begin(needle), std::end(needle));
      if (it == end) {
        socket->begin = end;

        return;
      }

      // Emulate read completion for payload data
      socket->begin = it + needle.size();
      socket->crlf = socket->begin;
      buf_size = end - socket->begin;

      fg.disable();
      handle_plaintext_payload(socket, ec, buf_size);
    }

    void handle_data(msg_t &&req) {
      handle_data_fn(shared_from_this(), std::move(req));
    }

    std::function<void(std::shared_ptr<socket_t>, msg_t &&)> handle_data_fn;

    tcp::socket sock;

    std::array<char, 2048> msg_buf;

    char *crlf;
    char *begin = msg_buf.data();

    std::shared_ptr<launch_session_t> session;
    std::shared_ptr<launch_session_t> plaintext_candidate;
    std::vector<std::shared_ptr<launch_session_t>> encrypted_candidates;
  };

  class rtsp_server_t {
  public:
    // Normal shutdown synchronously clears sessions while all cross-translation-unit
    // lifecycle state is alive. Namespace-global destruction must never re-enter
    // that state because destruction order relative to nvhttp/stream is unspecified.
    ~rtsp_server_t() = default;

    int bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
      auto bind_addr_str = net::get_bind_address(af);
      const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
      if (ec) {
        BOOST_LOG(error) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
        return -1;
      }

      acceptor.open(net::tcp_protocol_for_address(bind_addr), ec);
      if (ec) {
        return -1;
      }

      acceptor.set_option(boost::asio::socket_base::reuse_address {true});

      acceptor.bind(tcp::endpoint(bind_addr, port), ec);
      if (ec) {
        return -1;
      }

      acceptor.listen(4096, ec);
      if (ec) {
        return -1;
      }

      startup_pool.start(1);

      next_socket = std::make_shared<socket_t>(io_context, [this](std::shared_ptr<socket_t> socket, msg_t &&msg) {
        handle_msg(std::move(socket), std::move(msg));
      });

      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });

      return 0;
    }

    void handle_msg(std::shared_ptr<socket_t> socket, msg_t &&req) {
      auto session = socket->session;
      auto func = _map_cmd_cb.find(req->message.request.command);
      bool defer_socket_shutdown = false;
      if (func != std::end(_map_cmd_cb)) {
        defer_socket_shutdown = func->second(this, socket, session, std::move(req));
      } else {
        cmd_not_found(this, socket, session, std::move(req));
      }

      if (!defer_socket_shutdown) {
        shutdown_socket(*socket);
      }
    }

    void handle_accept(const boost::system::error_code &ec) {
      if (ec) {
        BOOST_LOG(error) << "Couldn't accept incoming connections: "sv << ec.message();

        // Stop server
        clear();
        return;
      }

      auto socket = std::move(next_socket);

      boost::system::error_code endpoint_ec;
      const auto remote_endpoint = socket->sock.remote_endpoint(endpoint_ec);
      const auto remote_address = endpoint_ec ? std::string {} : remote_endpoint.address().to_string();
      const auto candidates = route_candidates(remote_address);
      if (candidates.plaintext || !candidates.encrypted.empty()) {
        socket->plaintext_candidate = candidates.plaintext;
        socket->encrypted_candidates = candidates.encrypted;
        socket->read();
      } else {
        // This can happen due to normal things like port scanning, so let's not make these visible by default
        BOOST_LOG(debug) << "No pending session for incoming RTSP connection"sv;

        // If there is no session pending, close the connection immediately
        boost::system::error_code ec;
        socket->sock.close(ec);
      }

      // Queue another asynchronous accept for the next incoming connection
      next_socket = std::make_shared<socket_t>(io_context, [this](std::shared_ptr<socket_t> socket, msg_t &&msg) {
        handle_msg(std::move(socket), std::move(msg));
      });
      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });
    }

    void map(const std::string_view &type, cmd_func_t cb) {
      _map_cmd_cb.emplace(type, std::move(cb));
    }

    template<class Function>
    void post(Function &&fn) {
      boost::asio::post(io_context, std::forward<Function>(fn));
    }

    template<class Function>
    void run_startup(Function &&fn) {
      if (stopping.load(std::memory_order_acquire)) {
        throw std::runtime_error("RTSP server is stopping");
      }

      startup_tasks.fetch_add(1, std::memory_order_acq_rel);
      try {
        startup_pool.push([this, task = std::forward<Function>(fn)]() mutable {
          try {
            task();
          } catch (...) {
            finish_startup();
            throw;
          }
        });
      } catch (...) {
        finish_startup();
        throw;
      }
    }

    void finish_startup() {
      stream::session::cleanup_reservation_t cleanup_reservation;
      std::unique_lock<std::mutex> lifecycle_lock(nvhttp::stream_lifecycle_mutex());
      startup_tasks.fetch_sub(1, std::memory_order_acq_rel);

      std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes;
      if (const auto pending = any_pending_launch()) {
        virtual_display_guid_bytes = pending->virtual_display_guid_bytes;
      } else {
        virtual_display_guid_bytes = abandoned_startup_virtual_display_guid_bytes;
      }
      const stream::session::shared_runtime_finalize_context_t finalize_context {
        .virtual_display_guid_bytes = virtual_display_guid_bytes,
      };
      (void) stream::session::finalize_shared_runtime_if_idle(
        "rtsp_startup_finished",
        finalize_context
      );
      if (startup_count() == 0) {
        abandoned_startup_virtual_display_guid_bytes.reset();
      }
    }

    int startup_count() const {
      return startup_tasks.load(std::memory_order_acquire);
    }

    void shutdown_socket(socket_t &socket) {
      boost::system::error_code ec;
      socket.sock.shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
    }

    /**
     * @brief Launch a new streaming session.
     * @note If the client does not begin streaming within the ping_timeout,
     *       the session will be discarded.
     * @param launch_session Streaming session information.
     */
    bool session_raise(std::shared_ptr<launch_session_t> launch_session) {
      if (!launch_session || launch_session->id == 0) return false;
      // Function arguments may move the session before the map key is read.
      const auto launch_session_id = launch_session->id;
      const auto expires_at = std::chrono::steady_clock::now() + config::stream.ping_timeout;
      bool accepted = false;
      std::vector<std::shared_ptr<launch_session_t>> expired;
      {
        std::lock_guard lock {pending_launches_mutex};
        expire_pending_locked(std::chrono::steady_clock::now(), &expired);
        if (pending_launches.size() >= remote_session::max_client_vdds * 2) {
          BOOST_LOG(error) << "RTSP pending-launch registry is full; refusing launch " << launch_session_id;
        } else if (pending_launches.contains(launch_session_id)) {
          BOOST_LOG(error) << "RTSP pending-launch ID collision for " << launch_session_id;
        } else if (!launch_session->rtsp_cipher && std::any_of(pending_launches.begin(), pending_launches.end(), [&launch_session](const auto &entry) {
                     return !entry.second.session->rtsp_cipher && entry.second.source_address == launch_session->rtsp_source_address;
                   })) {
          plaintext_route_warning = "Plaintext RTSP has more than one pending launch for one source address; rejecting the new launch.";
          BOOST_LOG(error) << plaintext_route_warning;
        } else {
          pending_launches.emplace(launch_session_id, pending_launch_t {std::move(launch_session), expires_at});
          accepted = true;
        }
      }
      notify_expired_pending(expired);
      if (accepted) {
        set_pending_vulkan_hdr_layer_stream(pending_hdr_active());
        arm_pending_timer();
      }
      return accepted;
    }

    std::string plaintext_warning() {
      std::lock_guard lock {pending_launches_mutex};
      return plaintext_route_warning;
    }

    /**
     * @brief Clear state for the oldest launch session.
     * @param launch_session_id The ID of the session to clear.
     */
    void session_clear(uint32_t launch_session_id) {
      stream::session::cleanup_reservation_t cleanup_reservation;
      std::unique_lock<std::mutex> lifecycle_lock(nvhttp::stream_lifecycle_mutex());
      if (const auto cleared = take_pending_launch(launch_session_id)) {
          abandoned_startup_virtual_display_guid_bytes = cleared->virtual_display_guid_bytes;
          set_pending_vulkan_hdr_layer_stream(pending_hdr_active());
          const stream::session::shared_runtime_finalize_context_t finalize_context {
            .virtual_display_guid_bytes = cleared->virtual_display_guid_bytes,
          };
          (void) stream::session::finalize_shared_runtime_if_idle(
            "rtsp_launch_attached",
            finalize_context
          );
          if (startup_count() == 0) {
            abandoned_startup_virtual_display_guid_bytes.reset();
          }
      }
    }

    void cancel_pending_launch(std::string_view reason) {
      stream::session::cleanup_reservation_t cleanup_reservation;
      std::unique_lock<std::mutex> lifecycle_lock(nvhttp::stream_lifecycle_mutex());
      const auto discarded = take_all_pending_launches();
      for (const auto &launch_session : discarded) {
        abandoned_startup_virtual_display_guid_bytes = launch_session->virtual_display_guid_bytes;
        const stream::session::shared_runtime_finalize_context_t finalize_context {
          .virtual_display_guid_bytes = launch_session->virtual_display_guid_bytes,
        };
        (void) stream::session::finalize_shared_runtime_if_idle(reason, finalize_context);
      }
      set_pending_vulkan_hdr_layer_stream(false);
      if (startup_count() == 0) {
        abandoned_startup_virtual_display_guid_bytes.reset();
      }
    }

    /**
     * @brief Get the number of active sessions.
     * @return Count of active sessions.
     */
    int session_count() {
      auto lg = _session_state.lock();
      return static_cast<int>(_session_state->sessions.size());
    }

    bool has_pending_launch_or_startup() {
      return has_pending_launches() || startup_count() > 0;
    }

    struct pending_launch_t {
      std::shared_ptr<launch_session_t> session;
      std::chrono::steady_clock::time_point expires_at;
      std::string source_address;
      pending_launch_t(std::shared_ptr<launch_session_t> launch_session, std::chrono::steady_clock::time_point expires):
          session(std::move(launch_session)), expires_at(expires), source_address(session->rtsp_source_address) {}
    };

    struct route_candidates_t {
      std::shared_ptr<launch_session_t> plaintext;
      std::vector<std::shared_ptr<launch_session_t>> encrypted;
    };

    route_candidates_t route_candidates(std::string_view address) {
      std::vector<std::shared_ptr<launch_session_t>> expired;
      route_candidates_t result;
      {
        std::lock_guard lock {pending_launches_mutex};
        expire_pending_locked(std::chrono::steady_clock::now(), &expired);
        for (const auto &[id, pending] : pending_launches) {
          if (pending.session->rtsp_cipher) {
            result.encrypted.push_back(pending.session);
          } else if (pending.source_address == address) {
            if (result.plaintext) {
              plaintext_route_warning = "Plaintext RTSP source-address routing became ambiguous; rejecting transport.";
              BOOST_LOG(error) << plaintext_route_warning;
              result = {};
              break;
            }
            result.plaintext = pending.session;
          }
        }
      }
      notify_expired_pending(expired);
      return result;
    }

    void expire_pending_locked(std::chrono::steady_clock::time_point now, std::vector<std::shared_ptr<launch_session_t>> *expired = nullptr) {
      for (auto it = pending_launches.begin(); it != pending_launches.end();) {
        if (it->second.expires_at <= now) {
          if (expired) expired->push_back(std::move(it->second.session));
          it = pending_launches.erase(it);
        } else {
          ++it;
        }
      }
    }

    void notify_expired_pending(const std::vector<std::shared_ptr<launch_session_t>> &expired) {
      std::vector<pending_policy::pending_owner_t> owners;
      owners.reserve(expired.size());
      for (const auto &launch_session : expired) {
        if (!launch_session) continue;
        BOOST_LOG(warning) << "RTSP pending launch expired: " << launch_session->id << " client=" << launch_session->client_uuid;
        owners.push_back({.role = launch_session->role, .client_uuid = launch_session->client_uuid, .generation = launch_session->role_generation});
      }
      for (const auto &owner : pending_policy::expired_remote_input_owners(owners)) {
        nvhttp::notify_remote_input_transport_lost(owner.client_uuid, owner.generation);
      }
      if (!expired.empty()) set_pending_vulkan_hdr_layer_stream(pending_hdr_active());
    }

    void expire_pending() {
      std::vector<std::shared_ptr<launch_session_t>> expired;
      {
        std::lock_guard lock {pending_launches_mutex};
        expire_pending_locked(std::chrono::steady_clock::now(), &expired);
      }
      notify_expired_pending(expired);
    }

    void arm_pending_timer() {
      std::optional<std::chrono::steady_clock::time_point> deadline;
      {
        std::lock_guard lock {pending_launches_mutex};
        for (const auto &[id, pending] : pending_launches) {
          if (!deadline || pending.expires_at < *deadline) deadline = pending.expires_at;
        }
      }
      raised_timer.cancel();
      if (!deadline) return;
      raised_timer.expires_at(*deadline);
      raised_timer.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          expire_pending();
          arm_pending_timer();
        }
      });
    }

    std::shared_ptr<launch_session_t> plaintext_candidate(std::string_view address) {
      expire_pending();
      std::lock_guard lock {pending_launches_mutex};
      std::shared_ptr<launch_session_t> match;
      for (const auto &[id, pending] : pending_launches) {
        if (!pending.session->rtsp_cipher && pending.source_address == address) {
          if (match) {
            plaintext_route_warning = "Plaintext RTSP source-address routing became ambiguous; rejecting transport.";
            BOOST_LOG(error) << plaintext_route_warning;
            return {};
          }
          match = pending.session;
        }
      }
      return match;
    }

    std::vector<std::shared_ptr<launch_session_t>> encrypted_candidates() {
      expire_pending();
      std::lock_guard lock {pending_launches_mutex};
      std::vector<std::shared_ptr<launch_session_t>> candidates;
      for (const auto &[id, pending] : pending_launches) if (pending.session->rtsp_cipher) candidates.push_back(pending.session);
      return candidates;
    }

    std::shared_ptr<launch_session_t> any_pending_launch() {
      expire_pending();
      std::lock_guard lock {pending_launches_mutex};
      return pending_launches.empty() ? nullptr : pending_launches.begin()->second.session;
    }

    std::shared_ptr<launch_session_t> pending_launch(std::uint32_t id) {
      expire_pending();
      std::lock_guard lock {pending_launches_mutex};
      const auto it = pending_launches.find(id);
      return it == pending_launches.end() ? nullptr : it->second.session;
    }

    std::shared_ptr<launch_session_t> take_pending_launch(std::uint32_t id) {
      std::lock_guard lock {pending_launches_mutex};
      const auto it = pending_launches.find(id);
      if (it == pending_launches.end()) return {};
      auto session = std::move(it->second.session);
      pending_launches.erase(it);
      return session;
    }

    std::vector<std::shared_ptr<launch_session_t>> take_all_pending_launches() {
      std::lock_guard lock {pending_launches_mutex};
      std::vector<std::shared_ptr<launch_session_t>> sessions;
      for (auto &[id, pending] : pending_launches) sessions.push_back(std::move(pending.session));
      pending_launches.clear();
      return sessions;
    }

    bool has_pending_launches() {
      expire_pending();
      std::lock_guard lock {pending_launches_mutex};
      return !pending_launches.empty();
    }

    bool pending_hdr_active() {
      std::lock_guard lock {pending_launches_mutex};
      return std::any_of(pending_launches.begin(), pending_launches.end(), [](const auto &entry) {
        return effective_hdr_requested(*entry.second.session);
      });
    }

    bool vulkan_hdr_layer_active_locked() {
      return config::video.dd.vulkan_hdr_layer &&
             (_session_state->vulkan_hdr_layer_pending_stream || !_session_state->vulkan_hdr_layer_sessions.empty());
    }

    void set_pending_vulkan_hdr_layer_stream(bool active) {
      bool vulkan_hdr_layer_active = false;
      {
        auto lg = _session_state.lock();
        _session_state->vulkan_hdr_layer_pending_stream = active;
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif
    }

    /**
     * @brief Clear launch sessions.
     * @param all If true, clear all sessions. Otherwise, only clear timed out and stopped sessions.
     * @examples
     * clear(false);
     * @examples_end
     */
    void clear(bool all = true, bool preserve_pending_launch = false) {
      if (all && !preserve_pending_launch) {
        cancel_pending_launch("rtsp_sessions_terminated");
      }

      // Collect sessions to stop/join first while holding the set lock,
      // but perform the potentially blocking join() outside of the lock to
      // avoid deadlocks. Each join serializes only its final ownership change.
      std::vector<std::shared_ptr<stream::session_t>> to_cleanup;
      bool vulkan_hdr_layer_active = false;

      {
        auto lg = _session_state.lock();

        for (auto i = _session_state->sessions.begin(); i != _session_state->sessions.end();) {
          auto &slot = *(*i);
          if (all || stream::session::state(slot) == stream::session::state_e::STOPPING) {
            // Make a copy to operate on after releasing the lock
            auto session = *i;
            to_cleanup.emplace_back(session);

            // Remove from the active set now so counts reflect pending removal
            _session_state->client_uuids.erase(session.get());
            _session_state->vulkan_hdr_layer_sessions.erase(session.get());
            i = _session_state->sessions.erase(i);
          } else {
            ++i;
          }
        }
        if (all) {
          _session_state->vulkan_hdr_layer_pending_stream = false;
        }
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif

      // Stop and join outside the lock
      for (auto &slot : to_cleanup) {
        stream::session::stop(*slot);
        stream::session::join(*slot);
      }
    }

    /**
     * @brief Removes the provided session from the set of sessions.
     * @param session The session to remove.
     */
    void remove(const std::shared_ptr<stream::session_t> &session) {
      bool vulkan_hdr_layer_active = false;
      {
        auto lg = _session_state.lock();
        _session_state->sessions.erase(session);
        _session_state->client_uuids.erase(session.get());
        _session_state->vulkan_hdr_layer_sessions.erase(session.get());
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif
    }

    /**
     * @brief Inserts the provided session into the set of sessions.
     * @param session The session to insert.
     */
    void insert(const std::shared_ptr<stream::session_t> &session, const std::string &client_uuid, bool hdr_enabled) {
      const bool has_uuid = !client_uuid.empty();
      bool vulkan_hdr_layer_active = false;
      {
        auto lg = _session_state.lock();
        _session_state->sessions.emplace(session);
        if (has_uuid) {
          _session_state->client_uuids[session.get()] = client_uuid;
        } else {
          _session_state->client_uuids.erase(session.get());
        }
        if (hdr_enabled) {
          _session_state->vulkan_hdr_layer_sessions.emplace(session.get());
        } else {
          _session_state->vulkan_hdr_layer_sessions.erase(session.get());
        }
        _session_state->vulkan_hdr_layer_pending_stream = false;
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif
      if (has_uuid) {
        nvhttp::mark_client_last_seen(client_uuid);
      }
      BOOST_LOG(info) << "New streaming session started [active sessions: "sv << _session_state->sessions.size() << ']';
    }

    std::list<std::string> get_all_client_uuids() {
      std::list<std::string> out;
      auto lg = _session_state.lock();
      for (const auto &[_, uuid] : _session_state->client_uuids) {
        if (!uuid.empty()) {
          out.push_back(uuid);
        }
      }
      return out;
    }

    client_disconnect_result_t disconnect_client(const std::string &client_uuid) {
      if (client_uuid.empty()) {
        return {};
      }

      std::vector<std::shared_ptr<stream::session_t>> to_cleanup;
      bool removed_pending = false;
      client_disconnect_result_t result;
      bool vulkan_hdr_layer_active = false;
      {
        std::lock_guard lock {pending_launches_mutex};
        for (auto it = pending_launches.begin(); it != pending_launches.end();) {
          if (it->second.session->client_uuid == client_uuid) {
            result.pending_roles.push_back(it->second.session->role);
            result.pending_generations.push_back(it->second.session->role_generation);
            it = pending_launches.erase(it);
            removed_pending = true;
          } else {
            ++it;
          }
        }
      }
      {
        auto lg = _session_state.lock();
        for (auto i = _session_state->sessions.begin(); i != _session_state->sessions.end();) {
          auto session = *i;
          const auto it_uuid = _session_state->client_uuids.find(session.get());
          if (it_uuid != _session_state->client_uuids.end() && it_uuid->second == client_uuid) {
            to_cleanup.emplace_back(session);
            _session_state->client_uuids.erase(session.get());
            _session_state->vulkan_hdr_layer_sessions.erase(session.get());
            i = _session_state->sessions.erase(i);
          } else {
            ++i;
          }
        }
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif

      for (auto &slot : to_cleanup) {
        stream::session::stop(*slot);
        stream::session::join(*slot);
      }

      if (!to_cleanup.empty()) {
        nvhttp::mark_client_last_seen(client_uuid);
      }
      result.disconnected = removed_pending || !to_cleanup.empty();
      return result;
    }

    bool disconnect_remote_role(
      const std::string_view client_uuid,
      const remote_session::role_e role,
      const std::optional<std::uint64_t> generation,
      const bool lifecycle_lock_held = false
    ) {
      std::vector<std::shared_ptr<stream::session_t>> to_cleanup;
      bool removed_pending = false;
      bool vulkan_hdr_layer_active = false;
      {
        std::lock_guard lock {pending_launches_mutex};
        for (auto it = pending_launches.begin(); it != pending_launches.end();) {
          const auto &pending = it->second.session;
          if (pending->client_uuid == client_uuid && pending->role == role && (!generation || pending->role_generation == *generation)) {
            it = pending_launches.erase(it);
            removed_pending = true;
          } else {
            ++it;
          }
        }
      }
      {
        auto lg = _session_state.lock();
        for (auto it = _session_state->sessions.begin(); it != _session_state->sessions.end();) {
          const auto &session = *it;
          if (stream::session::uuid_match(*session, client_uuid) && stream::session::remote_role_match(*session, role, generation)) {
            to_cleanup.emplace_back(session);
            _session_state->client_uuids.erase(session.get());
            _session_state->vulkan_hdr_layer_sessions.erase(session.get());
            it = _session_state->sessions.erase(it);
          } else {
            ++it;
          }
        }
        vulkan_hdr_layer_active = vulkan_hdr_layer_active_locked();
      }
#ifdef _WIN32
      set_vulkan_hdr_layer_streaming_active(vulkan_hdr_layer_active);
#endif
      for (auto &session : to_cleanup) {
        stream::session::stop(*session);
        stream::session::join(*session, lifecycle_lock_held);
      }
      return removed_pending || !to_cleanup.empty();
    }

    /**
     * @brief Runs an iteration of the RTSP server loop
     */
    void iterate() {
      // If we have a session, we will return to the server loop every
      // 500ms to allow session cleanup to happen.
      if (session_count() > 0) {
        io_context.run_one_for(500ms);
      } else {
        io_context.run_one();
      }
    }

    /**
     * @brief Stop the RTSP server.
     */
    void stop_startup_pool() {
      stopping.store(true, std::memory_order_release);
      boost::system::error_code ec;
      acceptor.close(ec);
      startup_pool.stop();
      startup_pool.join();
    }

    void stop() {
      io_context.stop();
      clear();
    }

    std::shared_ptr<stream::session_t>
      find_session(const std::string_view &uuid) {
      auto lg = _session_state.lock();

      for (auto &session : _session_state->sessions) {
        if (stream::session::uuid_match(*session, uuid)) {
          return session;
        }
      }

      return nullptr;
    }

    std::list<std::string>
      get_all_session_uuids() {
      std::list<std::string> uuids;
      auto lg = _session_state.lock();
      for (auto &session : _session_state->sessions) {
        uuids.push_back(stream::session::uuid(*session));
      }
      return uuids;
    }

    std::vector<std::shared_ptr<stream::session_t>>
      get_sessions_snapshot() {
      std::vector<std::shared_ptr<stream::session_t>> sessions;
      auto lg = _session_state.lock();
      sessions.reserve(_session_state->sessions.size());
      for (auto &session : _session_state->sessions) {
        sessions.push_back(session);
      }
      return sessions;
    }

  private:
    std::unordered_map<std::string_view, cmd_func_t> _map_cmd_cb;

    struct session_state_t {
      std::set<std::shared_ptr<stream::session_t>> sessions;
      std::unordered_map<const stream::session_t *, std::string> client_uuids;
      std::unordered_set<const stream::session_t *> vulkan_hdr_layer_sessions;
      bool vulkan_hdr_layer_pending_stream = false;
    };

    sync_util::sync_t<session_state_t> _session_state;

    boost::asio::io_context io_context;
    tcp::acceptor acceptor {io_context};
    boost::asio::steady_timer raised_timer {io_context};
    std::mutex pending_launches_mutex;
    std::unordered_map<std::uint32_t, pending_launch_t> pending_launches;
    std::string plaintext_route_warning;
    thread_pool_util::ThreadPool startup_pool;
    std::atomic_int startup_tasks {0};
    std::atomic_bool stopping {false};
    std::optional<std::array<std::uint8_t, 16>> abandoned_startup_virtual_display_guid_bytes;

    std::shared_ptr<socket_t> next_socket;
  };

  rtsp_server_t server {};

  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session) {
    return server.session_raise(std::move(launch_session));
  }

  std::string plaintext_route_warning() { return server.plaintext_warning(); }

  bool disconnect_game_sessions(const std::string_view client_uuid, const bool lifecycle_lock_held) {
    return server.disconnect_remote_role(client_uuid, remote_session::role_e::game, std::nullopt, lifecycle_lock_held);
  }

  bool disconnect_remote_role_session(const std::string_view client_uuid, const remote_session::role_e role, const std::uint64_t generation, const bool lifecycle_lock_held) {
    return server.disconnect_remote_role(client_uuid, role, generation, lifecycle_lock_held);
  }

  void launch_session_clear(uint32_t launch_session_id) {
    server.session_clear(launch_session_id);
  }

  bool has_pending_launch_or_startup() {
    return server.has_pending_launch_or_startup();
  }

  void set_vulkan_hdr_layer_pending_stream(bool active) {
    server.set_pending_vulkan_hdr_layer_stream(active);
  }

  int session_count() {
    // Ensure session_count is up-to-date
    server.clear(false);

    return server.session_count();
  }

  int session_count_no_cleanup() {
    return server.session_count();
  }

  std::shared_ptr<stream::session_t> find_session(const std::string_view &uuid) {
    return server.find_session(uuid);
  }

  std::list<std::string> get_all_session_uuids() {
    return server.get_all_session_uuids();
  }

  std::vector<std::shared_ptr<stream::session_t>> get_sessions_snapshot() {
    return server.get_sessions_snapshot();
  }

  void terminate_sessions(bool preserve_pending_launch) {
    server.clear(true, preserve_pending_launch);
  }

  std::list<std::string> get_all_session_client_uuids() {
    server.clear(false);
    return server.get_all_client_uuids();
  }

  bool disconnect_client_sessions(const std::string &client_uuid) {
    return disconnect_client_sessions_with_result(client_uuid).disconnected;
  }

  client_disconnect_result_t disconnect_client_sessions_with_result(const std::string &client_uuid) {
    server.clear(false);
    return server.disconnect_client(client_uuid);
  }

  int send(tcp::socket &sock, const std::string_view &sv) {
    std::size_t bytes_send = 0;

    while (bytes_send != sv.size()) {
      boost::system::error_code ec;
      bytes_send += sock.send(boost::asio::buffer(sv.substr(bytes_send)), 0, ec);

      if (ec) {
        BOOST_LOG(error) << "RTSP: Couldn't send data over tcp socket: "sv << ec.message();
        return -1;
      }
    }

    return 0;
  }

  void respond(tcp::socket &sock, launch_session_t &session, msg_t &resp) {
    auto payload = std::make_pair(resp->payload, resp->payloadLength);

    // Restore response message for proper destruction
    auto lg = util::fail_guard([&]() {
      resp->payload = payload.first;
      resp->payloadLength = payload.second;
    });

    resp->payload = nullptr;
    resp->payloadLength = 0;

    int serialized_len;
    util::c_ptr<char> raw_resp {serializeRtspMessage(resp.get(), &serialized_len)};

    std::ostringstream summary;
    summary << "RTSP RESPONSE seq=" << resp->sequenceNumber;
    if (resp->type == TYPE_RESPONSE) {
      summary << " status=" << resp->message.response.statusString
              << " code=" << resp->message.response.statusCode;
    }
    summary << " payload_len=" << payload.second;

    BOOST_LOG(debug) << summary.str();
    BOOST_LOG(verbose)
      << "---Begin Response---"sv << std::endl
      << std::string_view {raw_resp.get(), (std::size_t) serialized_len} << std::endl
      << std::string_view {payload.first, (std::size_t) payload.second} << std::endl
      << "---End Response---"sv << std::endl;

    // Encrypt the RTSP message if encryption is enabled
    if (session.rtsp_cipher) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // sent to each client before the IV repeats.
      crypto::aes_t iv(12);
      session.rtsp_iv_counter++;
      std::copy_n((uint8_t *) &session.rtsp_iv_counter, sizeof(session.rtsp_iv_counter), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'R';  // RTSP

      // Allocate the message with an empty header and reserved space for the payload
      auto payload_length = serialized_len + payload.second;
      std::vector<uint8_t> message(sizeof(encrypted_rtsp_header_t));
      message.reserve(message.size() + payload_length);

      // Copy the complete plaintext into the message
      std::copy_n(raw_resp.get(), serialized_len, std::back_inserter(message));
      std::copy_n(payload.first, payload.second, std::back_inserter(message));

      // Initialize the message header
      auto header = (encrypted_rtsp_header_t *) message.data();
      header->typeAndLength = util::endian::big<std::uint32_t>(encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT + payload_length);
      header->sequenceNumber = util::endian::big<std::uint32_t>(session.rtsp_iv_counter);

      // Encrypt the RTSP message in place
      session.rtsp_cipher->encrypt(std::string_view {(const char *) header->payload(), (std::size_t) payload_length}, header->tag, &iv);

      // Send the full encrypted message
      send(sock, std::string_view {(char *) message.data(), message.size()});
    } else {
      std::string_view tmp_resp {raw_resp.get(), (size_t) serialized_len};

      // Send the plaintext RTSP message header
      if (send(sock, tmp_resp)) {
        return;
      }

      // Send the plaintext RTSP message payload (if present)
      send(sock, std::string_view {payload.first, (std::size_t) payload.second});
    }
  }

  void respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload) {
    msg_t resp {new msg_t::element_type};
    createRtspResponse(resp.get(), nullptr, 0, const_cast<char *>("RTSP/1.0"), statuscode, const_cast<char *>(status_msg), seqn, options, const_cast<char *>(payload.data()), (int) payload.size());

    respond(sock, session, resp);
  }

  bool cmd_not_found(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    respond(socket->sock, *session, nullptr, 404, "NOT FOUND", req->sequenceNumber, {});
    return false;
  }

  bool cmd_option(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(socket->sock, *session, &option, 200, "OK", req->sequenceNumber, {});
    return false;
  }

  bool cmd_describe(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::stringstream ss;

    // Tell the client about our supported features
    ss << "a=x-ss-general.featureFlags:" << (uint32_t) platf::get_capabilities() << std::endl;

    // Always request new control stream encryption if the client supports it
    uint32_t encryption_flags_supported = SS_ENC_CONTROL_V2 | SS_ENC_AUDIO;
    uint32_t encryption_flags_requested = SS_ENC_CONTROL_V2;

    // Determine the encryption desired for this remote endpoint
    auto encryption_mode = net::encryption_mode_for_address(socket->sock.remote_endpoint().address());
    if (encryption_mode != config::ENCRYPTION_MODE_NEVER) {
      // Advertise support for video encryption if it's not disabled
      encryption_flags_supported |= SS_ENC_VIDEO;

      // If it's mandatory, also request it to enable use if the client
      // didn't explicitly opt in, but it otherwise has support.
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
        encryption_flags_requested |= SS_ENC_VIDEO | SS_ENC_AUDIO;
      }
    }

    // Report supported and required encryption flags
    ss << "a=x-ss-general.encryptionSupported:" << encryption_flags_supported << std::endl;
    ss << "a=x-ss-general.encryptionRequested:" << encryption_flags_requested << std::endl;

    if (video::last_encoder_probe_supported_ref_frames_invalidation) {
      ss << "a=x-nv-video[0].refPicInvalidation:1"sv << std::endl;
    }

    if (video::active_hevc_mode != 1) {
      ss << "sprop-parameter-sets=AAAAAU"sv << std::endl;
    }

    if (video::active_av1_mode != 1) {
      ss << "a=rtpmap:98 AV1/90000"sv << std::endl;
    }

    if (!session->surround_params.empty()) {
      // If we have our own surround parameters, advertise them twice first
      ss << "a=fmtp:97 surround-params="sv << session->surround_params << std::endl;
      ss << "a=fmtp:97 surround-params="sv << session->surround_params << std::endl;
    }

    for (int x = 0; x < audio::MAX_STREAM_CONFIG; ++x) {
      auto &stream_config = audio::stream_configs[x];
      std::uint8_t mapping[platf::speaker::MAX_SPEAKERS];

      auto mapping_p = stream_config.mapping;

      /**
       * GFE advertises incorrect mapping for normal quality configurations,
       * as a result, Moonlight rotates all channels from index '3' to the right
       * To work around this, rotate channels to the left from index '3'
       */
      if (x == audio::SURROUND51 || x == audio::SURROUND71) {
        std::copy_n(mapping_p, stream_config.channelCount, mapping);
        std::rotate(mapping + 3, mapping + 4, mapping + audio::MAX_STREAM_CONFIG);

        mapping_p = mapping;
      }

      ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount << stream_config.streams << stream_config.coupledStreams;

      std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t digit) {
        ss << (char) (digit + '0');
      });

      ss << std::endl;
    }

    respond(socket->sock, *session, &option, 200, "OK", req->sequenceNumber, ss.str());
    return false;
  }

  bool cmd_setup(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    OPTION_ITEM options[4] {};

    auto &seqn = options[0];
    auto &session_option = options[1];
    auto &port_option = options[2];
    auto &payload_option = options[3];

    seqn.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    seqn.content = const_cast<char *>(seqn_str.c_str());

    std::string_view target {req->message.request.target};
    auto begin = std::find(std::begin(target), std::end(target), '=') + 1;
    auto end = std::find(begin, std::end(target), '/');
    std::string_view type {begin, (size_t) std::distance(begin, end)};

    std::uint16_t port;
    if (type == "audio"sv) {
      port = net::map_port(stream::AUDIO_STREAM_PORT);
    } else if (type == "video"sv) {
      port = net::map_port(stream::VIDEO_STREAM_PORT);
    } else if (type == "control"sv) {
      port = net::map_port(stream::CONTROL_PORT);
    } else {
      cmd_not_found(server, socket, session, std::move(req));
      return false;
    }

    seqn.next = &session_option;

    session_option.option = const_cast<char *>("Session");
    session_option.content = const_cast<char *>("DEADBEEFCAFE;timeout = 90");

    session_option.next = &port_option;

    // Moonlight merely requires 'server_port=<port>'
    auto port_value = std::format("server_port={}", static_cast<int>(port));

    port_option.option = const_cast<char *>("Transport");
    port_option.content = port_value.data();

    // Send identifiers that will be echoed in the other connections
    auto connect_data = std::to_string(session->control_connect_data);
    if (type == "control"sv) {
      payload_option.option = const_cast<char *>("X-SS-Connect-Data");
      payload_option.content = connect_data.data();
    } else {
      payload_option.option = const_cast<char *>("X-SS-Ping-Payload");
      payload_option.content = session->av_ping_payload.data();
    }

    port_option.next = &payload_option;

    respond(socket->sock, *session, &seqn, 200, "OK", req->sequenceNumber, {});
    return false;
  }

  bool cmd_announce(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::string_view payload {req->payload, (size_t) req->payloadLength};

    std::vector<std::string_view> lines;

    auto whitespace = [](char ch) {
      return ch == '\n' || ch == '\r';
    };

    {
      auto pos = std::begin(payload);
      auto begin = pos;
      while (pos != std::end(payload)) {
        if (whitespace(*pos++)) {
          lines.emplace_back(begin, pos - begin - 1);

          while (pos != std::end(payload) && whitespace(*pos)) {
            ++pos;
          }
          begin = pos;
        }
      }
    }

    std::string_view client;
    std::unordered_map<std::string_view, std::string_view> args;

    for (auto line : lines) {
      auto type = line.substr(0, 2);
      if (type == "s="sv) {
        client = line.substr(2);
      } else if (type == "a=") {
        auto pos = line.find(':');

        auto name = line.substr(2, pos - 2);
        auto val = line.substr(pos + 1);

        if (val[val.size() - 1] == ' ') {
          val = val.substr(0, val.size() - 1);
        }
        args.emplace(name, val);
      }
    }

    // Initialize any omitted parameters to defaults
    args.try_emplace("x-nv-video[0].encoderCscMode"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].bitStreamFormat"sv, "0"sv);
    args.try_emplace("x-nv-video[0].dynamicRangeMode"sv, "0"sv);
    args.try_emplace("x-nv-aqos.packetDuration"sv, "5"sv);
    args.try_emplace("x-nv-general.useReliableUdp"sv, "1"sv);
    args.try_emplace("x-nv-vqos[0].fec.minRequiredFecPackets"sv, "0"sv);
    args.try_emplace("x-nv-general.featureFlags"sv, "135"sv);
    args.try_emplace("x-ml-general.featureFlags"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].qosTrafficType"sv, "5"sv);
    args.try_emplace("x-nv-aqos.qosTrafficType"sv, "4"sv);
    args.try_emplace("x-ml-video.configuredBitrateKbps"sv, "0"sv);
    args.try_emplace("x-ss-general.encryptionEnabled"sv, "0"sv);
    args.try_emplace("x-ss-video[0].chromaSamplingType"sv, "0"sv);
    args.try_emplace("x-ss-video[0].intraRefresh"sv, "0"sv);
    args.try_emplace("x-nv-video[0].clientRefreshRateX100"sv, "0"sv);

    stream::config_t config {};
    config.gen1_framegen_fix = false;
    config.gen2_framegen_fix = false;
    config.frame_generation_enabled = false;

    std::int64_t configuredBitrateKbps;
    config.audio.flags[audio::config_t::HOST_AUDIO] = session->host_audio;
    try {
      config.audio.channels = (int) util::from_view(args.at("x-nv-audio.surround.numChannels"sv));
      config.audio.mask = (int) util::from_view(args.at("x-nv-audio.surround.channelMask"sv));
      config.audio.packetDuration = (int) util::from_view(args.at("x-nv-aqos.packetDuration"sv));

      config.audio.flags[audio::config_t::HIGH_QUALITY] =
        util::from_view(args.at("x-nv-audio.surround.AudioQuality"sv));

      config.controlProtocolType = (int) util::from_view(args.at("x-nv-general.useReliableUdp"sv));
      config.packetsize = (int) util::from_view(args.at("x-nv-video[0].packetSize"sv));
      config.minRequiredFecPackets = (int) util::from_view(args.at("x-nv-vqos[0].fec.minRequiredFecPackets"sv));
      config.mlFeatureFlags = (int) util::from_view(args.at("x-ml-general.featureFlags"sv));
      config.audioQosType = (int) util::from_view(args.at("x-nv-aqos.qosTrafficType"sv));
      config.videoQosType = (int) util::from_view(args.at("x-nv-vqos[0].qosTrafficType"sv));
      config.encryptionFlagsEnabled = (uint32_t) util::from_view(args.at("x-ss-general.encryptionEnabled"sv));

      // Legacy clients use nvFeatureFlags to indicate support for audio encryption
      if (util::from_view(args.at("x-nv-general.featureFlags"sv)) & 0x20) {
        config.encryptionFlagsEnabled |= SS_ENC_AUDIO;
      }

      config.monitor.height = (int) util::from_view(args.at("x-nv-video[0].clientViewportHt"sv));
      config.monitor.width = (int) util::from_view(args.at("x-nv-video[0].clientViewportWd"sv));
      config.monitor.framerate = (int) util::from_view(args.at("x-nv-video[0].maxFPS"sv));
      config.monitor.framerateX100 = (int) util::from_view(args.at("x-nv-video[0].clientRefreshRateX100"sv));
      config.monitor.bitrate = (int) util::from_view(args.at("x-nv-vqos[0].bw.maximumBitrateKbps"sv));
      config.monitor.client_requested_bitrate = config.monitor.bitrate;
      config.monitor.slicesPerFrame = (int) util::from_view(args.at("x-nv-video[0].videoEncoderSlicesPerFrame"sv));
      config.monitor.numRefFrames = (int) util::from_view(args.at("x-nv-video[0].maxNumReferenceFrames"sv));
      config.monitor.encoderCscMode = (int) util::from_view(args.at("x-nv-video[0].encoderCscMode"sv));
      config.monitor.videoFormat = (int) util::from_view(args.at("x-nv-vqos[0].bitStreamFormat"sv));
      config.monitor.dynamicRange = (int) util::from_view(args.at("x-nv-video[0].dynamicRangeMode"sv));
      config.monitor.chromaSamplingType = (int) util::from_view(args.at("x-ss-video[0].chromaSamplingType"sv));
      config.monitor.enableIntraRefresh = (int) util::from_view(args.at("x-ss-video[0].intraRefresh"sv));
      config.monitor.vrr_low_latency = session->client_vrr_requested;

      if (config.monitor.framerate > 1000) {
        config.monitor.encodingFramerate = config.monitor.framerate;
      } else {
        config.monitor.encodingFramerate = config.monitor.framerate * 1000;
      }

      // When fractional refresh rate requested from client side, it should be well above 1000fps.
      // 4000fps is when Warp2 Mode is enabled on the client, requested framerate can be actual * 4.
      if (config.monitor.framerate > 4000) {
        config.monitor.framerate = std::round((float) config.monitor.framerate / 1000);
      }

      // Validate that clientRefreshRateX100 is consistent with maxFPS.
      // Some clients send a stale or incorrect clientRefreshRateX100 (e.g. 6000 = 60fps)
      // while requesting a higher maxFPS (e.g. 120). Since framerateX100 unconditionally
      // overrides capture pacing, an inconsistent value caps the stream to the wrong fps.
      if (config.monitor.framerateX100 > 0 && config.monitor.framerate > 0) {
        int fps_from_x100 = (int) std::lround(config.monitor.framerateX100 / 100.0);
        if (fps_from_x100 != config.monitor.framerate) {
          BOOST_LOG(warning) << "clientRefreshRateX100 ("
                             << config.monitor.framerateX100 << " = " << fps_from_x100
                             << "fps) disagrees with maxFPS (" << config.monitor.framerate
                             << "); ignoring clientRefreshRateX100";
          config.monitor.framerateX100 = 0;
        }
      }

      configuredBitrateKbps = util::from_view(args.at("x-ml-video.configuredBitrateKbps"sv));
    } catch (std::out_of_range &) {
      respond(socket->sock, *session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return false;
    }

    // When using stereo audio, the audio quality is (strangely) indicated by whether the Host field
    // in the RTSP message matches a local interface's IP address. Fortunately, Moonlight always sends
    // 0.0.0.0 when it wants low quality, so it is easy to check without enumerating interfaces.
    if (config.audio.channels == 2) {
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Host"sv == option->option) {
          std::string_view content {option->content};
          BOOST_LOG(debug) << "Found Host: "sv << content;
          config.audio.flags[audio::config_t::HIGH_QUALITY] = (content.find("0.0.0.0"sv) == std::string::npos);
        }
      }
    } else if (session->surround_params.length() > 3) {
      // Channels
      std::uint8_t c = session->surround_params[0] - '0';
      // Streams
      std::uint8_t n = session->surround_params[1] - '0';
      // Coupled streams
      std::uint8_t m = session->surround_params[2] - '0';
      auto valid = false;
      if ((c == 6 || c == 8) && c == config.audio.channels && n + m == c && session->surround_params.length() == c + 3) {
        config.audio.customStreamParams.channelCount = c;
        config.audio.customStreamParams.streams = n;
        config.audio.customStreamParams.coupledStreams = m;
        valid = true;
        for (std::uint8_t i = 0; i < c; i++) {
          config.audio.customStreamParams.mapping[i] = session->surround_params[i + 3] - '0';
          if (config.audio.customStreamParams.mapping[i] >= c) {
            valid = false;
            break;
          }
        }
      }
      config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = valid;
    }
    if (session->continuous_audio) {
      BOOST_LOG(info) << "Client requested continuous audio"sv;
      config.audio.flags[audio::config_t::CONTINUOUS_AUDIO] = true;
    }

    if (config.monitor.vrr_low_latency) {
      BOOST_LOG(info) << "Client requested VRR low-latency stream policy";
    }

    const bool prefer_10bit_sdr = effective_10bit_sdr_requested(*session);
    const bool hevc_main10 = config.monitor.videoFormat == 1 && video::active_hevc_mode >= 3;
    const bool av1_main10 = config.monitor.videoFormat == 2 && video::active_av1_mode >= 3;
    const bool supports_10bit_dynamic_range = hevc_main10 || av1_main10;
    config.monitor.force_sdr = session->force_sdr;
    if (prefer_10bit_sdr) {
      if (supports_10bit_dynamic_range) {
        BOOST_LOG(info) << "Client requested HDR, but 10-bit SDR is enabled for it; encoding Main10 without HDR";
        config.monitor.dynamicRange = 1;
        config.monitor.prefer_sdr_10bit = true;
      } else {
        config.monitor.dynamicRange = 0;
        config.monitor.prefer_sdr_10bit = false;
        BOOST_LOG(info) << "10-bit SDR is enabled for this client, but Main10 is unavailable; using 8-bit SDR encode";
      }
    } else if (config.monitor.dynamicRange == 0) {
      if (session->enable_hdr && supports_10bit_dynamic_range) {
        BOOST_LOG(info) << "RTSP ANNOUNCE requested SDR while launch HDR is enabled; using HDR 10-bit encode";
        config.monitor.dynamicRange = 1;
      }
    }
    apply_rtx_hdr_stream_policy(config.monitor);

    // If the client sent a configured bitrate, we will choose the actual bitrate ourselves
    // by using FEC percentage and audio quality settings. If the calculated bitrate ends up
    // too low, we'll allow it to exceed the limits rather than reducing the encoding bitrate
    // down to nearly nothing.
    if (configuredBitrateKbps) {
      BOOST_LOG(debug) << "Client configured bitrate is "sv << configuredBitrateKbps << " Kbps"sv;

      // Preserve the original wire-bandwidth budget the client asked for so the
      // UI can show it alongside the post-adjustment encoder bitrate.
      config.monitor.client_requested_bitrate = static_cast<int>(configuredBitrateKbps);

      // If the FEC percentage isn't too high, adjust the configured bitrate to ensure video
      // traffic doesn't exceed the user's selected bitrate when the FEC shards are included.
      if (config::stream.fec_percentage <= 80) {
        configuredBitrateKbps /= 100.f / (100 - config::stream.fec_percentage);
      }

      // Adjust the bitrate to account for audio traffic bandwidth usage (capped at 20% reduction).
      // The bitrate per channel is 256 Kbps for high quality mode and 96 Kbps for normal quality.
      auto audioBitrateAdjustment = (config.audio.flags[audio::config_t::HIGH_QUALITY] ? 256 : 96) * config.audio.channels;
      configuredBitrateKbps -= std::min((std::int64_t) audioBitrateAdjustment, configuredBitrateKbps / 5);

      // Reduce it by another 500Kbps to account for A/V packet overhead and control data
      // traffic (capped at 10% reduction).
      configuredBitrateKbps -= std::min((std::int64_t) 500, configuredBitrateKbps / 10);

      BOOST_LOG(debug) << "Final adjusted video encoding bitrate is "sv << configuredBitrateKbps << " Kbps"sv;
      config.monitor.bitrate = (int) configuredBitrateKbps;
    }

    if (config.monitor.videoFormat == 1 && video::active_hevc_mode == 1) {
      BOOST_LOG(warning) << "HEVC is disabled, yet the client requested HEVC"sv;

      respond(socket->sock, *session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return false;
    }

    if (config.monitor.videoFormat == 2 && video::active_av1_mode == 1) {
      BOOST_LOG(warning) << "AV1 is disabled, yet the client requested AV1"sv;

      respond(socket->sock, *session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return false;
    }

    // Check that any required encryption is enabled
    auto encryption_mode = net::encryption_mode_for_address(socket->sock.remote_endpoint().address());
    if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY &&
        (config.encryptionFlagsEnabled & (SS_ENC_VIDEO | SS_ENC_AUDIO)) != (SS_ENC_VIDEO | SS_ENC_AUDIO)) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      respond(socket->sock, *session, &option, 403, "Forbidden", req->sequenceNumber, {});
      return false;
    }

    boost::system::error_code remote_ec;
    auto remote_endpoint = socket->sock.remote_endpoint(remote_ec);
    if (remote_ec) {
      BOOST_LOG(error) << "Failed to query RTSP remote endpoint: "sv << remote_ec.message();
      respond(socket->sock, *session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return false;
    }

    auto remote_address = remote_endpoint.address().to_string();

    const int sequence_number = req->sequenceNumber;
    const std::string client_uuid = session->client_uuid;
    auto launch_session = make_startup_launch_session_snapshot(*session);
    try {
      server->run_startup([server, socket = std::move(socket), session = std::move(session), launch_session, config = std::move(config), remote_address = std::move(remote_address), client_uuid, sequence_number]() mutable {
        // Apply deferred updates and take the hot-apply gate on the startup worker so
        // display/config churn cannot stall the RTSP io_context.
        std::unique_lock<std::mutex> lifecycle_lock(nvhttp::stream_lifecycle_mutex());
        const auto pending_launch = server->pending_launch(launch_session->id);
        if (!pending_launch ||
            pending_launch->id != launch_session->id ||
            pending_launch->unique_id != launch_session->unique_id) {
          // The launch may have timed out or been canceled while this worker
          // waited for lifecycle ownership. Never resurrect that stale request.
          server->post([server, socket = std::move(socket), session = std::move(session), sequence_number]() mutable {
            auto fg = util::fail_guard([server]() {
              server->finish_startup();
            });
            OPTION_ITEM completion_option {};
            completion_option.option = const_cast<char *>("CSeq");
            auto completion_seqn = std::to_string(sequence_number);
            completion_option.content = const_cast<char *>(completion_seqn.c_str());
            BOOST_LOG(info) << "Discarding canceled or expired RTSP startup request.";
            respond(socket->sock, *session, &completion_option, 503, "Service Unavailable", sequence_number, {});
            server->shutdown_socket(*socket);
          });
          return;
        }

        config::maybe_apply_deferred();
        auto _hot_apply_gate = config::acquire_apply_read_gate();

        config.gen1_framegen_fix = launch_session->gen1_framegen_fix;
        config.gen2_framegen_fix = launch_session->gen2_framegen_fix;
        config.frame_generation_enabled = launch_session->frame_generation_enabled;
        config.lossless_scaling_framegen = launch_session->lossless_scaling_framegen;
        config.frame_generation_provider = launch_session->frame_generation_provider;
        config.lossless_scaling_target_fps = launch_session->lossless_scaling_target_fps;
        config.lossless_scaling_rtss_limit = launch_session->lossless_scaling_rtss_limit;

        std::shared_ptr<stream::session_t> stream_session;
        bool startup_failed = true;
        std::string startup_error;

        try {
          stream_session = stream::session::alloc(config, *launch_session);
          startup_failed = stream::session::start(*stream_session, remote_address) != 0;
        } catch (const std::exception &e) {
          startup_error = e.what();
        } catch (...) {
          startup_error = "unknown exception";
        }

        const bool stream_hdr_enabled = activates_vulkan_hdr_layer_for_stream(config.monitor);
        if (!startup_failed) {
          // Publish the active session before releasing the lifecycle gate.
          // Cancellation can then find and synchronously join every started
          // session instead of racing the posted RTSP response callback.
          server->insert(stream_session, client_uuid, stream_session && stream_hdr_enabled);
        }
        server->post([server, socket = std::move(socket), session = std::move(session), sequence_number, startup_failed, startup_error = std::move(startup_error)]() mutable {
          auto fg = util::fail_guard([server]() {
            server->finish_startup();
          });
          OPTION_ITEM completion_option {};
          completion_option.option = const_cast<char *>("CSeq");
          auto completion_seqn = std::to_string(sequence_number);
          completion_option.content = const_cast<char *>(completion_seqn.c_str());

          if (startup_failed) {
            server->set_pending_vulkan_hdr_layer_stream(false);
            if (startup_error.empty()) {
              BOOST_LOG(error) << "Failed to start a streaming session"sv;
            } else {
              BOOST_LOG(error) << "Failed to start a streaming session: "sv << startup_error;
            }
            respond(socket->sock, *session, &completion_option, 500, "Internal Server Error", sequence_number, {});
          } else {
            respond(socket->sock, *session, &completion_option, 200, "OK", sequence_number, {});
          }

          server->shutdown_socket(*socket);
        });
      });
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to queue RTSP ANNOUNCE startup task: "sv << e.what();
      respond(socket->sock, *session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return false;
    } catch (...) {
      BOOST_LOG(error) << "Failed to queue RTSP ANNOUNCE startup task with an unknown exception"sv;
      respond(socket->sock, *session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return false;
    }

    return true;
  }

  bool cmd_play(rtsp_server_t *server, std::shared_ptr<socket_t> socket, std::shared_ptr<launch_session_t> session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(socket->sock, *session, &option, 200, "OK", req->sequenceNumber, {});
    return false;
  }

  void start() {
    platf::set_thread_name("rtsp");
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    server.map("OPTIONS"sv, &cmd_option);
    server.map("DESCRIBE"sv, &cmd_describe);
    server.map("SETUP"sv, &cmd_setup);
    server.map("ANNOUNCE"sv, &cmd_announce);
    server.map("PLAY"sv, &cmd_play);

    boost::system::error_code ec;
    if (server.bind(net::af_from_enum_string(config::sunshine.address_family), net::map_port(rtsp_stream::RTSP_SETUP_PORT), ec)) {
      BOOST_LOG(fatal) << "Couldn't bind RTSP server to port ["sv << net::map_port(rtsp_stream::RTSP_SETUP_PORT) << "], " << ec.message();
      shutdown_event->raise(true);

      return;
    }

    std::thread rtsp_thread {[&shutdown_event] {
      platf::set_thread_name("rtsp::handler");
      auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

      while (!shutdown_event->peek() || server.startup_count() > 0) {
        server.iterate();

        if (broadcast_shutdown_event->peek()) {
          server.clear();
        } else {
          // cleanup all stopped sessions
          server.clear(false);
        }
      }

      server.clear();
    }};

    // Wait for shutdown
    shutdown_event->view();

    // Drain startup workers before stopping the io_context so any posted ANNOUNCE
    // completion can run on the RTSP loop instead of being abandoned during shutdown.
    server.stop_startup_pool();
    // Stop the server and join the server thread
    server.stop();
    rtsp_thread.join();
  }

  void print_msg(PRTSP_MESSAGE msg) {
    std::string_view type = msg->type == TYPE_RESPONSE ? "RESPONSE"sv : "REQUEST"sv;

    std::string_view payload {msg->payload, (size_t) msg->payloadLength};
    std::string_view protocol {msg->protocol};
    auto seqnm = msg->sequenceNumber;
    std::string_view messageBuffer {msg->messageBuffer};

    std::ostringstream summary;
    summary << "RTSP " << type << " seq=" << seqnm << " protocol=" << protocol;
    BOOST_LOG(verbose) << "payload :: "sv << payload;

    if (msg->type == TYPE_RESPONSE) {
      auto &resp = msg->message.response;

      auto statuscode = resp.statusCode;
      std::string_view status {resp.statusString};

      summary << " status=" << status << " code=" << statuscode;
      BOOST_LOG(verbose) << "statuscode :: "sv << statuscode;
      BOOST_LOG(verbose) << "status :: "sv << status;
    } else {
      auto &req = msg->message.request;

      std::string_view command {req.command};
      std::string_view target {req.target};

      summary << " command=" << command << " target=" << target;
      BOOST_LOG(verbose) << "command :: "sv << command;
      BOOST_LOG(verbose) << "target :: "sv << target;
    }

    for (auto option = msg->options; option != nullptr; option = option->next) {
      std::string_view content {option->content};
      std::string_view name {option->option};

      BOOST_LOG(verbose) << name << " :: "sv << content;
    }

    BOOST_LOG(debug) << summary.str();

    BOOST_LOG(verbose) << "---Begin MessageBuffer---"sv << std::endl
                       << messageBuffer << std::endl
                       << "---End MessageBuffer---"sv << std::endl;
  }
}  // namespace rtsp_stream
