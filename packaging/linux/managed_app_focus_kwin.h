#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace managed_app_focus {
  // Used by the real one-shot KWin adapter and executed with a mock workspace
  // in regression tests. internalId works for native Wayland and XWayland.
  inline std::string kwin_script(const std::vector<std::uint64_t> &pids, const std::string &destination, const std::vector<std::string> &activate = {}) {
    return "var pids=" + nlohmann::json(pids).dump() + ";var activate=" + nlohmann::json(activate).dump() + R"JS(;
var result={windows:[],focused:false};
var active=workspace.activeWindow;
result.foreground_pid=active && active.normalWindow && !active.minimized ? Number(active.pid) : 0;
var windows=workspace.windowList();
// Prefer a fullscreen game over its launcher/dialog, without matching titles.
windows.sort(function(a,b) { return Number(b.fullScreen)-Number(a.fullScreen); });
for (var i=0;i<windows.length;i++) {
  var w=windows[i];
  if (pids.indexOf(w.pid)<0 || !w.normalWindow || w.skipTaskbar || !w.internalId) continue;
  var identity=String(w.pid)+":"+String(w.internalId);
  result.windows.push(identity);
  if (activate.indexOf(identity)<0 || result.focused) continue;
  w.minimized=false;
  workspace.activeWindow=w;
  result.focused=workspace.activeWindow===w;
}
callDBus()JS" + nlohmann::json(destination).dump() + ",'/Focus','dev.vibepollo.Focus','Result',JSON.stringify(result));";
  }
}  // namespace managed_app_focus
