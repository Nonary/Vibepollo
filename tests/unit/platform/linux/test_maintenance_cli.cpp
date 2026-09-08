#include "../../../../src/platform/linux/maintenance_cli.h"

#include <cstdlib>
#include <initializer_list>
#include <string>

static void check(std::initializer_list<const char *> input,
                  std::initializer_list<const char *> expected, bool recognized = true) {
  std::vector<char *> argv;
  for (auto value : input) {
    argv.push_back(const_cast<char *>(value));
  }
  const auto result = platf::linux_cli::command(static_cast<int>(argv.size()), argv.data());
  if (result.has_value() != recognized || (result && result->size() != expected.size())) {
    std::abort();
  }
  if (result) {
    auto actual = result->begin();
    for (auto value : expected) {
      if (std::string {*actual++} != value) {
        std::abort();
      }
    }
  }
}

int main() {
  constexpr auto helper = "/usr/libexec/vibeshine/vibepollo-machine-host";
  check({"vibepollo", "configure", "alice"}, {helper, "configure", "alice"});
  // Arguments remain literal and cannot become shell commands or helper options.
  check({"vibepollo", "configure", "alice; touch /tmp/unwanted"}, {helper, "configure", "alice; touch /tmp/unwanted"});
  check({"vibepollo", "configure", "--help"}, {});
  check({"vibepollo", "configure"}, {});
  check({"vibepollo", "configure", "alice", "bob"}, {});
  check({"vibepollo", "migrate"}, {helper, "configure-auto"});
  for (const auto source : {"vibepollo", "vibeshine", "sunshine", "machine-vibeshine"}) {
    check({"vibepollo", "migrate", source}, {helper, "configure-auto", source});
  }
  check({"vibepollo", "migrate", "--help"}, {});
  check({"vibepollo", "migrate", "/tmp/profile"}, {});
  check({"vibepollo", "migrate", "sunshine", "extra"}, {});
  check({"vibepollo", "reset"}, {helper, "reset"});
  check({"vibepollo", "reset", "anything"}, {});
  check({"vibepollo", "authorize-commands"}, {helper, "authorize-commands"});
  check({"vibepollo", "driver", "status"}, {"/usr/libexec/vibeshine/vibeshine-drm-install", "status"});
  check({"vibepollo", "driver", "install"}, {"/usr/libexec/vibeshine/vibeshine-drm-install", "install"});
  check({"vibepollo", "driver", "arbitrary-operation"}, {});
  check({"vibepollo", "status"}, {"/usr/bin/systemctl", "--no-pager", "--full", "status",
                                  "vibepollo-session-controller.service", "vibepollo-session-exec.socket", "vibepollo.service"});
  check({"vibepollo", "logs"}, {"/usr/bin/journalctl", "--no-pager", "-n", "200",
                                "-u", "vibepollo-session-controller.service", "-u", "vibepollo-session-exec@.service", "-u", "vibepollo.service"});
  check({"vibepollo"}, {}, false);
  check({"vibepollo", "/var/lib/vibepollo/vibepollo.conf"}, {}, false);
  check({"vibepollo", "--version"}, {}, false);
  check({"vibepollo", "encoder=nvenc"}, {}, false);

  char name[] = "vibepollo";
  char paths[] = "paths";
  char extra[] = "unexpected";
  char *argv[] = {name, paths, extra};
  if (platf::linux_cli::dispatch(2, argv) != 0 || platf::linux_cli::dispatch(3, argv) != 2) {
    return 1;
  }
  std::puts("PASS: Linux maintenance dispatch");
}
