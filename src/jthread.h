/**
 * @file src/jthread.h
 * @brief std::jthread and std::stop_token, or small stand-ins where the standard library lacks them.
 * @details Apple's libc++ has no std::jthread before Xcode 16.3, and only behind
 *          -fexperimental-library after that, while macOS 14 build hosts top out at Xcode 16.2.
 *          util::jthread and util::stop_token are the standard types wherever those exist.
 */
#pragma once

// standard includes
#include <thread>
#include <version>

#ifdef __cpp_lib_jthread
  #include <stop_token>

namespace util {
  using std::jthread;
  using std::stop_token;
}  // namespace util

#else
  #include <atomic>
  #include <memory>
  #include <type_traits>
  #include <utility>

namespace util {
  /**
   * @brief Stand-in for std::stop_token: tells a util::jthread's function whether it was asked to stop.
   */
  class stop_token {
  public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
      return state_ && state_->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
      return state_ != nullptr;
    }

  private:
    friend class jthread;

    explicit stop_token(std::shared_ptr<std::atomic<bool>> state) noexcept:
        state_ {std::move(state)} {}

    std::shared_ptr<std::atomic<bool>> state_;
  };

  /**
   * @brief Stand-in for std::jthread: passes a stop_token to a function that takes one, and when
   *        destroyed or assigned over, asks its thread to stop and joins it.
   */
  class jthread {
  public:
    jthread() noexcept = default;

    template<class F, class... Args>
      requires(!std::is_same_v<std::remove_cvref_t<F>, jthread>)
    explicit jthread(F &&f, Args &&...args):
        stop_state_ {std::make_shared<std::atomic<bool>>(false)} {
      if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, std::decay_t<Args>...>) {
        thread_ = std::thread(std::forward<F>(f), stop_token {stop_state_}, std::forward<Args>(args)...);
      } else {
        thread_ = std::thread(std::forward<F>(f), std::forward<Args>(args)...);
      }
    }

    jthread(const jthread &) = delete;
    jthread &operator=(const jthread &) = delete;

    jthread(jthread &&other) noexcept = default;

    jthread &operator=(jthread &&other) noexcept {
      if (this != &other) {
        stop_and_join();
        thread_ = std::move(other.thread_);
        stop_state_ = std::move(other.stop_state_);
      }
      return *this;
    }

    ~jthread() {
      stop_and_join();
    }

    [[nodiscard]] bool joinable() const noexcept {
      return thread_.joinable();
    }

    void join() {
      thread_.join();
    }

    void detach() {
      thread_.detach();
    }

    [[nodiscard]] std::thread::id get_id() const noexcept {
      return thread_.get_id();
    }

    [[nodiscard]] stop_token get_stop_token() const noexcept {
      return stop_token {stop_state_};
    }

    bool request_stop() noexcept {
      return stop_state_ && !stop_state_->exchange(true, std::memory_order_acq_rel);
    }

  private:
    void stop_and_join() {
      if (thread_.joinable()) {
        request_stop();
        thread_.join();
      }
    }

    std::thread thread_;
    std::shared_ptr<std::atomic<bool>> stop_state_;
  };
}  // namespace util
#endif
