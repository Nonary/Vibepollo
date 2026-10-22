#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace managed_app_focus::hyprland {
  // One IPC request/reply; empty when the socket is unusable or unproven.
  using transport = std::function<std::optional<std::string>(const std::string &)>;

  struct compositor {
    pid_t pid;
    std::uint64_t start;
  };

  inline std::optional<compositor> parse_compositor(std::string_view value) {
    const auto separator = value.find('-');
    if (separator == std::string_view::npos) return std::nullopt;
    compositor parsed {};
    const auto *end = value.data() + value.size();
    const auto pid = std::from_chars(value.data(), value.data() + separator, parsed.pid);
    const auto start = std::from_chars(value.data() + separator + 1, end, parsed.start);
    if (pid.ec != std::errc {} || pid.ptr != value.data() + separator || parsed.pid <= 0 ||
        start.ec != std::errc {} || start.ptr != end || !parsed.start) return std::nullopt;
    return parsed;
  }

  namespace detail {
    // starttime is field 22 of /proc/<pid>/stat, as in the controller probe.
    inline std::optional<std::uint64_t> start_time(pid_t pid) {
      std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
      std::string process(4097, '\0');
      stream.read(process.data(), static_cast<std::streamsize>(process.size()));
      process.resize(static_cast<std::size_t>(stream.gcount()));
      const auto command_end = process.rfind(") ");
      if (process.size() > 4096 || command_end == std::string::npos) return std::nullopt;
      std::istringstream fields(process.substr(command_end + 2));
      std::string field;
      for (int index = 0; index <= 19 && fields >> field; ++index) {}
      std::uint64_t start = 0;
      const auto parsed = std::from_chars(field.data(), field.data() + field.size(), start);
      return parsed.ec == std::errc {} && parsed.ptr == field.data() + field.size() && start ? std::optional(start) : std::nullopt;
    }

    struct descriptor {
      int fd = -1;
      ~descriptor() {
        if (fd >= 0) close(fd);
      }
    };

    // Hyprland does not authenticate IPC clients, so authenticate the server.
    inline bool bound_peer(int fd, const compositor &bound) {
      ucred peer {};
      socklen_t size = sizeof(peer);
      return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) && size == sizeof(peer) &&
             peer.uid == geteuid() && peer.pid == bound.pid && start_time(peer.pid) == bound.start;
    }

    inline std::optional<std::string> exchange(const std::string &path, const compositor &bound, const std::string &request, std::chrono::milliseconds timeout) {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      // Hyprland reads 1023-byte chunks until a short read.
      if (path.size() >= sizeof(address.sun_path) || request.empty() || request.size() >= 1023) return std::nullopt;
      std::copy(path.begin(), path.end(), address.sun_path);
      descriptor socket_fd {socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      const timeval limit {static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>(timeout.count() % 1000 * 1000)};
      if (socket_fd.fd < 0 || setsockopt(socket_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) ||
          connect(socket_fd.fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ||
          !bound_peer(socket_fd.fd, bound) ||
          send(socket_fd.fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) return std::nullopt;
      // The compositor writes one reply and closes the connection.
      constexpr std::size_t reply_limit = 4u << 20;
      std::string reply;
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (char buffer[4096];;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd ready {socket_fd.fd, POLLIN, 0};
        if (remaining.count() <= 0 || poll(&ready, 1, static_cast<int>(remaining.count())) != 1) return std::nullopt;
        const auto size = read(socket_fd.fd, buffer, sizeof(buffer));
        if (size < 0 || reply.size() + static_cast<std::size_t>(size) > reply_limit) return std::nullopt;
        if (!size) return reply;
        reply.append(buffer, static_cast<std::size_t>(size));
      }
    }
  }  // namespace detail

  // Nothing is sent before the peer is the bound compositor; a restarted
  // compositor fails closed until the controller binds the new generation.
  inline transport socket_transport(std::filesystem::path runtime, compositor bound, std::chrono::milliseconds timeout) {
    return [runtime = std::move(runtime), bound, timeout](const std::string &request) -> std::optional<std::string> {
      const auto instances = runtime / "hypr";
      detail::descriptor directory {open(instances.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
      struct stat attributes {};
      if (directory.fd < 0 || fstat(directory.fd, &attributes) || attributes.st_uid != geteuid()) return std::nullopt;
      std::vector<std::string> names;
      if (DIR *entries = fdopendir(dup(directory.fd))) {
        while (const auto *entry = readdir(entries)) {
          if (entry->d_name[0] != '.' && names.size() < 64) names.emplace_back(entry->d_name);
        }
        closedir(entries);
      }
      std::ranges::sort(names);
      for (const auto &name : names) {
        struct stat instance {}, endpoint {};
        const auto socket = name + "/.socket.sock";
        if (fstatat(directory.fd, name.c_str(), &instance, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(instance.st_mode) || instance.st_uid != geteuid() ||
            fstatat(directory.fd, socket.c_str(), &endpoint, AT_SYMLINK_NOFOLLOW) || !S_ISSOCK(endpoint.st_mode) || endpoint.st_uid != geteuid()) continue;
        if (auto reply = detail::exchange((instances / socket).string(), bound, request, timeout)) return reply;
      }
      return std::nullopt;
    };
  }

  namespace detail {
    inline nlohmann::json request_json(const transport &ipc, const std::string &request) {
      const auto reply = ipc(request);
      return reply ? nlohmann::json::parse(*reply, nullptr, false) : nlohmann::json {};
    }

    // A missing or mistyped field excludes the window instead of throwing.
    inline const nlohmann::json *field(const nlohmann::json &window, const char *key) {
      if (!window.is_object()) return nullptr;
      const auto found = window.find(key);
      return found == window.end() ? nullptr : &*found;
    }

    inline bool flag(const nlohmann::json &window, const char *key) {
      const auto *value = field(window, key);
      return value && value->is_boolean() && value->get<bool>();
    }

    inline std::uint64_t visible_pid(const nlohmann::json &window) {
      const auto *hidden = field(window, "hidden");
      const auto *pid = field(window, "pid");
      if (!flag(window, "mapped") || !hidden || !hidden->is_boolean() || hidden->get<bool>()) return 0;
      return pid && pid->is_number_unsigned() ? pid->get<std::uint64_t>() : 0;
    }

    // Only 0x-hex addresses: other text could extend the window selector.
    inline std::string address(const nlohmann::json &window) {
      const auto *value = field(window, "address");
      if (!value || !value->is_string()) return {};
      const auto &text = value->get_ref<const std::string &>();
      const bool valid = text.size() > 2 && text.size() <= 18 && text.starts_with("0x") &&
                         std::all_of(text.begin() + 2, text.end(), [](char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); });
      return valid ? text : std::string {};
    }

    // Hyprland has no window types but floats dialogs: fullscreen, then tiled.
    inline std::pair<bool, bool> rank(const nlohmann::json &window) {
      const auto *fullscreen = field(window, "fullscreen");
      return {fullscreen && fullscreen->is_number_integer() && fullscreen->get<std::int64_t>() > 0, !flag(window, "floating")};
    }
  }  // namespace detail

  namespace detail {
    // Lua configs, now Hyprland's default, parse dispatch as Lua; legacy
    // configs only accept the dispatcher form.
    inline bool focus_window(const transport &ipc, const std::string &address) {
      return ipc("/dispatch hl.dsp.focus({ window = \"address:" + address + "\" })") == "ok" ||
             ipc("/dispatch focuswindow address:" + address) == "ok";
    }
  }  // namespace detail

  // Same result shape as the KWin script; identities are pid:address.
  inline nlohmann::json apply(const transport &ipc, const std::vector<std::uint64_t> &pids, const std::vector<std::string> &activate = {}) {
    if (pids.empty()) return {};
    const auto clients = detail::request_json(ipc, "j/clients");
    const auto active = detail::request_json(ipc, "j/activewindow");
    if (!clients.is_array() || !active.is_object()) return {};
    std::vector<nlohmann::json> windows;
    for (const auto &window : clients) {
      const auto pid = detail::visible_pid(window);
      if (pid && std::ranges::find(pids, pid) != pids.end() && !detail::address(window).empty()) windows.push_back(window);
    }
    std::ranges::stable_sort(windows, std::greater {}, detail::rank);
    nlohmann::json result {{"windows", nlohmann::json::array()}, {"focused", false}, {"foreground_pid", detail::visible_pid(active)}};
    for (const auto &window : windows) {
      const auto address = detail::address(window);
      const auto identity = std::to_string(detail::visible_pid(window)) + ":" + address;
      result["windows"].push_back(identity);
      if (result["focused"].get<bool>() || std::ranges::find(activate, identity) == activate.end()) continue;
      // Only activewindow confirms the activation.
      if (!detail::focus_window(ipc, address)) continue;
      const auto focused = detail::request_json(ipc, "j/activewindow");
      result["focused"] = detail::address(focused) == address;
    }
    return result;
  }
}  // namespace managed_app_focus::hyprland
