#include "../../../../packaging/linux/managed_app_focus_kwin.h"

#include <cassert>
#include <iostream>

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
                            R"({"call":7,"windows":[701],"focused":true})", R"({"call":7,"windows":["701:7"],"focused":1})"}) {
    assert(!kwin_result(other, 7));
  }
  std::cout << "PASS: KWin focus results are accepted only from the current script\n";
  return 0;
}
