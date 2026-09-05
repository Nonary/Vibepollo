/**
 * @file src/platform/linux/capability_sanitizer.h
 * @brief Earliest-process capability sanitization for Linux startup.
 */
#pragma once

#include <cerrno>
#include <cstdio>

#include <sys/capability.h>
#include <sys/prctl.h>

namespace platf::linux_security {
  /**
   * Remove inherited execution privilege before configuration or logging is
   * parsed. Only CAP_SYS_ADMIN and CAP_SYS_NICE that were already permitted
   * remain permitted for the guarded KMS and priority elevation paths.
   */
  inline bool sanitize_startup_capabilities() noexcept {
    const int entry_errno = errno;
    cap_t original = cap_get_proc();
    cap_t sanitized = cap_init();
    cap_t verified = nullptr;
    auto fail = [&](int error_number, const char *operation) {
      if (verified) {
        cap_free(verified);
      }
      if (sanitized) {
        cap_free(sanitized);
      }
      if (original) {
        cap_free(original);
      }
      std::fprintf(stderr, "Vibepollo: capability sanitization failed while %s\n", operation);
      errno = error_number ? error_number : EPERM;
      return false;
    };
    if (!original || !sanitized) {
      return fail(errno, "allocating the capability sets");
    }

#ifdef SUNSHINE_BUILD_STEAMOS
    // SteamOS' user manager passes CAP_WAKE_ALARM in the inheritable set.
    // The user bundle needs no capabilities, and must retain ordinary Steam
    // and pressure-vessel launch behavior. Require empty P/E sets, then drop
    // inherited bits without imposing no_new_privs on child applications.
    if (cap_clear_flag(original, CAP_INHERITABLE) != 0) {
      return fail(errno, "checking the unprivileged SteamOS entry context");
    }
    const int unprivileged_comparison = cap_compare(original, sanitized);
    if (unprivileged_comparison != 0) {
      return fail(unprivileged_comparison < 0 ? errno : EPERM, "requiring an unprivileged SteamOS launch");
    }
    if (cap_set_proc(sanitized) != 0) {
      return fail(errno, "clearing inherited SteamOS capabilities");
    }
    cap_free(original);
    original = cap_get_proc();
    if (!original) {
      return fail(errno, "reading back the SteamOS capability set");
    }
#endif

    const int initial_comparison = cap_compare(original, sanitized);
    if (initial_comparison < 0) {
      return fail(errno, "comparing the initial capability set");
    }
    if (initial_comparison == 0) {
      // Linux requires every ambient capability to also be permitted and
      // inheritable. An empty P/E/I set therefore proves that a portable,
      // capability-free launch has no ambient privilege. Keep ordinary user
      // launches compatible with application launchers that may need their
      // own setuid/file-capability transitions.
      cap_free(sanitized);
      cap_free(original);
      errno = entry_errno;
      return true;
    }

    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
      return fail(errno, "clearing ambient capabilities");
    }

    constexpr cap_value_t allowed[] {CAP_SYS_ADMIN, CAP_SYS_NICE};
    cap_value_t retained[sizeof(allowed) / sizeof(allowed[0])] {};
    int retained_count = 0;
    for (const auto capability : allowed) {
      cap_flag_value_t permitted = CAP_CLEAR;
      if (cap_get_flag(original, capability, CAP_PERMITTED, &permitted) != 0) {
        return fail(errno, "reading the permitted capability set");
      }
      if (permitted == CAP_SET) {
        retained[retained_count++] = capability;
      }
    }
    if (retained_count > 0 &&
        cap_set_flag(sanitized, CAP_PERMITTED, retained_count, retained, CAP_SET) != 0) {
      return fail(errno, "constructing the permitted capability set");
    }
    // A privileged machine host must enter exactly as packaged: both allowed
    // capabilities permitted, with no effective or inheritable capabilities.
    // Reject rather than normalize any broader or partial execution context.
    if (retained_count != static_cast<int>(sizeof(allowed) / sizeof(allowed[0]))) {
      return fail(EPERM, "verifying both required permitted capabilities");
    }
    const int entry_comparison = cap_compare(original, sanitized);
    if (entry_comparison != 0) {
      return fail(entry_comparison < 0 ? errno : EPERM, "verifying the exact entry capability set");
    }
    if (cap_set_proc(sanitized) != 0) {
      return fail(errno, "applying the sanitized capability set");
    }

    verified = cap_get_proc();
    if (!verified) {
      return fail(errno, "reading back the sanitized capability set");
    }
    const int comparison = cap_compare(sanitized, verified);
    if (comparison != 0) {
      return fail(comparison < 0 ? errno : EPERM, "verifying the sanitized capability set");
    }

    // PR_CAP_AMBIENT_CLEAR_ALL is authoritative for capabilities newer than
    // the build headers. Probe the runtime namespace as well so startup fails
    // if the kernel reports any ambient capability still raised.
    bool reached_runtime_limit = false;
    for (unsigned long capability = 0; capability < 4096; ++capability) {
      errno = 0;
      const int ambient = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET,
                                capability, 0, 0);
      if (ambient == 0) {
        continue;
      }
      if (ambient > 0) {
        return fail(EPERM, "verifying that ambient capabilities are empty");
      }
      if (errno == EINVAL) {
        reached_runtime_limit = true;
        break;
      }
      return fail(errno, "probing the runtime ambient capability limit");
    }
    if (!reached_runtime_limit) {
      return fail(EOVERFLOW, "bounding the runtime ambient capability limit");
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
      return fail(errno, "setting no_new_privs");
    }
    errno = 0;
    const int no_new_privileges = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    if (no_new_privileges != 1) {
      return fail(no_new_privileges < 0 ? errno : EPERM, "verifying no_new_privs");
    }

    cap_free(verified);
    cap_free(sanitized);
    cap_free(original);
    errno = entry_errno;
    return true;
  }
}  // namespace platf::linux_security
