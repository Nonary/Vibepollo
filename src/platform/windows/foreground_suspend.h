/**
 * @file src/platform/windows/foreground_suspend.h
 * @brief Conservative foreground-process suspension for disconnect handling.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace platf::foreground_suspend {

  struct identity_t {
    std::uint32_t pid {0};
    std::string process_path;
    std::string executable_basename;
    std::optional<std::uint64_t> creation_time;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> active_session_id;
    std::string owner_sid;
    std::string active_user_sid;
    std::string system_root;
    std::optional<bool> critical;
    std::optional<bool> protected_process;
  };

  enum class rejection_reason_e {
    none,
    missing_foreground_window,
    invalid_foreground_window,
    shell_window,
    invalid_pid,
    own_process,
    open_denied,
    missing_identity,
    critical_process,
    protected_process,
    wrong_session,
    wrong_user,
    system_path,
    system_process,
    known_launcher,
    suspend_failed,
  };

  struct policy_result_t {
    bool allowed {false};
    rejection_reason_e reason {rejection_reason_e::missing_identity};
  };

  [[nodiscard]] policy_result_t evaluate_policy(const identity_t &identity);
  [[nodiscard]] bool is_known_launcher_basename(std::string_view basename);
  [[nodiscard]] bool is_known_system_basename(std::string_view basename);
  [[nodiscard]] bool path_is_under_directory(std::string_view path, std::string_view directory);
  [[nodiscard]] std::string_view describe_rejection(rejection_reason_e reason);

  enum class target_selection_e {
    none,
    owned_process_group,
    foreground_process,
  };

  struct selection_input_t {
    bool placeholder {false};
    bool owned_group_live {false};
    bool owned_group_is_launcher {false};
    bool foreground_allowed {false};
  };

  [[nodiscard]] target_selection_e select_target(const selection_input_t &input);

  enum class owned_group_scan_e {
    clear,
    contains_launcher,
    unknown,
  };

  [[nodiscard]] owned_group_scan_e scan_owned_group(std::uintptr_t native_job_handle);

  enum class resume_result_e {
    resumed,
    exited,
    failed,
  };

  struct acquire_result_t;

  class target_t {
  public:
    target_t() = default;
    // Internal module constructor used after a fully validated native handle has
    // been suspended. Callers should use acquire_and_suspend_foreground().
    target_t(void *process_handle, identity_t identity, bool suspended);
    target_t(const target_t &) = delete;
    target_t &operator=(const target_t &) = delete;
    target_t(target_t &&other) noexcept;
    target_t &operator=(target_t &&other) noexcept;
    ~target_t();

    [[nodiscard]] std::uint32_t pid() const noexcept;
    [[nodiscard]] const std::string &process_path() const noexcept;
    [[nodiscard]] const std::string &executable_basename() const noexcept;
    [[nodiscard]] std::uint64_t creation_time() const noexcept;
    [[nodiscard]] std::uint32_t session_id() const noexcept;
    [[nodiscard]] const std::string &owner_sid() const noexcept;
    [[nodiscard]] bool suspended() const noexcept;
    resume_result_e resume() noexcept;

#ifdef SUNSHINE_TESTS
    static target_t for_tests(
      std::uint32_t pid,
      std::string basename,
      std::function<resume_result_e()> resume_override
    );
#endif

  private:
    void release() noexcept;

    void *process_handle_ {nullptr};
    identity_t identity_;
    bool suspended_ {false};
#ifdef SUNSHINE_TESTS
    std::function<resume_result_e()> resume_override_;
#endif

  };

  struct acquire_result_t {
    std::optional<target_t> target;
    rejection_reason_e reason {rejection_reason_e::missing_identity};
    std::uint32_t pid {0};
    std::string executable_basename;
  };

  [[nodiscard]] acquire_result_t acquire_and_suspend_foreground();

  class foreground_target_slot_t {
  public:
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const target_t *target() const noexcept;
    bool attach(target_t target);
    resume_result_e recover() noexcept;

  private:
    std::optional<target_t> target_;
  };

}  // namespace platf::foreground_suspend
