#include "../../../tests_common.h"

#include <src/platform/linux/wayland_observation.h>
#include <atomic>
#include <fcntl.h>
#include <optional>
#include <thread>
#include <vector>

namespace {
  class environment_value {
  public:
    environment_value(const char *name, const char *value): name_(name) {
      if (const char *previous = std::getenv(name)) previous_ = previous;
      if (value) setenv(name, value, 1);
      else unsetenv(name);
    }
    ~environment_value() {
      if (previous_) setenv(name_.c_str(), previous_->c_str(), 1);
      else unsetenv(name_.c_str());
    }
  private:
    std::string name_;
    std::optional<std::string> previous_;
  };

  class private_socket: public testing::Test {
  protected:
    void SetUp() override {
      char path[] = "/tmp/vibeshine-wayland-observation-XXXXXX";
      const char *created = mkdtemp(path);
      ASSERT_NE(created, nullptr);
      directory = created;
      socket_path = directory + "/private-display";
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      ASSERT_GE(listener, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
      ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
      ASSERT_EQ(listen(listener, 0), 0);
    }
    void TearDown() override {
      for (const int descriptor : clients) close(descriptor);
      if (listener >= 0) close(listener);
      if (!socket_path.empty()) unlink(socket_path.c_str());
      if (!directory.empty()) rmdir(directory.c_str());
    }
    void fill_backlog() {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
      for (int attempt = 0; attempt < 8; ++attempt) {
        const int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        ASSERT_GE(descriptor, 0);
        clients.push_back(descriptor);
        if (connect(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
          ASSERT_EQ(errno, EAGAIN);
          return;
        }
      }
      FAIL() << "private socket backlog did not fill";
    }
    int listener {-1};
    std::vector<int> clients;
    std::string directory;
    std::string socket_path;
  };
}  // namespace

TEST_F(private_socket, AbsolutePathConnectsWithoutRuntimeDirectory) {
  environment_value display {"WAYLAND_DISPLAY", socket_path.c_str()};
  environment_value runtime {"XDG_RUNTIME_DIR", nullptr};
  auto *connection = platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; });
  ASSERT_NE(connection, nullptr);
  EXPECT_TRUE(fcntl(wl_display_get_fd(connection), F_GETFL) & O_NONBLOCK);
  wl_display_disconnect(connection);
}

TEST_F(private_socket, RelativePathUsesRuntimeDirectory) {
  environment_value display {"WAYLAND_DISPLAY", "private-display"};
  environment_value runtime {"XDG_RUNTIME_DIR", directory.c_str()};
  auto *connection = platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; });
  ASSERT_NE(connection, nullptr);
  wl_display_disconnect(connection);
}

TEST_F(private_socket, ConnectedSilentPeerRoundtripUsesDeadline) {
  environment_value display {"WAYLAND_DISPLAY", socket_path.c_str()};
  const auto start = std::chrono::steady_clock::now();
  auto *connection = platf::wayland_observation::connect_until(start + std::chrono::seconds {1}, [] { return true; });
  ASSERT_NE(connection, nullptr);
  EXPECT_FALSE(platf::wayland_observation::roundtrip_until(connection, std::chrono::steady_clock::now() + std::chrono::milliseconds {30}, [] { return true; }));
  EXPECT_EQ(errno, ETIMEDOUT);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds {500});
  wl_display_disconnect(connection);
}

TEST_F(private_socket, FullBacklogTimesOut) {
  fill_backlog();
  environment_value display {"WAYLAND_DISPLAY", socket_path.c_str()};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(platf::wayland_observation::connect_until(start + std::chrono::milliseconds {30}, [] { return true; }), nullptr);
  EXPECT_EQ(errno, ETIMEDOUT);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds {500});
}

TEST_F(private_socket, FullBacklogRespondsToCancellation) {
  fill_backlog();
  environment_value display {"WAYLAND_DISPLAY", socket_path.c_str()};
  std::atomic<bool> allowed {true};
  std::jthread cancel {[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
    allowed = false;
  }};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(platf::wayland_observation::connect_until(start + std::chrono::seconds {5}, [&] { return allowed.load(); }), nullptr);
  EXPECT_EQ(errno, ECANCELED);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds {500});
}

TEST_F(private_socket, FullBacklogConnectsWhenServerAccepts) {
  fill_backlog();
  environment_value display {"WAYLAND_DISPLAY", socket_path.c_str()};
  std::jthread server {[&] {
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
    const int accepted = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (accepted >= 0) close(accepted);
  }};
  auto *connection = platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; });
  ASSERT_NE(connection, nullptr);
  wl_display_disconnect(connection);
}

TEST(WaylandObservation, RelativeDisplayRequiresAbsoluteRuntimeDirectory) {
  environment_value display {"WAYLAND_DISPLAY", "private-display"};
  environment_value runtime {"XDG_RUNTIME_DIR", "relative-directory"};
  EXPECT_EQ(platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; }), nullptr);
  EXPECT_EQ(errno, EINVAL);
}

TEST(WaylandObservation, MissingDisplayFailsWithoutUsingInheritedSocket) {
  environment_value display {"WAYLAND_DISPLAY", nullptr};
  environment_value socket {"WAYLAND_SOCKET", "0"};
  EXPECT_EQ(platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; }), nullptr);
  EXPECT_EQ(errno, ENOENT);
}

TEST(WaylandObservation, AlreadyCancelledDoesNotConnect) {
  EXPECT_EQ(platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return false; }), nullptr);
  EXPECT_EQ(errno, ECANCELED);
}

TEST(WaylandObservation, OverlongSocketPathFails) {
  const std::string name = "/" + std::string(200, 'x');
  environment_value display {"WAYLAND_DISPLAY", name.c_str()};
  EXPECT_EQ(platf::wayland_observation::connect_until(std::chrono::steady_clock::now() + std::chrono::seconds {1}, [] { return true; }), nullptr);
  EXPECT_EQ(errno, ENAMETOOLONG);
}
