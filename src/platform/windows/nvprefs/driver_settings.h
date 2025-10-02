/**
 * @file src/platform/windows/nvprefs/driver_settings.h
 * @brief Declarations for nvidia driver settings.
 */
#pragma once

// nvapi headers
// disable clang-format header reordering
// wrapper keeps NvAPI SAL annotations friendly to non-MSVC toolchains
// clang-format off
#include "../nvapi_driver_settings.h"
// clang-format on

// local includes
#include "undo_data.h"

namespace nvprefs {

  class driver_settings_t {
  public:
    ~driver_settings_t();

    bool init();

    void destroy();

    bool load_settings();

    bool save_settings();

    bool restore_global_profile_to_undo(const undo_data_t &undo_data);

    bool check_and_modify_global_profile(std::optional<undo_data_t> &undo_data);

    bool check_and_modify_application_profile(bool &modified);

  private:
    NvDRSSessionHandle session_handle = nullptr;
  };

}  // namespace nvprefs
