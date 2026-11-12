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
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <sstream>
#include <system_error>
#include <tuple>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace managed_app_focus::hyprland {
  // Gates the send of one request; empty sends unconditionally.
  using send_gate = std::function<bool()>;
  // One IPC request/reply; empty when the socket is unusable or unproven, or
  // the gate refused the send.
  using transport = std::function<std::optional<std::string>(const std::string &, const send_gate &)>;

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

    // may_send is asked after connecting and authenticating, immediately
    // before the request leaves: nothing that can block lies in between.
    inline std::optional<std::string> exchange(const std::string &path, const compositor &bound, const std::string &request, const send_gate &may_send, std::chrono::milliseconds timeout) {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      // Hyprland reads 1023-byte chunks until a short read.
      if (path.size() >= sizeof(address.sun_path) || request.empty() || request.size() >= 1023) return std::nullopt;
      std::copy(path.begin(), path.end(), address.sun_path);
      descriptor socket_fd {socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      const timeval limit {static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>(timeout.count() % 1000 * 1000)};
      if (socket_fd.fd < 0 || setsockopt(socket_fd.fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) ||
          connect(socket_fd.fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ||
          !bound_peer(socket_fd.fd, bound) || (may_send && !may_send()) ||
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

  namespace detail {
    // The instance's socket, if it is a socket owned by this user in a
    // directory owned by this user; nothing is followed through a symlink.
    inline std::optional<struct stat> endpoint(int directory, const std::string &name) {
      struct stat instance {}, socket {};
      if (fstatat(directory, name.c_str(), &instance, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(instance.st_mode) || instance.st_uid != geteuid() ||
          fstatat(directory, (name + "/.socket.sock").c_str(), &socket, AT_SYMLINK_NOFOLLOW) || !S_ISSOCK(socket.st_mode) || socket.st_uid != geteuid()) return std::nullopt;
      return socket;
    }
  }  // namespace detail

  // Nothing is sent before the peer is the bound compositor; a restarted
  // compositor fails closed until the controller binds the new generation.
  // The instance that last authenticated is asked first; the directory is
  // scanned again only when it no longer answers.
  inline transport socket_transport(std::filesystem::path runtime, compositor bound, std::chrono::milliseconds timeout) {
    return [runtime = std::move(runtime), bound, timeout, last = std::make_shared<std::string>()](const std::string &request, const send_gate &may_send) -> std::optional<std::string> {
      const auto instances = runtime / "hypr";
      detail::descriptor directory {open(instances.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
      struct stat attributes {};
      if (directory.fd < 0 || fstat(directory.fd, &attributes) || attributes.st_uid != geteuid()) return std::nullopt;
      const std::string cached = std::exchange(*last, std::string {});
      if (!cached.empty() && detail::endpoint(directory.fd, cached)) {
        if (auto reply = detail::exchange((instances / cached / ".socket.sock").string(), bound, request, may_send, timeout)) {
          *last = cached;
          return reply;
        }
      }
      // Hyprland leaves the directories of earlier launches behind. Every
      // usable endpoint is ranked, newest socket first, so stale entries cannot
      // crowd out the live instance. A scan keeps at most 128 candidates in
      // memory and connects to at most 64, each authenticated.
      constexpr std::size_t attempt_limit = 64;
      struct candidate {
        timespec created;
        std::string name;
      };
      const auto newer = [](const candidate &a, const candidate &b) {
        if (a.created.tv_sec != b.created.tv_sec) return a.created.tv_sec > b.created.tv_sec;
        if (a.created.tv_nsec != b.created.tv_nsec) return a.created.tv_nsec > b.created.tv_nsec;
        return a.name < b.name;
      };
      std::vector<candidate> candidates;
      const int listing = dup(directory.fd);
      if (DIR *entries = listing < 0 ? nullptr : fdopendir(listing)) {
        while (const auto *entry = readdir(entries)) {
          // The cached instance already had its one chance at this request.
          if (entry->d_name[0] == '.' || entry->d_name == cached) continue;
          if (const auto socket = detail::endpoint(directory.fd, entry->d_name)) candidates.push_back({socket->st_mtim, entry->d_name});
          if (candidates.size() == 2 * attempt_limit) {
            std::ranges::nth_element(candidates, candidates.begin() + attempt_limit, newer);
            candidates.resize(attempt_limit);
          }
        }
        closedir(entries);
      } else if (listing >= 0) {
        close(listing);
      }
      std::ranges::sort(candidates, newer);
      if (candidates.size() > attempt_limit) candidates.resize(attempt_limit);
      for (const auto &[created, name] : candidates) {
        if (auto reply = detail::exchange((instances / name / ".socket.sock").string(), bound, request, may_send, timeout)) {
          *last = name;
          return reply;
        }
      }
      return std::nullopt;
    };
  }

  namespace detail {
    inline nlohmann::json request_json(const transport &ipc, const std::string &request) {
      const auto reply = ipc(request, {});
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

    // stableId counts up per window and is never reused, unlike the address,
    // which is a heap pointer; 0 means missing or malformed.
    inline std::uint64_t stable_id(const nlohmann::json &window) {
      const auto *value = field(window, "stableId");
      if (!value || !value->is_string()) return 0;
      const auto &text = value->get_ref<const std::string &>();
      std::uint64_t id = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id, 16);
      return error == std::errc {} && end == text.data() + text.size() ? id : 0;
    }

    inline std::string identity(const nlohmann::json &window) {
      const auto pid = visible_pid(window);
      const auto id = stable_id(window);
      if (!pid || !id || address(window).empty()) return {};
      char hex[16];
      const auto end = std::to_chars(hex, hex + sizeof(hex), id, 16).ptr;
      return std::to_string(pid) + ":" + std::string(hex, end);
    }

    // Hyprland has no window types but floats dialogs: fullscreen, then tiled,
    // then the newest window. Mode 1 is maximized, which a launcher may be too.
    // j/clients reorders windows as they gain focus, so its order never decides.
    inline std::tuple<bool, bool, std::uint64_t> rank(const nlohmann::json &window) {
      const auto *fullscreen = field(window, "fullscreen");
      return {fullscreen && fullscreen->is_number_integer() && fullscreen->get<std::int64_t>() >= 2, !flag(window, "floating"), stable_id(window)};
    }
  }  // namespace detail

  namespace detail {
    // Lua configs, now Hyprland's default, parse dispatch as Lua; legacy
    // configs only accept the dispatcher form.
    // allowed() is asked before connecting, so an expired target costs no
    // connection, and again by the transport right before the send.
    inline bool focus_window(const transport &ipc, const std::string &address, const send_gate &allowed) {
      return (allowed() && ipc("/dispatch hl.dsp.focus({ window = \"address:" + address + "\" })", allowed) == "ok") ||
             (allowed() && ipc("/dispatch focuswindow address:" + address, allowed) == "ok");
    }
  }  // namespace detail

  // Same result shape as the KWin script; identities are pid:stableId. Every
  // request may block, so an identity is activated only while allowed() says so.
  inline nlohmann::json apply(const transport &ipc, const std::vector<std::uint64_t> &pids, const std::vector<std::string> &activate = {}, const std::function<bool(const std::string &)> &allowed = {}) {
    if (pids.empty()) return {};
    const auto clients = detail::request_json(ipc, "j/clients");
    const auto active = detail::request_json(ipc, "j/activewindow");
    if (!clients.is_array() || !active.is_object()) return {};
    std::vector<nlohmann::json> windows;
    for (const auto &window : clients) {
      const auto pid = detail::visible_pid(window);
      if (pid && std::ranges::find(pids, pid) != pids.end() && !detail::identity(window).empty()) windows.push_back(window);
    }
    std::ranges::stable_sort(windows, std::greater {}, detail::rank);
    nlohmann::json result {{"windows", nlohmann::json::array()}, {"focused", false}, {"foreground_pid", detail::visible_pid(active)}, {"foreground", detail::identity(active)}, {"foreground_fullscreen", !detail::identity(active).empty() && std::get<0>(detail::rank(active))}};
    for (const auto &window : windows) {
      const auto address = detail::address(window);
      const auto identity = detail::identity(window);
      result["windows"].push_back(identity);
      if (result["focused"].get<bool>() || std::ranges::find(activate, identity) == activate.end()) continue;
      // Focus came back during this observation: nothing to activate.
      if (identity == detail::identity(active)) continue;
      // Only activewindow confirms the activation.
      if (!allowed || !detail::focus_window(ipc, address, [&] { return allowed(identity); })) continue;
      const auto focused = detail::request_json(ipc, "j/activewindow");
      result["focused"] = detail::identity(focused) == identity;
    }
    return result;
  }
}  // namespace managed_app_focus::hyprland
