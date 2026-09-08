/**
 * @file src/platform/linux/maintenance_cli.h
 * @brief Native-package maintenance commands, dispatched before host startup.
 */
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace platf::linux_cli {
  inline constexpr const char *help =
    "Linux maintenance (native packages):\n"
    "  vibepollo paths                    Show settings and program locations\n"
    "  vibepollo status                   Show machine service status\n"
    "  sudo vibepollo logs                 Show recent service logs\n"
    "  sudo vibepollo configure USER       Select the desktop owner and migrate settings\n"
    "  sudo vibepollo migrate [HOST]       Prepare settings; optionally select a legacy host\n"
    "    HOST: vibepollo, vibeshine, sunshine, or machine-vibeshine\n"
    "  sudo vibepollo authorize-commands   Approve commands in the application list\n"
    "  sudo vibepollo driver install       Install/update the virtual-display driver\n"
    "  sudo vibepollo driver status        Show virtual-display driver status\n"
    "  sudo vibepollo reset                Erase settings and pairings; ends active streams\n";

  // A missing optional means this is a normal host invocation. An empty vector
  // denotes a recognized command with invalid arguments; it must never fall
  // through to configuration parsing or start another host.
  inline std::optional<std::vector<const char *>> command(int argc, char **argv) {
    if (argc < 2) {
      return std::nullopt;
    }
    const std::string_view name {argv[1]};
    constexpr auto machine = "/usr/libexec/vibeshine/vibepollo-machine-host";
    if (name == "configure") {
      if (argc == 3 && argv[2][0] != '\0' && argv[2][0] != '-') {
        return std::vector<const char *> {machine, "configure", argv[2]};
      }
    } else if (name == "migrate") {
      if (argc == 2) {
        return std::vector<const char *> {machine, "configure-auto"};
      }
      if (argc == 3) {
        const std::string_view source {argv[2]};
        if (source == "vibepollo" || source == "vibeshine" || source == "sunshine" || source == "machine-vibeshine") {
          return std::vector<const char *> {machine, "configure-auto", argv[2]};
        }
      }
    } else if (name == "authorize-commands" || name == "reset") {
      if (argc == 2) {
        return std::vector<const char *> {machine, argv[1]};
      }
    } else if (name == "driver") {
      if (argc == 3 && (std::string_view {argv[2]} == "install" || std::string_view {argv[2]} == "status")) {
        return std::vector<const char *> {"/usr/libexec/vibeshine/vibeshine-drm-install", argv[2]};
      }
    } else if (name == "status") {
      if (argc == 2) {
        return std::vector<const char *> {"/usr/bin/systemctl", "--no-pager", "--full", "status",
                                        "vibepollo-session-controller.service", "vibepollo-session-exec.socket", "vibepollo.service"};
      }
    } else if (name == "logs") {
      if (argc == 2) {
        return std::vector<const char *> {"/usr/bin/journalctl", "--no-pager", "-n", "200",
                                        "-u", "vibepollo-session-controller.service", "-u", "vibepollo-session-exec@.service",
                                        "-u", "vibepollo.service"};
      }
    } else if (name != "paths" && name != "maintenance-help") {
      return std::nullopt;
    }
    return std::vector<const char *> {};
  }

  inline std::optional<int> dispatch(int argc, char **argv) {
    auto arguments = command(argc, argv);
    if (!arguments) {
      return std::nullopt;
    }
    if (argc == 2 && std::string_view {argv[1]} == "paths") {
      std::puts("Native Linux package locations:\n"
                "  Settings, credentials, pairings, apps: /var/lib/vibepollo\n"
                "  Configuration file: /var/lib/vibepollo/vibepollo.conf\n"
                "  Administrator policy: /etc/vibepollo\n"
                "  Temporary session data: /run/vibepollo\n"
                "  Programs: /usr/bin/vibepollo, /usr/libexec/vibeshine\n"
                "  Assets: /usr/share/vibepollo\n"
                "  Logs: sudo vibepollo logs\n"
                "  Legacy user settings (imported once): ~/.config/{vibepollo,vibeshine,sunshine}\n"
                "  Legacy machine settings (imported once): /var/lib/vibeshine");
      return 0;
    }
    if (argc == 2 && std::string_view {argv[1]} == "maintenance-help") {
      std::fputs(help, stdout);
      return 0;
    }
    if (arguments->empty()) {
      std::fputs(help, stderr);
      return 2;
    }
    arguments->push_back(nullptr);
    // Fixed executables and separate arguments: no shell, PATH search, or
    // privilege escalation. Administrative helpers enforce their own UID check.
    execv(arguments->front(), const_cast<char *const *>(arguments->data()));
    std::fprintf(stderr, "Vibepollo: cannot execute %s: %s\n", arguments->front(), std::strerror(errno));
    return 1;
  }
}  // namespace platf::linux_cli
