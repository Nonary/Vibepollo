/**
 * @file src/platform/windows/display_restore_task.h
 * @brief Read restore-task state without changing a user's disabled recovery task.
 */
#pragma once

#include <comdef.h>
#include <taskschd.h>

namespace display_helper {
  enum class restore_task_state_e {
    missing,
    enabled,
    disabled,
    unavailable,
  };

  inline const char *restore_task_state_name(restore_task_state_e state) {
    switch (state) {
      case restore_task_state_e::missing:
        return "missing";
      case restore_task_state_e::enabled:
        return "enabled";
      case restore_task_state_e::disabled:
        return "disabled";
      default:
        return "unavailable";
    }
  }

  inline bool can_manage_restore_task(restore_task_state_e state) {
    return state == restore_task_state_e::missing || state == restore_task_state_e::enabled;
  }

  inline restore_task_state_e read_restore_task_state(ITaskFolder *folder, const wchar_t *name) {
    IRegisteredTask *task = nullptr;
    const HRESULT found = folder->GetTask(_bstr_t(name), &task);
    if (FAILED(found)) {
      return found == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                 found == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ?
               restore_task_state_e::missing :
               restore_task_state_e::unavailable;
    }
    if (!task) {
      return restore_task_state_e::unavailable;
    }
    VARIANT_BOOL enabled = VARIANT_FALSE;
    const HRESULT read = task->get_Enabled(&enabled);
    task->Release();
    if (FAILED(read)) {
      return restore_task_state_e::unavailable;
    }
    return enabled != VARIANT_FALSE ? restore_task_state_e::enabled : restore_task_state_e::disabled;
  }
}  // namespace display_helper
