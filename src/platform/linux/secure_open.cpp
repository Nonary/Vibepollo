/**
 * @file src/platform/linux/secure_open.cpp
 * @brief Descriptor-relative fallback for kernels or sandboxes without openat2().
 */

#include "secure_open.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <linux/limits.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace platf::linux_security {
  namespace {
    constexpr std::size_t maximum_path_bytes = PATH_MAX - 1;
    constexpr std::size_t maximum_path_depth = 64;

    class owned_descriptor_t {
    public:
      owned_descriptor_t() = default;

      explicit owned_descriptor_t(const int descriptor):
          descriptor_ {descriptor} {}

      owned_descriptor_t(const owned_descriptor_t &) = delete;
      owned_descriptor_t &operator=(const owned_descriptor_t &) = delete;

      owned_descriptor_t(owned_descriptor_t &&other) noexcept:
          descriptor_ {other.release()} {}

      owned_descriptor_t &operator=(owned_descriptor_t &&other) noexcept {
        if (this != &other) {
          reset(other.release());
        }
        return *this;
      }

      ~owned_descriptor_t() {
        if (descriptor_ >= 0) {
          close(descriptor_);
        }
      }

      int get() const noexcept {
        return descriptor_;
      }

      int release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
      }

      void reset(const int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
          close(descriptor_);
        }
        descriptor_ = descriptor;
      }

    private:
      int descriptor_ {-1};
    };

    std::optional<std::uint64_t> mount_id(const int descriptor) {
      struct statx attributes {};
      if (statx(descriptor, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_MNT_ID, &attributes) != 0 || (attributes.stx_mask & STATX_MNT_ID) == 0) {
        return std::nullopt;
      }
      return attributes.stx_mnt_id;
    }

    bool valid_component(const std::filesystem::path &component) {
      return !component.empty() && component != "." && component != ".." &&
             component.native().size() <= NAME_MAX &&
             component.native().find('/') == std::string::npos &&
             component.native().find('\0') == std::string::npos;
    }

    bool trusted_directory(
      const struct stat &attributes,
      const dev_t root_device,
      const uid_t trusted_owner,
      const bool require_private_directories
    ) {
      return S_ISDIR(attributes.st_mode) &&
             attributes.st_dev == root_device &&
             attributes.st_uid == trusted_owner &&
             (attributes.st_mode & 07000) == 0 &&
             (attributes.st_mode & 0022) == 0 &&
             (!require_private_directories ||
              (attributes.st_mode & 0777) == 0700);
    }
  }  // namespace

  int open_readonly_beneath(
    const int root_descriptor,
    const std::filesystem::path &relative_path,
    const dev_t root_device,
    const uid_t trusted_owner,
    const bool require_private_directories
  ) {
    const auto raw_path = relative_path.native();
    if (root_descriptor < 0 || raw_path.empty() || raw_path.size() > maximum_path_bytes || raw_path.front() == '/' || raw_path.back() == '/' || raw_path.find("//") != std::string::npos || raw_path.find('\0') != std::string::npos || relative_path.is_absolute()) {
      errno = EINVAL;
      return -1;
    }

    const auto trusted_mount = mount_id(root_descriptor);
    if (!trusted_mount) {
      errno = ENOTSUP;
      return -1;
    }

    std::vector<std::filesystem::path> components;
    for (const auto &component : relative_path) {
      if (!valid_component(component) || components.size() >= maximum_path_depth) {
        errno = EINVAL;
        return -1;
      }
      components.push_back(component);
    }
    if (components.empty()) {
      errno = EINVAL;
      return -1;
    }

    int current_descriptor = root_descriptor;
    owned_descriptor_t owned_directory;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
      const auto component = components[index].native();
      owned_descriptor_t next_directory {openat(
        current_descriptor,
        component.c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      )};
      if (next_directory.get() < 0) {
        return -1;
      }

      struct stat attributes {};
      const bool safe = fstat(next_directory.get(), &attributes) == 0 &&
                        trusted_directory(
                          attributes,
                          root_device,
                          trusted_owner,
                          require_private_directories
                        ) &&
                        mount_id(next_directory.get()) == trusted_mount;
      if (!safe) {
        errno = EACCES;
        return -1;
      }

      owned_directory = std::move(next_directory);
      current_descriptor = owned_directory.get();
    }

    const auto leaf = components.back().native();
    owned_descriptor_t descriptor {openat(
      current_descriptor,
      leaf.c_str(),
      O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW
    )};
    if (descriptor.get() < 0) {
      return -1;
    }

    struct stat leaf_attributes {};
    if (fstat(descriptor.get(), &leaf_attributes) != 0 || leaf_attributes.st_dev != root_device || mount_id(descriptor.get()) != trusted_mount) {
      errno = EXDEV;
      return -1;
    }
    return descriptor.release();
  }
}  // namespace platf::linux_security
