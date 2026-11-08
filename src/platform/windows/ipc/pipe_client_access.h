#pragma once

#include <sstream>
#include <string>
#include <windows.h>

namespace platf::dxgi::pipe_security {
  // CreateFileW adds FILE_READ_ATTRIBUTES for pipe clients even when it was
  // not requested explicitly. Grant and request the same complete mask, without
  // GENERIC_WRITE (which would also grant FILE_CREATE_PIPE_INSTANCE).
  inline constexpr DWORD client_data_access =
    FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

  inline std::wstring client_access_sddl(const wchar_t *sid) {
    std::wostringstream descriptor;
    descriptor << L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x" << std::hex
               << client_data_access << L";;;" << sid << L")";
    return descriptor.str();
  }
}  // namespace platf::dxgi::pipe_security
