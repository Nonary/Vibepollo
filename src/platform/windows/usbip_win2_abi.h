/*
 * Public IOCTL declarations adapted from usbip-win2 v.0.9.8.1,
 * include/usbip/vhci.h, include/usbip/consts.h and include/usbip/ch9.h.
 * Source: https://github.com/vadimgrn/usbip-win2/tree/55e1fa7f0c2157017b02dc1f3236e98169a535e4
 * Only the unmodified signed driver's user-mode ABI is used.
 *
 * BSD 2-Clause License
 *
 * Copyright (c) 2021-2026, Vadym Hrynchyshyn <vadimgrn@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>
#include <winioctl.h>

namespace platf::dualsense_usbip::driver_abi {
  inline constexpr GUID interface_guid {0xB4030C06, 0xDC5F, 0x4FCC, {0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0}};

  // Keep the base classes and normal Win32 structure packing: the published
  // ABI contains padding between imported_device_location and the serial.
  struct base {
    ULONG size;
  };

  struct imported_device_location {
    int port;
    ULONG location_hash;
    char busid[32];
    char service[32];
    char host[1025];
  };

  struct imported_device_properties {
    std::uint32_t devid;
    int speed;
    std::uint16_t vendor;
    std::uint16_t product;
    char serial[16];
    UCHAR iserial;
    bool wsk_events;
  };

  struct imported_device: imported_device_location, imported_device_properties {};

  struct plugin_hardware: base, imported_device_location {
    char serial[16];
    bool wsk_events;
  };

  struct plugout_hardware: base {
    int port;
  };

  struct stop_attach_attempts: base, imported_device_location {
    int count;
  };

  struct get_imported_devices: base {
    imported_device devices[1];
  };

  inline constexpr DWORD plugin_hardware_once = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD plugout_hardware_ioctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD get_imported_devices_ioctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD stop_attach_attempts_ioctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
  inline constexpr DWORD attach_reply_size = 2 * sizeof(ULONG);
  inline constexpr DWORD imported_devices_offset = sizeof(base);

  static_assert(sizeof(ULONG) == 4 && sizeof(int) == 4 && sizeof(bool) == 1);
  static_assert(sizeof(imported_device_location) == 1100);
  static_assert(sizeof(imported_device_properties) == 32);
  static_assert(sizeof(imported_device) == 1132);
  static_assert(sizeof(plugin_hardware) == 1124);
  static_assert(sizeof(plugout_hardware) == 8);
  static_assert(sizeof(stop_attach_attempts) == 1108);
  static_assert(sizeof(get_imported_devices) == 1136);
}  // namespace platf::dualsense_usbip::driver_abi
