#pragma once

#include "src/platform/windows/display_helper_v2/interfaces.h"

#include <winioctl.h>
#include <sudovda/sudovda.h>

namespace display_helper::v2 {
  class WinVirtualDisplayDriver final : public IVirtualDisplayDriver {
  public:
    ~WinVirtualDisplayDriver() override {
      close_handle();
    }

    bool disable() override {
      close_handle();
      return true;
    }

    bool enable() override {
      if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        return true;
      }
      handle_ = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID);
      return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

    bool is_available() override {
      HANDLE h = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID);
      if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return false;
      }
      CloseHandle(h);
      return true;
    }

    std::string device_id() override {
      return {};
    }

  private:
    void close_handle() {
      if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
      }
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
  };
}  // namespace display_helper::v2
