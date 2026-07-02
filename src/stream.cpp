/**
 * @file src/stream.cpp
 * @brief Definitions for the streaming protocols.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/endian/arithmetic.hpp>
#include <openssl/err.h>
#include <opus/opus.h>

extern "C" {
  // clang-format off
#include <moonlight-common-c/src/Limelight-internal.h>
#include "rswrapper.h"
  // clang-format on
}

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "display_helper_integration.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "session_history.h"
#include "stream.h"
#include "sync.h"
#include "system_tray.h"
#include "thread_safe.h"
#include "update.h"
#include "utility.h"
#include "uuid.h"
#include "webrtc_stream.h"
#ifdef _WIN32
  #include "platform/windows/frame_limiter.h"
  #include "platform/windows/display.h"
  #include "platform/windows/ipc/misc_utils.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
  #include "platform/windows/virtual_display_cleanup.h"
#endif

#define IDX_START_A 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_PERIODIC_PING 8
#define IDX_REQUEST_IDR_FRAME 9
#define IDX_ENCRYPTED 10
#define IDX_HDR_MODE 11
#define IDX_RUMBLE_TRIGGER_DATA 12
#define IDX_SET_MOTION_EVENT 13
#define IDX_SET_RGB_LED 14
#define IDX_EXEC_SERVER_CMD 15
#define IDX_SET_CLIPBOARD 16
#define IDX_FILE_TRANSFER_NONCE_REQUEST 17
#define IDX_SET_ADAPTIVE_TRIGGERS 18
#define IDX_MIC_AUDIO_DATA 19

static const short packetTypes[] = {
  0x0305,  // Start A
  0x0307,  // Start B
  0x0301,  // Invalidate reference frames
  0x0201,  // Loss Stats
  0x0204,  // Frame Stats (unused)
  0x0206,  // Input data
  0x010b,  // Rumble data
  0x0109,  // Termination
  0x0200,  // Periodic Ping
  0x0302,  // IDR frame
  0x0001,  // fully encrypted
  0x010e,  // HDR mode
  0x5500,  // Rumble triggers (Sunshine protocol extension)
  0x5501,  // Set motion event (Sunshine protocol extension)
  0x5502,  // Set RGB LED (Sunshine protocol extension)
  0x3000,  // Execute Server Command (Apollo protocol extension)
  0x3001,  // Set Clipboard (Apollo protocol extension)
  0x3002,  // File transfer nonce request (Apollo protocol extension)
  0x5503,  // Set Adaptive triggers (Sunshine protocol extension)
  0x3003,  // Mic audio data (Apollo protocol extension)
};

namespace asio = boost::asio;
namespace sys = boost::system;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace stream {
  namespace {
    std::string current_server_version() {
      std::string version = PROJECT_VERSION;
#ifdef PROJECT_VERSION_PRERELEASE
      if (std::string_view(PROJECT_VERSION_PRERELEASE).size() > 0) {
        version += ' ';
        version += PROJECT_VERSION_PRERELEASE;
      }
#endif
      return version;
    }

    template<typename T>
    void saturating_add_relaxed(std::atomic<T> &target, T delta) {
      static_assert(std::is_integral_v<T>, "saturating_add_relaxed requires integral atomics");

      auto current = target.load(std::memory_order_relaxed);
      while (true) {
        T next {};
        if constexpr (std::is_unsigned_v<T>) {
          next = current > std::numeric_limits<T>::max() - delta ? std::numeric_limits<T>::max() : static_cast<T>(current + delta);
        } else {
          if (delta > 0 && current > std::numeric_limits<T>::max() - delta) {
            next = std::numeric_limits<T>::max();
          } else if (delta < 0 && current < std::numeric_limits<T>::lowest() - delta) {
            next = std::numeric_limits<T>::lowest();
          } else {
            next = static_cast<T>(current + delta);
          }
        }

        if (target.compare_exchange_weak(current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
          return;
        }
      }
    }
  }  // namespace

  // Set to true on first 0x3003 mic packet receipt for timeout diagnostics.
  static std::atomic<bool> mic_first_packet_received {false};

  // Stats from the most recently ended session, for the web UI mic status card.
  static std::atomic<bool> g_mic_has_last_session {false};
  static std::atomic<std::uint64_t> g_mic_last_packets_received {0};
  static std::atomic<std::uint64_t> g_mic_last_frames_written {0};

  // Count of live sessions currently holding the default-capture switch.
  // mic_first_packet_received alone is unsafe as a "capture in use" signal:
  // a second session's start resets it while the first still holds the switch.
  static std::atomic<int> g_mic_capture_switched_sessions {0};

  enum class socket_e : int {
    video,  ///< Video
    audio  ///< Audio
  };

  namespace session {
  }

#ifdef _WIN32
  namespace {
    std::atomic_uint64_t g_paused_display_cleanup_generation {0};

    void schedule_paused_display_cleanup(
      std::chrono::seconds timeout,
      std::string reason,
      bool enforce_display_restore,
      std::optional<std::array<std::uint8_t, 16>> virtual_display_guid_bytes = std::nullopt
    ) {
      const auto generation = g_paused_display_cleanup_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
      std::thread([timeout, generation, reason = std::move(reason), enforce_display_restore, virtual_display_guid_bytes]() {
        std::this_thread::sleep_for(timeout);

        if (g_paused_display_cleanup_generation.load(std::memory_order_acquire) != generation) {
          return;
        }

        if (session::running_sessions.load(std::memory_order_acquire) != 0 || webrtc_stream::has_active_sessions()) {
          return;
        }

        if (proc::proc.running() <= 0) {
          return;
        }

        BOOST_LOG(info) << "Display cleanup: paused stream timeout reached; removing virtual display(s) (reason="
                        << reason << ").";
        const auto cleanup = platf::virtual_display_cleanup::run(
          "paused_session_timeout",
          enforce_display_restore,
          platf::virtual_display_cleanup::revert_order_t::remove_before_restore,
          true,
          virtual_display_guid_bytes
        );
        if (cleanup.helper_revert_dispatched) {
          display_helper_integration::stop_watchdog();
        }
      }).detach();
    }
  }  // namespace
#endif

  void cancel_paused_display_cleanup() {
#ifdef _WIN32
    g_paused_display_cleanup_generation.fetch_add(1, std::memory_order_acq_rel);
#endif
  }

#pragma pack(push, 1)

  struct video_short_frame_header_t {
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint8_t headerType;  // Always 0x01 for short headers

    // Sunshine extension
    // Frame processing latency, in 1/10 ms units
    //     zero when the frame is repeated or there is no backend implementation
    boost::endian::little_uint16_at frame_processing_latency;

    // Currently known values:
    // 1 = Normal P-frame
    // 2 = IDR-frame
    // 4 = P-frame with intra-refresh blocks
    // 5 = P-frame after reference frame invalidation
    std::uint8_t frameType;

    // Length of the final packet payload for codecs that cannot handle
    // zero padding, such as AV1 (Sunshine extension).
    boost::endian::little_uint16_at lastPayloadLen;

    std::uint8_t unknown[2];
  };

  static_assert(
    sizeof(video_short_frame_header_t) == 8,
    "Short frame header must be 8 bytes"
  );

  struct video_packet_raw_t {
    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    RTP_PACKET rtp;
    char reserved[4];

    NV_VIDEO_PACKET packet;
  };

  struct video_packet_enc_prefix_t {
    std::uint8_t iv[12];  // 12-byte IV is ideal for AES-GCM
    std::uint32_t frameNumber;
    std::uint8_t tag[16];
  };

  struct audio_packet_t {
    RTP_PACKET rtp;
  };

  struct control_header_v2 {
    std::uint16_t type;
    std::uint16_t payloadLength;

    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }
  };

  struct control_terminate_t {
    control_header_v2 header;

    std::uint32_t ec;
  };

  struct control_rumble_t {
    control_header_v2 header;

    std::uint32_t useless;

    std::uint16_t id;
    std::uint16_t lowfreq;
    std::uint16_t highfreq;
  };

  struct control_rumble_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t left;
    std::uint16_t right;
  };

  struct control_set_motion_event_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t reportrate;
    std::uint8_t type;
  };

  struct control_set_rgb_led_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
  };

  struct control_adaptive_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    /**
     * 0x04 - Right trigger
     * 0x08 - Left trigger
     */
    std::uint8_t event_flags;
    std::uint8_t type_left;
    std::uint8_t type_right;
    std::uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
    std::uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
  };

  struct control_hdr_mode_t {
    control_header_v2 header;

    std::uint8_t enabled;

    // Sunshine protocol extension
    SS_HDR_METADATA metadata;
  };

  typedef struct control_encrypted_t {
    std::uint16_t encryptedHeaderType;  // Always LE 0x0001
    std::uint16_t length;  // sizeof(seq) + 16 byte tag + secondary header and data

    // seq is accepted as an arbitrary value in Moonlight
    std::uint32_t seq;  // Monotonically increasing sequence number (used as IV for AES-GCM)

    uint8_t *payload() {
      return (uint8_t *) (this + 1);
    }

    // encrypted control_header_v2 and payload data follow
  } *control_encrypted_p;

  struct audio_fec_packet_t {
    RTP_PACKET rtp;
    AUDIO_FEC_HEADER fecHeader;
  };

#pragma pack(pop)

  constexpr std::size_t round_to_pkcs7_padded(std::size_t size) {
    return ((size + 15) / 16) * 16;
  }

  constexpr std::size_t MAX_AUDIO_PACKET_SIZE = 1400;

  using audio_aes_t = std::array<char, round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE)>;

  using av_session_id_t = std::variant<asio::ip::address, std::string>;  // IP address or SS-Ping-Payload from RTSP handshake
  using message_queue_t = std::shared_ptr<safe::queue_t<std::pair<udp::endpoint, std::string>>>;
  using message_queue_queue_t = std::shared_ptr<safe::queue_t<std::tuple<socket_e, av_session_id_t, message_queue_t>>>;

  // return bytes written on success
  // return -1 on error
  static inline int encode_audio(bool encrypted, const audio::buffer_t &plaintext, uint8_t *destination, crypto::aes_t &iv, crypto::cipher::cbc_t &cbc) {
    // If encryption isn't enabled
    if (!encrypted) {
      std::copy(std::begin(plaintext), std::end(plaintext), destination);
      return (int) plaintext.size();
    }

    return cbc.encrypt(std::string_view {(char *) std::begin(plaintext), plaintext.size()}, destination, &iv);
  }

  static inline void while_starting_do_nothing(std::atomic<session::state_e> &state) {
    while (state.load(std::memory_order_acquire) == session::state_e::STARTING) {
      std::this_thread::sleep_for(1ms);
    }
  }

  class control_server_t {
  public:
    int bind(net::af_e address_family, std::uint16_t port) {
      _host = net::host_create(address_family, _addr, port);

      return !(bool) _host;
    }

    // Get session associated with address.
    // If none are found, try to find a session not yet claimed. (It will be marked by a port of value 0
    // If none of those are found, return nullptr
    session_t *get_session(const net::peer_t peer, uint32_t connect_data);

    // Circular dependency:
    //   iterate refers to session
    //   session refers to broadcast_ctx_t
    //   broadcast_ctx_t refers to control_server_t
    // Therefore, iterate is implemented further down the source file
    void iterate(std::chrono::milliseconds timeout);

    /**
     * @brief Call the handler for a given control stream message.
     * @param type The message type.
     * @param session The session the message was received on.
     * @param payload The payload of the message.
     * @param reinjected `true` if this message is being reprocessed after decryption.
     */
    void call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected);

    void map(uint16_t type, std::function<void(session_t *, const std::string_view &)> cb) {
      _map_type_cb.emplace(type, std::move(cb));
    }

    int send(const std::string_view &payload, net::peer_t peer) {
      auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(peer, 0, packet)) {
        enet_packet_destroy(packet);

        return -1;
      }

      return 0;
    }

    void flush() {
      enet_host_flush(_host.get());
    }

    // Callbacks
    std::unordered_map<std::uint16_t, std::function<void(session_t *, const std::string_view &)>> _map_type_cb;

    // All active sessions (including those still waiting for a peer to connect)
    sync_util::sync_t<std::vector<session_t *>> _sessions;

    // ENet peer to session mapping for sessions with a peer connected
    sync_util::sync_t<std::map<net::peer_t, session_t *>> _peer_to_session;

    ENetAddress _addr;
    net::host_t _host;
  };

  struct broadcast_ctx_t {
    message_queue_queue_t message_queue_queue;

    // Raw datagram counters for the video/audio sockets. Used to distinguish
    // "no UDP reached us at all" (blocked by firewall/VPN filter driver) from
    // "UDP arrived but didn't match a session" when waiting for pings.
    std::atomic<std::uint64_t> video_recv_count {0};
    std::atomic<std::uint64_t> audio_recv_count {0};

    std::thread recv_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::thread control_thread;

    asio::io_context io_context;

    udp::socket video_sock {io_context};
    udp::socket audio_sock {io_context};

    control_server_t control_server;
  };

  // Comparator for the mic jitter buffer: circular (modulo-65536) comparison
  // so that the wraparound from seq 65535 → 0 doesn't cause begin() to return
  // the newest packet. (int16_t)(a - b) < 0 is the RFC 1982 serial comparator.
  struct mic_seq_order_t {
    bool operator()(std::uint16_t a, std::uint16_t b) const {
      return static_cast<std::int16_t>(a - b) < 0;
    }
  };

  struct session_t {
    config_t config;
    int stream_fps = 0;
    int stream_fps_scaled = 0;

    safe::mail_t mail;

    std::shared_ptr<input::input_t> input;

#ifdef _WIN32
    std::shared_future<rtsp_stream::launch_session_t::display_helper_gate_status_e> display_helper_gate;
#endif

    std::thread audioThread;
    std::thread videoThread;

    std::chrono::steady_clock::time_point pingTimeout;

    safe::shared_t<broadcast_ctx_t>::ptr_t broadcast_ref;

    boost::asio::ip::address localAddress;

    struct {
      std::string ping_payload;

      int lowseq;
      udp::endpoint peer;

      std::optional<crypto::cipher::gcm_t> cipher;
      std::uint64_t gcm_iv_counter;

      safe::mail_raw_t::event_t<bool> idr_events;
      safe::mail_raw_t::event_t<std::pair<int64_t, int64_t>> invalidate_ref_frames_events;
      safe::mail_raw_t::event_t<int> bitrate_events;

      std::unique_ptr<platf::deinit_t> qos;
    } video;

    struct {
      crypto::cipher::cbc_t cipher;
      std::string ping_payload;

      std::uint16_t sequenceNumber;
      // avRiKeyId == util::endian::big(First (sizeof(avRiKeyId)) bytes of launch_session->iv)
      std::uint32_t avRiKeyId;
      std::uint32_t timestamp;
      udp::endpoint peer;

      util::buffer_t<char> shards;
      util::buffer_t<uint8_t *> shards_p;

      audio_fec_packet_t fec_packet;
      std::unique_ptr<platf::deinit_t> qos;
    } audio;

    struct {
      crypto::cipher::gcm_t cipher;
      crypto::aes_t legacy_input_enc_iv;  // Only used when the client doesn't support full control stream encryption
      crypto::aes_t incoming_iv;
      crypto::aes_t outgoing_iv;

      std::uint32_t connect_data;  // Used for new clients with ML_FF_SESSION_ID_V1
      std::string expected_peer_address;  // Only used for legacy clients without ML_FF_SESSION_ID_V1

      // Control socket datagram count at session start, used to tell whether any
      // UDP reached the control port when the peer never connects.
      std::uint32_t enet_recv_baseline;

      net::peer_t peer;
      std::uint32_t seq;

      platf::feedback_queue_t feedback_queue;
      safe::mail_raw_t::event_t<video::hdr_info_t> hdr_queue;
    } control;

    std::uint32_t launch_session_id;
    std::mutex metadata_mutex;
    std::string device_name;
    std::string history_device_name;
    std::string device_uuid;
    // Per-stream identifier used by the session_history subsystem. Distinct
    // from device_uuid so that consecutive streams from the same Moonlight
    // client produce separate history rows.
    std::string history_uuid;
    std::string stream_gpu_model;
    crypto::PERM permission;

    std::list<crypto::command_entry_t> do_cmds;
    std::list<crypto::command_entry_t> undo_cmds;

    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::signal_t controlEnd;

    std::atomic<session::state_e> state;

    // Real-time performance counters (updated by broadcast/control threads)
    struct {
      std::atomic<std::uint64_t> frames_sent {0};
      std::atomic<std::uint64_t> packets_sent {0};
      std::atomic<std::uint64_t> bytes_sent {0};
      std::atomic<std::uint32_t> idr_requests {0};
      std::atomic<std::uint32_t> invalidate_ref_count {0};
      std::atomic<std::int64_t> client_reported_losses {0};
      std::atomic<std::uint16_t> last_encode_latency_us10 {0};  // in 1/10 ms units
      std::atomic<std::int64_t> last_frame_index {0};
      std::chrono::steady_clock::time_point start_time {std::chrono::steady_clock::now()};
    } stats;
    // Mic passthrough: Opus decode → virtual audio render endpoint
    static constexpr std::size_t mic_max_queued_packets = 32;

    struct mic_queued_packet_t {
      std::vector<std::uint8_t> payload;
      std::uint16_t seq;
    };

    struct {
      std::unique_ptr<OpusDecoder, void (*)(OpusDecoder *)> decoder {nullptr, opus_decoder_destroy};
      int channels = 0;

      // Armed at session start when mic passthrough is configured + encrypted.
      // Full init (vmic backend, capture switch, reapply thread) is deferred to
      // the first mic packet so clients that never send mic data (e.g. Aurora /
      // moonlight-tv) never touch host audio state. One-shot: cleared on first
      // attempt regardless of outcome.
      bool lazy_init_pending = false;

      // Snapshot of default capture roles saved before switching; restored on session end.
      platf::capture_snapshot_t capture_snap;
      bool capture_switched = false;

      // Kept alive across the session to avoid re-running audio_control() on teardown.
      // Shared pointer so the capture-reapply detached thread can hold a reference
      // past session teardown without use-after-free.
      std::shared_ptr<platf::audio_control_t> audio_ctrl;

      // Cancel flag for the capture-reapply retry thread. Set to true on teardown
      // so the thread exits early rather than retrying after session objects are gone.
      std::shared_ptr<std::atomic<bool>> reapply_cancel;

      std::map<std::uint16_t, mic_queued_packet_t, mic_seq_order_t> pending_packets;
      std::uint16_t expected_seq = 0;
      bool has_playout_cursor = false;

      // Stats
      std::uint64_t packets_received = 0;
      std::uint64_t decode_errors = 0;
      std::uint64_t plc_events = 0;
      std::uint64_t silence_frames = 0;
      std::uint64_t frames_written = 0;
      std::uint64_t encryption_failures = 0;

      std::chrono::steady_clock::time_point last_recv_time;
    } mic;

#ifdef _WIN32
    struct {
      bool active = false;
      std::array<std::uint8_t, 16> guid_bytes {};
    } virtual_display;
#endif
  };

  /**
   * @brief Full mic-passthrough init: vmic backend, Opus decoder, default-capture
   * switch and the capture-reapply thread. Deferred to the first mic packet
   * (see session_t::mic.lazy_init_pending) so non-mic clients never touch host
   * audio state. Runs on the control thread; serialized against teardown by the
   * controlEnd ordering in session::join().
   */
  static bool init_mic_passthrough(session_t &session) {
    auto audio_ctrl = platf::audio_control();
    if (!audio_ctrl) return false;

    const auto snap = audio_ctrl->snapshot_capture_defaults();

    // Write pre-session default capture name to state file.
    // Used by startup restore guard to recover if Vibepollo is hard-killed.
    if (!config::audio.mic_capture_device.empty() && !snap.console_id.empty()) {
      const auto state_file = platf::appdata() / "mic_capture_prev.txt";
      const auto pre_session_name = audio_ctrl->get_current_default_capture_name();
      if (!pre_session_name.empty() && pre_session_name != config::audio.mic_capture_device) {
        std::ofstream f(state_file, std::ios::trunc);
        f << pre_session_name;
        f.close();
        BOOST_LOG(debug) << "[mic] state file written: pre-session default = " << pre_session_name;
      }
    }

    // Initialize Steam Streaming Microphone backend
    if (audio_ctrl->init_mic_redirect_device() != 0) {
      BOOST_LOG(warning) << "[mic] Steam Streaming Microphone unavailable — passthrough disabled"sv;
      return false;
    }
    BOOST_LOG(info) << "[mic] using Steam Streaming Microphone backend"sv;

    int err = OPUS_OK;
    auto *dec = opus_decoder_create(48000, 1, &err);
    if (err != OPUS_OK || !dec) {
      BOOST_LOG(error) << "[mic] decoder create failed: "sv << opus_strerror(err);
      return false;
    }

    session.mic.decoder.reset(dec);
    session.mic.channels = 1;
    // Store as shared_ptr so the capture-reapply thread can co-own it safely.
    session.mic.audio_ctrl = std::shared_ptr<platf::audio_control_t>(std::move(audio_ctrl));

    if (!config::audio.mic_capture_device.empty()) {
      session.mic.audio_ctrl->switch_capture_to(config::audio.mic_capture_device);
      session.mic.capture_snap = snap;
      session.mic.capture_switched = true;
      g_mic_capture_switched_sessions.fetch_add(1, std::memory_order_relaxed);

      // Capture retry thread: re-applies the default capture device switch at
      // increasing intervals because SteamOS/PipeWire may reset it asynchronously.
      // Captures audio_ctrl by shared_ptr value and cancel by shared_ptr value —
      // no reference to session, so session teardown cannot cause use-after-free.
      auto cancel = std::make_shared<std::atomic<bool>>(false);
      session.mic.reapply_cancel = cancel;
      auto ctrl = session.mic.audio_ctrl;  // shared ownership
      auto device = config::audio.mic_capture_device;
      std::thread([cancel, ctrl, device]() {
        const int schedule[] = {2, 5, 10, 20};
        int prev = 0;
        for (int t : schedule) {
          std::this_thread::sleep_for(std::chrono::seconds(t - prev));
          prev = t;
          if (cancel->load(std::memory_order_acquire) || device.empty()) break;
          ctrl->switch_capture_to(device);
          BOOST_LOG(info) << "[mic] capture re-apply at +"sv << t << "s"sv;
        }
      }).detach();
    }

    BOOST_LOG(info) << "[mic] passthrough active → "sv << config::audio.mic_sink;
    return true;
  }

  /**
   * First part of cipher must be struct of type control_encrypted_t
   *
   * returns empty string_view on failure
   * returns string_view pointing to payload data
   */
  template<std::size_t max_payload_size>
  static inline std::string_view encode_control(session_t *session, const std::string_view &plaintext, std::array<std::uint8_t, max_payload_size> &tagged_cipher) {
    static_assert(
      max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size),
      "max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size)"
    );

    if (session->config.controlProtocolType != 13) {
      return plaintext;
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'CH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 control stream messages
      // to be sent to each client before the IV repeats.
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'C';  // Control stream
    } else {
      // Nvidia's old style encryption uses a 16-byte IV
      iv.resize(16);

      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) tagged_cipher.data();

    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "Couldn't encrypt control data"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);

    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view {(char *) tagged_cipher.data(), packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq)};
  }

  int start_broadcast(broadcast_ctx_t &ctx);
  void end_broadcast(broadcast_ctx_t &ctx);

  static auto broadcast = safe::make_shared<broadcast_ctx_t>(start_broadcast, end_broadcast);

  std::optional<control_packet_view_t> decode_control_packet(std::string_view packet_bytes) {
    if (packet_bytes.size() < sizeof(std::uint16_t)) {
      return std::nullopt;
    }

    const auto type = util::packet::read_u16_le(packet_bytes, 0);
    const auto payload = util::packet::slice(
      packet_bytes,
      sizeof(std::uint16_t),
      packet_bytes.size() - sizeof(std::uint16_t)
    );
    if (!type || !payload) {
      return std::nullopt;
    }

    return control_packet_view_t {*type, *payload};
  }

#ifdef SUNSHINE_TESTS
  std::optional<control_packet_view_t> decode_control_packet_for_tests(std::string_view packet_bytes) {
    return decode_control_packet(packet_bytes);
  }
#endif
  mic_status_t get_mic_status() {
    const bool session_active = session::running_sessions.load(std::memory_order_relaxed) > 0;
    return mic_status_t {
      .session_active = session_active,
      .mic_active = session_active && mic_first_packet_received.load(std::memory_order_relaxed),
      .capture_switched = g_mic_capture_switched_sessions.load(std::memory_order_relaxed) > 0,
      .has_last_session = g_mic_has_last_session.load(std::memory_order_relaxed),
      .last_packets_received = g_mic_last_packets_received.load(std::memory_order_relaxed),
      .last_frames_written = g_mic_last_frames_written.load(std::memory_order_relaxed),
    };
  }

  void request_idr_for_all_sessions() {
    auto ref = broadcast.ref();
    if (!ref) {
      return;
    }
    auto lg = ref->control_server._sessions.lock();
    for (auto *session : *ref->control_server._sessions) {
      if (!session || !session->video.idr_events) {
        continue;
      }
      session->video.idr_events->raise(true);
    }
  }

  int set_bitrate_for_sessions(const std::string &client_uuid, int bitrate_kbps) {
    if (bitrate_kbps <= 0) {
      return 0;
    }
    auto ref = broadcast.ref();
    if (!ref) {
      return 0;
    }
    int updated = 0;
    auto lg = ref->control_server._sessions.lock();
    for (auto *session : *ref->control_server._sessions) {
      if (!session || !session->video.bitrate_events) {
        continue;
      }
      if (!client_uuid.empty() && session->device_uuid != client_uuid) {
        continue;
      }
      // Keep the session metadata (runtime sessions API, history, stats) in sync with the
      // value the encoder thread will adopt from the event below.
      session->config.monitor.bitrate = bitrate_kbps;
      session->config.monitor.client_requested_bitrate = bitrate_kbps;
      session->video.bitrate_events->raise(bitrate_kbps);
      ++updated;
    }
    return updated;
  }

  static const char *state_name(session::state_e st) {
    switch (st) {
      case session::state_e::STOPPED: return "stopped";
      case session::state_e::STOPPING: return "stopping";
      case session::state_e::STARTING: return "starting";
      case session::state_e::RUNNING: return "running";
      default: return "unknown";
    }
  }

  std::vector<session_info_t> get_all_session_info() {
    std::vector<session_info_t> result;
    auto now = std::chrono::steady_clock::now();
    auto sessions = rtsp_stream::get_sessions_snapshot();
    result.reserve(sessions.size());
    for (auto &session : sessions) {
      if (!session) continue;

      session_info_t info;
      {
        std::lock_guard lg {session->metadata_mutex};
        info.uuid = session->history_uuid;
        info.device_name = !session->history_device_name.empty() ? session->history_device_name : session->device_name;
        info.stream_gpu_model = session->stream_gpu_model;
      }
      info.width = session->config.monitor.width;
      info.height = session->config.monitor.height;
      info.fps = session->stream_fps;
      info.encoder_bitrate_kbps = session->config.monitor.bitrate;
      info.requested_bitrate_kbps = session->config.monitor.client_requested_bitrate
                                      ? session->config.monitor.client_requested_bitrate
                                      : session->config.monitor.bitrate;
      info.video_format = session->config.monitor.videoFormat;
      info.dynamic_range = session->config.monitor.dynamicRange;
      info.yuv444 = session->config.monitor.chromaSamplingType != 0;
      info.audio_channels = session->config.audio.channels;
      info.state = state_name(session->state.load(std::memory_order_relaxed));

      // Real-time performance counters
      info.frames_sent = session->stats.frames_sent.load(std::memory_order_relaxed);
      info.packets_sent = session->stats.packets_sent.load(std::memory_order_relaxed);
      info.bytes_sent = session->stats.bytes_sent.load(std::memory_order_relaxed);
      info.idr_requests = session->stats.idr_requests.load(std::memory_order_relaxed);
      info.invalidate_ref_count = session->stats.invalidate_ref_count.load(std::memory_order_relaxed);
      info.client_reported_losses = session->stats.client_reported_losses.load(std::memory_order_relaxed);
      info.encode_latency_ms = session->stats.last_encode_latency_us10.load(std::memory_order_relaxed) / 10.0;
      info.last_frame_index = session->stats.last_frame_index.load(std::memory_order_relaxed);
      info.uptime_seconds = std::chrono::duration<double>(now - session->stats.start_time).count();

      result.push_back(std::move(info));
    }
    return result;
  }

#ifdef _WIN32
  struct deferred_stream_start_t {
    framegen::stream_start_policy_t policy;
  };

  std::mutex &deferred_stream_start_mutex() {
    static std::mutex m;
    return m;
  }

  std::optional<deferred_stream_start_t> &deferred_stream_start_state() {
    static std::optional<deferred_stream_start_t> state;
    return state;
  }

  bool user_session_ready() {
    HANDLE user_token = platf::dxgi::retrieve_users_token(false);
    if (!user_token) {
      return false;
    }
    CloseHandle(user_token);
    return true;
  }

  bool stream_start_actions_still_needed() {
    return session::running_sessions.load(std::memory_order_acquire) != 0 || webrtc_stream::has_active_sessions();
  }

  void defer_stream_start_actions(deferred_stream_start_t deferred) {
    std::lock_guard<std::mutex> lock(deferred_stream_start_mutex());
    deferred_stream_start_state() = std::move(deferred);
  }

  void clear_deferred_stream_start_actions() {
    std::lock_guard<std::mutex> lock(deferred_stream_start_mutex());
    deferred_stream_start_state().reset();
  }

  bool apply_deferred_stream_start_actions_if_ready() {
    {
      std::lock_guard<std::mutex> lock(deferred_stream_start_mutex());
      if (!deferred_stream_start_state()) {
        return false;
      }
    }

    if (!user_session_ready()) {
      return false;
    }

    std::optional<deferred_stream_start_t> deferred;
    {
      std::lock_guard<std::mutex> lock(deferred_stream_start_mutex());
      if (!deferred_stream_start_state()) {
        return false;
      }
      if (!stream_start_actions_still_needed()) {
        deferred_stream_start_state().reset();
        BOOST_LOG(debug) << "Stream-start actions skipped because no active stream remains.";
        return false;
      }
      deferred = std::move(*deferred_stream_start_state());
      deferred_stream_start_state().reset();
    }

    BOOST_LOG(info) << "Stream-start actions applied after user session became available.";
    platf::frame_limiter_streaming_start(deferred->policy);
    platf::streaming_will_start();
    return true;
  }
#endif

  session_t *control_server_t::get_session(const net::peer_t peer, uint32_t connect_data) {
    {
      // Fast path - look up existing session by peer
      auto lg = _peer_to_session.lock();
      auto it = _peer_to_session->find(peer);
      if (it != _peer_to_session->end()) {
        return it->second;
      }
    }

    // Slow path - process new session
    TUPLE_2D(peer_port, peer_addr, platf::from_sockaddr_ex((sockaddr *) &peer->address.address));
    auto lg = _sessions.lock();
    for (auto pos = std::begin(*_sessions); pos != std::end(*_sessions); ++pos) {
      auto session_p = *pos;

      // Skip sessions that are already established
      if (session_p->control.peer) {
        continue;
      }

      // Identify the connection by the unique connect data if the client supports it.
      // Only fall back to IP address matching for clients without session ID support.
      if (session_p->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) {
        if (session_p->control.connect_data != connect_data) {
          continue;
        } else {
          BOOST_LOG(debug) << "Initialized new control stream session by connect data match [v2]"sv;
        }
      } else {
        if (session_p->control.expected_peer_address != peer_addr) {
          continue;
        } else {
          BOOST_LOG(debug) << "Initialized new control stream session by IP address match [v1]"sv;
        }
      }

      // Once the control stream connection is established, RTSP session state can be torn down
      rtsp_stream::launch_session_clear(session_p->launch_session_id);

      session_p->control.peer = peer;

      // Use the local address from the control connection as the source address
      // for other communications to the client. This is necessary to ensure
      // proper routing on multi-homed hosts.
      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      try {
        session_p->localAddress = boost::asio::ip::make_address(local_address);
      } catch (const boost::system::system_error &e) {
        BOOST_LOG(error) << "boost::system::system_error in address parsing: " << e.what() << " (code: " << e.code() << ")"sv;
        throw;
      }

      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';

      // Insert this into the map for O(1) lookups in the future
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session_p);
      return session_p;
    }

    return nullptr;
  }

  /**
   * @brief Call the handler for a given control stream message.
   * @param type The message type.
   * @param session The session the message was received on.
   * @param payload The payload of the message.
   * @param reinjected `true` if this message is being reprocessed after decryption.
   */
  void control_server_t::call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected) {
    // If we are using the encrypted control stream protocol, drop any messages that come off the wire unencrypted
    if (session->config.controlProtocolType == 13 && !reinjected && type != packetTypes[IDX_ENCRYPTED]) {
      BOOST_LOG(error) << "Dropping unencrypted message on encrypted control stream: "sv << util::hex(type).to_string_view();
      return;
    }

    auto cb = _map_type_cb.find(type);
    if (cb == std::end(_map_type_cb)) {
      BOOST_LOG(debug)
        << "type [Unknown] { "sv << util::hex(type).to_string_view() << " }"sv << std::endl
        << "---data---"sv << std::endl
        << util::hex_vec(payload) << std::endl
        << "---end data---"sv;
    } else {
      cb->second(session, payload);
    }
  }

  void control_server_t::iterate(std::chrono::milliseconds timeout) {
    ENetEvent event;
    auto res = enet_host_service(_host.get(), &event, (enet_uint32) timeout.count());

    if (res > 0) {
      auto session = get_session(event.peer, event.data);
      if (!session) {
        BOOST_LOG(warning) << "Rejected connection from ["sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address) << "]: it's not properly set up"sv;
        enet_peer_disconnect_now(event.peer, 0);

        return;
      }

      session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
          {
            net::packet_t packet {event.packet};

            std::string_view packet_bytes {
              reinterpret_cast<const char *>(packet->data),
              packet->dataLength
            };
            const auto decoded = decode_control_packet(packet_bytes);
            if (!decoded) {
              BOOST_LOG(warning) << "Ignoring runt control packet (" << packet->dataLength << " bytes)";
              break;
            }

            call(decoded->type, session, decoded->payload, false);
          }
          break;
        case ENET_EVENT_TYPE_CONNECT:
          BOOST_LOG(info) << "CLIENT CONNECTED"sv;
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          BOOST_LOG(info) << "CLIENT DISCONNECTED"sv;
          // No more clients to send video data to ^_^
          if (session->state == session::state_e::RUNNING) {
            session::stop(*session);
          }
          break;
        case ENET_EVENT_TYPE_NONE:
          break;
      }
    }
  }

  namespace fec {
    using rs_t = util::safe_ptr<reed_solomon, [](reed_solomon *rs) {
      reed_solomon_release(rs);
    }>;

    struct fec_t {
      size_t data_shards;
      size_t nr_shards;
      size_t percentage;

      size_t blocksize;
      size_t prefixsize;
      util::buffer_t<char> shards;
      util::buffer_t<char> headers;
      util::buffer_t<uint8_t *> shards_p;

      std::vector<platf::buffer_descriptor_t> payload_buffers;

      char *data(size_t el) {
        return (char *) shards_p[el];
      }

      char *prefix(size_t el) {
        return prefixsize ? &headers[el * prefixsize] : nullptr;
      }

      size_t size() const {
        return nr_shards;
      }
    };

    static fec_t encode(const std::string_view &payload, size_t blocksize, size_t fecpercentage, size_t minparityshards, size_t prefixsize) {
      auto payload_size = payload.size();

      auto pad = payload_size % blocksize != 0;

      auto aligned_data_shards = payload_size / blocksize;
      auto data_shards = aligned_data_shards + (pad ? 1 : 0);
      auto parity_shards = (data_shards * fecpercentage + 99) / 100;

      // increase the FEC percentage for this frame if the parity shard minimum is not met
      if (parity_shards < minparityshards && fecpercentage != 0) {
        parity_shards = minparityshards;
        fecpercentage = (100 * parity_shards) / data_shards;

        BOOST_LOG(verbose) << "Increasing FEC percentage to "sv << fecpercentage << " to meet parity shard minimum"sv << std::endl;
      }

      auto nr_shards = data_shards + parity_shards;

      // If we need to store a zero-padded data shard, allocate that first to
      // to keep the shards in order and reduce buffer fragmentation
      auto parity_shard_offset = pad ? 1 : 0;
      util::buffer_t<char> shards {(parity_shard_offset + parity_shards) * blocksize};
      util::buffer_t<uint8_t *> shards_p {nr_shards};
      std::vector<platf::buffer_descriptor_t> payload_buffers;
      payload_buffers.reserve(2);

      // Point into the payload buffer for all except the final padded data shard
      auto next = std::begin(payload);
      for (auto x = 0; x < aligned_data_shards; ++x) {
        shards_p[x] = (uint8_t *) next;
        next += blocksize;
      }
      payload_buffers.emplace_back(std::begin(payload), aligned_data_shards * blocksize);

      // If the last data shard needs to be zero-padded, we must use the shards buffer
      if (pad) {
        shards_p[aligned_data_shards] = (uint8_t *) &shards[0];

        // GCC doesn't figure out that std::copy_n() can be replaced with memcpy() here
        // and ends up compiling a horribly slow element-by-element copy loop, so we
        // help it by using memcpy()/memset() directly.
        auto copy_len = std::min<size_t>(blocksize, std::end(payload) - next);
        std::memcpy(shards_p[aligned_data_shards], next, copy_len);
        if (copy_len < blocksize) {
          // Zero any additional space after the end of the payload
          std::memset(shards_p[aligned_data_shards] + copy_len, 0, blocksize - copy_len);
        }
      }

      // Add a payload buffer describing the shard buffer
      payload_buffers.emplace_back(std::begin(shards), shards.size());

      if (fecpercentage != 0) {
        // Point into our allocated buffer for the parity shards
        for (auto x = 0; x < parity_shards; ++x) {
          shards_p[data_shards + x] = (uint8_t *) &shards[(parity_shard_offset + x) * blocksize];
        }

        // packets = parity_shards + data_shards
        rs_t rs {reed_solomon_new((int) data_shards, (int) parity_shards)};

        reed_solomon_encode(rs.get(), shards_p.begin(), (int) nr_shards, (int) blocksize);
      }

      return {
        data_shards,
        nr_shards,
        fecpercentage,
        blocksize,
        prefixsize,
        std::move(shards),
        util::buffer_t<char> {nr_shards * prefixsize},
        std::move(shards_p),
        std::move(payload_buffers),
      };
    }
  }  // namespace fec

  /**
   * @brief Combines two buffers and inserts new buffers at each slice boundary of the result.
   * @param insert_size The number of bytes to insert.
   * @param slice_size The number of bytes between insertions.
   * @param data1 The first data buffer.
   * @param data2 The second data buffer.
   */
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    auto data_size = data1.size() + data2.size();
    auto pad = data_size % slice_size != 0;
    auto elements = data_size / slice_size + (pad ? 1 : 0);

    std::vector<uint8_t> result;
    result.resize(elements * insert_size + data_size);

    auto next = std::begin(data1);
    auto end = std::end(data1);
    for (auto x = 0; x < elements; ++x) {
      void *p = &result[x * (insert_size + slice_size)];

      // For the last iteration, only copy to the end of the data
      if (x == elements - 1) {
        slice_size = data_size - (x * slice_size);
      }

      // Test if this slice will extend into the next buffer
      if (next + slice_size > end) {
        // Copy the first portion from the first buffer
        auto copy_len = end - next;
        std::copy(next, end, (char *) p + insert_size);

        // Copy the remaining portion from the second buffer
        next = std::begin(data2);
        end = std::end(data2);
        std::copy(next, next + (slice_size - copy_len), (char *) p + copy_len + insert_size);
        next += slice_size - copy_len;
      } else {
        std::copy(next, next + slice_size, (char *) p + insert_size);
        next += slice_size;
      }
    }

    return result;
  }

  std::vector<uint8_t> replace(const std::string_view &original, const std::string_view &old, const std::string_view &_new) {
    std::vector<uint8_t> replaced;
    replaced.reserve(original.size() + _new.size() - old.size());

    auto begin = std::begin(original);
    auto end = std::end(original);
    auto next = std::search(begin, end, std::begin(old), std::end(old));

    std::copy(begin, next, std::back_inserter(replaced));
    if (next != end) {
      std::copy(std::begin(_new), std::end(_new), std::back_inserter(replaced));
      std::copy(next + old.size(), end, std::back_inserter(replaced));
    }

    return replaced;
  }

  /**
   * @brief Pass gamepad feedback data back to the client.
   * @param session The session object.
   * @param msg The message to pass.
   * @return 0 on success.
   */
  int send_feedback_msg(session_t *session, platf::gamepad_feedback_msg_t &msg) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send gamepad feedback data, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    std::string payload;
    if (msg.type == platf::gamepad_feedback_e::rumble) {
      control_rumble_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble;

      plaintext.useless = 0xC0FFEE;
      plaintext.id = util::endian::little(msg.id);
      plaintext.lowfreq = util::endian::little(data.lowfreq);
      plaintext.highfreq = util::endian::little(data.highfreq);

      BOOST_LOG(verbose) << "Rumble: "sv << msg.id << " :: "sv << util::hex(data.lowfreq).to_string_view() << " :: "sv << util::hex(data.highfreq).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::rumble_triggers) {
      control_rumble_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_TRIGGER_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble_triggers;

      plaintext.id = util::endian::little(msg.id);
      plaintext.left = util::endian::little(data.left_trigger);
      plaintext.right = util::endian::little(data.right_trigger);

      BOOST_LOG(verbose) << "Rumble triggers: "sv << msg.id << " :: "sv << util::hex(data.left_trigger).to_string_view() << " :: "sv << util::hex(data.right_trigger).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_motion_event_state) {
      control_set_motion_event_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_MOTION_EVENT];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.motion_event_state;

      plaintext.id = util::endian::little(msg.id);
      plaintext.reportrate = util::endian::little(data.report_rate);
      plaintext.type = data.motion_type;

      BOOST_LOG(verbose) << "Motion event state: "sv << msg.id << " :: "sv << util::hex(data.report_rate).to_string_view() << " :: "sv << util::hex(data.motion_type).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_rgb_led) {
      control_set_rgb_led_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_RGB_LED];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rgb_led;

      plaintext.id = util::endian::little(msg.id);
      plaintext.r = data.r;
      plaintext.g = data.g;
      plaintext.b = data.b;

      BOOST_LOG(verbose) << "RGB: "sv << msg.id << " :: "sv << util::hex(data.r).to_string_view() << util::hex(data.g).to_string_view() << util::hex(data.b).to_string_view();
      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else if (msg.type == platf::gamepad_feedback_e::set_adaptive_triggers) {
      control_adaptive_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_ADAPTIVE_TRIGGERS];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      plaintext.id = util::endian::little(msg.id);
      plaintext.event_flags = msg.data.adaptive_triggers.event_flags;
      plaintext.type_left = msg.data.adaptive_triggers.type_left;
      std::ranges::copy(msg.data.adaptive_triggers.left, plaintext.left);
      plaintext.type_right = msg.data.adaptive_triggers.type_right;
      std::ranges::copy(msg.data.adaptive_triggers.right, plaintext.right);

      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    } else {
      BOOST_LOG(error) << "Unknown gamepad feedback message type"sv;
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send gamepad feedback to ["sv << addr << ':' << port << ']';

      return -1;
    }

    return 0;
  }

  int send_hdr_mode(session_t *session, video::hdr_info_t hdr_info) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send HDR mode, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    control_hdr_mode_t plaintext {};
    plaintext.header.type = packetTypes[IDX_HDR_MODE];
    plaintext.header.payloadLength = sizeof(control_hdr_mode_t) - sizeof(control_header_v2);

    plaintext.enabled = hdr_info->enabled;
    plaintext.metadata = hdr_info->metadata;

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send HDR mode to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent HDR mode: " << hdr_info->enabled;
    return 0;
  }

  void controlBroadcastThread(control_server_t *server) {
    server->map(packetTypes[IDX_PERIODIC_PING], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_PERIODIC_PING]"sv;
    });

    server->map(packetTypes[IDX_START_A], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_A]"sv;
    });

    server->map(packetTypes[IDX_START_B], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_B]"sv;
    });

    auto handle_client_termination = [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_TERMINATION]"sv;

      std::optional<std::uint32_t> termination_code;
      if (payload.size() >= sizeof(std::uint32_t)) {
        std::uint32_t raw {};
        std::memcpy(&raw, payload.data(), sizeof(raw));
        termination_code = util::endian::big(raw);
      } else if (payload.size() >= sizeof(std::uint16_t)) {
        std::uint16_t raw {};
        std::memcpy(&raw, payload.data(), sizeof(raw));
        termination_code = util::endian::little(raw);
      }

      if (termination_code) {
        BOOST_LOG(info) << "Client requested termination with reason 0x"sv << util::hex(*termination_code).to_string_view();
      } else {
        BOOST_LOG(info) << "Client requested termination with empty reason payload ("sv << payload.size() << " bytes)";
      }

      session::stop(*session);
    };

    server->map(packetTypes[IDX_TERMINATION], handle_client_termination);

    constexpr std::uint16_t LEGACY_TERMINATION_PACKET_TYPE = 0x0100;
    if (packetTypes[IDX_TERMINATION] != LEGACY_TERMINATION_PACKET_TYPE) {
      server->map(LEGACY_TERMINATION_PACKET_TYPE, handle_client_termination);
    }

    server->map(packetTypes[IDX_LOSS_STATS], [&](session_t *session, const std::string_view &payload) {
      const auto count_value = util::packet::read_i32_le(payload, 0);
      const auto elapsed_ms = util::packet::read_i32_le(payload, sizeof(std::int32_t));
      const auto last_good_frame = util::packet::read_i32_le(payload, sizeof(std::int32_t) * 3);
      if (!count_value || !elapsed_ms || !last_good_frame) {
        BOOST_LOG(warning) << "Ignoring short IDX_LOSS_STATS payload (" << payload.size() << " bytes)";
        return;
      }

      auto count = std::clamp(*count_value, 0, 1'000'000);
      std::chrono::milliseconds t {*elapsed_ms};
      auto lastGoodFrame = *last_good_frame;

      saturating_add_relaxed(session->stats.client_reported_losses, static_cast<std::int64_t>(count));

      BOOST_LOG(verbose)
        << "type [IDX_LOSS_STATS]"sv << std::endl
        << "---begin stats---" << std::endl
        << "loss count since last report [" << count << ']' << std::endl
        << "time in milli since last report [" << t.count() << ']' << std::endl
        << "last good frame [" << lastGoodFrame << ']' << std::endl
        << "---end stats---";
    });

    server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]"sv;

      saturating_add_relaxed(session->stats.idr_requests, 1u);
      session->video.idr_events->raise(true);
    });

    server->map(packetTypes[IDX_INVALIDATE_REF_FRAMES], [&](session_t *session, const std::string_view &payload) {
      const auto first_frame = util::packet::read_i64_le(payload, 0);
      const auto last_frame = util::packet::read_i64_le(payload, sizeof(std::int64_t));
      if (!first_frame || !last_frame) {
        BOOST_LOG(warning) << "Ignoring short IDX_INVALIDATE_REF_FRAMES payload (" << payload.size() << " bytes)";
        return;
      }

      const auto firstFrame = *first_frame;
      const auto lastFrame = *last_frame;
      if (firstFrame < 0 || lastFrame < 0 || lastFrame < firstFrame) {
        BOOST_LOG(warning) << "Ignoring invalid IDX_INVALIDATE_REF_FRAMES payload first=" << firstFrame
                           << " last=" << lastFrame;
        return;
      }

      saturating_add_relaxed(session->stats.invalidate_ref_count, 1u);

      BOOST_LOG(debug)
        << "type [IDX_INVALIDATE_REF_FRAMES]"sv << std::endl
        << "firstFrame [" << firstFrame << ']' << std::endl
        << "lastFrame [" << lastFrame << ']';

      session->video.invalidate_ref_frames_events->raise(std::make_pair(firstFrame, lastFrame));
    });

    server->map(packetTypes[IDX_INPUT_DATA], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_INPUT_DATA]"sv;

      const auto tagged_cipher_length = util::packet::read_i32_be(payload, 0);
      if (!tagged_cipher_length || *tagged_cipher_length < 0) {
        BOOST_LOG(warning) << "Ignoring malformed IDX_INPUT_DATA payload (" << payload.size() << " bytes)";
        session::stop(*session);
        return;
      }

      const auto tagged_cipher = util::packet::slice(
        payload,
        sizeof(std::int32_t),
        static_cast<std::size_t>(*tagged_cipher_length));
      if (!tagged_cipher) {
        BOOST_LOG(warning) << "Ignoring truncated IDX_INPUT_DATA payload (" << payload.size() << " bytes, cipher="
                           << *tagged_cipher_length << ')';
        session::stop(*session);
        return;
      }

      std::vector<uint8_t> plaintext;

      auto &cipher = session->control.cipher;
      auto &iv = session->control.legacy_input_enc_iv;
      if (cipher.decrypt(*tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      if (tagged_cipher->size() >= 16 + iv.size()) {
        std::copy(payload.end() - 16, payload.end(), std::begin(iv));
      }

      input::passthrough(session->input, std::move(plaintext), session->permission);
    });

    server->map(packetTypes[IDX_EXEC_SERVER_CMD], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_EXEC_SERVER_CMD]"sv;

      if (!(session->permission & crypto::PERM::server_cmd)) {
        BOOST_LOG(debug) << "Permission Exec Server Cmd deined for [" << session->device_name << "]";
        return;
      }

      const auto cmdIndex = util::packet::read_u8(payload, 0);
      if (!cmdIndex) {
        BOOST_LOG(warning) << "Ignoring short IDX_EXEC_SERVER_CMD payload (" << payload.size() << " bytes)";
        return;
      }

      if (*cmdIndex < config::sunshine.server_cmds.size()) {
        const auto &cmd = config::sunshine.server_cmds[*cmdIndex];
        BOOST_LOG(info) << "Executing server command: " << cmd.cmd_name;

        auto exec_thread = std::thread([&cmd] {
          std::error_code ec;
          auto env = proc::proc.get_env();
          boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd_val, env);
          auto child = platf::run_command(cmd.elevated, true, cmd.cmd_val, working_dir, env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Failed to execute server command: " << ec.message();
          } else {
            child.detach();
          }
        });

        exec_thread.detach();
      } else {
        BOOST_LOG(error) << "Invalid server command index: " << static_cast<int>(*cmdIndex);
      }
    });

    server->map(packetTypes[IDX_SET_CLIPBOARD], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(info) << "type [IDX_SET_CLIPBOARD]: "sv << payload << " size: " << payload.size();

      if (!(session->permission & crypto::PERM::clipboard_set)) {
        BOOST_LOG(debug) << "Permission Clipboard Set deined for [" << session->device_name << "]";
        return;
      }
    });

    server->map(packetTypes[IDX_FILE_TRANSFER_NONCE_REQUEST], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(info) << "type [IDX_FILE_TRANSFER_NONCE_REQUEST]: "sv << payload << " size: " << payload.size();

      if (!(session->permission & crypto::PERM::file_upload)) {
        BOOST_LOG(debug) << "Permission File Upload deined for [" << session->device_name << "]";
        return;
      }
    });

    server->map(packetTypes[IDX_MIC_AUDIO_DATA], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_MIC_AUDIO_DATA]"sv;

      if (!session->mic.audio_ctrl || !session->mic.decoder) {
        // Lazy init on first mic packet (one-shot — failure disables for this session)
        if (!session->mic.lazy_init_pending) return;
        session->mic.lazy_init_pending = false;
        bool init_ok = false;
        try {
          init_ok = init_mic_passthrough(*session);
        } catch (...) {
          BOOST_LOG(error) << "[mic] init threw exception — mic disabled"sv;
        }
        if (!init_ok) {
          session->mic.decoder.reset();
          session->mic.audio_ctrl.reset();
          return;
        }
      }

      // Require encrypted control stream
      if (!(session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2)) {
        if (session->mic.encryption_failures++ == 0) {
          BOOST_LOG(warning) << "[mic] Plaintext control stream — mic passthrough disabled."sv;
          session->mic.audio_ctrl.reset();
          session->mic.decoder.reset();
        }
        return;
      }

      // Need at least the 4-byte framing header
      if (payload.size() < 4 || payload.size() >= 4096) {
        BOOST_LOG(warning) << "[mic] malformed packet (len="sv << payload.size() << ")"sv;
        return;
      }

      // Fix 1: parse real sender sequence number from 4-byte wire header [seq_hi, seq_lo, ch, flags]
      const auto *hdr = reinterpret_cast<const std::uint8_t *>(payload.data());
      const std::uint16_t incoming_seq = (static_cast<std::uint16_t>(hdr[0]) << 8) | hdr[1];

      // Fix A: validate ch and flags fields
      if (hdr[2] != 1 || hdr[3] != 0) {
        BOOST_LOG(warning) << "[mic] invalid header (ch="sv << (int)hdr[2]
                           << " flags="sv << (int)hdr[3] << ") — dropping"sv;
        return;
      }

      const std::string_view opus_payload { payload.data() + 4, payload.size() - 4 };

      // Fix A: validate Opus packet structure before inserting into jitter buffer
      {
        unsigned char toc_byte = 0;
        const unsigned char *frames_arr[48] = {};
        opus_int16 sizes[48] = {};
        int payload_offset = 0;
        int nb_frames = opus_packet_parse(
          reinterpret_cast<const unsigned char *>(opus_payload.data()),
          static_cast<opus_int32>(opus_payload.size()),
          &toc_byte, frames_arr, sizes, &payload_offset);
        if (nb_frames < 0) {
          session->mic.decode_errors++;
          BOOST_LOG(warning) << "[mic] opus_packet_parse failed ("sv << nb_frames << ") — dropping"sv;
          return;
        }
      }

      // Fix A: derive frame size from packet instead of hardcoding
      const int expected_samples = opus_packet_get_nb_samples(
        reinterpret_cast<const unsigned char *>(opus_payload.data()),
        static_cast<opus_int32>(opus_payload.size()),
        48000);
      const int frame_size = (expected_samples > 0 && expected_samples <= 5760)
                             ? expected_samples : 960;

      if (!stream::mic_first_packet_received.exchange(true, std::memory_order_relaxed))
        BOOST_LOG(info) << "[mic] First mic packet received from client"sv;

      auto recv_time = std::chrono::steady_clock::now();
      auto &mic = session->mic;
      mic.packets_received++;

      // Steam mic path handles underruns natively via jitter buffer
      mic.last_recv_time = recv_time;

      // Guard: reject packets too far outside the playout window.
      // mic_seq_order_t uses RFC 1982 serial arithmetic (int16_t)(a-b) < 0, which is
      // NOT a valid std::map comparator for keys exactly 32768 apart (antisymmetry
      // fails, corrupting the tree). Clamping insertions to ±2*mic_max_queued_packets
      // around expected_seq keeps all keys within a region where the comparator is
      // consistent. Packets outside this window are from a reconnect or misbehaving
      // client and can be safely dropped.
      // Guard applies both after cursor is established and during prebuffering once
      // we have a reference key — preventing the comparator edge from being reached
      // with keys exactly 32768 apart at any stage of the pipeline.
      if (mic.has_playout_cursor || !mic.pending_packets.empty()) {
        std::uint16_t ref_seq = mic.has_playout_cursor ? mic.expected_seq : mic.pending_packets.begin()->first;
        auto delta = static_cast<std::int16_t>(incoming_seq - ref_seq);
        if (delta < 0 || delta > static_cast<std::int16_t>(session_t::mic_max_queued_packets * 2)) {
          BOOST_LOG(debug) << "[mic] dropping out-of-window packet seq="sv << incoming_seq
                           << " ref="sv << ref_seq << " delta="sv << (int)delta;
          return;
        }
      }

      // Insert into jitter buffer
      mic.pending_packets.emplace(incoming_seq, session_t::mic_queued_packet_t {
        std::vector<std::uint8_t> {
          reinterpret_cast<const std::uint8_t *>(opus_payload.data()),
          reinterpret_cast<const std::uint8_t *>(opus_payload.data()) + opus_payload.size()
        },
        incoming_seq
      });
      if (mic.pending_packets.size() > session_t::mic_max_queued_packets)
        mic.pending_packets.erase(mic.pending_packets.begin());

      // Prebuffer
      if (!mic.has_playout_cursor) {
        if (mic.pending_packets.size() < (std::size_t) config::audio.mic_buffer_packets) return;
        mic.expected_seq = mic.pending_packets.begin()->first;
        mic.has_playout_cursor = true;
        BOOST_LOG(info) << "[mic] Jitter buffer ready ("sv << config::audio.mic_buffer_packets << " packets)"sv;
      }

      // Drain in sequence order
      while (!mic.pending_packets.empty()) {
        auto it = mic.pending_packets.find(mic.expected_seq);
        std::vector<float> pcm((size_t)(frame_size * mic.channels));
        int frames = 0;

        if (it != mic.pending_packets.end()) {
          frames = opus_decode_float(mic.decoder.get(), it->second.payload.data(),
            (opus_int32) it->second.payload.size(), pcm.data(), frame_size, 0);
          mic.pending_packets.erase(it);
        } else {
          auto next_it = mic.pending_packets.find(static_cast<std::uint16_t>(mic.expected_seq + 1));
          if (next_it != mic.pending_packets.end()) {
            frames = opus_decode_float(mic.decoder.get(), next_it->second.payload.data(),
              (opus_int32) next_it->second.payload.size(), pcm.data(), frame_size, 1);
          } else if (!mic.pending_packets.empty() && mic_seq_order_t()(mic.expected_seq, mic.pending_packets.begin()->first)) {
            frames = opus_decode_float(mic.decoder.get(), nullptr, 0, pcm.data(), frame_size, 0);
            mic.plc_events++;
          } else {
            break;
          }
        }

        mic.expected_seq = static_cast<std::uint16_t>(mic.expected_seq + 1);

        if (frames < 0) {
          mic.decode_errors++;
          BOOST_LOG(warning) << "[mic] Opus decode error: "sv << opus_strerror(frames);
          continue;
        }
        if (frames == 0) continue;

        int write_ret = mic.audio_ctrl->write_mic_pcm(pcm.data(), (std::uint32_t) frames);
        if (write_ret < 0) {
          BOOST_LOG(error) << "[mic] Steam mic render path lost — disabling mic for this session"sv;
          mic.audio_ctrl->release_mic_redirect_device();
          return;
        }
        mic.frames_written++;
        if (mic.packets_received % 100 == 0) {
          std::size_t qsz = mic.pending_packets.size();
          BOOST_LOG(info) << "[mic] stats: recv="sv << mic.packets_received
                          << " decoded="sv << mic.frames_written
                          << " plc="sv << mic.plc_events
                          << " silence="sv << mic.silence_frames
                          << " errors="sv << mic.decode_errors
                          << " queue="sv << qsz;
        }
      }
    });

    server->map(packetTypes[IDX_ENCRYPTED], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_ENCRYPTED]"sv;

      if (payload.size() < sizeof(control_encrypted_t) - sizeof(std::uint16_t)) {
        BOOST_LOG(warning) << "Control: Runt encrypted packet payload";
        return;
      }

      const std::string_view packet_bytes {payload.data() - sizeof(std::uint16_t), payload.size() + sizeof(std::uint16_t)};
      const auto encrypted_header_type = util::packet::read_u16_le(packet_bytes, 0);
      const auto length = util::packet::read_u16_le(packet_bytes, sizeof(std::uint16_t));
      const auto seq = util::packet::read_u32_le(packet_bytes, sizeof(std::uint16_t) * 2);
      if (!encrypted_header_type || !length || !seq) {
        BOOST_LOG(warning) << "Control: malformed encrypted packet header";
        return;
      }
      if (*encrypted_header_type != packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(warning) << "Control: unexpected encrypted packet header type 0x"
                           << util::hex(*encrypted_header_type).to_string_view();
        return;
      }
      if (packet_bytes.size() != static_cast<std::size_t>(*length) + sizeof(std::uint16_t) * 2) {
        BOOST_LOG(warning) << "Control: malformed encrypted packet length " << *length
                           << " for payload " << packet_bytes.size();
        return;
      }

      if (*length < (16 + 4 + 4)) {
        BOOST_LOG(warning) << "Control: Runt packet"sv;
        return;
      }

      const auto tagged_cipher_length = static_cast<std::size_t>(*length) - sizeof(std::uint32_t);
      const auto tagged_cipher = util::packet::slice(packet_bytes, sizeof(control_encrypted_t), tagged_cipher_length);
      if (!tagged_cipher) {
        BOOST_LOG(warning) << "Control: truncated encrypted payload";
        return;
      }

      auto &cipher = session->control.cipher;
      auto &iv = session->control.incoming_iv;
      if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
        // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
        // Section 8.2.1. The sequence number is our "invocation" field and the 'CC' in the
        // high bytes is the "fixed" field. Because each client provides their own unique
        // key, our values in the fixed field need only uniquely identify each independent
        // use of the client's key with AES-GCM in our code.
        //
        // The sequence number is 32 bits long which allows for 2^32 control stream messages
        // to be received from each client before the IV repeats.
        iv.resize(12);
        std::copy_n(reinterpret_cast<const uint8_t *>(&*seq), sizeof(*seq), std::begin(iv));
        iv[10] = 'C';  // Client originated
        iv[11] = 'C';  // Control stream
      } else {
        // Nvidia's old style encryption uses a 16-byte IV
        iv.resize(16);

        iv[0] = static_cast<std::uint8_t>(*seq);
      }

      std::vector<uint8_t> plaintext;
      if (cipher.decrypt(*tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      const auto plaintext_view = std::string_view {
        reinterpret_cast<const char *>(plaintext.data()),
        plaintext.size()
      };
      const auto type = util::packet::read_u16_le(plaintext_view, 0);
      if (!type || plaintext.size() < 4) {
        BOOST_LOG(warning) << "Control: malformed decrypted control packet";
        session::stop(*session);
        return;
      }
      std::string_view next_payload {reinterpret_cast<const char *>(plaintext.data()) + 4, plaintext.size() - 4};

      if (*type == packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(error) << "Bad packet type [IDX_ENCRYPTED] found"sv;
        session::stop(*session);
        return;
      }

      // IDX_INPUT_DATA callback will attempt to decrypt unencrypted data, therefore we need pass it directly
      if (*type == packetTypes[IDX_INPUT_DATA]) {
        plaintext.erase(std::begin(plaintext), std::begin(plaintext) + 4);
        input::passthrough(session->input, std::move(plaintext), session->permission);
      } else {
        server->call(*type, session, next_payload, true);
      }
    });

    // This thread handles latency-sensitive control messages
    platf::set_thread_name("stream::controlBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // Check for both the full shutdown event and the shutdown event for this
    // broadcast to ensure we can inform connected clients of our graceful
    // termination when we shut down.
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    while (!shutdown_event->peek() && !broadcast_shutdown_event->peek()) {
      bool has_session_awaiting_peer = false;

      {
        auto lg = server->_sessions.lock();

        auto now = std::chrono::steady_clock::now();

        KITTY_WHILE_LOOP(auto pos = std::begin(*server->_sessions), pos != std::end(*server->_sessions), {
          // Don't perform additional session processing if we're shutting down
          if (shutdown_event->peek() || broadcast_shutdown_event->peek()) {
            break;
          }

          auto session = *pos;

          if (now > session->pingTimeout) {
            auto address = session->control.peer ? platf::from_sockaddr((sockaddr *) &session->control.peer->address.address) : session->control.expected_peer_address;
            BOOST_LOG(info) << address << ": Ping Timeout"sv;
            if (!session->control.peer) {
              const auto control_received = server->_host->totalReceivedPackets - session->control.enet_recv_baseline;
              if (control_received == 0) {
                BOOST_LOG(error) << "The control stream peer never connected and no UDP datagrams reached the control socket (port "sv
                                 << net::map_port(CONTROL_PORT) << ") during this session. "sv
                                 << "Inbound UDP is most likely being blocked before it reaches Sunshine. "sv
                                 << "Check Windows Firewall, VPN clients with kill switches or LAN blocking "sv
                                 << "(e.g. NordVPN, Private Internet Access - these filter traffic even while disconnected), "sv
                                 << "and router/NAT port forwarding."sv;
              } else {
                BOOST_LOG(error) << "The control stream peer never connected, but "sv << control_received
                                 << " UDP datagram(s) reached the control socket (port "sv << net::map_port(CONTROL_PORT)
                                 << "). The ENet handshake did not complete; outbound replies to the client may be getting dropped."sv;
              }
            }
            session::stop(*session);
          }

          if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING) {
            pos = server->_sessions->erase(pos);

            if (session->control.peer) {
              {
                auto ptslg = server->_peer_to_session.lock();
                server->_peer_to_session->erase(session->control.peer);
              }

              enet_peer_disconnect_now(session->control.peer, 0);
            }

            session->controlEnd.raise(true);
            continue;
          }

          // Remember if we have a session that's waiting for a peer to connect to the
          // control stream. This ensures the clients are properly notified even when
          // the app terminates before they finish connecting.
          if (!session->control.peer) {
            has_session_awaiting_peer = true;
          } else {
            auto &feedback_queue = session->control.feedback_queue;
            while (feedback_queue->peek()) {
              auto feedback_msg = feedback_queue->pop();

              send_feedback_msg(session, *feedback_msg);
            }

            auto &hdr_queue = session->control.hdr_queue;
            while (session->control.peer && hdr_queue->peek()) {
              auto hdr_info = hdr_queue->pop();

              send_hdr_mode(session, std::move(hdr_info));
            }
          }

          ++pos;
        })
      }

#ifdef _WIN32
      if (session::running_sessions.load(std::memory_order_relaxed) > 0) {
        (void) display_helper_integration::apply_pending_if_ready();
        (void) apply_deferred_stream_start_actions_if_ready();
      }
#endif

      // Don't break until any pending sessions either expire or connect
      if (proc::proc.running() == 0 && !has_session_awaiting_peer) {
        BOOST_LOG(info) << "Process terminated"sv;
        break;
      }

      server->iterate(150ms);
    }

    // Let all remaining connections know the server is shutting down
    // reason: graceful termination
    std::uint32_t reason = 0x80030023;

    control_terminate_t plaintext;
    plaintext.header.type = packetTypes[IDX_TERMINATION];
    plaintext.header.payloadLength = sizeof(plaintext.ec);
    plaintext.ec = util::endian::big<uint32_t>(reason);

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto lg = server->_sessions.lock();
    for (auto pos = std::begin(*server->_sessions); pos != std::end(*server->_sessions); ++pos) {
      auto session = *pos;

      // We may not have gotten far enough to have an ENet connection yet
      if (session->control.peer) {
        auto payload = encode_control(session, util::view(plaintext), encrypted_payload);

        if (server->send(payload, session->control.peer)) {
          TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
          BOOST_LOG(warning) << "Couldn't send termination code to ["sv << addr << ':' << port << ']';
        }
      }

      session->shutdown_event->raise(true);
      session->controlEnd.raise(true);
    }

    server->flush();
  }

  void recvThread(broadcast_ctx_t &ctx) {
    std::map<av_session_id_t, message_queue_t> peer_to_video_session;
    std::map<av_session_id_t, message_queue_t> peer_to_audio_session;

    auto &video_sock = ctx.video_sock;
    auto &audio_sock = ctx.audio_sock;

    auto &message_queue_queue = ctx.message_queue_queue;
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    auto &io = ctx.io_context;

    udp::endpoint peer;

    std::array<char, 2048> buf[2];
    std::function<void(const boost::system::error_code, size_t)> recv_func[2];

    platf::set_thread_name("stream::recv");

    auto populate_peer_to_session = [&]() {
      while (message_queue_queue->peek()) {
        auto message_queue_opt = message_queue_queue->pop();
        TUPLE_3D_REF(socket_type, session_id, message_queue, *message_queue_opt);

        switch (socket_type) {
          case socket_e::video:
            if (message_queue) {
              peer_to_video_session.insert_or_assign(session_id, message_queue);
            } else {
              peer_to_video_session.erase(session_id);
            }
            break;
          case socket_e::audio:
            if (message_queue) {
              peer_to_audio_session.insert_or_assign(session_id, message_queue);
            } else {
              peer_to_audio_session.erase(session_id);
            }
            break;
        }
      }
    };

    auto recv_func_init = [&](udp::socket &sock, int buf_elem, std::map<av_session_id_t, message_queue_t> &peer_to_session) {
      recv_func[buf_elem] = [&, buf_elem](const boost::system::error_code &ec, size_t bytes) {
        auto fg = util::fail_guard([&]() {
          sock.async_receive_from(asio::buffer(buf[buf_elem]), peer, 0, recv_func[buf_elem]);
        });

        auto type_str = buf_elem ? "AUDIO"sv : "VIDEO"sv;
        BOOST_LOG(verbose) << "Recv: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;

        populate_peer_to_session();

        // No data, yet no error
        if (ec == boost::system::errc::connection_refused || ec == boost::system::errc::connection_reset) {
          return;
        }

        if (ec || !bytes) {
          BOOST_LOG(error) << "Couldn't receive data from udp socket: "sv << ec.message();
          return;
        }

        (buf_elem ? ctx.audio_recv_count : ctx.video_recv_count).fetch_add(1, std::memory_order_relaxed);

        if (bytes == 4) {
          // For legacy PING packets, find the matching session by address.
          auto it = peer_to_session.find(peer.address());
          if (it != std::end(peer_to_session)) {
            it->second->raise(peer, std::string {buf[buf_elem].data(), bytes});
          }
        } else if (bytes >= sizeof(SS_PING)) {
          auto ping = (PSS_PING) buf[buf_elem].data();

          // For new PING packets that include a client identifier, search by payload.
          auto it = peer_to_session.find(std::string {ping->payload, sizeof(ping->payload)});
          if (it != std::end(peer_to_session)) {
            it->second->raise(peer, std::string {buf[buf_elem].data(), bytes});
          }
        }
      };
    };

    recv_func_init(video_sock, 0, peer_to_video_session);
    recv_func_init(audio_sock, 1, peer_to_audio_session);

    video_sock.async_receive_from(asio::buffer(buf[0]), peer, 0, recv_func[0]);
    audio_sock.async_receive_from(asio::buffer(buf[1]), peer, 0, recv_func[1]);

    while (!broadcast_shutdown_event->peek()) {
      io.run();
    }
  }

  void videoBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto video_epoch = std::chrono::steady_clock::now();

    // Video traffic is sent on this thread
    platf::set_thread_name("stream::videoBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    logging::min_max_avg_periodic_logger<double> frame_processing_latency_logger(debug, "Frame processing latency", "ms");
    logging::min_max_avg_periodic_logger<double> frame_capture_interval_logger(debug, "Frame capture interval", "ms");
    logging::min_max_avg_periodic_logger<double> packet_queue_latency_logger(debug, "Video packet queue latency", "ms");
    logging::min_max_avg_periodic_logger<double> ratecontrol_sleep_logger(debug, "Network: rate control sleep", "ms");
    logging::min_max_avg_periodic_logger<double> ratecontrol_late_logger(debug, "Network: rate control late", "ms");
    logging::min_max_avg_periodic_logger<double> ratecontrol_frame_packets_logger(debug, "Network: frame packets sent", "packets");
    logging::min_max_avg_periodic_logger<double> ratecontrol_batch_packets_logger(debug, "Network: send_batch packet count", "packets");

    logging::time_delta_periodic_logger frame_send_batch_latency_logger(debug, "Network: each send_batch() latency");
    logging::time_delta_periodic_logger frame_fec_latency_logger(debug, "Network: each FEC block latency");
    logging::time_delta_periodic_logger frame_network_latency_logger(debug, "Network: frame's overall network latency");

    crypto::aes_t iv(12);

    auto timer = platf::create_high_precision_timer();
    if (!timer || !*timer) {
      BOOST_LOG(error) << "Failed to create timer, aborting video broadcast thread";
      return;
    }

    auto ratecontrol_next_frame_start = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> last_frame_timestamp;

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      frame_network_latency_logger.first_point_now();

      auto session = (session_t *) packet->channel_data;
      if (!session) {
        continue;
      }
      const auto packet_pop_timestamp = std::chrono::steady_clock::now();
      packet_queue_latency_logger.collect_and_log(std::chrono::duration<double, std::milli>(packet_pop_timestamp - packet->packet_enqueue_timestamp).count());
      if (packet->frame_timestamp) {
        if (last_frame_timestamp) {
          frame_capture_interval_logger.collect_and_log(std::chrono::duration<double, std::milli>(*packet->frame_timestamp - *last_frame_timestamp).count());
        }
        last_frame_timestamp = *packet->frame_timestamp;
      }

      auto lowseq = session->video.lowseq;

      std::string_view payload {(char *) packet->data(), packet->data_size()};
      std::vector<uint8_t> payload_with_replacements;

      // Apply replacements on the packet payload before performing any other operations.
      // We need to know the final frame size to calculate the last packet size, and we
      // must avoid matching replacements against the frame header or any other non-video
      // part of the payload.
      if (packet->is_idr() && packet->replacements) {
        for (auto &replacement : *packet->replacements) {
          auto frame_old = replacement.old;
          auto frame_new = replacement._new;

          payload_with_replacements = replace(payload, frame_old, frame_new);
          payload = {(char *) payload_with_replacements.data(), payload_with_replacements.size()};
        }
      }

      video_short_frame_header_t frame_header = {};
      frame_header.headerType = 0x01;  // Short header type
      frame_header.frameType = packet->is_idr()                     ? 2 :
                               packet->after_ref_frame_invalidation ? 5 :
                                                                      1;
      frame_header.lastPayloadLen = (payload.size() + sizeof(frame_header)) % (session->config.packetsize - sizeof(NV_VIDEO_PACKET));
      if (frame_header.lastPayloadLen == 0) {
        frame_header.lastPayloadLen = session->config.packetsize - sizeof(NV_VIDEO_PACKET);
      }

      auto host_processing_timestamp = packet->host_processing_timestamp ? packet->host_processing_timestamp : packet->frame_timestamp;
      if (host_processing_timestamp) {
        auto duration_to_latency = [](const std::chrono::steady_clock::duration &duration) {
          const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
          return (uint16_t) std::clamp<decltype(duration_us)>((duration_us + 50) / 100, 0, std::numeric_limits<uint16_t>::max());
        };

        uint16_t latency = duration_to_latency(std::chrono::steady_clock::now() - *host_processing_timestamp);
        frame_header.frame_processing_latency = latency;
        frame_processing_latency_logger.collect_and_log(latency / 10.);
        session->stats.last_encode_latency_us10.store(latency, std::memory_order_relaxed);
      } else {
        frame_header.frame_processing_latency = 0;
        session->stats.last_encode_latency_us10.store(0, std::memory_order_relaxed);
      }

      auto fecPercentage = config::stream.fec_percentage;

      // Insert space for packet headers
      auto blocksize = session->config.packetsize + MAX_RTP_HEADER_SIZE;
      auto payload_blocksize = blocksize - sizeof(video_packet_raw_t);
      auto payload_new = concat_and_insert(sizeof(video_packet_raw_t), payload_blocksize, std::string_view {(char *) &frame_header, sizeof(frame_header)}, payload);

      payload = std::string_view {(char *) payload_new.data(), payload_new.size()};

      // There are 2 bits for FEC block count for a maximum of 4 FEC blocks
      constexpr auto MAX_FEC_BLOCKS = 4;
      constexpr auto MAX_TOTAL_FEC_SHARDS = 255;

      // The max number of data shards per block is found by solving this system of equations for D:
      // D = 255 - P
      // P = D * F
      // which results in the solution:
      // D = 255 / (1 + F)
      // multiplied by 100 since F is the percentage as an integer:
      // D = (255 * 100) / (100 + F)
      auto max_data_shards_per_fec_block = (MAX_TOTAL_FEC_SHARDS * 100) / (100 + fecPercentage);

      // Compute the number of FEC blocks needed for this frame using the block size and max shards
      auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
      auto fec_blocks_needed = (payload.size() + (max_data_per_fec_block - 1)) / max_data_per_fec_block;

      // If the number of FEC blocks needed exceeds the protocol limit, turn off FEC for this frame.
      // For normal FEC percentages, this should only happen for enormous frames (over 800 packets at 20%).
      if (fec_blocks_needed > MAX_FEC_BLOCKS) {
        BOOST_LOG(warning) << "Skipping FEC for abnormally large encoded frame (needed "sv << fec_blocks_needed << " FEC blocks)"sv;
        fecPercentage = 0;
        fec_blocks_needed = MAX_FEC_BLOCKS;
      }

      std::array<std::string_view, MAX_FEC_BLOCKS> fec_blocks;
      decltype(fec_blocks)::iterator
        fec_blocks_begin = std::begin(fec_blocks),
        fec_blocks_end = std::begin(fec_blocks) + fec_blocks_needed;

      BOOST_LOG(verbose) << "Generating "sv << fec_blocks_needed << " FEC blocks"sv;

      // Align individual FEC blocks to blocksize
      auto unaligned_size = payload.size() / fec_blocks_needed;
      auto aligned_size = ((unaligned_size + (blocksize - 1)) / blocksize) * blocksize;

      // If we exceed the 10-bit FEC packet index (which means our frame exceeded 4096 packets),
      // the frame will be unrecoverable. Log an error for this case.
      if (aligned_size / blocksize >= 1024) {
        BOOST_LOG(error) << "Encoder produced a frame too large to send! Is the encoder broken? (needed "sv << (aligned_size / blocksize) << " packets)"sv;
      }

      // Split the data into aligned FEC blocks
      for (int x = 0; x < fec_blocks_needed; ++x) {
        if (x == fec_blocks_needed - 1) {
          // The last block must extend to the end of the payload
          fec_blocks[x] = payload.substr(x * aligned_size);
        } else {
          // Earlier blocks just extend to the next block offset
          fec_blocks[x] = payload.substr(x * aligned_size, aligned_size);
        }
      }

      try {
        // Use around 80% of 1Gbps          1Gbps            percent    ms     packet      byte
        size_t ratecontrol_packets_in_1ms = std::giga::num * 80 / 100 / 1000 / blocksize / 8;

        // Send less than 64K in a single batch.
        // On Windows, batches above 64K seem to bypass SO_SNDBUF regardless of its size,
        // appear in "Other I/O" and begin waiting for interrupts.
        // This gives inconsistent performance so we'd rather avoid it.
        size_t max_batch_size_bytes = 64 * 1024;
        max_batch_size_bytes = std::min<size_t>(max_batch_size_bytes, (size_t) config::stream.video_max_batch_size_kb * 1024);

        size_t send_batch_size = std::max<size_t>(1, max_batch_size_bytes / blocksize);
        // Also don't exceed 64 packets, which can happen when Moonlight requests
        // unusually small packet size.
        // Generic Segmentation Offload on Linux can't do more than 64.
        send_batch_size = std::min<size_t>(64, send_batch_size);

        // Don't ignore the last ratecontrol group of the previous frame
        auto ratecontrol_frame_start = std::max(ratecontrol_next_frame_start, std::chrono::steady_clock::now());

        size_t ratecontrol_frame_packets_sent = 0;
        size_t ratecontrol_group_packets_sent = 0;

        auto blockIndex = 0;
        std::for_each(fec_blocks_begin, fec_blocks_end, [&](std::string_view &current_payload) {
          auto packets = (current_payload.size() + (blocksize - 1)) / blocksize;

          for (int x = 0; x < packets; ++x) {
            auto *inspect = (video_packet_raw_t *) &current_payload[x * blocksize];

            inspect->packet.frameIndex = (uint32_t) packet->frame_index();
            inspect->packet.streamPacketIndex = ((uint32_t) lowseq + x) << 8;

            // Match multiFecFlags with Moonlight
            inspect->packet.multiFecFlags = 0x10;
            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);

            inspect->packet.flags = FLAG_CONTAINS_PIC_DATA;
            if (x == 0) {
              inspect->packet.flags |= FLAG_SOF;
            }
            if (x == packets - 1) {
              inspect->packet.flags |= FLAG_EOF;
            }
          }

          frame_fec_latency_logger.first_point_now();
          // If video encryption is enabled, we allocate space for the encryption header before each shard
          auto shards = fec::encode(current_payload, blocksize, fecPercentage, session->config.minRequiredFecPackets, session->video.cipher ? sizeof(video_packet_enc_prefix_t) : 0);
          frame_fec_latency_logger.second_point_now_and_log();

          auto peer_address = session->video.peer.address();
          auto batch_info = platf::batched_send_info_t {
            shards.headers.begin(),
            shards.prefixsize,
            shards.payload_buffers,
            shards.blocksize,
            0,
            0,
            (uintptr_t) sock.native_handle(),
            peer_address,
            session->video.peer.port(),
            session->localAddress,
          };

          size_t next_shard_to_send = 0;

          // RTP video timestamps use a 90 KHz clock and the paced/scheduled frame timestamp.
          // Raw capture timing is tracked separately for frame_processing_latency.
          // When a timestamp isn't available (duplicate frames), the timestamp from rate control is used instead.
          bool frame_is_dupe = false;
          if (!packet->frame_timestamp) {
            packet->frame_timestamp = ratecontrol_next_frame_start;
            frame_is_dupe = true;
          }
          using rtp_tick = std::chrono::duration<uint32_t, std::ratio<1, 90000>>;
          uint32_t timestamp = std::chrono::round<rtp_tick>(*packet->frame_timestamp - video_epoch).count();

          // set FEC info now that we know for sure what our percentage will be for this frame
          for (auto x = 0; x < shards.size(); ++x) {
            auto *inspect = (video_packet_raw_t *) shards.data(x);

            inspect->packet.fecInfo =
              (uint32_t) (x << 12 |
                          shards.data_shards << 22 |
                          shards.percentage << 4);

            inspect->rtp.header = 0x80 | FLAG_EXTENSION;
            inspect->rtp.sequenceNumber = util::endian::big<uint16_t>(lowseq + x);
            inspect->rtp.timestamp = util::endian::big<uint32_t>(timestamp);

            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);
            inspect->packet.frameIndex = (uint32_t) packet->frame_index();

            // Encrypt this shard if video encryption is enabled
            if (session->video.cipher) {
              // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
              // Section 8.2.1. The sequence number is our "invocation" field and the 'V' in the
              // high bytes is the "fixed" field. Because each client provides their own unique
              // key, our values in the fixed field need only uniquely identify each independent
              // use of the client's key with AES-GCM in our code.
              //
              // The IV counter is 64 bits long which allows for 2^64 encrypted video packets
              // to be sent to each client before the IV repeats.
              std::copy_n((uint8_t *) &session->video.gcm_iv_counter, sizeof(session->video.gcm_iv_counter), std::begin(iv));
              iv[11] = 'V';  // Video stream
              session->video.gcm_iv_counter++;

              // Encrypt the target buffer in place
              auto *prefix = (video_packet_enc_prefix_t *) shards.prefix(x);
              prefix->frameNumber = (std::uint32_t) packet->frame_index();
              std::copy(std::begin(iv), std::end(iv), prefix->iv);
              session->video.cipher->encrypt(std::string_view {(char *) inspect, (size_t) blocksize}, prefix->tag, (uint8_t *) inspect, &iv);
            }

            if (x - next_shard_to_send + 1 >= send_batch_size ||
                x + 1 == shards.size()) {
              // Do pacing within the frame.
              // Also trigger pacing before the first send_batch() of the frame
              // to account for the last send_batch() of the previous frame.
              if (ratecontrol_group_packets_sent >= ratecontrol_packets_in_1ms ||
                  ratecontrol_frame_packets_sent == 0) {
                auto due = ratecontrol_frame_start +
                           std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                             ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;

                auto now = std::chrono::steady_clock::now();
                if (now < due) {
                  auto sleep_time = due - now;
                  ratecontrol_sleep_logger.collect_and_log(std::chrono::duration<double, std::milli>(sleep_time).count());
                  timer->sleep_for(sleep_time);
                } else {
                  ratecontrol_late_logger.collect_and_log(std::chrono::duration<double, std::milli>(now - due).count());
                }

                ratecontrol_group_packets_sent = 0;
              }

              size_t current_batch_size = x - next_shard_to_send + 1;
              ratecontrol_batch_packets_logger.collect_and_log((double) current_batch_size);
              batch_info.block_offset = next_shard_to_send;
              batch_info.block_count = current_batch_size;

              frame_send_batch_latency_logger.first_point_now();
              // Use a batched send if it's supported on this platform
              if (!platf::send_batch(batch_info)) {
                // Batched send is not available, so send each packet individually
                BOOST_LOG(verbose) << "Falling back to unbatched send"sv;
                for (auto y = 0; y < current_batch_size; y++) {
                  auto send_info = platf::send_info_t {
                    shards.prefix(next_shard_to_send + y),
                    shards.prefixsize,
                    shards.data(next_shard_to_send + y),
                    shards.blocksize,
                    (uintptr_t) sock.native_handle(),
                    peer_address,
                    session->video.peer.port(),
                    session->localAddress,
                  };

                  platf::send(send_info);
                }
              }
              frame_send_batch_latency_logger.second_point_now_and_log();

              ratecontrol_group_packets_sent += current_batch_size;
              ratecontrol_frame_packets_sent += current_batch_size;
              next_shard_to_send = x + 1;
            }
          }

          // remember this in case the next frame comes immediately
          ratecontrol_next_frame_start = ratecontrol_frame_start +
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                                           ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
          ratecontrol_frame_packets_logger.collect_and_log((double) ratecontrol_frame_packets_sent);

          frame_network_latency_logger.second_point_now_and_log();

          BOOST_LOG(verbose) << "Sent Frame seq ["sv << packet->frame_index() << "] pts ["sv << timestamp
                             << "] shards ["sv << shards.size() << "/"sv << shards.percentage << "%]"sv
                             << (frame_is_dupe ? " Dupe" : "")
                             << (packet->is_idr() ? " Key" : "")
                             << (packet->after_ref_frame_invalidation ? " RFI" : "");

          ++blockIndex;
          lowseq += shards.size();
        });

        session->video.lowseq = lowseq;

        // Update per-session performance counters
        session->stats.frames_sent.fetch_add(1, std::memory_order_relaxed);
        session->stats.packets_sent.fetch_add(ratecontrol_frame_packets_sent, std::memory_order_relaxed);
        auto bytes_per_packet = blocksize + ((session->config.encryptionFlagsEnabled & SS_ENC_VIDEO) ? sizeof(video_packet_enc_prefix_t) : 0);
        session->stats.bytes_sent.fetch_add(ratecontrol_frame_packets_sent * bytes_per_packet, std::memory_order_relaxed);
        session->stats.last_frame_index.store(packet->frame_index(), std::memory_order_relaxed);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast video failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  void audioBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    audio_packet_t audio_packet;
    fec::rs_t rs {reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS)};
    crypto::aes_t iv(16);

    // For unknown reasons, the RS parity matrix computed by our RS implementation
    // doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
    // but we can simply replace it with the matrix generated by OpenFEC which
    // works correctly. This is possible because the data and FEC shard count is
    // constant and known in advance.
    const unsigned char parity[] = {0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c};
    memcpy(rs.get()->p, parity, sizeof(parity));

    audio_packet.rtp.header = 0x80;
    audio_packet.rtp.packetType = 97;
    audio_packet.rtp.ssrc = 0;

    // Audio traffic is sent on this thread
    platf::set_thread_name("stream::audioBroadcast");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      TUPLE_2D_REF(channel_data, packet_data, *packet);
      auto session = (session_t *) channel_data;
      if (!session) {
        continue;
      }

      auto sequenceNumber = session->audio.sequenceNumber;
      auto timestamp = session->audio.timestamp;

      *(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(session->audio.avRiKeyId + sequenceNumber);

      auto &shards_p = session->audio.shards_p;

      auto bytes = encode_audio(session->config.encryptionFlagsEnabled & SS_ENC_AUDIO, packet_data, shards_p[sequenceNumber % RTPA_DATA_SHARDS], iv, session->audio.cipher);
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio packet"sv;
        break;
      }

      BOOST_LOG(verbose) << "Audio [seq "sv << sequenceNumber << ", pts "sv << timestamp << "] ::  send..."sv;

      audio_packet.rtp.sequenceNumber = util::endian::big(sequenceNumber);
      audio_packet.rtp.timestamp = util::endian::big(timestamp);

      session->audio.sequenceNumber++;
      session->audio.timestamp += session->config.audio.packetDuration;

      auto peer_address = session->audio.peer.address();
      try {
        auto send_info = platf::send_info_t {
          (const char *) &audio_packet,
          sizeof(audio_packet),
          (const char *) shards_p[sequenceNumber % RTPA_DATA_SHARDS],
          (size_t) bytes,
          (uintptr_t) sock.native_handle(),
          peer_address,
          session->audio.peer.port(),
          session->localAddress,
        };
        platf::send(send_info);

        auto &fec_packet = session->audio.fec_packet;
        // initialize the FEC header at the beginning of the FEC block
        if (sequenceNumber % RTPA_DATA_SHARDS == 0) {
          fec_packet.fecHeader.baseSequenceNumber = util::endian::big(sequenceNumber);
          fec_packet.fecHeader.baseTimestamp = util::endian::big(timestamp);
        }

        // generate parity shards at the end of the FEC block
        if ((sequenceNumber + 1) % RTPA_DATA_SHARDS == 0) {
          reed_solomon_encode(rs.get(), shards_p.begin(), RTPA_TOTAL_SHARDS, bytes);

          for (auto x = 0; x < RTPA_FEC_SHARDS; ++x) {
            fec_packet.rtp.sequenceNumber = util::endian::big<std::uint16_t>(sequenceNumber + x + 1);
            fec_packet.fecHeader.fecShardIndex = x;

            auto send_info = platf::send_info_t {
              (const char *) &fec_packet,
              sizeof(fec_packet),
              (const char *) shards_p[RTPA_DATA_SHARDS + x],
              (size_t) bytes,
              (uintptr_t) sock.native_handle(),
              peer_address,
              session->audio.peer.port(),
              session->localAddress,
            };
            platf::send(send_info);
            BOOST_LOG(verbose) << "Audio FEC ["sv << (sequenceNumber & ~(RTPA_DATA_SHARDS - 1)) << ' ' << x << "] ::  send..."sv;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast audio failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  int start_broadcast(broadcast_ctx_t &ctx) {
    // Reset the shutdown event to ensure it's cleared even if something
    // raised it between the last end_broadcast and now.
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    broadcast_shutdown_event->reset();

    // Reset the packet queues which were stopped in end_broadcast.
    // If not reset, the broadcast threads will exit immediately when pop() returns null.
    auto video_packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail::man->queue<audio::packet_t>(mail::audio_packets);
    video_packets->reset();
    audio_packets->reset();

    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto control_port = net::map_port(CONTROL_PORT);
    auto video_port = net::map_port(VIDEO_STREAM_PORT);
    auto audio_port = net::map_port(AUDIO_STREAM_PORT);

    // Parse the bind address up front so the socket protocol can follow the
    // actual endpoint family. An IPv4 bind_address must open IPv4 sockets even
    // when address_family is set to "both".
    boost::system::error_code ec;
    auto bind_addr_str = net::get_bind_address(address_family);
    const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
      return -1;
    }
    auto protocol = net::udp_protocol_for_address(bind_addr);

    if (ctx.control_server.bind(address_family, control_port)) {
      BOOST_LOG(error) << "Couldn't bind Control server to port ["sv << control_port << "], likely another process already bound to the port"sv;

      return -1;
    }

    ctx.video_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Video server: "sv << ec.message();

      return -1;
    }

    // Set video socket send buffer size (SO_SENDBUF) to 1MB
    try {
      ctx.video_sock.set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
    } catch (...) {
      BOOST_LOG(error) << "Failed to set video socket send buffer size (SO_SENDBUF)";
    }

    ctx.video_sock.bind(udp::endpoint(bind_addr, video_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Video server to port ["sv << video_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Audio server: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.bind(udp::endpoint(bind_addr, audio_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Audio server to port ["sv << audio_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.message_queue_queue = std::make_shared<message_queue_queue_t::element_type>(30);

    // Restart the io_context in case it was stopped from a previous session.
    // After calling stop(), restart() must be called before run() will work again.
    ctx.io_context.restart();

    ctx.video_thread = std::thread {videoBroadcastThread, std::ref(ctx.video_sock)};
    ctx.audio_thread = std::thread {audioBroadcastThread, std::ref(ctx.audio_sock)};
    ctx.control_thread = std::thread {controlBroadcastThread, &ctx.control_server};

    ctx.recv_thread = std::thread {recvThread, std::ref(ctx)};

    return 0;
  }

  void end_broadcast(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    broadcast_shutdown_event->raise(true);

    auto video_packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    // Minimize delay stopping video/audio threads
    video_packets->stop();
    audio_packets->stop();

    ctx.message_queue_queue->stop();
    ctx.io_context.stop();

    ctx.video_sock.close();
    ctx.audio_sock.close();

    video_packets.reset();
    audio_packets.reset();

    BOOST_LOG(debug) << "Waiting for main listening thread to end..."sv;
    ctx.recv_thread.join();
    BOOST_LOG(debug) << "Waiting for main video thread to end..."sv;
    ctx.video_thread.join();
    BOOST_LOG(debug) << "Waiting for main audio thread to end..."sv;
    ctx.audio_thread.join();
    BOOST_LOG(debug) << "Waiting for main control thread to end..."sv;
    ctx.control_thread.join();
    BOOST_LOG(debug) << "All broadcasting threads ended"sv;

    broadcast_shutdown_event->reset();
  }

  int recv_ping(session_t *session, decltype(broadcast)::ptr_t ref, socket_e type, std::string_view expected_payload, udp::endpoint &peer, std::chrono::milliseconds timeout) {
    auto messages = std::make_shared<message_queue_t::element_type>(30);
    av_session_id_t session_id = std::string {expected_payload};

    auto &recv_counter = type == socket_e::video ? ref->video_recv_count : ref->audio_recv_count;
    const auto recv_baseline = recv_counter.load(std::memory_order_relaxed);

    // Only allow matches on the peer address for legacy clients
    if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
      ref->message_queue_queue->raise(type, peer.address(), messages);
    }
    ref->message_queue_queue->raise(type, session_id, messages);

    auto fg = util::fail_guard([&]() {
      messages->stop();

      // remove message queue from session
      if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
        ref->message_queue_queue->raise(type, peer.address(), nullptr);
      }
      ref->message_queue_queue->raise(type, session_id, nullptr);
    });

    auto start_time = std::chrono::steady_clock::now();
    auto current_time = start_time;

    while (current_time - start_time < config::stream.ping_timeout) {
      auto delta_time = current_time - start_time;

      auto msg_opt = messages->pop(config::stream.ping_timeout - delta_time);
      if (!msg_opt) {
        break;
      }

      TUPLE_2D_REF(recv_peer, msg, *msg_opt);
      if (msg.find(expected_payload) != std::string::npos) {
        // Match the new PING payload format
        BOOST_LOG(debug) << "Received ping [v2] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      } else if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) && msg == "PING"sv) {
        // Match the legacy fixed PING payload only if the new type is not supported
        BOOST_LOG(debug) << "Received ping [v1] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      } else {
        BOOST_LOG(debug) << "Received non-ping from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      // Update connection details.
      peer = recv_peer;
      return 0;
    }

    BOOST_LOG(error) << "Initial Ping Timeout"sv;

    const auto type_str = type == socket_e::video ? "video"sv : "audio"sv;
    const auto received = recv_counter.load(std::memory_order_relaxed) - recv_baseline;
    if (received == 0) {
      BOOST_LOG(error) << "No UDP datagrams reached the "sv << type_str << " socket while waiting for the client's ping. "sv
                       << "Inbound UDP is most likely being blocked before it reaches Sunshine. "sv
                       << "Check Windows Firewall, VPN clients with kill switches or LAN blocking "sv
                       << "(e.g. NordVPN, Private Internet Access - these filter traffic even while disconnected), "sv
                       << "and router/NAT port forwarding."sv;
    } else {
      BOOST_LOG(error) << received << " UDP datagram(s) reached the "sv << type_str << " socket, but none matched this session's ping. "sv
                       << "The client may be pinging from an unexpected address or with an unexpected payload."sv;
    }
    return -1;
  }

  void videoThread(session_t *session) {
    platf::set_thread_name("session::video");
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast.ref();
    const auto ping_error = recv_ping(session, ref, socket_e::video, session->video.ping_payload, session->video.peer, config::stream.ping_timeout);
    if (ping_error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on video traffic if requested by the client
    auto address = session->video.peer.address();
    session->video.qos = platf::enable_socket_qos(ref->video_sock.native_handle(), address, session->video.peer.port(), platf::qos_data_type_e::video, session->config.videoQosType != 0);

#ifdef _WIN32
    if (session->display_helper_gate.valid()) {
      BOOST_LOG(debug) << "Display helper: waiting for apply/validation gate before starting capture.";
      rtsp_stream::launch_session_t::display_helper_gate_status_e gate_status {};
      try {
        constexpr auto kGateTimeout = std::chrono::seconds(7);
        if (session->display_helper_gate.wait_for(kGateTimeout) == std::future_status::ready) {
          gate_status = session->display_helper_gate.get();
        } else {
          BOOST_LOG(warning) << "Display helper: gate wait timed out; proceeding with capture.";
          gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Display helper: gate wait failed (" << e.what() << "); proceeding with capture.";
        gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;
      } catch (...) {
        BOOST_LOG(warning) << "Display helper: gate wait failed (unknown); proceeding with capture.";
        gate_status = rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup;
      }

      if (gate_status == rtsp_stream::launch_session_t::display_helper_gate_status_e::abort_failed) {
        BOOST_LOG(error) << "Display helper validation failed; starting capture anyway.";
      }
      if (gate_status == rtsp_stream::launch_session_t::display_helper_gate_status_e::proceed_gaveup) {
        BOOST_LOG(warning) << "Display helper verification result unavailable; starting capture anyway.";
      }
    }
#endif

    BOOST_LOG(debug) << "Start capturing Video"sv;
    video::capture(session->mail, session->config.monitor, session);
  }

  void audioThread(session_t *session) {
    platf::set_thread_name("session::audio");
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast.ref();
    auto error = recv_ping(session, ref, socket_e::audio, session->audio.ping_payload, session->audio.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on audio traffic if requested by the client
    auto address = session->audio.peer.address();
    session->audio.qos = platf::enable_socket_qos(ref->audio_sock.native_handle(), address, session->audio.peer.port(), platf::qos_data_type_e::audio, session->config.audioQosType != 0);

    BOOST_LOG(debug) << "Start capturing Audio"sv;
    audio::capture(session->mail, session->config.audio, session);
  }

  namespace session {
    std::atomic_uint running_sessions;

    state_e state(session_t &session) {
      return session.state.load(std::memory_order_relaxed);
    }

    inline bool send(session_t &session, const std::string_view &payload) {
      return session.broadcast_ref->control_server.send(payload, session.control.peer);
    }

    std::string uuid(const session_t &session) {
      return session.device_uuid;
    }

    bool uuid_match(const session_t &session, const std::string_view &uuid) {
      return session.device_uuid == uuid || session.history_uuid == uuid;
    }

    bool update_device_info(session_t &session, const std::string &name, const crypto::PERM &newPerm) {
      session.permission = newPerm;
      std::string device_name;
      {
        std::lock_guard lg {session.metadata_mutex};
        device_name = session.device_name;
      }
      if (!(newPerm & crypto::PERM::_allow_view)) {
        BOOST_LOG(debug) << "Session: View permission revoked for [" << device_name << "], disconnecting...";
        graceful_stop(session);
        return true;
      }

      BOOST_LOG(debug) << "Session: Permission updated for [" << device_name << "]";

      if (device_name != name) {
        BOOST_LOG(debug) << "Session: Device name changed from [" << device_name << "] to [" << name << "]";
        std::lock_guard lg {session.metadata_mutex};
        session.device_name = name;
      }

      return false;
    }

    void stop(session_t &session) {
      while_starting_do_nothing(session.state);
      auto expected = state_e::RUNNING;
      auto already_stopping = !session.state.compare_exchange_strong(expected, state_e::STOPPING);
      if (already_stopping) {
        return;
      }

      session.shutdown_event->raise(true);
    }

    void graceful_stop(session_t &session) {
      while_starting_do_nothing(session.state);
      auto expected = state_e::RUNNING;
      auto already_stopping = !session.state.compare_exchange_strong(expected, state_e::STOPPING);
      if (already_stopping) {
        return;
      }

      // reason: graceful termination
      std::uint32_t reason = 0x80030023;

      control_terminate_t plaintext;
      plaintext.header.type = packetTypes[IDX_TERMINATION];
      plaintext.header.payloadLength = sizeof(plaintext.ec);
      plaintext.ec = util::endian::big<uint32_t>(reason);

      // We may not have gotten far enough to have an ENet connection yet
      if (session.control.peer) {
        std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
          encrypted_payload;
        auto payload = stream::encode_control(&session, util::view(plaintext), encrypted_payload);

        if (send(session, payload)) {
          TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session.control.peer->address.address));
          BOOST_LOG(warning) << "Couldn't send termination code to ["sv << addr << ':' << port << ']';
        }
      }

      session.shutdown_event->raise(true);
      session.controlEnd.raise(true);
    }

    void join(session_t &session) {
      // Current Nvidia drivers have a bug where NVENC can deadlock the encoder thread with hardware-accelerated
      // GPU scheduling enabled. If this happens, we will terminate ourselves and the service can restart.
      // The alternative is that Sunshine can never start another session until it's manually restarted.
      // Name the join stage for the watchdog so a crash bundle says outright what hung
      // (e.g. vibeshine#187 took dump archaeology to learn it was the video thread).
      auto hung_stage = std::make_shared<std::atomic<const char *>>("video thread");
      auto task = [hung_stage]() {
        BOOST_LOG(fatal) << "Hang detected! Session failed to terminate in 10 seconds. Stuck waiting for: "sv << hung_stage->load();
        logging::log_flush();
        lifetime::debug_trap();
      };
      auto force_kill = task_pool.pushDelayed(task, 10s).task_id;
      auto fg = util::fail_guard([&force_kill]() {
        // Cancel the kill task if we manage to return from this function
        task_pool.cancel(force_kill);
      });

      // Teardown progress logged at info so that if join() hangs (the 10s watchdog
      // above fires), the last "Teardown:" line before the Fatal pinpoints the
      // blocking step. Cheap; sessions end infrequently.
      BOOST_LOG(info) << "Teardown: joining video thread..."sv;
      session.videoThread.join();
      hung_stage->store("audio thread");
      BOOST_LOG(debug) << "Waiting for audio to end..."sv;
      session.audioThread.join();
      hung_stage->store("control end");
      BOOST_LOG(debug) << "Waiting for control to end..."sv;
      session.controlEnd.view();
      hung_stage->store("post-join cleanup");
      // Reset input on session stop to avoid stuck repeated keys
      BOOST_LOG(info) << "Teardown: resetting input..."sv;
      input::reset(session.input);

      // Cancel the capture-reapply retry thread so it doesn't access audio_ctrl
      // after we reset it below. The detached thread holds shared ownership of
      // audio_ctrl, so the object stays alive until the thread exits naturally.
      if (session.mic.reapply_cancel) {
        session.mic.reapply_cancel->store(true, std::memory_order_release);
        session.mic.reapply_cancel.reset();
      }

      // Restore default capture device if we switched it on session start
      if (session.mic.capture_switched && session.mic.audio_ctrl) {
        session.mic.audio_ctrl->restore_capture_from(session.mic.capture_snap);
        session.mic.capture_switched = false;
        g_mic_capture_switched_sessions.fetch_sub(1, std::memory_order_relaxed);
      }
      if (session.mic.audio_ctrl) {
        session.mic.audio_ctrl->release_mic_redirect_device();
      }
      session.mic.decoder.reset();
      session.mic.audio_ctrl.reset();
      BOOST_LOG(info) << "[mic] session end: recv="sv << session.mic.packets_received
                      << " decoded="sv << session.mic.frames_written
                      << " plc="sv << session.mic.plc_events
                      << " silence="sv << session.mic.silence_frames
                      << " errors="sv << session.mic.decode_errors;
      // Keep last-session stats for the web UI mic status card (only sessions
      // that actually received mic data — Aurora-style sessions don't count).
      if (session.mic.packets_received > 0) {
        g_mic_last_packets_received.store(session.mic.packets_received, std::memory_order_relaxed);
        g_mic_last_frames_written.store(session.mic.frames_written, std::memory_order_relaxed);
        g_mic_has_last_session.store(true, std::memory_order_relaxed);
      }

      if (!session.undo_cmds.empty()) {
        auto exec_thread = std::thread([cmd_list = session.undo_cmds] {
          for (auto &cmd : cmd_list) {
            std::error_code ec;
            auto env = proc::proc.get_env();
            boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd, env);
            auto child = platf::run_command(cmd.elevated, true, cmd.cmd, working_dir, env, nullptr, ec, nullptr);
            BOOST_LOG(info) << "Spawning client undo command ["sv << cmd.cmd << "] in ["sv << working_dir << ']';
            if (ec) {
              BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd.cmd << "]: System: "sv << ec.message();
            } else {
              child.detach();
            }
          }
        });

        exec_thread.detach();
      }

      // If this is the last session, invoke the platform callbacks
      const bool last_rtsp_session = --running_sessions == 0;
      if (last_rtsp_session) {
        webrtc_stream::set_rtsp_sessions_active(false);
        config::set_runtime_output_name_override(std::nullopt);
#ifdef _WIN32
        display_helper_integration::clear_pending_apply();
        clear_deferred_stream_start_actions();
#endif
        // Only revert on disconnect when explicitly enabled by config.
        bool revert_display_config {config::video.dd.config_revert_on_disconnect};

        const bool webrtc_active = webrtc_stream::has_active_sessions();
        if (!webrtc_active) {
          BOOST_LOG(info) << "Teardown: pausing app process..."sv;
          proc::proc.pause();
          BOOST_LOG(info) << "Teardown: app process paused."sv;
        }
        // Poll for up to 2 seconds to let the game process finish exiting before
        // deciding paused vs stopped. Fixes the race where the user quits the game
        // and immediately hits the disconnect combo — the process is still alive for
        // a brief moment during teardown and would otherwise always show "paused".
        bool is_paused = proc::proc.running() > 0;
        if (is_paused) {
          const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (proc::proc.running() && std::chrono::steady_clock::now() < poll_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          is_paused = proc::proc.running();
        }
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
        if (is_paused) {
          system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
        } else {
          system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
        }
#endif
        const int paused_timeout_secs = std::max(0, config::video.dd.paused_virtual_display_timeout_secs);
        const bool delay_virtual_display_cleanup_due_to_pause = is_paused && !revert_display_config && paused_timeout_secs > 0;
        const bool keep_virtual_display_due_to_pause = is_paused && !revert_display_config && paused_timeout_secs == 0;

#ifdef _WIN32
        if (webrtc_active) {
          BOOST_LOG(debug) << "Display cleanup: WebRTC session is still active; skipping RTSP-triggered teardown.";
        } else if (delay_virtual_display_cleanup_due_to_pause) {
          BOOST_LOG(info) << "Display cleanup: session paused with revert-on-disconnect disabled; "
                          << "scheduling virtual display removal without display restore in " << paused_timeout_secs << "s.";
          schedule_paused_display_cleanup(
            std::chrono::seconds(paused_timeout_secs),
            "rtsp_session_paused",
            false,
            session.virtual_display.guid_bytes
          );
        } else if (keep_virtual_display_due_to_pause) {
          BOOST_LOG(debug) << "Display cleanup: session is paused; keeping virtual display alive (config_revert_on_disconnect=false, paused timeout disabled).";
        } else {
          g_paused_display_cleanup_generation.fetch_add(1, std::memory_order_acq_rel);
          const auto cleanup_reason = is_paused && !revert_display_config ? "rtsp_session_paused" : "rtsp_session_end";
          const auto cleanup = platf::virtual_display_cleanup::run(
            cleanup_reason,
            revert_display_config,
            platf::virtual_display_cleanup::revert_order_t::remove_before_restore,
            true,
            session.virtual_display.guid_bytes
          );
          if (cleanup.helper_revert_dispatched) {
            // If we reverted the display configuration, the helper watchdog is no longer needed.
            display_helper_integration::stop_watchdog();
          } else if (revert_display_config) {
            BOOST_LOG(debug) << "Display helper: revert dispatch failed; leaving watchdog running.";
          } else if (is_paused) {
            BOOST_LOG(info) << "Display cleanup: session paused with revert-on-disconnect disabled; "
                            << "removed virtual display(s) without restoring physical display configuration.";
          }
        }
#else
        if (revert_display_config && !webrtc_active) {
          (void) display_helper_integration::revert();
        }
#endif

        // Restore any Windows-only integrations first
#ifdef _WIN32
        VDISPLAY::restorePhysicalHdrProfiles();
        platf::rtss_set_sync_limiter_override(std::nullopt);
        platf::frame_limiter_streaming_stop(is_paused);
#endif
        platf::streaming_will_stop();
      }

      BOOST_LOG(info) << "Session ended"sv;

      // Record session end in persistent history (fires exactly once, after join)
      session_history::end_session(session.history_uuid);

      if (last_rtsp_session) {
        // Apply deferred config updates only after the session end is queued.
        // This prevents a deferred session_history_enabled=false reload from
        // disabling the writer before it records stream_ended/end_time_unix.
        config::maybe_apply_deferred();
      }
    }

    int start(session_t &session, const std::string &addr_string) {
      session.input = input::alloc(session.mail);

      session.broadcast_ref = broadcast.ref();
      if (!session.broadcast_ref) {
        return -1;
      }

      session.control.expected_peer_address = addr_string;
      session.control.enet_recv_baseline = session.broadcast_ref->control_server._host->totalReceivedPackets;
      BOOST_LOG(debug) << "Expecting incoming session connections from "sv << addr_string;

#ifdef _WIN32
      const auto stream_gpu_model = platf::dxgi::current_display_adapter_name();
      {
        std::lock_guard lg {session.metadata_mutex};
        session.stream_gpu_model = stream_gpu_model;
      }
#endif

      // Insert this session into the session list
      {
        auto lg = session.broadcast_ref->control_server._sessions.lock();
        session.broadcast_ref->control_server._sessions->push_back(&session);
      }

      auto addr = boost::asio::ip::make_address(addr_string);
      session.video.peer.address(addr);
      session.video.peer.port(0);

      session.audio.peer.address(addr);
      session.audio.peer.port(0);

      session.pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      // Initialize mic passthrough
      stream::mic_first_packet_received.store(false, std::memory_order_relaxed);
      const bool mic_encrypted = (session.config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) != 0;

      if (config::audio.mic_sink.empty()) {
        BOOST_LOG(info) << "[mic] mic_sink not configured — passthrough disabled"sv;
      } else if (!mic_encrypted) {
        BOOST_LOG(warning) << "[mic] no encrypted control stream — passthrough disabled"sv;
      } else {
        // Defer full init (vmic backend, capture switch, reapply thread) to the
        // first mic packet — see init_mic_passthrough(). Non-mic clients (e.g.
        // Aurora / moonlight-tv) never send one, so host capture state stays
        // untouched for their sessions.
        session.mic.lazy_init_pending = true;
        BOOST_LOG(info) << "[mic] passthrough armed — init deferred to first mic packet"sv;
      }

      session.audioThread = std::thread {audioThread, &session};
      session.videoThread = std::thread {videoThread, &session};

      session.state.store(state_e::RUNNING, std::memory_order_relaxed);

      // Record session in persistent history
      {
        session_history::session_metadata_t meta;
        meta.protocol = "rtsp";
        {
          std::lock_guard lg {session.metadata_mutex};
          meta.uuid = session.history_uuid;
          session.history_device_name = session.device_name;
          meta.client_name = session.history_device_name;
          meta.device_name = session.history_device_name;
          meta.stream_gpu_model = session.stream_gpu_model;
        }
        meta.app_name = proc::proc.get_last_run_app_name();
        meta.width = session.config.monitor.width;
        meta.height = session.config.monitor.height;
        meta.target_fps = session.stream_fps;
        meta.encoder_bitrate_kbps = session.config.monitor.bitrate;
        meta.requested_bitrate_kbps = session.config.monitor.client_requested_bitrate
                                        ? session.config.monitor.client_requested_bitrate
                                        : session.config.monitor.bitrate;
        meta.codec = std::string(video_format_name(session.config.monitor.videoFormat));
        meta.hdr = session.config.monitor.dynamicRange != 0;
        meta.yuv444 = session.config.monitor.chromaSamplingType != 0;
        meta.audio_channels = session.config.audio.channels;
        meta.server_version = current_server_version();
        session_history::begin_session(meta);
      }

      // If this is the first session, invoke the platform callbacks
      if (++running_sessions == 1) {
        if (!webrtc_stream::has_active_sessions()) {
          webrtc_stream::set_rtsp_capture_config(session.config.monitor, session.config.audio);
        }
        webrtc_stream::set_rtsp_sessions_active(true);
#ifdef _WIN32
        if (!session.config.monitor.input_only) {
          // Apply RTSS frame limit if enabled (Windows-only)
          std::optional<int> lossless_rtss_limit;
          const bool using_lossless_provider = session.config.lossless_scaling_framegen &&
                                               boost::iequals(session.config.frame_generation_provider, "lossless-scaling");
          if (using_lossless_provider) {
            if (session.config.lossless_scaling_rtss_limit && *session.config.lossless_scaling_rtss_limit > 0) {
              lossless_rtss_limit = session.config.lossless_scaling_rtss_limit;
            } else if (session.config.lossless_scaling_target_fps && *session.config.lossless_scaling_target_fps > 0) {
              int computed = (int) std::lround(*session.config.lossless_scaling_target_fps * 0.5);
              if (computed > 0) {
                lossless_rtss_limit = computed;
              }
            }
          }
          // Frame limiter should follow the stream FPS the user/client selected (NVHTTP "mode" fps),
          // not the capture display refresh rate.
          const auto policy = framegen::make_stream_start_policy({
            .fps = session.stream_fps,
            .fps_scaled = session.stream_fps_scaled,
            .frame_generation_enabled = session.config.frame_generation_enabled,
            .gen1_framegen_fix = session.config.gen1_framegen_fix,
            .gen2_framegen_fix = session.config.gen2_framegen_fix,
            .lossless_scaling_framegen = session.config.lossless_scaling_framegen,
            .lossless_rtss_limit = lossless_rtss_limit,
            .frame_generation_provider = session.config.frame_generation_provider,
            .uses_virtual_display = session.virtual_display.active,
            .capture_mode = config::video.capture,
            .auto_capture_uses_wgc = platf::dxgi::should_use_wgc_default(),
            .auto_virtual_framegen_limiter = config::frame_limiter.auto_virtual_framegen,
          });
          const bool defer_stream_start = platf::is_running_as_system() && !user_session_ready();
          if (defer_stream_start) {
            deferred_stream_start_t deferred {.policy = policy};
            defer_stream_start_actions(std::move(deferred));
            BOOST_LOG(info) << "Stream-start actions deferred until user session is ready.";
          } else {
            platf::frame_limiter_streaming_start(policy);
            platf::streaming_will_start();
          }
        } else {
          platf::streaming_will_start();
        }
#else
        platf::streaming_will_start();
#endif
        proc::proc.resume();
      }

      if (!session.do_cmds.empty()) {
        auto exec_thread = std::thread([cmd_list = session.do_cmds] {
          for (auto &cmd : cmd_list) {
            std::error_code ec;
            auto env = proc::proc.get_env();
            boost::filesystem::path working_dir = proc::find_working_directory(cmd.cmd, env);
            auto child = platf::run_command(cmd.elevated, true, cmd.cmd, working_dir, env, nullptr, ec, nullptr);
            BOOST_LOG(info) << "Spawning client do command ["sv << cmd.cmd << "] in ["sv << working_dir << ']';
            if (ec) {
              BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd.cmd << "]: System: "sv << ec.message();
            } else {
              child.detach();
            }
          }
        });

        exec_thread.detach();
      }

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
      update::on_stream_started();
  #if defined(_WIN32)
      // If ViGEm is not installed, notify the user that gamepad input won't work
      try {
        if (!platf::is_vigem_installed(nullptr)) {
          system_tray::update_tray_vigem_missing();
        }
      } catch (...) {
        // best-effort: ignore any unexpected errors while checking
      }
  #endif
#endif

      return 0;
    }

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session) {
      auto session = std::make_shared<session_t>();

      auto mail = std::make_shared<safe::mail_raw_t>();

      session->shutdown_event = mail->event<bool>(mail::shutdown);
      session->launch_session_id = launch_session.id;
      session->device_name = launch_session.device_name;
      session->device_uuid = !launch_session.client_uuid.empty() ? launch_session.client_uuid : launch_session.unique_id;
      // Fresh history identifier per stream so each start/stop cycle produces
      // a distinct row in the session_history database.
      session->history_uuid = uuid_util::uuid_t::generate().string();
      session->permission = launch_session.perm;

      session->do_cmds = std::move(launch_session.client_do_cmds);
      session->undo_cmds = std::move(launch_session.client_undo_cmds);

      session->config = config;
      session->stream_fps = session->config.monitor.framerate;
      session->stream_fps_scaled = session->config.monitor.encodingFramerate;
      if (launch_session.fps > 0) {
        int fps_millihz = launch_session.fps;
        if (fps_millihz > 0 && fps_millihz < 1000) {
          fps_millihz *= 1000;
        }
        session->stream_fps = (int) std::lround((double) fps_millihz / 1000.0);
        session->stream_fps_scaled = fps_millihz;
      }

#ifdef _WIN32
      session->virtual_display.active = launch_session.virtual_display;
      session->virtual_display.guid_bytes = launch_session.virtual_display_guid_bytes;
      if (session->virtual_display.active) {
        VDISPLAY::setWatchdogFeedingEnabled(true);
      }
      session->display_helper_gate = launch_session.display_helper_gate;
#endif

      session->control.connect_data = launch_session.control_connect_data;
      session->control.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      session->control.hdr_queue = mail->event<video::hdr_info_t>(mail::hdr);
      session->control.legacy_input_enc_iv = launch_session.iv;
      session->control.cipher = crypto::cipher::gcm_t {
        launch_session.gcm_key,
        false
      };

      session->video.idr_events = mail->event<bool>(mail::idr);
      session->video.invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
      session->video.bitrate_events = mail->event<int>(mail::dynamic_bitrate);
      session->video.lowseq = 0;
      session->video.ping_payload = launch_session.av_ping_payload;
      if (config.encryptionFlagsEnabled & SS_ENC_VIDEO) {
        BOOST_LOG(info) << "Video encryption enabled"sv;
        session->video.cipher = crypto::cipher::gcm_t {
          launch_session.gcm_key,
          false
        };
        session->video.gcm_iv_counter = 0;
      }

      constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(2048);

      util::buffer_t<char> shards {RTPA_TOTAL_SHARDS * max_block_size};
      util::buffer_t<uint8_t *> shards_p {RTPA_TOTAL_SHARDS};

      for (auto x = 0; x < RTPA_TOTAL_SHARDS; ++x) {
        shards_p[x] = (uint8_t *) &shards[x * max_block_size];
      }

      // Audio FEC spans multiple audio packets,
      // therefore its session specific
      session->audio.shards = std::move(shards);
      session->audio.shards_p = std::move(shards_p);

      session->audio.fec_packet.rtp.header = 0x80;
      session->audio.fec_packet.rtp.packetType = 127;
      session->audio.fec_packet.rtp.timestamp = 0;
      session->audio.fec_packet.rtp.ssrc = 0;

      session->audio.fec_packet.fecHeader.payloadType = 97;
      session->audio.fec_packet.fecHeader.ssrc = 0;

      session->audio.cipher = crypto::cipher::cbc_t {
        launch_session.gcm_key,
        true
      };

      session->audio.ping_payload = launch_session.av_ping_payload;
      session->audio.avRiKeyId = util::endian::big(*(std::uint32_t *) launch_session.iv.data());
      session->audio.sequenceNumber = 0;
      session->audio.timestamp = 0;

      session->control.peer = nullptr;
      session->state.store(state_e::STOPPED, std::memory_order_relaxed);

      session->mail = std::move(mail);

      return session;
    }
  }  // namespace session
}  // namespace stream
