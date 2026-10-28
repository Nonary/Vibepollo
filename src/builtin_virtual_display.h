/**
 * @file src/builtin_virtual_display.h
 * @brief Construct the built-in desktop with the same global hooks as
 * configured apps.
 */
#pragma once

#include "process.h"

namespace proc {
  // Keep construction independent of driver availability so the Windows-only
  // catalog entry can be covered by deterministic tests on every platform.
  template<class Expand>
  ctx_t make_builtin_virtual_display(const config::sunshine_t &global, Expand &&expand) {
    ctx_t ctx {};
    ctx.uuid = VIRTUAL_DISPLAY_UUID;
    ctx.name = "Virtual Display";
    ctx.image_path = expand("virtual_desktop.png");
    ctx.virtual_display = true;
    ctx.scale_factor = 100;
    ctx.auto_detach = true;
    ctx.exit_timeout = std::chrono::seconds(5);

    // Preparation belongs to the app lifetime, including rollback on failed
    // launch. State hooks belong to stream resume/pause. Retain the desktop
    // after pause, just like a configured desktop, so disconnect does not undo
    // preparation or remove the display before a later resume.
    ctx.prep_cmds.reserve(global.prep_cmds.size());
    for (const auto &cmd : global.prep_cmds) {
      ctx.prep_cmds.emplace_back(expand(cmd.do_cmd), expand(cmd.undo_cmd), bool(cmd.elevated));
    }
    ctx.state_cmds.reserve(global.state_cmds.size());
    for (const auto &cmd : global.state_cmds) {
      ctx.state_cmds.emplace_back(expand(cmd.do_cmd), expand(cmd.undo_cmd), bool(cmd.elevated));
    }
    return ctx;
  }
}  // namespace proc
