/**
 * @file src/prep_command_lifetime.h
 * @brief Track the completed preparation prefix for one application lifetime.
 */
#pragma once

namespace proc::command_lifetime {
  // The caller owns the cursor for the app, rather than for each attached
  // client. Leave it on a failing command so only the completed prefix rolls
  // back. Empty do commands still participate in teardown (undo-only hooks).
  template<class Iterator, class Run>
  bool prepare(Iterator &next, Iterator end, Run &&run) {
    for (; next != end; ++next) {
      if (!next->do_cmd.empty() && !run(*next)) {
        return false;
      }
    }
    return true;
  }

  template<class Iterator, class Run>
  void undo(Iterator &next, Iterator begin, Run &&run) {
    while (next != begin) {
      const auto &cmd = *--next;
      if (!cmd.undo_cmd.empty()) {
        run(cmd);
      }
    }
  }
}  // namespace proc::command_lifetime
