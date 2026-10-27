#include "../../../../packaging/linux/managed_app_focus_hyprland.h"
#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
            assert(write(connection, reply_.data(), reply_.size()) == static_cast<ssize_t>(reply_.size()));
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
    std::string reply_;
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

  void wait_for(const std::atomic<int> &value, int expected) {
    for (int attempt = 0; attempt < 200 && value < expected; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // Shapes follow Hyprland 0.56 `j/clients` and `j/activewindow`. Titles are
  // omitted: selection must never depend on them.
  const char *clients = R"([
    {"address":"0x5a01","mapped":true,"hidden":false,"pid":700,"xwayland":true,"fullscreen":0,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a02","mapped":true,"hidden":false,"pid":701,"xwayland":true,"fullscreen":2,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a03","mapped":true,"hidden":false,"pid":900,"xwayland":false,"fullscreen":0,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a04","mapped":false,"hidden":false,"pid":700,"xwayland":true,"fullscreen":0,"workspace":{"id":1,"name":"1"}},
    {"address":"0x5a05","mapped":true,"hidden":true,"pid":701,"xwayland":true,"fullscreen":0,"workspace":{"id":1,"name":"1"}}
  ])";

  struct fake_hyprland {
    std::string listed = clients;
    std::string active = R"({"address":"0x5a03","mapped":true,"hidden":false,"pid":900})";
    bool steal_back = false;  // Another client takes focus again immediately.
    bool lua_config = false;  // Hyprland 0.56 Lua configs parse dispatch as Lua.
    std::vector<std::string> requests;

    managed_app_focus::hyprland::transport transport() {
      return [this](const std::string &request) -> std::optional<std::string> {
        requests.push_back(request);
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
          if (!steal_back) active = R"({"address":")" + address + R"(","mapped":true,"hidden":false,"pid":701})";
          return "ok";
        }
        return std::nullopt;
      };
    }
  };
}  // namespace

int main() {
  using managed_app_focus::hyprland::apply;
  using managed_app_focus::focus_step;
  {
    // Observation alone lists the fullscreen game first and changes nothing.
    fake_hyprland hyprland;
    const auto observed = apply(hyprland.transport(), {700, 701});
    assert(observed.value("focused", true) == false);
    assert(observed.value("foreground_pid", 0) == 900);
    assert(observed.value("foreground", std::string {}) == "900:0x5a03");
    assert((observed["windows"] == nlohmann::json {"701:0x5a02", "700:0x5a01"}));
    assert((hyprland.requests == std::vector<std::string> {"j/clients", "j/activewindow"}));
  }
  {
    // Only an allowed identity is activated, and Hyprland must confirm it.
    fake_hyprland hyprland;
    const auto applied = apply(hyprland.transport(), {700, 701}, {"701:0x5a02", "900:0x5a03"});
    assert(applied.value("focused", false));
    assert(hyprland.requests.back() == "j/activewindow");
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0x5a02"}) == 1);
    assert(std::ranges::count_if(hyprland.requests, [](const auto &request) { return request.find("0x5a03") != std::string::npos; }) == 0);
  }
  {
    // A launcher of the game's own process in front of the fullscreen game
    // is lost focus, not the game.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0xaaaa","mapped":true,"hidden":false,"pid":700,"fullscreen":2},
                          {"address":"0xbbbb","mapped":true,"hidden":false,"pid":700,"fullscreen":0}])";
    hyprland.active = R"({"address":"0xbbbb","mapped":true,"hidden":false,"pid":700})";
    managed_app_focus::budget focus({1, 15, false}, managed_app_focus::budget::clock::now());
    const auto ipc = hyprland.transport();
    assert(focus_step([&](const auto &activate) { return apply(ipc, {700}, activate); }, focus, managed_app_focus::budget::clock::now));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0xaaaa"}) == 1);
  }
  {
    // The exact game window already active gets no dispatch and spends nothing.
    fake_hyprland hyprland;
    hyprland.active = R"({"address":"0x5a02","mapped":true,"hidden":false,"pid":701})";
    const auto now = managed_app_focus::budget::clock::now();
    managed_app_focus::budget focus({1, 15, false}, now);
    const auto ipc = hyprland.transport();
    assert(!focus_step([&](const auto &activate) { return apply(ipc, {700, 701}, activate); }, focus, [now] { return now; }));
    assert(std::ranges::none_of(hyprland.requests, [](const auto &request) { return request.starts_with("/dispatch"); }));
    assert(focus.can_focus(now));
  }
  {
    // A Lua config refuses dispatcher syntax; the same validated address is
    // then sent as the Lua focus expression.
    fake_hyprland hyprland;
    hyprland.lua_config = true;
    const auto applied = apply(hyprland.transport(), {701}, {"701:0x5a02"});
    assert(applied.value("focused", false));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch hl.dsp.focus({ window = \"address:0x5a02\" })"}) == 1);
  }
  {
    // An accepted dispatch without the window becoming active is not success.
    fake_hyprland hyprland;
    hyprland.steal_back = true;
    const auto applied = apply(hyprland.transport(), {701}, {"701:0x5a02"});
    assert(!applied.value("focused", true));
    assert(applied["windows"] == nlohmann::json {"701:0x5a02"});
  }
  {
    // Addresses are compositor data, never trusted dispatcher syntax.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0x5a01 class:.*","mapped":true,"hidden":false,"pid":700},
                          {"address":"5a02","mapped":true,"hidden":false,"pid":700},
                          {"address":"0x","mapped":true,"hidden":false,"pid":700}])";
    const auto applied = apply(hyprland.transport(), {700}, {"700:0x5a01 class:.*", "700:5a02", "700:0x"});
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
  const std::filesystem::path runtime = mkdtemp(pattern);
  const compositor self {getpid(), own_start_time()};
  const auto timeout = std::chrono::milliseconds(500);
  // The runtime has no hypr directory yet: nothing to connect to.
  assert(!socket_transport(runtime, self, timeout)("j/clients"));
  const auto instances = runtime / "hypr";
  std::filesystem::create_directories(instances / "a-stale-instance");
  std::filesystem::create_directories(instances / "b-current-instance");
  std::filesystem::permissions(instances, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  {
    // A stale instance directory without a listener is skipped.
    fake_socket server(instances / "b-current-instance/.socket.sock", "[]");
    assert(socket_transport(runtime, self, timeout)("j/clients") == "[]");
    wait_for(server.connections, 1);
    assert((server.requests == std::vector<std::string> {"j/clients"}));
    // Another PID or a reused PID with a different start time is never sent
    // a request: Hyprland itself does not authenticate its IPC clients.
    assert(!socket_transport(runtime, {self.pid + 1, self.start}, timeout)("j/clients"));
    assert(!socket_transport(runtime, {self.pid, self.start + 1}, timeout)("j/clients"));
    wait_for(server.connections, 3);
    assert(server.connections == 3 && server.requests.size() == 1);
  }
  std::filesystem::remove_all(runtime);
  std::cout << "PASS: Hyprland focus adapter selects bound candidate windows through IPC\n";
  return 0;
}
