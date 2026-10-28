/**
 * @file entry_credentials.h
 * @brief Command-line argument routing for credential updates.
 */
#pragma once

#include <string_view>

namespace args {
  /**
   * @brief Dispatch the production credential CLI path through its callbacks.
   *
   * Keeping the callbacks injectable lets the argument contract be exercised
   * without opening or modifying the credential store.
   */
  template<typename ShowHelp, typename SaveCredentials>
  int creds(
    const char *name,
    int argc,
    char *argv[],
    ShowHelp show_help,
    SaveCredentials save_credentials
  ) {
    if (
      argc < 2 || !argv || !argv[0] || !argv[1] ||
      std::string_view {argv[0]} == "help" || std::string_view {argv[1]} == "help"
    ) {
      return show_help(name);
    }

    return save_credentials(argv[0], argv[1]);
  }
}  // namespace args
