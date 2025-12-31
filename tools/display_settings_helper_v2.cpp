/**
 * @file tools/display_settings_helper_v2.cpp
 * @brief Display helper v2 implementation.
 */
#ifdef _WIN32

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdint>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <span>
  #include <string>
  #include <thread>
  #include <utility>
  #include <vector>

  #include "src/logging.h"
  #include "src/platform/windows/ipc/pipes.h"
  #include "src/platform/windows/display_helper_v2/async_dispatcher.h"
  #include "src/platform/windows/display_helper_v2/operations.h"
  #include "src/platform/windows/display_helper_v2/runtime_support.h"
  #include "src/platform/windows/display_helper_v2/snapshot.h"
  #include "src/platform/windows/display_helper_v2/state_machine.h"
  #include "src/platform/windows/display_helper_v2/win_display_settings.h"
  #include "src/platform/windows/display_helper_v2/win_event_pump.h"
  #include "src/platform/windows/display_helper_v2/win_platform_workarounds.h"
  #include "src/platform/windows/display_helper_v2/win_scheduled_task_manager.h"
  #include "src/platform/windows/display_helper_v2/win_virtual_display_driver.h"

  #include <display_device/json.h>
  #include <nlohmann/json.hpp>

  #include <shlobj.h>
  #include <windows.h>

namespace {
  enum class MsgType : uint8_t {
    Apply = 1,
    Revert = 2,
    Reset = 3,
    ExportGolden = 4,
    ApplyResult = 6,
    Disarm = 7,
    SnapshotCurrent = 8,
    VerificationResult = 9,
    Ping = 0xFE,
    Stop = 0xFF,
  };

  HANDLE make_named_mutex(const wchar_t *name) {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreateMutexW(&sa, FALSE, name);
  }

  bool ensure_single_instance(HANDLE &out_handle) {
    out_handle = make_named_mutex(L"Global\\SunshineDisplayHelper");
    if (!out_handle && GetLastError() == ERROR_ACCESS_DENIED) {
      out_handle = make_named_mutex(L"Local\\SunshineDisplayHelper");
    }
    if (!out_handle) {
      return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return false;
    }
    return true;
  }

  std::filesystem::path compute_log_dir() {
    std::wstring appdata;
    appdata.resize(MAX_PATH);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata.data()))) {
      appdata.resize(wcslen(appdata.c_str()));
      auto path = std::filesystem::path(appdata) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }

    std::wstring env_appdata;
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed > 0) {
      env_appdata.resize(needed);
      DWORD written = GetEnvironmentVariableW(L"APPDATA", env_appdata.data(), needed);
      if (written > 0) {
        env_appdata.resize(written);
        auto path = std::filesystem::path(env_appdata) / L"Sunshine";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
      }
    }

    std::wstring temp;
    temp.resize(MAX_PATH);
    DWORD tlen = GetTempPathW(MAX_PATH, temp.data());
    if (tlen > 0 && tlen < MAX_PATH) {
      temp.resize(tlen);
      auto path = std::filesystem::path(temp) / L"Sunshine";
      std::error_code ec;
      std::filesystem::create_directories(path, ec);
      return path;
    }

    auto fallback = std::filesystem::path(L".") / L"Sunshine";
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return fallback;
  }

  void hide_console_window() {
    if (HWND console = GetConsoleWindow()) {
      ShowWindow(console, SW_HIDE);
    }
  }

  void send_framed_content(platf::dxgi::AsyncNamedPipe &pipe, MsgType type, std::span<const uint8_t> payload = {}) {
    std::vector<uint8_t> out;
    out.reserve(1 + payload.size());
    out.push_back(static_cast<uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    pipe.send(out);
  }

  std::vector<std::string> parse_snapshot_exclude_json_node(const nlohmann::json &node) {
    std::vector<std::string> ids;
    const nlohmann::json *arr = &node;
    nlohmann::json nested;
    if (node.is_object()) {
      if (node.contains("exclude_devices")) {
        nested = node["exclude_devices"];
        arr = &nested;
      } else if (node.contains("devices")) {
        nested = node["devices"];
        arr = &nested;
      }
    }

    if (!arr->is_array()) {
      return ids;
    }

    for (const auto &el : *arr) {
      if (el.is_string()) {
        ids.push_back(el.get<std::string>());
      } else if (el.is_object()) {
        if (el.contains("device_id") && el["device_id"].is_string()) {
          ids.push_back(el["device_id"].get<std::string>());
        } else if (el.contains("id") && el["id"].is_string()) {
          ids.push_back(el["id"].get<std::string>());
        }
      }
    }

    return ids;
  }

  std::optional<std::vector<std::string>> parse_snapshot_exclude_payload(std::span<const uint8_t> payload) {
    if (payload.empty()) {
      return std::nullopt;
    }

    try {
      std::string raw(reinterpret_cast<const char *>(payload.data()), payload.size());
      if (raw.empty()) {
        return std::vector<std::string> {};
      }
      auto j = nlohmann::json::parse(raw, nullptr, false);
      if (j.is_discarded()) {
        return std::nullopt;
      }
      return parse_snapshot_exclude_json_node(j);
    } catch (...) {
      return std::nullopt;
    }
  }

  bool load_vibeshine_snapshot_exclusions(const std::filesystem::path &path, std::vector<std::string> &ids_out) {
    ids_out.clear();
    if (path.empty()) {
      return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
      return false;
    }

    try {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return false;
      }
      auto j = nlohmann::json::parse(file, nullptr, false);
      if (!j.is_object()) {
        return false;
      }
      if (j.contains("root") && j["root"].is_object()) {
        const auto &root = j["root"];
        if (root.contains("snapshot_exclude_devices")) {
          ids_out = parse_snapshot_exclude_json_node(root["snapshot_exclude_devices"]);
          return !ids_out.empty() || root["snapshot_exclude_devices"].is_array();
        }
      }
    } catch (...) {
      return false;
    }

    return false;
  }

  bool migrate_legacy_snapshot(const std::filesystem::path &legacy_path, const std::filesystem::path &current_path) {
    std::error_code ec_legacy;
    if (!std::filesystem::exists(legacy_path, ec_legacy) || ec_legacy) {
      return false;
    }

    std::error_code ec_dir;
    std::filesystem::create_directories(current_path.parent_path(), ec_dir);

    std::error_code ec_copy;
    std::filesystem::copy_file(legacy_path, current_path, std::filesystem::copy_options::overwrite_existing, ec_copy);
    if (ec_copy) {
      return false;
    }

    std::error_code ec_rm;
    std::filesystem::remove(legacy_path, ec_rm);
    return true;
  }

  bool parse_apply_payload(
    std::span<const uint8_t> payload,
    display_helper::v2::ApplyRequest &out_request,
    std::optional<std::vector<std::string>> &snapshot_exclusions,
    std::string &error) {
    std::string json(reinterpret_cast<const char *>(payload.data()), payload.size());
    std::string sanitized_json = json;

    try {
      auto j = nlohmann::json::parse(json, nullptr, false);
      if (j.is_object()) {
        if (j.contains("wa_hdr_toggle")) {
          out_request.hdr_blank = j["wa_hdr_toggle"].get<bool>();
          j.erase("wa_hdr_toggle");
        }
        if (j.contains("sunshine_virtual_layout") && j["sunshine_virtual_layout"].is_string()) {
          out_request.virtual_layout = j["sunshine_virtual_layout"].get<std::string>();
          j.erase("sunshine_virtual_layout");
        }
        if (j.contains("sunshine_monitor_positions") && j["sunshine_monitor_positions"].is_object()) {
          for (auto it = j["sunshine_monitor_positions"].begin(); it != j["sunshine_monitor_positions"].end(); ++it) {
            if (!it.value().is_object()) {
              continue;
            }
            const auto &node = it.value();
            if (!node.contains("x") || !node.contains("y")) {
              continue;
            }
            if (!node["x"].is_number_integer() || !node["y"].is_number_integer()) {
              continue;
            }
            out_request.monitor_positions.emplace_back(
              it.key(),
              display_device::Point {node["x"].get<int>(), node["y"].get<int>()}
            );
          }
          j.erase("sunshine_monitor_positions");
        }
        if (j.contains("sunshine_snapshot_exclude_devices")) {
          snapshot_exclusions = parse_snapshot_exclude_json_node(j["sunshine_snapshot_exclude_devices"]);
          j.erase("sunshine_snapshot_exclude_devices");
        }
        if (j.contains("sunshine_topology") && j["sunshine_topology"].is_array()) {
          display_device::ActiveTopology topo;
          for (const auto &grp_node : j["sunshine_topology"]) {
            if (!grp_node.is_array()) {
              continue;
            }
            std::vector<std::string> group;
            for (const auto &id_node : grp_node) {
              if (id_node.is_string()) {
                group.push_back(id_node.get<std::string>());
              }
            }
            if (!group.empty()) {
              topo.push_back(std::move(group));
            }
          }
          if (!topo.empty()) {
            out_request.topology = std::move(topo);
          }
          j.erase("sunshine_topology");
        }
        if (j.contains("sunshine_always_restore_from_golden") && j["sunshine_always_restore_from_golden"].is_boolean()) {
          out_request.prefer_golden_first = j["sunshine_always_restore_from_golden"].get<bool>();
          j.erase("sunshine_always_restore_from_golden");
        }
        sanitized_json = j.dump();
      }
    } catch (...) {
    }

    display_device::SingleDisplayConfiguration cfg {};
    std::string parse_error;
    if (!display_device::fromJson(sanitized_json, cfg, &parse_error)) {
      error = parse_error;
      return false;
    }

    out_request.configuration = std::move(cfg);
    return true;
  }

  bool parse_frame(
    std::span<const uint8_t> frame,
    MsgType &type,
    std::span<const uint8_t> &payload) {
    if (frame.empty()) {
      return false;
    }

    if (frame.size() >= 5) {
      uint32_t len = 0;
      std::memcpy(&len, frame.data(), sizeof(len));
      if (len > 0 && frame.size() >= 4u + len) {
        type = static_cast<MsgType>(frame[4]);
        if (len > 1) {
          payload = std::span<const uint8_t>(frame.data() + 5, len - 1);
        } else {
          payload = {};
        }
        return true;
      }
    }

    type = static_cast<MsgType>(frame[0]);
    payload = frame.subspan(1);
    return true;
  }
}  // namespace

int main(int argc, char *argv[]) {
  bool restore_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--restore") == 0) {
      restore_mode = true;
    } else if (std::strcmp(argv[i], "--no-startup-restore") == 0) {
      BOOST_LOG(info) << "--no-startup-restore is deprecated and ignored.";
    }
  }

  if (restore_mode) {
    FreeConsole();
    hide_console_window();
  }

  // Initialize logging early so we can log singleton conflicts and other early exits
  const auto log_dir = compute_log_dir();
  const auto log_file = log_dir / L"sunshine_display_helper.log";
  auto log_guard = logging::init(2, log_file);

  BOOST_LOG(info) << "Display helper v2 starting up...";

  HANDLE singleton = nullptr;
  if (!ensure_single_instance(singleton)) {
    BOOST_LOG(warning) << "Display helper: another instance is already running (singleton conflict). Exiting with code 3.";
    logging::log_flush();
    return 3;
  }

  const auto golden_path = log_dir / L"display_golden_restore.json";
  const auto current_path = log_dir / L"display_session_current.json";
  const auto previous_path = log_dir / L"display_session_previous.json";
  const auto legacy_path = log_dir / L"display_session_restore.json";
  const auto vibeshine_state = log_dir / L"vibeshine_state.json";

  if (!std::filesystem::exists(current_path)) {
    (void) migrate_legacy_snapshot(legacy_path, current_path);
  }

  display_helper::v2::SystemClock clock;
  display_helper::v2::WinDisplaySettings display_settings;
  display_helper::v2::SnapshotService snapshot_service(display_settings);

  display_helper::v2::SnapshotPaths paths {
    .current = current_path,
    .previous = previous_path,
    .golden = golden_path,
  };
  display_helper::v2::FileSnapshotStorage storage(paths);
  display_helper::v2::SnapshotPersistence persistence(storage);
  display_helper::v2::ApplyPolicy apply_policy(clock);
  display_helper::v2::WinVirtualDisplayDriver virtual_display;
  display_helper::v2::WinPlatformWorkarounds workarounds;
  display_helper::v2::WinScheduledTaskManager task_manager;
  display_helper::v2::HeartbeatMonitor heartbeat(clock);
  display_helper::v2::CancellationSource cancellation;
  display_helper::v2::SystemPorts system_ports(workarounds, task_manager, heartbeat, clock, cancellation);
  display_helper::v2::ApplyOperation apply_operation(display_settings);
  display_helper::v2::VerificationOperation verification_operation(display_settings, clock);
  display_helper::v2::RecoveryOperation recovery_operation(display_settings, snapshot_service, persistence, apply_policy, clock);
  display_helper::v2::RecoveryValidationOperation recovery_validation(snapshot_service, clock);
  display_helper::v2::AsyncDispatcher dispatcher(
    apply_operation,
    verification_operation,
    recovery_operation,
    recovery_validation,
    virtual_display,
    clock
  );

  display_helper::v2::MessageQueue<display_helper::v2::Message> queue;
  std::atomic<bool> running {true};

  std::set<std::string> initial_blacklist;
  {
    std::vector<std::string> exclusions;
    if (load_vibeshine_snapshot_exclusions(vibeshine_state, exclusions)) {
      for (auto &id : exclusions) {
        if (!id.empty()) {
          initial_blacklist.insert(std::move(id));
        }
      }
    }
  }

  auto enqueue_message = [&](display_helper::v2::Message message) {
    queue.push(std::move(message));
  };
  display_helper::v2::ApplyPipeline apply_pipeline(dispatcher, apply_policy, system_ports, enqueue_message);
  display_helper::v2::RecoveryPipeline recovery_pipeline(dispatcher, system_ports, enqueue_message);
  display_helper::v2::SnapshotLedger snapshot_ledger(snapshot_service, persistence);

  display_helper::v2::StateMachine state_machine(
    apply_pipeline,
    recovery_pipeline,
    snapshot_ledger,
    system_ports,
    virtual_display);

  state_machine.set_snapshot_blacklist(std::move(initial_blacklist));

  int exit_code = 0;
  state_machine.set_exit_callback([&](int code) {
    exit_code = code;
    running.store(false, std::memory_order_release);
  });

  std::atomic<platf::dxgi::AsyncNamedPipe *> active_pipe {nullptr};
  state_machine.set_apply_result_callback([&](display_helper::v2::ApplyStatus status) {
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    std::vector<uint8_t> payload;
    payload.push_back(status == display_helper::v2::ApplyStatus::Ok ? 1u : 0u);
    send_framed_content(*pipe, MsgType::ApplyResult, payload);
  });
  state_machine.set_verification_result_callback([&](bool success) {
    auto *pipe = active_pipe.load(std::memory_order_acquire);
    if (!pipe) {
      return;
    }
    std::vector<uint8_t> payload;
    payload.push_back(success ? 1u : 0u);
    send_framed_content(*pipe, MsgType::VerificationResult, payload);
  });

  display_helper::v2::DebouncedTrigger debouncer(std::chrono::milliseconds(500));
  std::mutex debounce_mutex;
  display_helper::v2::WinEventPump event_pump;
  event_pump.start([&](display_helper::v2::DisplayEvent) {
    std::lock_guard<std::mutex> lock(debounce_mutex);
    debouncer.notify(clock.now());
  });

  auto process_queue = [&]() {
    auto message = queue.wait_for(std::chrono::milliseconds(100));
    if (message) {
      state_machine.handle_message(*message);
      return;
    }

    if (heartbeat.check_timeout()) {
      queue.push(display_helper::v2::HelperEventMessage {
        display_helper::v2::HelperEvent::HeartbeatTimeout,
        cancellation.current_generation()
      });
    }

    bool fire = false;
    {
      std::lock_guard<std::mutex> lock(debounce_mutex);
      fire = debouncer.should_fire(clock.now());
    }
    if (fire) {
      queue.push(display_helper::v2::DisplayEventMessage {
        display_helper::v2::DisplayEvent::DisplayChange,
        cancellation.current_generation()
      });
    }
  };

  if (restore_mode) {
    BOOST_LOG(info) << "Display helper v2 running in restore mode.";
    queue.push(display_helper::v2::RevertCommand {cancellation.current_generation()});
    while (running.load(std::memory_order_acquire)) {
      process_queue();
    }
    BOOST_LOG(info) << "Display helper v2 restore mode completed with exit code " << exit_code << ".";
    logging::log_flush();
    return exit_code;
  }

  while (running.load(std::memory_order_acquire)) {
    platf::dxgi::FramedPipeFactory pipe_factory(std::make_unique<platf::dxgi::AnonymousPipeFactory>());
    auto server_pipe = pipe_factory.create_server("sunshine_display_helper");
    if (!server_pipe) {
      platf::dxgi::FramedPipeFactory fallback_factory(std::make_unique<platf::dxgi::NamedPipeFactory>());
      server_pipe = fallback_factory.create_server("sunshine_display_helper");
      if (!server_pipe) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
    }

    platf::dxgi::AsyncNamedPipe async_pipe(std::move(server_pipe));
    active_pipe.store(&async_pipe, std::memory_order_release);
    display_helper::v2::ReconnectController reconnect_controller(clock, std::chrono::seconds(30));

    auto on_message = [&](std::span<const uint8_t> bytes) {
      MsgType type {};
      std::span<const uint8_t> payload;
      if (!parse_frame(bytes, type, payload)) {
        return;
      }

      switch (type) {
        case MsgType::Apply: {
          display_helper::v2::ApplyRequest request;
          std::optional<std::vector<std::string>> snapshot_exclusions;
          std::string error;
          if (!parse_apply_payload(payload, request, snapshot_exclusions, error)) {
            std::vector<uint8_t> result_payload;
            result_payload.push_back(0u);
            if (!error.empty()) {
              result_payload.insert(result_payload.end(), error.begin(), error.end());
            }
            send_framed_content(async_pipe, MsgType::ApplyResult, result_payload);
            return;
          }
          if (snapshot_exclusions.has_value()) {
            std::set<std::string> blacklist;
            for (auto &id : *snapshot_exclusions) {
              if (!id.empty()) {
                blacklist.insert(std::move(id));
              }
            }
            state_machine.set_snapshot_blacklist(std::move(blacklist));
          }

          queue.push(display_helper::v2::ApplyCommand {request, cancellation.current_generation()});
          break;
        }
        case MsgType::Revert:
          queue.push(display_helper::v2::RevertCommand {cancellation.current_generation()});
          break;
        case MsgType::Disarm:
          queue.push(display_helper::v2::DisarmCommand {cancellation.current_generation()});
          break;
        case MsgType::ExportGolden: {
          display_helper::v2::SnapshotCommandPayload payload_struct;
          if (auto parsed = parse_snapshot_exclude_payload(payload)) {
            payload_struct.exclude_devices = std::move(*parsed);
          }
          queue.push(display_helper::v2::ExportGoldenCommand {payload_struct, cancellation.current_generation()});
          break;
        }
        case MsgType::SnapshotCurrent: {
          display_helper::v2::SnapshotCommandPayload payload_struct;
          if (auto parsed = parse_snapshot_exclude_payload(payload)) {
            payload_struct.exclude_devices = std::move(*parsed);
          }
          queue.push(display_helper::v2::SnapshotCurrentCommand {payload_struct, cancellation.current_generation()});
          break;
        }
        case MsgType::Reset:
          queue.push(display_helper::v2::ResetCommand {cancellation.current_generation()});
          break;
        case MsgType::Ping:
          send_framed_content(async_pipe, MsgType::Ping);
          queue.push(display_helper::v2::PingCommand {cancellation.current_generation()});
          break;
        case MsgType::Stop:
          queue.push(display_helper::v2::StopCommand {cancellation.current_generation()});
          break;
        default:
          break;
      }
    };

    auto on_error = [&](const std::string &) {
      active_pipe.store(nullptr, std::memory_order_release);
      reconnect_controller.on_error();
    };

    auto on_broken = [&]() {
      active_pipe.store(nullptr, std::memory_order_release);
      reconnect_controller.on_broken();
    };

    async_pipe.start(on_message, on_error, on_broken);

    while (running.load(std::memory_order_acquire)) {
      process_queue();

      const bool connected = async_pipe.is_connected();
      const bool should_revert = reconnect_controller.update_connection(connected);
      if (!connected && should_revert) {
        queue.push(display_helper::v2::RevertCommand {cancellation.current_generation()});
      }

      if (!connected && reconnect_controller.should_restart_pipe()) {
        break;
      }
    }

    active_pipe.store(nullptr, std::memory_order_release);
    async_pipe.stop();
  }

  BOOST_LOG(info) << "Display helper v2 shutting down with exit code " << exit_code << ".";
  logging::log_flush();
  return exit_code;
}

#else
int main() {
  return 0;
}
#endif
