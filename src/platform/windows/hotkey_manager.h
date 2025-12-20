/**
 * @file src/platform/windows/hotkey_manager.h
 * @brief Global hotkey registration for Windows.
 */
#pragma once

#ifdef _WIN32
namespace platf::hotkey {
  // Update the restore hotkey (virtual-key code + modifier flags). Use 0 to disable.
  void update_restore_hotkey(int vk_code, unsigned int modifiers);
}  // namespace platf::hotkey
#endif
