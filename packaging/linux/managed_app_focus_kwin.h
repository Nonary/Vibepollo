#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace managed_app_focus {
  // Used by the real one-shot KWin adapter and executed with a mock workspace
  // in regression tests. internalId works for native Wayland and XWayland.
  // The result echoes call, so a late result from an earlier script is not
  // taken for this one.
  inline std::string kwin_script(const std::vector<std::uint64_t> &pids, const std::string &destination, const std::vector<std::string> &activate = {}, std::uint64_t call = 0) {
    return "var pids=" + nlohmann::json(pids).dump() + ";var activate=" + nlohmann::json(activate).dump() + R"JS(;
var result={windows:[],focused:false};
var active=workspace.activeWindow;
result.foreground_pid=active && active.normalWindow && !active.minimized ? Number(active.pid) : 0;
result.foreground=result.foreground_pid && active.internalId ? String(active.pid)+":"+String(active.internalId) : "";
var windows=workspace.windowList();
// Prefer a fullscreen game over its launcher/dialog, without matching titles.
windows.sort(function(a,b) { return Number(b.fullScreen)-Number(a.fullScreen); });
for (var i=0;i<windows.length;i++) {
  var w=windows[i];
  if (pids.indexOf(w.pid)<0 || !w.normalWindow || w.skipTaskbar || !w.internalId) continue;
  var identity=String(w.pid)+":"+String(w.internalId);
  result.windows.push(identity);
  if (activate.indexOf(identity)<0 || result.focused || w===active) continue;
  w.minimized=false;
  workspace.activeWindow=w;
  result.focused=workspace.activeWindow===w;
}
result.call=)JS" + std::to_string(call) + ";callDBus(" + nlohmann::json(destination).dump() + ",'/Focus','dev.vibepollo.Focus','Result',JSON.stringify(result));";
  }

  // A Result counts only from the script of this call and in the shape the
  // focus step reads; anything else, including malformed JSON, is dropped.
  inline std::optional<nlohmann::json> kwin_result(std::string_view text, std::uint64_t call) {
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    const auto is_string = [](const nlohmann::json &window) { return window.is_string(); };
    if (!parsed.is_object() || !parsed.contains("call") || !parsed["call"].is_number_unsigned() || parsed["call"].get<std::uint64_t>() != call ||
        !parsed.contains("focused") || !parsed["focused"].is_boolean() || !parsed.contains("windows") || !parsed["windows"].is_array() ||
        !std::ranges::all_of(parsed["windows"], is_string) || (parsed.contains("foreground") && !parsed["foreground"].is_string()) ||
        (parsed.contains("foreground_pid") && !parsed["foreground_pid"].is_number_unsigned())) {
      return std::nullopt;
    }
    return parsed;
  }

  // Loading the script may block, and the script activates as soon as it
  // runs, so its targets are rechecked between load and run. Returns the
  // loaded script id for unloading.
  inline int kwin_load_and_run(const std::vector<std::string> &activate, const std::function<bool(const std::string &)> &allowed, const std::function<int()> &load, const std::function<void(int)> &run) {
    const int id = load();
    if (id >= 0 && (activate.empty() || (allowed && std::ranges::all_of(activate, allowed)))) {
      run(id);
    }
    return id;
  }
}  // namespace managed_app_focus
