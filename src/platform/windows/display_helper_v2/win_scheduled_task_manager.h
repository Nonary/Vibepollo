#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"
#include "src/platform/windows/display_restore_task.h"

namespace display_helper::v2 {
  class WinScheduledTaskManager final : public IScheduledTaskManager {
  public:
    bool create_restore_task(const std::wstring &username) override;
    bool delete_restore_task() override;
    display_helper::restore_task_state_e restore_task_state();
    bool is_task_present() override;

  private:
    static std::wstring resolve_username(const std::wstring &username_hint);
    static std::wstring resolve_user_sid(const std::wstring &username_hint);
    static std::wstring build_restore_task_name(const std::wstring &username);
  };
}  // namespace display_helper::v2
