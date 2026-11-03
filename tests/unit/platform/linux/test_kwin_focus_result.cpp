#include "../../../../packaging/linux/managed_app_focus_kwin.h"
#include "../../../../packaging/linux/managed_app_focus_step.h"

#include <cassert>
#include <iostream>

using namespace std::chrono_literals;

int main() {
  using managed_app_focus::kwin_result;
  using managed_app_focus::kwin_script;
  // Each script echoes the call number it was generated for.
  assert(kwin_script({701}, ":1.42", {"701:7"}, 7).ends_with("result.call=7;callDBus(\":1.42\",'/Focus','dev.vibepollo.Focus','Result',JSON.stringify(result));"));

  const char *current = R"({"call":7,"windows":["701:7"],"focused":true,"foreground":"701:7"})";
  assert(kwin_result(current, 7) == nlohmann::json::parse(current));
  // A late result from the script of an earlier call is not this call's.
  assert(!kwin_result(current, 8));
  // Neither is one without a usable call number; none of them may throw.
  for (const char *other : {R"({"windows":["701:7"],"focused":true})", R"({"call":"7","windows":["701:7"],"focused":true})",
                            R"({"call":-7,"windows":["701:7"],"focused":true})", R"({"call":7.5,"windows":["701:7"],"focused":true})"}) {
    assert(!kwin_result(other, 7));
  }
  // The shape focus_step reads is still required.
  for (const char *other : {"", "not json", "[]", R"({"call":7,"windows":["701:7"]})", R"({"call":7,"windows":"701:7","focused":true})",
                            R"({"call":7,"windows":[701],"focused":true})", R"({"call":7,"windows":["701:7"],"focused":1})",
                            R"({"call":7,"windows":[],"focused":false,"foreground":1})", R"({"call":7,"windows":[],"focused":false,"foreground_pid":"7"})"}) {
    assert(!kwin_result(other, 7));
  }
  {
    // Writing and loading the script block between the focus step's check
    // and the run; a load that carries the target across its allowance, or
    // the hard cap, never runs the script. A fast load still does.
    using managed_app_focus::budget;
    const auto start = budget::clock::time_point(1h);
    struct gap {
      budget::clock::duration seen, checked, load;
      bool runs;
    };
    for (const auto &[seen, checked, load, runs] : {gap {0s, 14900ms, 300ms, false}, gap {170s, 179900ms, 300ms, false}, gap {0s, 14s, 300ms, true}}) {
      budget focus({1, 15, false}, start);
      auto now = start + seen;
      std::string foreground = "701:7";
      int ran = 0;
      const auto desktop = [&](const std::vector<std::string> &activate, const std::function<bool(const std::string &)> &allowed) {
        bool focused = false;
        managed_app_focus::kwin_load_and_run(activate, allowed, [&] {
          if (!activate.empty()) now += load;
          return 3;
        }, [&](int id) {
          assert(id == 3);
          if (!activate.empty()) ++ran, focused = true, foreground = activate.front();
        });
        return nlohmann::json {{"windows", {"701:7"}}, {"focused", focused}, {"foreground", foreground}};
      };
      assert(!managed_app_focus::focus_step(desktop, focus, [&] { return now; }));
      foreground = "900:1";
      now = start + checked;
      assert(managed_app_focus::focus_step(desktop, focus, [&] { return now; }) == runs);
      assert(ran == (runs ? 1 : 0));
    }
  }
  std::cout << "PASS: KWin focus results are accepted only from the current script\n";
  return 0;
}
