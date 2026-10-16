/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.h
 * @brief Declarations for nvidia preferences interface.
 */
#pragma once

// standard includes
#include <memory>

namespace nvprefs {

  class nvprefs_interface {
  public:
    nvprefs_interface();
    ~nvprefs_interface() noexcept;

    // Terminal, idempotent cleanup. Call while logging is alive. On failure,
    // release the journal lock without deleting its durable recovery data;
    // the next startup owns the retry. Destruction never attempts restoration.
    // As with the other operations, the lifecycle owner serializes this call.
    bool shutdown() noexcept;

    bool load();

    bool unload() noexcept;

    bool restore_from_and_delete_undo_file_if_exists() noexcept;

    bool modify_application_profile();

    bool modify_global_profile();

    bool owning_undo_file();

    bool restore_global_profile();

  private:
    struct impl;
    std::unique_ptr<impl> pimpl;
    bool shutdown_started = false;
    bool shutdown_succeeded = true;
  };

}  // namespace nvprefs
