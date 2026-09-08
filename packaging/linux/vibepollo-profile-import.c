#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/close_range.h>
#include <linux/openat2.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char destination_prefix[] = "/var/lib/.vibepollo-profile.";
static const char destination_suffix[] = "/incoming";
static int open_beneath(int directory, const char *path, int flags);
static const char *source_profiles[] = {".config/vibepollo", ".config/vibeshine", ".config/sunshine"};

/* Selection happens only after permanently dropping privilege. Never silently
 * choose between two host identities, and never follow a legacy profile link. */
static int open_source_profile(int home, const char *selection) {
  if (!strcmp(selection, "machine-vibeshine")) {
    return open_beneath(home, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  int selected = -1;
  for (unsigned i = 0; i < sizeof(source_profiles) / sizeof(source_profiles[0]); ++i) {
    if (strcmp(selection, "auto") && strcmp(selection, source_profiles[i] + 8)) continue;
    const int candidate = open_beneath(home, source_profiles[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (candidate < 0) {
      if (errno == ENOENT) continue;
      if (selected >= 0) close(selected);
      return -1;
    }
    if (selected >= 0) {
      close(selected); close(candidate); errno = EEXIST;
      return -1;
    }
    selected = candidate;
  }
  if (selected < 0) errno = ENOENT;
  return selected;
}

enum {
  maximum_depth = 32,
  maximum_entries = 16384,
  maximum_seconds = 60,
};

static const uint64_t maximum_file_bytes = UINT64_C(67108864);
static const uint64_t maximum_total_bytes = UINT64_C(536870912);
static const uint64_t destination_free_space_reserve = UINT64_C(536870912);
static const uint64_t maximum_entry_overhead = UINT64_C(4096);

struct import_budget {
  uint64_t entries;
  uint64_t bytes;
  uint64_t maximum_bytes;
  uint64_t deadline_milliseconds;
  dev_t source_device;
  int destination_root;
};

static bool report_error(const char *message) {
  const int saved_errno = errno;
  fprintf(stderr, "vibepollo-profile-import: %s", message);
  if (saved_errno) fprintf(stderr, ": %s", strerror(saved_errno));
  fputc('\n', stderr);
  errno = saved_errno;
  return false;
}

static uint64_t monotonic_milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return (uint64_t) now.tv_sec * UINT64_C(1000) + (uint64_t) now.tv_nsec / UINT64_C(1000000);
}

static bool budget_is_available(struct import_budget *budget) {
  const uint64_t now = monotonic_milliseconds();
  if (!now || now > budget->deadline_milliseconds) {
    errno = ETIMEDOUT;
    return false;
  }
  return true;
}

static bool close_unrelated_descriptors(void) {
#ifdef SYS_close_range
  if (!syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_UNSHARE)) return true;
  if (errno != ENOSYS && errno != EINVAL) return false;
#endif
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit)) return false;
  rlim_t maximum = limit.rlim_cur;
  if (maximum == RLIM_INFINITY || maximum > UINT64_C(1048576)) maximum = UINT64_C(1048576);
  for (int descriptor = 3; (rlim_t) descriptor < maximum; ++descriptor) close(descriptor);
  return true;
}

static bool exact_mode(const struct stat *attributes, mode_t mode) {
  return (attributes->st_mode & 07777) == mode;
}

static bool validate_destination_path(const char *path, uid_t uid, gid_t gid,
                                      char **parent_result) {
  const size_t prefix_length = sizeof(destination_prefix) - 1;
  if (strncmp(path, destination_prefix, prefix_length)) return false;
  const char *slash = strchr(path + prefix_length, '/');
  if (!slash || strcmp(slash, destination_suffix)) return false;
  if ((size_t) (slash - (path + prefix_length)) != 6) return false;
  for (const unsigned char *cursor = (const unsigned char *) path + prefix_length;
       (const char *) cursor < slash; ++cursor) {
    if (!((*cursor >= '0' && *cursor <= '9') || (*cursor >= 'A' && *cursor <= 'Z') ||
          (*cursor >= 'a' && *cursor <= 'z'))) return false;
  }

  char *parent = strndup(path, (size_t) (slash - path));
  if (!parent) return false;
  struct stat attributes;
  if (lstat("/var/lib", &attributes) || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != 0 || attributes.st_gid != 0 || !exact_mode(&attributes, 0755) ||
      lstat(parent, &attributes) || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != 0 || attributes.st_gid != 0 || !exact_mode(&attributes, 0700) ||
      lstat(path, &attributes) || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != uid || attributes.st_gid != gid || !exact_mode(&attributes, 0700)) {
    free(parent);
    return false;
  }
  *parent_result = parent;
  return true;
}

static int open_beneath(int directory, const char *path, int flags) {
  const struct open_how how = {
    .flags = (uint64_t) flags,
    .resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV,
  };
  return (int) syscall(SYS_openat2, directory, path, &how, sizeof(how));
}

static bool same_snapshot(const struct stat *before, const struct stat *after) {
  return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
         before->st_mode == after->st_mode && before->st_uid == after->st_uid &&
         before->st_gid == after->st_gid && before->st_size == after->st_size &&
         before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
         before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
         before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
         before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static bool copy_directory(int source, int destination, unsigned int depth,
                           struct import_budget *budget);
static bool destination_copy_budget(int destination, uint64_t *maximum_bytes);

static bool charge_destination_budget(struct import_budget *budget,
                                      uint64_t amount) {
  uint64_t currently_available = 0;
  if (amount > budget->maximum_bytes ||
      budget->bytes > budget->maximum_bytes - amount) {
    errno = EFBIG;
    return false;
  }
  if (!destination_copy_budget(budget->destination_root,
                               &currently_available) ||
      amount > currently_available) {
    errno = ENOSPC;
    return false;
  }
  budget->bytes += amount;
  return true;
}

static bool copy_regular_file(int source_directory, int destination_directory,
                              const char *name, struct import_budget *budget) {
  const int source = open_beneath(source_directory, name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (source < 0) return false;
  struct stat before, after;
  if (fstat(source, &before) || !S_ISREG(before.st_mode) || before.st_dev != budget->source_device ||
      before.st_size < 0 || (uint64_t) before.st_size > maximum_file_bytes ||
      (uint64_t) before.st_size > budget->maximum_bytes ||
      budget->bytes > budget->maximum_bytes - (uint64_t) before.st_size) {
    close(source);
    errno = EPERM;
    return false;
  }
  const int destination = openat(destination_directory, name,
                                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (destination < 0) {
    close(source);
    return false;
  }

  bool success = true;
  uint64_t copied = 0;
  unsigned char buffer[65536];
  for (;;) {
    if (!budget_is_available(budget)) {
      success = false;
      break;
    }
    const ssize_t received = read(source, buffer, sizeof(buffer));
    if (!received) break;
    if (received < 0) {
      if (errno == EINTR) continue;
      success = false;
      break;
    }
    if (copied > maximum_file_bytes - (uint64_t) received) {
      errno = EFBIG;
      success = false;
      break;
    }
    if (!charge_destination_budget(budget, (uint64_t) received)) {
      success = false;
      break;
    }
    size_t offset = 0;
    while (offset < (size_t) received) {
      const ssize_t written = write(destination, buffer + offset, (size_t) received - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        success = false;
        break;
      }
      offset += (size_t) written;
    }
    if (!success) break;
    copied += (uint64_t) received;
  }
  if (success && (copied != (uint64_t) before.st_size || fstat(source, &after) ||
                  !same_snapshot(&before, &after) || fsync(destination))) {
    errno = ESTALE;
    success = false;
  }
  if (close(destination)) success = false;
  close(source);
  if (!success) unlinkat(destination_directory, name, 0);
  return success;
}

static bool copy_entry(int source_directory, int destination_directory,
                       const char *name, unsigned int depth,
                       struct import_budget *budget) {
  if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) {
    errno = EINVAL;
    return false;
  }
  if (++budget->entries > maximum_entries || !budget_is_available(budget)) {
    errno = E2BIG;
    return false;
  }
  if (!charge_destination_budget(budget, maximum_entry_overhead)) return false;

  int source = open_beneath(source_directory, name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (source >= 0) {
    if (depth >= maximum_depth || mkdirat(destination_directory, name, 0700)) {
      close(source);
      return false;
    }
    const int destination = openat(destination_directory, name,
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (destination < 0) {
      close(source);
      return false;
    }
    const bool success = copy_directory(source, destination, depth + 1, budget);
    close(destination);
    close(source);
    if (!success) unlinkat(destination_directory, name, AT_REMOVEDIR);
    return success;
  }
  if (errno != ENOTDIR) return false;
  return copy_regular_file(source_directory, destination_directory, name, budget);
}

static bool copy_directory(int source, int destination, unsigned int depth,
                           struct import_budget *budget) {
  struct stat before, after;
  if (fstat(source, &before) || !S_ISDIR(before.st_mode) ||
      before.st_dev != budget->source_device) return false;
  const int duplicate = dup(source);
  if (duplicate < 0) return false;
  DIR *directory = fdopendir(duplicate);
  if (!directory) {
    close(duplicate);
    return false;
  }

  bool success = true;
  errno = 0;
  for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
    if (!copy_entry(source, destination, entry->d_name, depth, budget)) {
      success = false;
      break;
    }
    errno = 0;
  }
  if (errno) success = false;
  if (closedir(directory)) success = false;
  if (success && (fstat(source, &after) || !same_snapshot(&before, &after) || fsync(destination))) {
    errno = ESTALE;
    success = false;
  }
  return success;
}

static bool drop_to_user(const struct passwd *account) {
  if (prctl(PR_SET_DUMPABLE, 0) ||
      prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
      initgroups(account->pw_name, account->pw_gid) ||
      setresgid(account->pw_gid, account->pw_gid, account->pw_gid) ||
      setresuid(account->pw_uid, account->pw_uid, account->pw_uid)) return false;
  cap_t empty = cap_init();
  if (!empty) return false;
  const int capability_result = cap_set_proc(empty);
  cap_free(empty);
  if (capability_result || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
      prctl(PR_SET_DUMPABLE, 0) || geteuid() != account->pw_uid || getegid() != account->pw_gid) return false;
  return true;
}

static bool destination_copy_budget(int destination, uint64_t *maximum_bytes) {
  struct statvfs filesystem;
  if (fstatvfs(destination, &filesystem)) return false;
  const uint64_t block_size = filesystem.f_frsize ?
                                (uint64_t) filesystem.f_frsize :
                                (uint64_t) filesystem.f_bsize;
  if (!block_size || (uint64_t) filesystem.f_bavail > UINT64_MAX / block_size) {
    errno = EOVERFLOW;
    return false;
  }
  const uint64_t available_bytes = (uint64_t) filesystem.f_bavail * block_size;
  if (available_bytes <= destination_free_space_reserve) {
    errno = ENOSPC;
    return false;
  }
  const uint64_t safe_bytes = available_bytes - destination_free_space_reserve;
  *maximum_bytes = safe_bytes < maximum_total_bytes ? safe_bytes : maximum_total_bytes;
  return *maximum_bytes > 0;
}

int main(int argc, char **argv) {
  umask(0077);
  const char *selection = argc == 4 ? argv[3] : "auto";
  const bool machine_source = !strcmp(selection, "machine-vibeshine");
  if ((argc != 3 && argc != 4) || geteuid() != 0 ||
      (strcmp(selection, "auto") && strcmp(selection, "vibepollo") &&
       strcmp(selection, "vibeshine") && strcmp(selection, "sunshine") && !machine_source)) {
    errno = EINVAL;
    report_error("usage: vibepollo-profile-import USER /var/lib/.vibepollo-profile.XXXXXX/incoming [auto|vibepollo|vibeshine|sunshine|machine-vibeshine]");
    return 2;
  }
  if (!close_unrelated_descriptors()) {
    report_error("could not close inherited descriptors");
    return 1;
  }

  long passwd_buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (passwd_buffer_size < 16384) passwd_buffer_size = 16384;
  if (passwd_buffer_size > 1048576) passwd_buffer_size = 1048576;
  char *passwd_buffer = malloc((size_t) passwd_buffer_size);
  struct passwd account_storage;
  struct passwd *account = NULL;
  if (!passwd_buffer || getpwnam_r(argv[1], &account_storage, passwd_buffer,
                                  (size_t) passwd_buffer_size, &account) ||
      !account || account->pw_uid == 0 || (!machine_source && account->pw_uid < 1000) ||
      (machine_source && strcmp(account->pw_name, "vibeshine")) ||
      !account->pw_dir || account->pw_dir[0] != '/') {
    free(passwd_buffer);
    errno = EINVAL;
    report_error("invalid desktop account");
    return 1;
  }
  char *destination_parent = NULL;
  if (!validate_destination_path(argv[2], account->pw_uid, account->pw_gid,
                                 &destination_parent)) {
    errno = EPERM;
    report_error("unsafe destination staging path");
    free(passwd_buffer);
    return 1;
  }

  const int home = open(machine_source ? "/var/lib/vibeshine" : account->pw_dir,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const int destination = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat home_attributes, destination_attributes;
  if (home < 0 || destination < 0 || fstat(home, &home_attributes) ||
      !S_ISDIR(home_attributes.st_mode) || home_attributes.st_uid != account->pw_uid ||
      fstat(destination, &destination_attributes) || !S_ISDIR(destination_attributes.st_mode) ||
      destination_attributes.st_uid != account->pw_uid ||
      destination_attributes.st_gid != account->pw_gid || !exact_mode(&destination_attributes, 0700)) {
    free(destination_parent);
    if (home >= 0) close(home);
    if (destination >= 0) close(destination);
    errno = EPERM;
    report_error("unsafe source home or destination directory");
    free(passwd_buffer);
    return 1;
  }
  free(destination_parent);

  if (!drop_to_user(account)) {
    close(home);
    close(destination);
    report_error("could not drop to the desktop account");
    free(passwd_buffer);
    return 1;
  }
  const int source = open_source_profile(home, selection);
  if (source < 0) {
    const int open_error = errno;
    close(home);
    close(destination);
    free(passwd_buffer);
    if (open_error == ENOENT && !strcmp(selection, "auto")) {
      /* A first installation has no desktop profile to import. The empty
       * staging directory becomes a fresh machine profile. */
      fprintf(stderr, "vibepollo-profile-import: no desktop profile to import; starting fresh\n");
      return 0;
    }
    errno = open_error;
    report_error(open_error == EEXIST ?
      "multiple legacy profiles exist; select one with sudo vibepollo migrate HOST; no identity was overwritten" :
      "could not open the confined legacy profile");
    return 1;
  }
  struct stat source_attributes;
  if (fstat(source, &source_attributes) || !S_ISDIR(source_attributes.st_mode) ||
      source_attributes.st_uid != account->pw_uid) {
    close(source);
    close(home);
    close(destination);
    errno = EPERM;
    report_error("desktop profile root is unsafe");
    free(passwd_buffer);
    return 1;
  }

  uint64_t copy_budget = 0;
  if (!destination_copy_budget(destination, &copy_budget)) {
    close(source);
    close(home);
    close(destination);
    report_error("destination filesystem lacks the required free-space reserve");
    free(passwd_buffer);
    return 1;
  }

  const uint64_t now = monotonic_milliseconds();
  struct import_budget budget = {
    .entries = 0,
    .bytes = 0,
    .maximum_bytes = copy_budget,
    .deadline_milliseconds = now + (uint64_t) maximum_seconds * UINT64_C(1000),
    .source_device = source_attributes.st_dev,
    .destination_root = destination,
  };
  const bool success = now && copy_directory(source, destination, 0, &budget);
  close(source);
  close(home);
  close(destination);
  free(passwd_buffer);
  if (!success) {
    report_error("confined profile copy failed");
    return 1;
  }
  return 0;
}
