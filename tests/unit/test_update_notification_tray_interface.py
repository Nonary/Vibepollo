"""Compile and execute production notification delivery against the real tray ABI.

Only OS delivery, config and persistence are mocked; the production method and
system_tray.h/update.h are compiled with SUNSHINE_TRAY=1. No UI is opened.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2]
source = (root / "src/update.cpp").read_text()
start = source.index("  static void notify_new_version(")
end = source.index("\n  static void perform_check()", start)
method = source[start:end]

program = r'''
#include <algorithm>
#include <cassert>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "src/system_tray.h"
#include "src/update.h"
using namespace std::literals;
struct NullLog { template<class T> NullLog &operator<<(const T &) { return *this; } };
#define BOOST_LOG(level) NullLog {}
namespace config { struct { bool notify_pre_releases = true; } sunshine; }
namespace nvhttp {
  int saves = 0;
  void save_state() {
    // Persistence must occur outside the notification lock.
    assert(update::state.notification_mutex.try_lock());
    update::state.notification_mutex.unlock();
    ++saves;
  }
}
struct Notice { std::string title, body; void (*callback)(); };
std::vector<Notice> notices;
namespace system_tray {
  static_assert(std::is_same_v<decltype(&tray_notify), void (*)(const char *, const char *, void (*)())>);
  void tray_notify(const char *title, const char *text, void (*callback)()) {
    notices.push_back({title, text, callback});
  }
}
namespace update {
  state_t state;
PRODUCTION_METHOD
}
int main() {
  using update::notify_new_version;
  update::state.latest_release.url = "https://github.com/Nonary/Vibepollo/releases/tag/2.0.1";
  update::state.latest_prerelease.url = "https://github.com/Nonary/Vibepollo/releases/tag/daily-2026-10-11";
  notify_new_version("", false);
  assert(notices.empty() && nvhttp::saves == 0);
  config::sunshine.notify_pre_releases = false;
  notify_new_version("2.0.2-daily.d20261011", true);
  assert(notices.empty() && update::state.notified_versions.empty());
  config::sunshine.notify_pre_releases = true;
  const auto first_url = update::state.latest_prerelease.url;
  notify_new_version("2.0.2-daily.d20261011", true);
  assert(notices.size() == 1 && nvhttp::saves == 1);
  assert(notices[0].body == "Version 2.0.2-daily.d20261011\n" + first_url);
  assert(notices[0].callback == nullptr);
  assert(update::state.last_notified_url == first_url);
  assert(update::state.last_notified_is_prerelease);
  // Changing selected release/state cannot redirect an older toast click.
  update::state.latest_prerelease.url = "https://github.com/Nonary/Vibepollo/releases/tag/daily-2026-10-12";
  notify_new_version("2.0.2-daily.d20261012", true);
  assert(notices.size() == 2 && notices[1].callback == nullptr);
  assert(notices[0].body == "Version 2.0.2-daily.d20261011\n" + first_url);
  notify_new_version("2.0.1", false);
  assert(notices.size() == 3 && !update::state.last_notified_is_prerelease);
  assert(notices[2].body == "Version 2.0.1\n" + update::state.latest_release.url);
  assert(notices[2].callback == nullptr);
  // Dismissal identity survives a newer notification/channel switch.
  notify_new_version("2.0.2-daily.d20261011", true);
  assert(notices.size() == 3 && nvhttp::saves == 3);
  update::state.latest_release.url.clear();
  notify_new_version("2.0.3", false);
  assert(notices.back().body == "Version 2.0.3" && notices.back().callback == nullptr);
  assert(nvhttp::saves == 4);
}
'''.replace("PRODUCTION_METHOD", method)

with tempfile.TemporaryDirectory(prefix="update-tray-interface-") as temp:
    path = pathlib.Path(temp)
    cpp, exe = path / "test.cpp", path / "test.exe"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-DSUNSHINE_TRAY=1", "-I", str(root),
                    str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Production updater/tray ABI, exact URL, suppression and persistence tests passed")
