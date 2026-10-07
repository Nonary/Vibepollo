/**
 * @file src/platform/linux/secure_open.h
 * @brief Descriptor-relative fallback for kernels or sandboxes without openat2().
 */
#pragma once

#include <filesystem>
#include <sys/types.h>

namespace platf::linux_security {
  /**
   * Open a file beneath an already-validated directory without following any
   * symlink. Every intermediate directory must stay on the root filesystem,
   * have the expected owner, and be protected from untrusted writes.
   *
   * This is the fail-closed fallback for environments where openat2() returns
   * ENOSYS, including systemd's RestrictSUIDSGID filter. The returned file
   * descriptor is owned by the caller.
   */
  int open_readonly_beneath(
    int root_descriptor,
    const std::filesystem::path &relative_path,
    dev_t root_device,
    uid_t trusted_owner,
    bool require_private_directories
  );
}  // namespace platf::linux_security
