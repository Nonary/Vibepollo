#include "../../../../packaging/linux/managed_app_focus_hyprland.h"

#include <atomic>
#include <cassert>
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
  // A one-reply-per-connection server like Hyprland's .socket.sock. It runs in
  // this process, so its peer credentials are this test's PID and start time.
  class fake_socket {
  public:
    explicit fake_socket(const std::filesystem::path &path, std::string reply):
        reply_(std::move(reply)) {
      fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
      assert(fd_ >= 0 && !bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) && !listen(fd_, 4));
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
    std::string refused;  // A window that Hyprland reports as gone.
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
          if (address == refused) return "No such window found";
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
  {
    // Observation alone lists the fullscreen game first and changes nothing.
    fake_hyprland hyprland;
    const auto observed = apply(hyprland.transport(), {700, 701});
    assert(observed.value("focused", true) == false);
    assert(observed.value("foreground_pid", 0) == 900);
    assert((observed["windows"] == nlohmann::json {"701:0x5a02", "700:0x5a01"}));
    assert((hyprland.requests == std::vector<std::string> {"j/clients", "j/activewindow"}));
  }
  {
    // Hyprland has no window types; dialogs float by default. A tiled game
    // outranks its floating dialog, and fullscreen outranks both.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"address":"0x5b01","mapped":true,"hidden":false,"pid":700,"floating":true,"fullscreen":0},
                          {"address":"0x5b02","mapped":true,"hidden":false,"pid":700,"floating":false,"fullscreen":0},
                          {"address":"0x5b03","mapped":true,"hidden":false,"pid":700,"floating":true,"fullscreen":1}])";
    const auto observed = apply(hyprland.transport(), {700});
    assert((observed["windows"] == nlohmann::json {"700:0x5b03", "700:0x5b02", "700:0x5b01"}));
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
    // A Lua config refuses dispatcher syntax; the same validated address is
    // then sent as the Lua focus expression.
    fake_hyprland hyprland;
    hyprland.lua_config = true;
    const auto applied = apply(hyprland.transport(), {701}, {"701:0x5a02"});
    assert(applied.value("focused", false));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch hl.dsp.focus({ window = \"address:0x5a02\" })"}) == 1);
  }
  {
    // A vanished first choice falls through to the next allowed window.
    fake_hyprland hyprland;
    hyprland.refused = "0x5a02";
    const auto applied = apply(hyprland.transport(), {700, 701}, {"701:0x5a02", "700:0x5a01"});
    assert(applied.value("focused", false));
    assert(std::ranges::count(hyprland.requests, std::string {"/dispatch focuswindow address:0x5a01"}) == 1);
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
  {
    // Unexpected field types or missing keys skip a window instead of
    // aborting the helper.
    fake_hyprland hyprland;
    hyprland.listed = R"([{"mapped":true,"hidden":false,"pid":700},
                          {"address":"0x5a01","mapped":"yes","hidden":false,"pid":700},
                          {"address":"0x5a02","mapped":true,"hidden":false,"pid":"700"},
                          {"address":"0x5a03","mapped":true,"hidden":false,"pid":700,"fullscreen":"2"},
                          "0x5a04"])";
    hyprland.active = R"({"address":5,"mapped":true,"hidden":false,"pid":700})";
    const auto applied = apply(hyprland.transport(), {700}, {"700:0x5a03"});
    assert(applied["windows"] == nlohmann::json {"700:0x5a03"});
    assert(applied.value("foreground_pid", 0) == 700 && applied.value("focused", false));
  }
  {
    // Unusable or malformed replies produce no observation at all.
    fake_hyprland hyprland;
    hyprland.listed = "unknown request";
    assert(apply(hyprland.transport(), {700}).is_null());
    const managed_app_focus::hyprland::transport absent = [](const std::string &) { return std::nullopt; };
    assert(apply(absent, {700}).is_null());
    // Like the KWin adapter, no candidate process means no compositor query.
    fake_hyprland idle;
    assert(apply(idle.transport(), {}).is_null() && idle.requests.empty());
  }

  using managed_app_focus::hyprland::compositor;
  using managed_app_focus::hyprland::parse_compositor;
  using managed_app_focus::hyprland::socket_transport;
  // The session record binds Hyprland as pid-starttime; reject anything else.
  assert(parse_compositor("4242-987654").has_value());
  assert(parse_compositor("4242-987654")->pid == 4242 && parse_compositor("4242-987654")->start == 987654);
  for (const char *invalid : {"", "4242", "-1-2", "0-5", "4242-0", "4242-5-6", "4242-x", " 4242-5", "4242-5 "}) {
    assert(!parse_compositor(invalid));
  }

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
  {
    // Symlinked instance directories are not followed.
    std::filesystem::remove_all(instances / "b-current-instance");
    const auto elsewhere = runtime / "elsewhere";
    std::filesystem::create_directories(elsewhere);
    fake_socket server(elsewhere / ".socket.sock", "[]");
    std::filesystem::create_directory_symlink(elsewhere, instances / "c-linked-instance");
    assert(!socket_transport(runtime, self, timeout)("j/clients"));
    assert(server.connections == 0);
  }
  std::filesystem::remove_all(runtime);
  std::cout << "PASS: Hyprland focus adapter selects bound candidate windows through IPC\n";
  return 0;
}
