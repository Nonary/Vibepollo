// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file src/crypto_perm.h
 * @brief Client permission bitmask definitions without OpenSSL/JSON dependencies.
 */
#pragma once

#include <cstdint>

namespace crypto {
  /**
   * @brief The permissions of a client.
   */
  enum class PERM : uint32_t {
    _reserved = 1,

    _input = _reserved << 8,  // Input permission group
    input_controller = _input << 0,  // Allow controller input
    input_touch = _input << 1,  // Allow touch input
    input_pen = _input << 2,  // Allow pen input
    input_mouse = _input << 3,  // Allow mouse input
    input_kbd = _input << 4,  // Allow keyboard input
    _all_inputs = input_controller | input_touch | input_pen | input_mouse | input_kbd,

    _operation = _input << 8,  // Operation permission group
    clipboard_set = _operation << 0,  // Allow set clipboard from client
    clipboard_read = _operation << 1,  // Allow read clipboard from host
    file_upload = _operation << 2,  // Allow upload files to host
    file_dwnload = _operation << 3,  // Allow download files from host
    server_cmd = _operation << 4,  // Allow execute server cmd
    _all_opeiations = clipboard_set | clipboard_read | file_upload | file_dwnload | server_cmd,

    _action = _operation << 8,  // Action permission group
    list = _action << 0,  // Allow list apps
    view = _action << 1,  // Allow view streams
    launch = _action << 2,  // Allow launch apps
    _allow_view = view | launch,  // If no view permission is granted, disconnect the device upon permission update
    _all_actions = list | view | launch,

    _default = view | list,  // Default permissions for new clients
    _no = 0,  // No permissions are granted
    _all = _all_inputs | _all_opeiations | _all_actions,  // All current permissions
  };

  inline constexpr PERM
    operator|(PERM x, PERM y) {
    return static_cast<PERM>(static_cast<uint32_t>(x) | static_cast<uint32_t>(y));
  }

  inline constexpr PERM
    operator&(PERM x, PERM y) {
    return static_cast<PERM>(static_cast<uint32_t>(x) & static_cast<uint32_t>(y));
  }

  inline constexpr bool
    operator!(PERM p) {
    return static_cast<uint32_t>(p) == 0;
  }
}  // namespace crypto
