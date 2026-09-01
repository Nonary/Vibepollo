/**
 * @file src/platform/linux/mangohud_state.h
 * @brief Runtime handoff used by the Steam last-mile MangoHud wrapper.
 */
#pragma once

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace platf::mangohud {

  class owned_fd final {
  public:
    owned_fd() = default;
    explicit owned_fd(int value): value_ {value} {}
    owned_fd(const owned_fd &) = delete;
    owned_fd &operator=(const owned_fd &) = delete;
    owned_fd(owned_fd &&other) noexcept: value_ {other.value_} {
      other.value_ = -1;
    }
    owned_fd &operator=(owned_fd &&other) noexcept {
      if (this != &other) {
        if (value_ >= 0) close(value_);
        value_ = other.value_;
        other.value_ = -1;
      }
      return *this;
    }
    ~owned_fd() {
      if (value_ >= 0) close(value_);
    }
    [[nodiscard]] int get() const { return value_; }
    [[nodiscard]] explicit operator bool() const { return value_ >= 0; }

  private:
    int value_;
  };

  inline std::filesystem::path state_directory() {
    if (const char *override_dir = std::getenv("VIBEPOLLO_MANGOHUD_STATE_DIR");
        override_dir && *override_dir) {
      return override_dir;
    }
    if (const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR"); runtime_dir && *runtime_dir) {
      return std::filesystem::path(runtime_dir) / "vibepollo" / "mangohud";
    }
    if (const char *config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home) {
      return std::filesystem::path(config_home) / "vibepollo" / "mangohud-runtime";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
      return std::filesystem::path(home) / ".config" / "vibepollo" / "mangohud-runtime";
    }
    return {};
  }

  inline bool valid_steam_app_id(std::string_view app_id) {
    if (app_id.empty()) {
      return false;
    }
    for (const char ch : app_id) {
      if (ch < '0' || ch > '9') {
        return false;
      }
    }
    return app_id != "0";
  }

  inline std::filesystem::path state_path(std::string_view app_id) {
    const auto directory = state_directory();
    if (directory.empty() || !valid_steam_app_id(app_id)) {
      return {};
    }
    return directory / (std::string(app_id) + ".state");
  }

  inline bool private_directory(int descriptor, uid_t owner) {
    struct stat attributes {};
    return descriptor >= 0 && fstat(descriptor, &attributes) == 0 &&
           S_ISDIR(attributes.st_mode) && attributes.st_uid == owner &&
           (attributes.st_mode & 0777) == 0700;
  }

  inline owned_fd open_private_directory_at(
    int parent,
    const char *name,
    uid_t owner,
    bool create
  ) {
    int descriptor = openat(
      parent,
      name,
      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (descriptor < 0 && create && errno == ENOENT &&
        mkdirat(parent, name, 0700) == 0) {
      descriptor = openat(
        parent,
        name,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
    }
    owned_fd result {descriptor};
    return private_directory(result.get(), owner) ? std::move(result) : owned_fd {};
  }

  inline std::pair<owned_fd, owned_fd> open_runtime_state_directories(bool create) {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/') {
      return std::pair<owned_fd, owned_fd> {};
    }
    owned_fd runtime_fd {
      open(runtime, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
    };
    const uid_t owner = getuid();
    if (!private_directory(runtime_fd.get(), owner)) {
      return std::pair<owned_fd, owned_fd> {};
    }
    auto vibepollo_fd = open_private_directory_at(
      runtime_fd.get(), "vibepollo", owner, create
    );
    if (!vibepollo_fd) {
      return std::pair<owned_fd, owned_fd> {};
    }
    auto state_fd = open_private_directory_at(
      vibepollo_fd.get(), "mangohud", owner, create
    );
    if (!state_fd) {
      return std::pair<owned_fd, owned_fd> {};
    }
    return {std::move(runtime_fd), std::move(state_fd)};
  }

  inline std::string serialize_state(
    std::string_view provider,
    std::string_view limit,
    std::string_view preset,
    bool always_show_graph,
    std::string_view limiter_method,
    std::chrono::system_clock::time_point expires_at
  ) {
    const bool standard_preset = preset == "1" || preset == "2" || preset == "3" || preset == "4";
    const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
      expires_at.time_since_epoch()
    ).count();
    return "version=2\nprovider=" + std::string(provider) +
           "\nlimit=" + std::string(limit) +
           "\npreset=" + (standard_preset ? std::string(preset) : "custom") +
           "\nalways_show_graph=" + (always_show_graph ? "1" : "0") +
           "\nlimiter_method=" + (limiter_method == "early" ? "early" : "late") +
           "\nowner_pid=" + std::to_string(static_cast<unsigned long>(getpid())) +
           "\nexpires=" + std::to_string(expires) + "\n";
  }

  inline std::filesystem::path write_state(
    std::string_view app_id,
    std::string_view provider,
    std::string_view limit,
    std::string_view preset,
    bool always_show_graph,
    std::string_view limiter_method = "late"
  ) {
    const auto path = state_path(app_id);
    if (path.empty()) {
      return {};
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return {};
    }
    std::filesystem::permissions(
      path.parent_path(),
      std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace,
      ec
    );
    if (ec) {
      return {};
    }

    auto temporary = path;
    temporary += ".tmp-" + std::to_string(static_cast<unsigned long>(getpid()));
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) {
        return {};
      }
      output << serialize_state(
        provider,
        limit,
        preset,
        always_show_graph,
        limiter_method,
        std::chrono::system_clock::now() + std::chrono::hours(1)
      );
      if (!output) {
        output.close();
        std::filesystem::remove(temporary, ec);
        return {};
      }
    }
    std::filesystem::permissions(
      temporary,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      ec
    );
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return {};
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
      std::filesystem::remove(temporary, ec);
      return {};
    }
    return path;
  }

  inline std::filesystem::path write_runtime_state(
    std::string_view app_id,
    std::string_view provider,
    std::string_view limit,
    std::string_view preset,
    bool always_show_graph,
    std::string_view limiter_method = "late"
  ) {
    if (!valid_steam_app_id(app_id)) {
      return {};
    }
    auto [runtime_fd, state_fd] = open_runtime_state_directories(true);
    if (!runtime_fd || !state_fd) {
      return {};
    }
    const auto contents = serialize_state(
      provider,
      limit,
      preset,
      always_show_graph,
      limiter_method,
      std::chrono::system_clock::now() + std::chrono::hours(1)
    );
    if (contents.empty() || contents.size() > 4096) {
      return {};
    }

    const std::string final_name = std::string(app_id) + ".state";
    std::string temporary_name;
    owned_fd temporary;
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
      temporary_name = final_name + ".tmp-" + std::to_string(getpid()) +
                       "-" + std::to_string(attempt);
      temporary = owned_fd {openat(
        state_fd.get(),
        temporary_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600
      )};
      if (temporary || errno != EEXIST) {
        break;
      }
    }
    if (!temporary) {
      return {};
    }

    std::size_t written = 0;
    while (written < contents.size()) {
      const auto result = write(
        temporary.get(), contents.data() + written, contents.size() - written
      );
      if (result > 0) {
        written += static_cast<std::size_t>(result);
      } else if (result < 0 && errno == EINTR) {
        continue;
      } else {
        (void) unlinkat(state_fd.get(), temporary_name.c_str(), 0);
        return {};
      }
    }
    struct stat attributes {};
    if (fchmod(temporary.get(), 0600) != 0 ||
        fstat(temporary.get(), &attributes) != 0 ||
        !S_ISREG(attributes.st_mode) || attributes.st_uid != getuid() ||
        (attributes.st_mode & 0777) != 0600 || fsync(temporary.get()) != 0 ||
        renameat(state_fd.get(), temporary_name.c_str(),
                 state_fd.get(), final_name.c_str()) != 0) {
      (void) unlinkat(state_fd.get(), temporary_name.c_str(), 0);
      return {};
    }
    (void) fsync(state_fd.get());
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::filesystem::path(runtime) / "vibepollo" / "mangohud" /
           final_name;
  }

  inline void remove_runtime_state(std::string_view app_id) {
    if (!valid_steam_app_id(app_id)) {
      return;
    }
    auto [runtime_fd, state_fd] = open_runtime_state_directories(false);
    if (!runtime_fd || !state_fd) {
      return;
    }
    const std::string final_name = std::string(app_id) + ".state";
    (void) unlinkat(state_fd.get(), final_name.c_str(), 0);
  }

  inline void remove_state(std::string_view app_id) {
    const auto path = state_path(app_id);
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

}  // namespace platf::mangohud
