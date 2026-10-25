// Compatibility proof against the exact updater shipped before this repair.
// The fixture is byte-for-byte feda692f:src/update_release.h, not a rewritten
// approximation. Keep its legacy acceptance limits visible in this test.
#include "../fixtures/update_release_feda692f.h"
#include <cassert>
#ifdef NDEBUG
#error Legacy updater compatibility assertions must remain enabled.
#endif
using nlohmann::json;

json package(std::string tag) {
#ifdef _WIN32
  const auto name = "VibepolloSetup.exe";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  const auto name = "Vibepollo-macOS-arm64.dmg";
#else
  const auto name = "Vibepollo-macOS-x86_64.dmg";
#endif
#elif defined(__aarch64__)
  const auto name = "vibepollo-fixture-aarch64.pkg.tar.zst";
#else
  const auto name = "vibepollo-fixture-x86_64.pkg.tar.zst";
#endif
  return {{"tag_name", tag}, {"draft", false}, {"prerelease", true},
          {"assets", json::array({{{"name", name}, {"state", "uploaded"}, {"size", 1}}})}};
}
int main() {
  const std::string old = "2.0.2-nightly.d20261025.ffffffffffff";
  const std::string first = "2.0.2-nightly.d20261025.r.1.000000000000";
  const std::string second = "2.0.2-nightly.d20261025.r.2.aaaaaaaaaaaa";
  const std::string tenth = "2.0.2-nightly.d20261025.r.10.111111111111";
  const std::string tomorrow = "2.0.2-nightly.d20261026.r.1.000000000000";
  for (const auto &installed : std::vector<std::string> {old, "2.0.2-nightly.d20261024.ffffffffffff",
                                 "2.0.2-daily.d20261024.3+ffffffffffff"}) {
    const auto result = release_selection::select(json::array({package(first)}), installed, true);
    assert(!result.is_null() && result["tag_name"] == first);
  }
  assert(release_selection::compare_notification_versions(first, old) > 0);
  assert(release_selection::compare_notification_versions(old, first) < 0);
  assert(release_selection::compare_notification_versions(second, first) > 0);
  assert(release_selection::compare_notification_versions(tenth, second) > 0);
  assert(release_selection::compare_notification_versions(tomorrow, tenth) > 0);
  assert(release_selection::select(json::array({package(first)}), second, true).is_null());
  assert(release_selection::select(json::array({package(second)}), "2.0.2", true).is_null());
  assert(release_selection::select(json::array({package(second)}), old, false).is_null());
  auto incomplete = package(second);
  incomplete["assets"] = json::array();
  assert(release_selection::select(json::array({incomplete}), old, true).is_null());
  // Old binaries see the new shape through their existing ordinary-SemVer
  // route, so they cannot enforce the repaired metadata contract themselves.
  // The publisher still emits complete checked metadata/checksums; the new
  // updater's own tests require and validate both metadata and source binding.
  assert(!release_selection::daily_tag(second));
}
