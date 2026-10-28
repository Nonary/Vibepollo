#pragma once

#include "src/platform/windows/display_helper_borrow_gate.h"

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <windows.h>
  #include <sddl.h>

  #include <atomic>
  #include <condition_variable>
  #include <memory>
  #include <string>
  #include <thread>

namespace display_helper::borrow {
  inline std::atomic<bool> &helper_process() { static std::atomic<bool> value {false}; return value; }

  inline std::optional<process_context_t> process_context(HANDLE process, DWORD pid) {
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return std::nullopt;
    return process_context_t {pid, (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
  }

  inline process_context_t current_context() {
    return process_context(GetCurrentProcess(), GetCurrentProcessId()).value_or(process_context_t {});
  }

  inline std::optional<bool> controller_alive(const process_context_t &context) {
    if (!context.valid()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, context.pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER ? std::optional<bool>(false) : std::nullopt;
    const auto actual = process_context(process, context.pid);
    const auto status = WaitForSingleObject(process, 0);
    CloseHandle(process);
    if (!actual || status == WAIT_FAILED) return std::nullopt;
    return *actual == context && status == WAIT_TIMEOUT;
  }

  // Helper native writers use one machine-wide display mutation object.
  // The host retains it in addition to its own drained process gate. Never
  // fall back to a different
  // namespace/object after an access error. That would bypass a live inhibit.
  // The helper's existing user, SYSTEM and administrators retain access. This
  // grants no display operation or IPC privilege to another account.
  inline HANDLE open_mutation_mutex(const process_context_t &helper) {
    HANDLE process = helper.pid == GetCurrentProcessId() ? GetCurrentProcess() :
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, helper.pid);
    if (!process) return nullptr;
    const auto actual = process_context(process, helper.pid);
    HANDLE token = nullptr;
    const bool opened = actual && *actual == helper && OpenProcessToken(process, TOKEN_QUERY, &token);
    if (process != GetCurrentProcess()) CloseHandle(process);
    if (!opened) return nullptr;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> user(size);
    const bool queried = size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (!queried) return nullptr;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid)) return nullptr;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    HANDLE mutex = CreateMutexW(&attributes, FALSE, L"Global\\SunshineDisplayHelperMutationV1");
    LocalFree(descriptor);
    return mutex;
  }

  inline thread_local std::size_t native_mutex_depth = 0;

  class native_mutation_scope_t {
  public:
    native_mutation_scope_t() = default;
    native_mutation_scope_t(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t &operator=(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t(native_mutation_scope_t &&other) noexcept
      : local_(std::move(other.local_)), mutex_(std::exchange(other.mutex_, nullptr)) {}
    ~native_mutation_scope_t() { if (mutex_) { --native_mutex_depth; ReleaseMutex(mutex_); CloseHandle(mutex_); } }
    explicit operator bool() const { return static_cast<bool>(local_); }
    // All host paths close/drain the shared process gate. Helper native work
    // additionally owns the cross-process mutex retained by a borrowed host.
    // Never take that mutex around a host IPC dispatch or worker join: the
    // helper/child needs to finish its native work before those waits return.
    static native_mutation_scope_t enter(bool wait, const std::function<bool()> &cancelled = {}) {
      native_mutation_scope_t result;
      auto &gate = process_gate();
      const bool nested = native_mutex_depth != 0;
      result.local_ = wait ? gate.wait_enter(cancelled) : gate.try_enter();
      if (!result.local_ || !helper_process().load(std::memory_order_acquire)) return result;
      result.mutex_ = open_mutation_mutex(current_context());
      if (!result.mutex_) { result.local_.reset(); return result; }
      while (true) {
        const auto status = WaitForSingleObject(result.mutex_, 25);
        if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) { ++native_mutex_depth; return result; }
        if (status != WAIT_TIMEOUT || (cancelled && cancelled()) || (!nested && (!wait || gate.inhibited()))) {
          CloseHandle(result.mutex_); result.mutex_ = nullptr; result.local_.reset(); return result;
        }
      }
    }
  private:
    gate_t::scope_t local_;
    HANDLE mutex_ = nullptr;
  };

  // Windows mutex ownership belongs to a thread, whereas lease holds cross
  // pending/capture/teardown threads. Keep the owning thread alive until the
  // last shared kernel hold is released. No stale protocol RELEASE can unlock
  // this thread's handle. The OS abandons it only after controller death.
  class kernel_hold_t {
  public:
    static std::shared_ptr<kernel_hold_t> acquire(const process_context_t &helper,
      std::chrono::steady_clock::time_point deadline, bool &completion_unknown,
      const std::function<bool()> &cancelled = {}) {
      completion_unknown = false;
      auto value = std::shared_ptr<kernel_hold_t>(new kernel_hold_t(helper, deadline));
      std::unique_lock lock(value->mutex_);
      while (!value->finished_acquire_) {
        if ((cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline) {
          value->release_ = true; value->cv_.notify_all(); lock.unlock();
          if (value->worker_.joinable()) value->worker_.join();
          completion_unknown = value->uncertain_;
          value.reset(); return {};
        }
        value->cv_.wait_for(lock, std::chrono::milliseconds(10));
      }
      const bool acquired = value->acquired_;
      completion_unknown = value->uncertain_;
      lock.unlock();
      return acquired ? value : nullptr;
    }
    ~kernel_hold_t() {
      { std::lock_guard lock(mutex_); release_ = true; }
      cv_.notify_all();
      if (worker_.joinable()) worker_.join();
    }
  private:
    kernel_hold_t(process_context_t helper, std::chrono::steady_clock::time_point deadline)
      : worker_([this, helper, deadline] {
        HANDLE native = open_mutation_mutex(helper);
        bool acquired = false, uncertain = false;
        if (native) {
          while (std::chrono::steady_clock::now() < deadline) {
            { std::lock_guard lock(mutex_); if (release_) break; }
            const auto status = WaitForSingleObject(native, 25);
            if (status == WAIT_OBJECT_0) { acquired = true; break; }
            // A helper worker abandoned inside native work is uncertainty,
            // not successful borrow quiescence. Release but reject admission.
            if (status == WAIT_ABANDONED) { uncertain = true; ReleaseMutex(native); break; }
            if (status != WAIT_TIMEOUT) break;
          }
        }
        std::unique_lock lock(mutex_);
        acquired_ = acquired; uncertain_ = uncertain; finished_acquire_ = true; cv_.notify_all();
        if (acquired) cv_.wait(lock, [this] { return release_; });
        lock.unlock();
        if (acquired) ReleaseMutex(native);
        if (native) CloseHandle(native);
      }) {}
    std::mutex mutex_;
    std::condition_variable cv_;
    bool acquired_ = false, uncertain_ = false, finished_acquire_ = false, release_ = false;
    std::thread worker_;
  };
}
#endif
