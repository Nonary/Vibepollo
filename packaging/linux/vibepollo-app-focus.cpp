/** Unprivileged managed-library focus worker, run in the bound desktop session. */
#include "src/lutris_integration.h"
#include "src/managed_app_focus.h"
#include "managed_app_focus_hyprland.h"
#include "managed_app_focus_kwin.h"
#include "src/managed_app_focus_candidates.h"
#include "src/steam_integration.h"
#include "src/steam_process_tracker.h"

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <gio/gio.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <syslog.h>
#include <thread>
#include <unistd.h>

namespace {
  volatile sig_atomic_t cancelled = 0;

  // The session broker discards stderr; mirror diagnostics into the journal.
  void report(int priority, const std::string &message) {
    std::cerr << "Managed focus: " << message << '\n';
    syslog(priority, "%s", message.c_str());
  }

  void cancel(int) {
    cancelled = 1;
  }

  // Every focus attempt is a short-lived script. Even abrupt worker teardown
  // cannot leave a compositor timer that keeps stealing focus.
  class kwin_focus {
  public:
    explicit kwin_focus(int timeout_ms = 1000): timeout_ms_(timeout_ms) {
      bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
      if (!bus_) {
        return;
      }
      schema_ = g_dbus_node_info_new_for_xml(
        "<node><interface name='dev.vibepollo.Focus'><method name='Result'>"
        "<arg type='s' direction='in'/></method></interface></node>",
        nullptr
      );
      static const GDBusInterfaceVTable callbacks {on_result, nullptr, nullptr, {nullptr}};
      registration_ = g_dbus_connection_register_object(bus_, "/Focus", schema_->interfaces[0], &callbacks, this, nullptr, nullptr);
    }

    ~kwin_focus() {
      if (registration_) {
        g_dbus_connection_unregister_object(bus_, registration_);
      }
      if (schema_) {
        g_dbus_node_info_unref(schema_);
      }
      if (bus_) {
        g_object_unref(bus_);
      }
    }

    nlohmann::json apply(const std::vector<std::uint64_t> &pids, const std::vector<std::string> &activate = {}) {
      if (!registration_ || pids.empty() || cancelled) {
        return {};
      }
      const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      if (!runtime) {
        return {};
      }
      std::string path = std::string(runtime) + "/vibepollo-focus-XXXXXX";
      const int fd = mkstemp(path.data());
      if (fd < 0) {
        return {};
      }
      const std::string name = "vibepollo-focus-" + std::to_string(getpid());
      const std::string script = managed_app_focus::kwin_script(pids, g_dbus_connection_get_unique_name(bus_), activate);
      const bool written = write(fd, script.data(), script.size()) == static_cast<ssize_t>(script.size());
      close(fd);
      received_ = false;
      result_ = {};
      GVariant *loaded = written ? call("/Scripting", "org.kde.kwin.Scripting", "loadScript", g_variant_new("(ss)", path.c_str(), name.c_str())) : nullptr;
      int id = -1;
      if (loaded) {
        g_variant_get(loaded, "(i)", &id);
        g_variant_unref(loaded);
      }
      if (id >= 0) {
        const auto object = "/Scripting/Script" + std::to_string(id);
        if (auto *reply = call(object.c_str(), "org.kde.kwin.Script", "run", nullptr)) {
          g_variant_unref(reply);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min(500,timeout_ms_));
          while (!received_ && !cancelled && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, false)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
        if (auto *reply = call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", g_variant_new("(s)", name.c_str()))) {
          g_variant_unref(reply);
        }
      }
      unlink(path.c_str());
      return received_ ? result_ : nlohmann::json {};
    }

  private:
    static void on_result(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *args, GDBusMethodInvocation *invocation, gpointer data) {
      auto &self = *static_cast<kwin_focus *>(data);
      const char *result = nullptr;
      g_variant_get(args, "(&s)", &result);
      auto parsed = nlohmann::json::parse(result ? result : "", nullptr, false);
      if (parsed.is_object() && parsed.contains("focused") && parsed["focused"].is_boolean() &&
          parsed.contains("windows") && parsed["windows"].is_array() &&
          std::all_of(parsed["windows"].begin(), parsed["windows"].end(), [](const auto &window) { return window.is_string(); })) {
        self.result_ = std::move(parsed);
        self.received_ = true;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    GVariant *call(const char *object, const char *interface, const char *method, GVariant *args) {
      GError *error = nullptr;
      auto *reply = g_dbus_connection_call_sync(bus_, "org.kde.KWin", object, interface, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout_ms_, nullptr, &error);
      if (error) {
        report(LOG_WARNING, error->message);
        g_error_free(error);
      }
      return reply;
    }

    GDBusConnection *bus_ = nullptr;
    GDBusNodeInfo *schema_ = nullptr;
    guint registration_ = 0;
    bool received_ = false;
    nlohmann::json result_;
    int timeout_ms_;
  };

  bool steam_identity(std::uint64_t pid, const std::string &id) {
    std::ifstream env("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
    std::string item;
    for (int i = 0; i < 8192 && std::getline(env, item, '\0'); ++i) {
      if (item == "SteamAppId=" + id || item == "SteamGameId=" + id) {
        return true;
      }
    }
    return false;
  }
}  // namespace

int main(int argc, char **argv) {
  openlog("vibepollo-app-focus", LOG_PID, LOG_USER);
  if (argc == 4 && std::string_view(argv[1]) == "--foreground") {
    // This is a read-only query in the selected desktop identity. The dummy
    // candidate cannot activate anything: the script gets no activation list.
    const std::string provider=argv[2], identity=argv[3];
    kwin_focus focus(250);
    const auto observed=focus.apply({1});
    const auto pid=observed.is_object()?observed.value("foreground_pid",std::uint64_t{0}):0;
    bool matches=false;
    if (pid && provider=="steam") matches=steam_identity(pid,identity);
    else if (pid && provider=="executable") {
      std::error_code error;
      const auto executable=std::filesystem::read_symlink("/proc/"+std::to_string(pid)+"/exe",error);
      if (!error) matches=identity.find('/')==std::string::npos ? executable.filename()==identity : executable==identity;
    } else if (pid && provider=="lutris") {
      for (const auto &game:platf::lutris::discover()) {
        if (std::to_string(game.id)!=identity) continue;
        if (auto snapshot=platf::steam::lifecycle::snapshot_processes()) {
          const auto pids=managed_app_focus::candidates({provider,identity,game.directory},*snapshot);
          matches=std::find(pids.begin(),pids.end(),pid)!=pids.end();
        }
        break;
      }
    }
    std::cout<<(matches?"1\n":"0\n");
    return observed.is_object()?0:1;
  }
  std::optional<managed_app_focus::hyprland::compositor> bound_compositor;
  if (argc == 8 && std::string_view(argv[1]) == "--managed-hyprland") {
    bound_compositor = managed_app_focus::hyprland::parse_compositor(argv[2]);
    if (!bound_compositor) {
      return 2;
    }
    --argc;
    ++argv;
  } else if (argc != 7 || std::string_view(argv[1]) != "--managed") {
    return 2;
  }
  const std::string provider = argv[2], id = argv[3];
  if (provider != "steam" && provider != "lutris") {
    return 2;
  }
  std::uint64_t numeric_id = 0;
  const auto parsed = std::from_chars(id.data(), id.data() + id.size(), numeric_id);
  if (parsed.ec != std::errc {} || parsed.ptr != id.data() + id.size() || !numeric_id) {
    return 2;
  }
  std::unordered_map<std::string, std::string> vars {{"app_focus_attempts", argv[4]}, {"app_focus_timeout_secs", argv[5]}, {"app_focus_exit_on_first", argv[6]}};
  const auto policy = managed_app_focus::parse_policy(vars);
  if (!policy.enabled()) {
    return 0;
  }
  signal(SIGTERM, cancel);
  signal(SIGINT, cancel);
  signal(SIGHUP, cancel);
  sigset_t termination;
  sigemptyset(&termination);
  sigaddset(&termination, SIGTERM);
  sigaddset(&termination, SIGINT);
  sigaddset(&termination, SIGHUP);
  sigprocmask(SIG_UNBLOCK, &termination, nullptr);
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  managed_app_focus::budget budget(policy, managed_app_focus::budget::clock::now());
  std::filesystem::path directory;
  if (provider == "steam") {
    for (const auto &game : platf::steam::discover_catalog(platf::steam::default_library_roots())) {
      if (game.app_id == numeric_id) {
        directory = game.install_dir;
        break;
      }
    }
  } else {
    for (const auto &game : platf::lutris::discover()) {
      if (static_cast<std::uint64_t>(game.id) == numeric_id) {
        directory = game.directory;
        break;
      }
    }
  }
  if (directory.empty() || directory == directory.root_path()) {
    report(LOG_ERR, "no installed catalog target for " + provider + ':' + id);
    return 1;
  }
  std::optional<kwin_focus> kwin;
  managed_app_focus::hyprland::transport hyprland_ipc;
  if (bound_compositor) {
    // The runtime directory the broker validated, not the inherited one.
    const auto runtime = "/run/user/" + std::to_string(geteuid());
    hyprland_ipc = managed_app_focus::hyprland::socket_transport(runtime, *bound_compositor, std::chrono::milliseconds(1000));
  } else {
    kwin.emplace();
  }
  const auto apply = [&](const std::vector<std::uint64_t> &pids, const std::vector<std::string> &activate = {}) {
    return kwin ? kwin->apply(pids, activate) : managed_app_focus::hyprland::apply(hyprland_ipc, pids, activate);
  };
  bool unavailable_reported = false;
  while (!cancelled && budget.active(managed_app_focus::budget::clock::now())) {
    std::vector<std::uint64_t> pids;
    if (auto snapshot = platf::steam::lifecycle::snapshot_processes()) {
      if (provider == "steam") {
        for (auto &[pid, process] : snapshot->processes) {
          if (numeric_id <= UINT32_MAX && steam_identity(pid, id)) {
            process.steam_app_id = static_cast<std::uint32_t>(numeric_id);
          }
        }
      }
      pids = managed_app_focus::candidates({provider, id, directory}, *snapshot);
    }
    const auto observed = apply(pids);
    if (bound_compositor && !pids.empty() && observed.is_null() && !unavailable_reported) {
      report(LOG_WARNING, "the bound Hyprland IPC socket is unavailable");
      unavailable_reported = true;
    }
    if (observed.is_object() && observed.contains("windows") && observed["windows"].is_array()) {
      for (const auto &window : observed["windows"]) {
        if (window.is_string()) {
          budget.observe(window.get<std::string>(), managed_app_focus::budget::clock::now());
        }
      }
      if (!cancelled && budget.can_focus(managed_app_focus::budget::clock::now()) && !observed["windows"].empty()) {
        std::vector<std::string> allowed;
        for (const auto &window : observed["windows"]) {
          const auto identity = window.get<std::string>();
          if (budget.can_focus(identity, managed_app_focus::budget::clock::now())) {
            allowed.push_back(identity);
          }
        }
        const auto applied = apply(pids, allowed);
        if (!cancelled && applied.is_object() && applied.value("focused", false)) {
          budget.confirmed();
          report(LOG_INFO, "confirmed " + provider + ':' + id);
        }
      }
    }
    for (int i = 0; i < 10 && !cancelled; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  return 0;
}
