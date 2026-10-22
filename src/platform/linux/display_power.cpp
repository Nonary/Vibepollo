#include "display_power.h"
#include "display_borrow_client.h"

#include "display_helper_process.h"
#include "src/logging.h"
#include "src/display_target_lease.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace platf::display_power {
  namespace {
    struct lease_t {
      GSubprocess *process = nullptr;

      bool alive() const {
        auto *output = g_subprocess_get_stdout_pipe(process);
        struct pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
        // After READY the worker writes no more stdout. EOF/error means its
        // generation or bus has gone away; do not reuse that stale lease.
        const int result = poll(&watched, 1, 0);
        return result == 0 || (result < 0 && errno == EINTR);
      }

      ~lease_t() {
        if (process) {
          // The session broker cancels its generation-bound worker when its
          // client disconnects; the worker's D-Bus connection owns the inhibit.
          g_subprocess_force_exit(process);
          g_subprocess_wait(process, nullptr, nullptr);
          g_object_unref(process);
        }
      }
    };

    std::mutex lease_mutex;
    std::weak_ptr<lease_t> shared_lease;

    std::shared_ptr<lease_t> start_ready(const char *operation) {
      auto lease = std::make_shared<lease_t>();
      GError *spawn_error = nullptr;
      lease->process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
        &spawn_error,
        "/usr/libexec/vibeshine/vibepollo-session-exec",
        operation,
        nullptr
      );
      if (!lease->process) {
        BOOST_LOG(error) << "Linux display power: " << (spawn_error ? spawn_error->message : "could not start session helper");
        g_clear_error(&spawn_error);
        return {};
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      auto *output = g_subprocess_get_stdout_pipe(lease->process);
      struct pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
      while (std::chrono::steady_clock::now() < deadline) {
        const int ready = poll(&watched, 1, 100);
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        if (ready < 0) {
          break;
        }
        if (!ready) {
          continue;
        }
        char token = 0;
        if ((watched.revents & POLLIN) && read(watched.fd, &token, 1) == 1 && token == 'R') {
          return lease;
        }
        break;
      }
      BOOST_LOG(error) << "Linux display power: session wake/inhibition did not become ready.";
      return {};
    }
  }  // namespace

  std::shared_ptr<void> acquire_broker_mutation() {
    if(!std::getenv("VIBEPOLLO_MACHINE_HOST")) {static const auto noop=std::make_shared<int>(0);return noop;}
    return borrow_protocol::lease_t::connect(true);
  }
  void broker_mutation_completion_unknown(const std::shared_ptr<void> &hold) {
    if(hold && std::getenv("VIBEPOLLO_MACHINE_HOST")) std::static_pointer_cast<borrow_protocol::lease_t>(hold)->completion_unknown();
  }

  std::shared_ptr<void> acquire_borrow_inhibit() {
    if(!std::getenv("VIBEPOLLO_MACHINE_HOST")) {static const auto noop=std::make_shared<int>(0);return noop;}
    static std::mutex mutex;static std::weak_ptr<borrow_protocol::lease_t> current;
    std::lock_guard lock{mutex};
    if(auto lease=current.lock()) return lease->alive()?lease:nullptr;
    auto lease=borrow_protocol::lease_t::connect();current=lease;return lease;
  }

  std::shared_ptr<void> retain_existing() {
    if (!std::getenv("VIBEPOLLO_MACHINE_HOST")) {
      static const auto noop = std::make_shared<int>(0);
      return noop;
    }
    std::lock_guard lock {lease_mutex};
    const auto lease = shared_lease.lock();
    return lease && lease->alive() ? lease : nullptr;
  }

  std::shared_ptr<void> acquire() {
    // Serialize admission before taking the display mutation gate. The gate
    // rejects a second thread rather than waiting, while this lease mutex
    // already provides the ordering needed for concurrent stream starts.
    // Taking the gate first would make otherwise compatible concurrent
    // callers fail while the first caller is starting the shared worker.
    std::lock_guard lock {lease_mutex};
    auto target_mutation = display_target::leases().begin_mutation();
    if (!target_mutation) return {};
    // Standalone/SteamOS does not have a machine session broker.
    if (!std::getenv("VIBEPOLLO_MACHINE_HOST")) {
      static const auto noop = std::make_shared<int>(0);
      return noop;
    }
    if (auto lease = shared_lease.lock()) {
      if (!lease->alive()) {
        auto replacement = start_ready("display-power");
        if (!replacement) {
          return {};
        }
        // Keep the owner object stable: already-active streams still hold it.
        // Replacing only the weak cache would release the new worker as soon
        // as this one reconnecting client ends, despite those older owners.
        std::swap(lease->process, replacement->process);
        return lease;
      }
      // Share one long-lived broker connection across pending/active RTSP and
      // WebRTC owners. A new launch still gets a fresh wake before topology.
      if (!start_ready("display-wake")) {
        return {};
      }
      return lease;
    }
    auto lease = start_ready("display-power");
    shared_lease = lease;
    return lease;
  }

  std::shared_ptr<void> retain_screen() {
    if (!std::getenv("VIBEPOLLO_MACHINE_HOST")) {
      static const auto noop = std::make_shared<int>(0);
      return noop;
    }
    // Never share the capture lease: that one also inhibits suspend.
    return start_ready("display-retain");
  }

  std::optional<std::map<std::string, dpms_state_t>> query_dpms_states(
    std::chrono::steady_clock::time_point deadline
  ) {
    constexpr auto maximum_query_time = std::chrono::seconds {4};
    const auto now = std::chrono::steady_clock::now();
    deadline = std::min(deadline, now + maximum_query_time);
    if (deadline <= now) return std::nullopt;
    const bool brokered = std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr;
    std::vector<const gchar *> arguments;
    if (brokered) {
      arguments = {"/usr/libexec/vibeshine/vibepollo-session-exec", "display-observe", nullptr};
    } else {
      arguments = {"/usr/libexec/vibeshine/vibepollo-display-observer", nullptr};
    }
    GError *spawn_error = nullptr;
    auto *process = g_subprocess_newv(arguments.data(),
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
      &spawn_error);
    if (!process) {
      g_clear_error(&spawn_error);
      return std::nullopt;
    }
    const auto reply = linux_private_display::helper_process::communicate_until(process, deadline, brokered);
    g_object_unref(process);
    if (!reply.success || reply.stdout_text.size() > 12 * 1024) return std::nullopt;

    std::map<std::string, dpms_state_t> result;
    std::istringstream lines {reply.stdout_text};
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream fields {line};
      std::string name;
      unsigned supported = 0;
      unsigned mode = 0;
      if (!std::getline(fields, name, '\t') || name.empty() ||
          !(fields >> supported) || fields.get() != '\t' || !(fields >> mode) ||
          name.size() > 127 ||
          !std::ranges::all_of(name, [](const unsigned char character) {
            return std::isalnum(character) || character == '_' || character == '.' || character == '-';
          }) || supported > 1 || mode > 3 || fields.peek() != std::char_traits<char>::eof() ||
          result.size() >= 64) {
        return std::nullopt;
      }
      result[name] = {.known = supported != 0, .on = supported != 0 && mode == 0};
    }
    return result;
  }

  void observe_events(const std::function<bool(const observation_t &)> &on_event,
                      const std::function<bool()> &keep_running) {
    const bool brokered = std::getenv("VIBEPOLLO_MACHINE_HOST") != nullptr;
    const gchar *broker_arguments[] = {"/usr/libexec/vibeshine/vibepollo-session-exec", "display-observe-events", nullptr};
    const gchar *local_arguments[] = {"/usr/libexec/vibeshine/vibepollo-display-observer", "--watch", nullptr};
    auto subscription = std::make_shared<lease_t>();
    GError *error = nullptr;
    subscription->process = g_subprocess_newv(brokered ? broker_arguments : local_arguments,
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), &error);
    if (!subscription->process) {
      g_clear_error(&error);
      return;
    }
    auto *output = g_subprocess_get_stdout_pipe(subscription->process);
    struct pollfd watched {g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(output)), POLLIN, 0};
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {8};
    bool ready = false;
    std::string pending;
    while (keep_running()) {
      if (!ready && std::chrono::steady_clock::now() >= startup_deadline) return;
      const int available = poll(&watched, 1, 100);
      if (available < 0 && errno == EINTR) continue;
      if (available < 0 || (watched.revents & (POLLERR | POLLNVAL))) return;
      if (!available) continue;
      char bytes[4096];
      const auto count = read(watched.fd, bytes, sizeof(bytes));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return;
      pending.append(bytes, static_cast<std::size_t>(count));
      while (true) {
        const auto newline = pending.find('\n');
        if (newline == std::string::npos) break;
        const auto line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        observation_t event {observation_t::kind_e::baseline_ready, {}, {}};
        if (line == "READY") {
          if (ready) return;
          ready = true;
        } else {
          if (line.size() < 3 || line[1] != '\t') return;
          const auto separator = line.find('\t', 2);
          event.output = line.substr(2, separator == std::string::npos ? separator : separator - 2);
          if (event.output.empty() || event.output.size() > 127 ||
              !std::ranges::all_of(event.output, [](const unsigned char character) {
                return std::isalnum(character) || character == '_' || character == '.' || character == '-';
              })) return;
          if (line[0] == 'T' && separator == std::string::npos) {
            event.kind = observation_t::kind_e::topology;
          } else if (line[0] == 'S' && separator != std::string::npos) {
            std::istringstream fields {line.substr(separator + 1)};
            unsigned supported = 0, mode = 0;
            if (!(fields >> supported) || fields.get() != '\t' || !(fields >> mode) ||
                supported > 1 || mode > 3 || fields.peek() != std::char_traits<char>::eof()) return;
            event.kind = observation_t::kind_e::power;
            event.power = {.known = supported != 0, .on = supported != 0 && mode == 0};
          } else return;
        }
        if (!on_event(event)) return;
      }
      if (pending.size() > 256) return;
    }
  }

}  // namespace platf::display_power
