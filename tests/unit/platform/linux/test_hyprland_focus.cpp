#include "../../../../packaging/linux/managed_app_focus_hyprland.h"
#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {
  // Names the failed fixture syscall; the sandbox may forbid any of them.
  void require(bool ok, const char *operation) {
    if (!ok) {
      std::cerr << "fake_socket: " << operation << ": " << std::strerror(errno) << '\n';
      std::abort();
    }
  }

  // A one-reply-per-connection server like Hyprland's .socket.sock. It runs in
  // this process, so its peer credentials are this test's PID and start time.
  class fake_socket {
  public:
    explicit fake_socket(const std::filesystem::path &path, std::string reply):
        fake_socket(path, [reply = std::move(reply)](const std::string &) { return reply; }) {}

    // Answers each request through reply, e.g. to refuse one dispatch form.
    explicit fake_socket(const std::filesystem::path &path, std::function<std::string(const std::string &)> reply):
        reply_(std::move(reply)) {
      fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      require(fd_ >= 0, "socket");
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
      require(!bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "bind");
      require(!listen(fd_, 4), "listen");
      worker_ = std::thread([this] {
        while (const int connection = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC)) {
          if (connection < 0) return;
          char buffer[1024];
          const auto size = read(connection, buffer, sizeof(buffer));
          if (size > 0) {
            requests.emplace_back(buffer, static_cast<std::size_t>(size));
            const auto answer = reply_(requests.back());
            // A client that timed out has closed its end already.
            const auto sent = send(connection, answer.data(), answer.size(), MSG_NOSIGNAL);
            assert(sent == static_cast<ssize_t>(answer.size()) || errno == EPIPE || errno == ECONNRESET);
          }
          ++connections;
          close(connection);
        }
      });
    }

    ~fake_socket() {
      shutdown(fd_, SHUT_RDWR);
      close(fd_);
      worker_.join();
    }

    std::vector<std::string> requests;
    std::atomic<int> connections = 0;

  private:
    int fd_ = -1;
    std::function<std::string(const std::string &)> reply_;
    std::thread worker_;
  };

  std::uint64_t own_start_time() {
    std::ifstream stat("/proc/self/stat");
    std::string text((std::istreambuf_iterator<char>(stat)), {});
    std::istringstream fields(text.substr(text.rfind(") ") + 2));
    std::string field;
    for (int index = 0; index <= 19; ++index) fields >> field;
    return std::stoull(field);
  }

  // Requests are recorded before connections counts them, so a successful wait
  // also makes them safe to read.
  bool wait_for(const std::atomic<int> &value, int expected) {
    for (int attempt = 0; attempt < 1000 && value < expected; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return value >= expected;
  }

  // Shapes follow Hyprland 0.56 `j/clients` and `j/activewindow`. Titles are
  // omitted: selection must never depend on them.
  const char *clients = R"([
    {"address":"0x5a01","stableId":"5a01","mapped":true,"hidden":false,"pid":700,"xwayland":true,"fullscreen":1,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a02","stableId":"5a02","mapped":true,"hidden":false,"pid":701,"xwayland":true,"fullscreen":2,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a03","stableId":"5a03","mapped":true,"hidden":false,"pid":900,"xwayland":false,"fullscreen":0,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a04","stableId":"5a04","mapped":false,"hidden":false,"pid":700,"xwayland":true,"fullscreen":0,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a05","stableId":"5a05","mapped":true,"hidden":true,"pid":701,"xwayland":true,"fullscreen":0,"workspace":{"id":1,"name":"1"}}
  ])";

  struct fake_hyprland {
    std::string listed = clients;
    std::string active = R"({"address":"0x5a03","stableId":"5a03","mapped":true,"hidden":false,"pid":900})";
    bool steal_back = false;  // Another client takes focus again immediately.
    bool lua_config = false;  // Hyprland 0.56 Lua configs parse dispatch as Lua.
    std::vector<std::string> requests;
    std::function<void(const std::string &)> on_request;  // Simulates a slow reply.

    managed_app_focus::hyprland::transport transport() {
      return [this](const std::string &request, const managed_app_focus::hyprland::send_gate &may_send) -> std::optional<std::string> {
        if (may_send && !may_send()) return std::nullopt;
        requests.push_back(request);
        if (on_request) on_request(request);
        if (request == "j/clients") return listed;
        if (request == "j/activewindow") return active;
        const std::string dispatch = "/dispatch focuswindow address:";
        const std::string lua = "/dispatch hl.dsp.focus({ window = \"address:";
        std::string address;
        if (request.starts_with(dispatch)) {
          if (lua_config) return R"lua(error: [string "return hl.dispatch(focuswindow address:0x5a02)"]:1: ')' expected near 'address')lua";
          address = request.substr(dispatch.size());
        } else if (request.starts_with(lua) && request.ends_with("\" })")) {
          if (!lua_config) return "Invalid dispatcher";
          address = request.substr(lua.size(), request.size() - lua.size() - 4);
        }
        if (!address.empty()) {
          // The focused window is the listed one, as Hyprland reports it.
          for (const auto &window : nlohmann::json::parse(listed)) {
            if (!steal_back && window.value("address", std::string {}) == address) active = window.dump();
          }
          return "ok";
        }
        return std::nullopt;
      };
    }
  };
}  // namespace

int main() {
  using managed_app_focus::hyprland::apply;
  const auto always = [](const std::string &) { return true; };
  const auto dispatched = [](const fake_hyprland &hyprland) {
    return std::ranges::count_if(hyprland.requests, [](const auto &request) { return request.starts_with("/dispatch"); });
  };
  using managed_app_focus::focus_step;
  {
    // Observation alone lists the fullscreen game before a maximized launcher
    // and changes nothing.
    fake_hyprland hyprland;
    const auto observed = apply(hyprland.transport(), {700, 701});
    assert(observed.value("focused", true) == false);
    assert(observed.value("foreground_pid", 0) == 900);
    assert(observed.value("foreground", std::string {}) == "900:5a03");
    assert(observed["foreground_fullscreen"] == false);
    assert((observed["windows"] == nlohmann::json {"701:5a02", "700:5a01"}));
    assert((hyprland.requests == std::vector<std::string> {"j/clients", "j/activewindow"}));
  }
  {
    // Only an allowed identity is activated, and Hyprland must confirm it.
    fake_hyprland hyprland;
    const auto applied = apply(hyprland.transport(), {700, 701}, {"701:5a02", "900:5a03"}, always);
    assert(applied.value("focused", false));
    assert(hyprland.requests.back() == "j/activewindow");
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0x5a02"}) == 1);
    assert(std::ranges::count_if(hyprland.requests, [](const auto &request) { return request.find("0x5a03") != std::string::npos; }) == 0);
  }
  {
    // A launcher of the game's own process in front of the fullscreen game
    // is lost focus, not the game.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0xaaaa","stableId":"aaaa","mapped":true,"hidden":false,"pid":700,"fullscreen":2},
                          {"address":"0xbbbb","stableId":"bbbb","mapped":true,"hidden":false,"pid":700,"fullscreen":0}])";
    hyprland.active = R"({"address":"0xbbbb","stableId":"bbbb","mapped":true,"hidden":false,"pid":700})";
    managed_app_focus::budget focus({1, 15, false}, managed_app_focus::budget::clock::now());
    const auto ipc = hyprland.transport();
    assert(focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {700}, activate, allowed); }, focus, managed_app_focus::budget::clock::now));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0xaaaa"}) == 1);
  }
  {
    // The exact game window already active gets no dispatch and spends nothing.
    fake_hyprland hyprland;
    hyprland.active = R"({"address":"0x5a02","stableId":"5a02","mapped":true,"hidden":false,"pid":701,"fullscreen":2})";
    assert(apply(hyprland.transport(), {701})["foreground_fullscreen"] == true);
    hyprland.active = R"({"address":"0x5a02","mapped":true,"hidden":false,"pid":701,"fullscreen":2})";
    assert(apply(hyprland.transport(), {701})["foreground_fullscreen"] == false);  // Not an identifiable window.
    hyprland.active = R"({"address":"0x5a02","stableId":"5a02","mapped":true,"hidden":false,"pid":701})";
    const auto now = managed_app_focus::budget::clock::now();
    managed_app_focus::budget focus({1, 15, false}, now);
    const auto ipc = hyprland.transport();
    assert(!focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {700, 701}, activate, allowed); }, focus, [now] { return now; }));
    assert(std::ranges::none_of(hyprland.requests, [](const auto &request) { return request.starts_with("/dispatch"); }));
    assert(focus.can_focus(now));
  }
  {
    // Hyprland moves the focused window to the end of j/clients. Of two
    // windows of equal rank the newer one is preferred whatever the list
    // order, so focus never flips between them.
    fake_hyprland hyprland;
    const std::string launcher = R"({"address":"0x10","stableId":"10","mapped":true,"hidden":false,"pid":701})";
    const std::string game = R"({"address":"0x20","stableId":"20","mapped":true,"hidden":false,"pid":701})";
    hyprland.on_request = [&](const std::string &request) {
      if (request != "j/clients") return;
      const bool game_focused = hyprland.active.find("0x20") != std::string::npos;
      hyprland.listed = "[" + (game_focused ? launcher + "," + game : game + "," + launcher) + "]";
    };
    const auto ipc = hyprland.transport();
    const auto now = managed_app_focus::budget::clock::now();
    managed_app_focus::budget focus({3, 15, false}, now);
    const auto step = [&] {
      return focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {701}, activate, allowed); }, focus, [now] { return now; });
    };
    hyprland.active = game;
    for (int poll = 0; poll < 4; ++poll) assert(!step());
    assert(dispatched(hyprland) == 0);
    hyprland.active = launcher;
    assert(step());
    for (int poll = 0; poll < 4; ++poll) assert(!step());
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0x20"}) == 1);
    assert(std::ranges::none_of(hyprland.requests, [](const auto &request) { return request.find("0x10") != std::string::npos; }));
    assert(hyprland.active.find("0x20") != std::string::npos);
    assert((apply(ipc, {701})["windows"] == nlohmann::json {"701:20", "701:10"}));
  }
  {
    // The identity is the stableId, never reused, not the address, which is
    // a heap pointer a later window may get again. Windows without a valid
    // stableId are skipped.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0x10","stableId":"2A","mapped":true,"hidden":false,"pid":701},
                          {"address":"0x11","stableId":"0x2b","mapped":true,"hidden":false,"pid":701},
                          {"address":"0x12","stableId":"","mapped":true,"hidden":false,"pid":701},
                          {"address":"0x13","stableId":"10000000000000000","mapped":true,"hidden":false,"pid":701},
                          {"address":"0x14","stableId":44,"mapped":true,"hidden":false,"pid":701},
                          {"address":"0x15","stableId":"0","mapped":true,"hidden":false,"pid":701},
                          {"address":"0x16","mapped":true,"hidden":false,"pid":701}])";
    assert((apply(hyprland.transport(), {701})["windows"] == nlohmann::json {"701:2a"}));
    hyprland.listed = R"([{"address":"0x10","stableId":"2c","mapped":true,"hidden":false,"pid":701}])";
    assert((apply(hyprland.transport(), {701})["windows"] == nlohmann::json {"701:2c"}));
    // A window that got the address of the dispatched one after it closed
    // does not confirm the focus.
    const auto ipc = hyprland.transport();
    const auto applied = apply([&](const std::string &request, const auto &may_send) {
      auto reply = ipc(request, may_send);
      if (request.starts_with("/dispatch")) hyprland.active = R"({"address":"0x10","stableId":"2d","mapped":true,"hidden":false,"pid":701})";
      return reply;
    }, {701}, {"701:2c"}, always);
    assert(dispatched(hyprland) >= 1 && applied["focused"] == false);
  }
  {
    // A Lua config refuses dispatcher syntax; the same validated address is
    // then sent as the Lua focus expression.
    fake_hyprland hyprland;
    hyprland.lua_config = true;
    const auto applied = apply(hyprland.transport(), {701}, {"701:5a02"}, always);
    assert(applied.value("focused", false));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch hl.dsp.focus({ window = \"address:0x5a02\" })"}) == 1);
  }
  {
    // An accepted dispatch without the window becoming active is not success.
    fake_hyprland hyprland;
    hyprland.steal_back = true;
    const auto applied = apply(hyprland.transport(), {701}, {"701:5a02"}, always);
    assert(!applied.value("focused", true));
    assert(applied["windows"] == nlohmann::json {"701:5a02"});
  }
  {
    // A refused game focus does not fall through to its launcher: the step
    // activates only the preferred window and spends nothing.
    fake_hyprland hyprland;
    hyprland.steal_back = true;
    const auto now = managed_app_focus::budget::clock::now();
    managed_app_focus::budget focus({1, 15, false}, now);
    const auto ipc = hyprland.transport();
    assert(!focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {700, 701}, activate, allowed); }, focus, [now] { return now; }));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0x5a02"}) == 1);
    assert(std::ranges::none_of(hyprland.requests, [](const auto &request) { return request.find("0x5a01") != std::string::npos; }));
    assert(focus.can_focus(now));
  }
  {
    // The game takes focus back during the adapter's second observation: no
    // dispatch, and no attempt is spent.
    fake_hyprland hyprland;
    int listings = 0;
    hyprland.on_request = [&](const std::string &request) {
      if (request == "j/clients" && ++listings == 2) hyprland.active = R"({"address":"0x5a02","stableId":"5a02","mapped":true,"hidden":false,"pid":701})";
    };
    const auto now = managed_app_focus::budget::clock::now();
    managed_app_focus::budget focus({1, 15, false}, now);
    const auto ipc = hyprland.transport();
    assert(!focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {700, 701}, activate, allowed); }, focus, [now] { return now; }));
    assert(dispatched(hyprland) == 0);
    assert(focus.can_focus(now));
  }
  {
    // Hyprland's replies block between the focus step's eligibility check and
    // the dispatch. A reply that carries an identity across its allowance, or
    // the hard cap, or a refused Lua dispatch that does, sends no focus request.
    using managed_app_focus::budget;
    const auto start = budget::clock::time_point(std::chrono::hours(1));
    struct gap {
      budget::clock::duration seen, checked;
      const char *slow;
    };
    for (const auto &[seen, checked, slow] : {gap {0s, 14900ms, "j/clients"}, gap {170s, 179900ms, "j/clients"},
                                             gap {0s, 14900ms, "/dispatch hl.dsp.focus"}}) {
      fake_hyprland hyprland;
      hyprland.active = R"({"address":"0x5a02","stableId":"5a02","mapped":true,"hidden":false,"pid":701})";
      budget focus({1, 15, false}, start);
      auto now = start + seen;
      const auto ipc = hyprland.transport();
      const auto step = [&] { return focus_step([&](const auto &activate, const auto &allowed) { return apply(ipc, {701}, activate, allowed); }, focus, [&] { return now; }); };
      assert(!step());
      hyprland.active = R"({"address":"0x5a03","stableId":"5a03","mapped":true,"hidden":false,"pid":900})";
      now = start + checked;
      int replies = 0;
      hyprland.on_request = [&](const std::string &request) {
        if (request.starts_with(slow) && ++replies == (request == "j/clients" ? 2 : 1)) now += 300ms;
      };
      assert(!step());
      assert(dispatched(hyprland) == (std::string_view(slow) == "j/clients" ? 0 : 1));
    }
  }
  {
    // Addresses are compositor data, never trusted dispatcher syntax.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0x5a01 class:.*","stableId":"7f","mapped":true,"hidden":false,"pid":700},
                          {"address":"5a02","stableId":"7f","mapped":true,"hidden":false,"pid":700},
                          {"address":"0x","stableId":"7f","mapped":true,"hidden":false,"pid":700}])";
    const auto applied = apply(hyprland.transport(), {700}, {"700:7f"}, always);
    assert(applied["windows"].empty() && !applied.value("focused", true));
    assert(hyprland.requests.size() == 2);
  }

  using managed_app_focus::hyprland::compositor;
  using managed_app_focus::hyprland::parse_compositor;
  using managed_app_focus::hyprland::socket_transport;
  // The session record binds Hyprland as pid-starttime; reject anything else.
  assert(parse_compositor("4242-987654").has_value());
  assert(parse_compositor("4242-987654")->pid == 4242 && parse_compositor("4242-987654")->start == 987654);
  for (const char *invalid : {"", "4242", "0-5", "4242-5 "}) assert(!parse_compositor(invalid));

  char pattern[] = "/tmp/vibepollo-hyprland-focus-XXXXXX";
  require(mkdtemp(pattern), "mkdtemp");
  const std::filesystem::path runtime = pattern;
  const compositor self {getpid(), own_start_time()};
  const auto timeout = std::chrono::milliseconds(500);
  // The runtime has no hypr directory yet: nothing to connect to.
  assert(!socket_transport(runtime, self, timeout)("j/clients", {}));
  const auto instances = runtime / "hypr";
  std::filesystem::create_directories(instances / "a-stale-instance");
  std::filesystem::create_directories(instances / "b-current-instance");
  std::filesystem::permissions(instances, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  {
    // A stale instance directory without a listener is skipped.
    fake_socket server(instances / "b-current-instance/.socket.sock", "[]");
    assert(socket_transport(runtime, self, timeout)("j/clients", {}) == "[]");
    assert(wait_for(server.connections, 1));
    assert((server.requests == std::vector<std::string> {"j/clients"}));
    // Another PID or a reused PID with a different start time is never sent
    // a request: Hyprland itself does not authenticate its IPC clients.
    assert(!socket_transport(runtime, {self.pid + 1, self.start}, timeout)("j/clients", {}));
    assert(!socket_transport(runtime, {self.pid, self.start + 1}, timeout)("j/clients", {}));
    assert(wait_for(server.connections, 3));
    assert(server.connections == 3 && server.requests.size() == 1);
  }
  {
    // The production transport scans, connects and authenticates after each
    // allowance check. If that work carries the target across its allowance,
    // or the hard cap, the dispatch is not sent: the Lua form, or the legacy
    // form after Hyprland refused the Lua one. A fast transport sends.
    // Only this instance listens, so each case reaches the send decision.
    std::filesystem::remove(instances / "b-current-instance/.socket.sock");
    std::filesystem::create_directories(instances / "c-dispatch-instance");
    std::string lua_reply;
    fake_socket server(instances / "c-dispatch-instance/.socket.sock", [&](const std::string &request) {
      return request.starts_with("/dispatch hl.dsp") ? lua_reply : std::string {"ok"};
    });
    using managed_app_focus::budget;
    const auto start = budget::clock::time_point(std::chrono::hours(1));
    const auto ipc = socket_transport(runtime, self, timeout);
    const std::string lua = "/dispatch hl.dsp.focus({ window = \"address:0x5a02\" })", legacy = "/dispatch focuswindow address:0x5a02";
    struct gap {
      budget::clock::duration seen, checked;
      int slow_check;  // The allowance check after which the transport takes 300 ms.
      const char *lua_reply;
      int connections;
      std::vector<std::string> sent;
    };
    int connections = 0;
    for (const auto &[seen, checked, slow_check, reply, connected, sent] :
         {gap {0s, 14900ms, 1, "ok", 1, {}}, gap {170s, 179900ms, 1, "ok", 1, {}}, gap {0s, 14s, 1, "ok", 1, {lua}},
          gap {0s, 14900ms, 3, "Invalid dispatcher", 2, {lua}}, gap {170s, 179900ms, 3, "Invalid dispatcher", 2, {lua}},
          gap {0s, 14s, 3, "Invalid dispatcher", 2, {lua, legacy}}}) {
      lua_reply = reply;
      budget focus({1, 15, false}, start);
      focus.observe("701:5a02", start + seen);
      auto now = start + checked;
      int checks = 0;
      const auto allowed = [&](const std::string &identity) {
        const bool ok = focus.can_focus(identity, now);
        if (++checks == slow_check) now += 300ms;
        return ok;
      };
      const auto applied = apply([&](const std::string &request, const auto &may_send) {
        return request.starts_with("j/") ? std::optional<std::string> {request == "j/clients" ? clients : R"({"address":"0x5a03","stableId":"5a03","mapped":true,"hidden":false,"pid":900})"} : ipc(request, may_send);
      }, {701}, {"701:5a02"}, allowed);
      assert(applied.value("focused", true) == false);  // The fake activewindow never confirms.
      connections += connected;
      assert(wait_for(server.connections, connections));
      assert(server.requests == sent);
      server.requests.clear();
    }
  }
  {
    // Hyprland leaves the sockets of earlier launches behind. Endpoints are
    // tried newest first, the name breaking ties, and at most 64 of them, so
    // any number of older stale entries never hides the live instance.
    char scan_pattern[] = "/tmp/vibepollo-hyprland-scan-XXXXXX";
    require(mkdtemp(scan_pattern), "mkdtemp");
    const std::filesystem::path scan = scan_pattern;
    const auto hypr = scan / "hypr";
    std::filesystem::create_directories(hypr);
    std::filesystem::permissions(hypr, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    const auto age = [&](const std::string &name, time_t seconds, long nanoseconds = 0) {
      const timespec times[2] {{seconds, nanoseconds}, {seconds, nanoseconds}};
      require(!utimensat(AT_FDCWD, (hypr / name / ".socket.sock").c_str(), times, AT_SYMLINK_NOFOLLOW), "utimensat");
    };
    // A socket file nobody listens on any more, like a crashed launch leaves.
    const auto stale = [&](const std::string &name, time_t seconds) {
      std::filesystem::create_directories(hypr / name);
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      require(fd >= 0, "socket");
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", (hypr / name / ".socket.sock").c_str());
      require(!bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "bind");
      close(fd);
      age(name, seconds);
    };
    // Entries without a usable socket, all newer than any socket: they must
    // not take a slot. A symlinked instance is never followed.
    // The symlinks lead to a newer stale socket outside the instance directory.
    std::filesystem::create_directories(scan / "elsewhere");
    {
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      require(fd >= 0, "socket");
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", (scan / "elsewhere/.socket.sock").c_str());
      require(!bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "bind");
      close(fd);
      const timespec times[2] {{9500, 0}, {9500, 0}};
      require(!utimensat(AT_FDCWD, (scan / "elsewhere/.socket.sock").c_str(), times, AT_SYMLINK_NOFOLLOW), "utimensat");
    }
    const auto junk = [&] {
      for (int i = 0; i < 70; ++i) {
        const auto name = "j-" + std::to_string(i);
        if (i % 3 == 0) {
          std::filesystem::create_directories(hypr / name);
        } else if (i % 3 == 1) {
          std::filesystem::create_directories(hypr / name);
          std::ofstream(hypr / name / ".socket.sock") << "not a socket";
          age(name, 9000 + i);
        } else {
          std::filesystem::create_directory_symlink(scan / "elsewhere", hypr / name);
        }
      }
    };
    const auto clear = [&] {
      for (const auto &entry : std::filesystem::directory_iterator(hypr)) std::filesystem::remove_all(entry.path());
    };
    for (const auto &[stale_count, stale_age, live_age, found] :
         {std::tuple {200, 1000, 2000, true}, std::tuple {63, 3000, 2000, true}, std::tuple {64, 3000, 2000, false}}) {
      for (const int before : {0, 100, stale_count}) {
        // Stale names sort before and after the live one, and the live entry
        // is created first, amid or last, so directory order cannot decide.
        const auto stale_entries = [&](int from, int to) {
          for (int i = from; i < to; ++i) stale((i % 2 ? "a-" : "zz-") + std::to_string(i), stale_age + i);
        };
        stale_entries(0, std::min(before, stale_count));
        std::filesystem::create_directories(hypr / "m-live");
        fake_socket live(hypr / "m-live/.socket.sock", "[]");
        stale_entries(std::min(before, stale_count), stale_count);
        junk();
        age("m-live", live_age);
        // A new transport has no instance that answered before.
        assert(socket_transport(scan, self, timeout)("j/clients", {}).has_value() == found);
        assert(wait_for(live.connections, found ? 1 : 0));
        assert(live.connections == (found ? 1 : 0));
        clear();
      }
    }
    {
      // Of two authenticating endpoints the newer socket answers, whatever
      // the names, to the nanosecond; equal ages fall back to name order.
      std::filesystem::create_directories(hypr / "a-older");
      std::filesystem::create_directories(hypr / "b-newer");
      fake_socket older(hypr / "a-older/.socket.sock", "older"), newer(hypr / "b-newer/.socket.sock", "newer");
      const auto first = [&] {
        return socket_transport(scan, self, timeout)("j/clients", {});
      };
      age("a-older", 1000, 200000000);
      age("b-newer", 1000, 700000000);
      assert(first() == "newer");
      age("b-newer", 1000, 100000000);
      assert(first() == "older");
      age("b-newer", 1000, 200000000);
      assert(first() == "older");
      // The instance that answered is asked first while it answers, even
      // once another one ranks higher; the directory is scanned again only
      // when it stops answering.
      age("b-newer", 2000);
      const auto ipc = socket_transport(scan, self, timeout);
      assert(ipc("j/clients", {}) == "newer");
      age("b-newer", 500);
      assert(ipc("j/clients", {}) == "newer");
      assert(ipc("j/clients", {}) == "newer");
      assert(wait_for(newer.connections, 4) && older.connections == 2);
      std::filesystem::remove(hypr / "b-newer/.socket.sock");
      assert(ipc("j/clients", {}) == "older");
      assert(ipc("j/clients", {}) == "older");
      assert(wait_for(older.connections, 4) && newer.connections == 4);
    }
    {
      // An instance that took the request but timed out is not sent it again
      // by the rescan that follows.
      clear();
      std::filesystem::create_directories(hypr / "s-slow");
      std::atomic<bool> slow = false;
      fake_socket server(hypr / "s-slow/.socket.sock", [&](const std::string &) {
        if (slow) std::this_thread::sleep_for(timeout + 200ms);
        return std::string {"ok"};
      });
      const auto ipc = socket_transport(scan, self, timeout);
      assert(ipc("j/clients", {}) == "ok");
      slow = true;
      assert(!ipc("/dispatch focuswindow address:0x5a02", {}));
      assert(wait_for(server.connections, 2));
      std::this_thread::sleep_for(300ms);
      assert(server.connections == 2 && server.requests.size() == 2);
    }
    std::filesystem::remove_all(scan);
  }
  std::filesystem::remove_all(runtime);
  std::cout << "PASS: Hyprland focus adapter selects bound candidate windows through IPC\n";
  return 0;
}
