/**
 * @file src/thread_safe.h
 * @brief Declarations for thread-safe data structures.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

// local includes
#include "utility.h"

namespace safe {
  namespace detail {
    // Advisory state for per-frame polls. Payloads and all state writes remain
    // protected by the owning mailbox's mutex, but observing an empty/running
    // mailbox must not block capture behind a descheduled producer or consumer.
    // Publish only after changing the payload; readers must still lock before
    // accessing it and cannot treat peek() as a reservation.
    class poll_state_t {
    public:
      bool running() const {
        return _state.load(std::memory_order_acquire) != state_e::stopped;
      }

      bool peek() const {
        return _state.load(std::memory_order_acquire) == state_e::ready;
      }

      void set_ready(bool ready) {
        _state.store(ready ? state_e::ready : state_e::empty, std::memory_order_release);
      }

      void stop() {
        _state.store(state_e::stopped, std::memory_order_release);
      }

    private:
      enum class state_e : std::uint8_t {
        stopped,
        empty,
        ready
      };
      static_assert(std::atomic<state_e>::is_always_lock_free);
      std::atomic<state_e> _state {state_e::empty};
    };
  }  // namespace detail

  template<class T>
  class event_t {
  public:
    using status_t = util::optional_t<T>;

    template<class... Args>
    void raise(Args &&...args) {
      std::lock_guard lg {_lock};
      if (!_poll_state.running()) {
        return;
      }

      if constexpr (std::is_same_v<std::optional<T>, status_t>) {
        _status = std::make_optional<T>(std::forward<Args>(args)...);
      } else {
        _status = status_t {std::forward<Args>(args)...};
      }
      _poll_state.set_ready((bool) _status);
      _generation.fetch_add(1, std::memory_order_release);

      _cv.notify_all();
    }

    // pop and view should not be used interchangeably
    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      _poll_state.set_ready(false);
      return val;
    }

    // pop and view should not be used interchangeably
    template<typename Rep, typename Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock, std::defer_lock};
      if (delay <= decltype(delay)::zero()) {
        if (!peek() || !ul.try_lock()) {
          return util::false_v<status_t>;
        }
      } else {
        ul.lock();
      }

      // Another consumer may have won after peek(). Never enter a timed wait
      // for a nonpositive poll, even when it races with that consumer.
      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }
      if (!_status) {
        if (delay <= decltype(delay)::zero() || !_cv.wait_for(ul, delay, [this] {
              return (bool) _status || !_poll_state.running();
            }) ||
            !_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_status);
      _status = util::false_v<status_t>;
      _poll_state.set_ready(false);
      return val;
    }

    // pop and view should not be used interchangeably
    status_t view() {
      std::unique_lock ul {_lock};

      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        _cv.wait(ul);

        if (!_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    // pop and view should not be used interchangeably
    template<class Rep, class Period>
    status_t view(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock, std::defer_lock};
      if (delay <= decltype(delay)::zero()) {
        if (!peek() || !ul.try_lock()) {
          return util::false_v<status_t>;
        }
      } else {
        ul.lock();
      }

      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }

      while (!_status) {
        if (delay <= decltype(delay)::zero() || _cv.wait_for(ul, delay) == std::cv_status::timeout || !_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      return _status;
    }

    bool peek() {
      return _poll_state.peek();
    }

    [[nodiscard]] std::uint64_t generation() const {
      return _generation.load(std::memory_order_acquire);
    }

    status_t view_if_newer(std::uint64_t &observed_generation) {
      if (generation() == observed_generation) {
        return util::false_v<status_t>;
      }
      std::lock_guard lg {_lock};
      const auto current_generation = generation();
      if (!_poll_state.running() || !_status || observed_generation == current_generation) {
        return util::false_v<status_t>;
      }

      observed_generation = current_generation;
      return _status;
    }

    void stop() {
      std::lock_guard lg {_lock};

      _poll_state.stop();

      _cv.notify_all();
    }

    void reset() {
      std::lock_guard lg {_lock};

      _status = util::false_v<status_t>;
      _poll_state.set_ready(false);
    }

    [[nodiscard]] bool running() const {
      return _poll_state.running();
    }

  private:
    detail::poll_state_t _poll_state;
    status_t _status {util::false_v<status_t>};
    std::atomic<std::uint64_t> _generation {};

    std::condition_variable _cv;
    std::mutex _lock;
  };

  template<class T>
  class alarm_raw_t {
  public:
    using status_t = util::optional_t<T>;

    void ring(const status_t &status) {
      std::lock_guard lg(_lock);

      _status = status;
      _rang = true;
      _cv.notify_one();
    }

    void ring(status_t &&status) {
      std::lock_guard lg(_lock);

      _status = std::move(status);
      _rang = true;
      _cv.notify_one();
    }

    template<class Rep, class Period>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this]() {
        return _rang;
      });
    }

    template<class Rep, class Period, class Pred>
    auto wait_for(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_for(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    template<class Rep, class Period>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this]() {
        return _rang;
      });
    }

    template<class Rep, class Period, class Pred>
    auto wait_until(const std::chrono::duration<Rep, Period> &rel_time, Pred &&pred) {
      std::unique_lock ul(_lock);

      return _cv.wait_until(ul, rel_time, [this, &pred]() {
        return _rang || pred();
      });
    }

    auto wait() {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this]() {
        return _rang;
      });
    }

    template<class Pred>
    auto wait(Pred &&pred) {
      std::unique_lock ul(_lock);
      _cv.wait(ul, [this, &pred]() {
        return _rang || pred();
      });
    }

    const status_t &status() const {
      return _status;
    }

    status_t &status() {
      return _status;
    }

    void reset() {
      _status = status_t {};
      _rang = false;
    }

  private:
    std::mutex _lock;
    std::condition_variable _cv;

    status_t _status {util::false_v<status_t>};
    bool _rang {false};
  };

  template<class T>
  using alarm_t = std::shared_ptr<alarm_raw_t<T>>;

  template<class T>
  alarm_t<T> make_alarm() {
    return std::make_shared<alarm_raw_t<T>>();
  }

  template<class T>
  class queue_t {
  public:
    using status_t = util::optional_t<T>;

    queue_t(std::uint32_t max_elements = 32):
        _max_elements {max_elements} {
    }

    template<class... Args>
    void raise(Args &&...args) {
      std::lock_guard ul {_lock};

      if (!_poll_state.running()) {
        return;
      }

      if (_queue.size() == _max_elements) {
        _queue.clear();
        _poll_state.set_ready(false);
      }

      _queue.emplace_back(std::forward<Args>(args)...);
      _poll_state.set_ready(true);

      _cv.notify_all();
    }

    template<class... Args>
    bool try_raise(Args &&...args) {
      std::lock_guard ul {_lock};

      if (!_poll_state.running() || _queue.size() >= _max_elements) {
        return false;
      }

      _queue.emplace_back(std::forward<Args>(args)...);
      _poll_state.set_ready(true);
      _cv.notify_all();
      return true;
    }

    bool peek() {
      return _poll_state.peek();
    }

    template<class Rep, class Period>
    bool wait_for_data(std::chrono::duration<Rep, Period> delay) {
      if (delay <= decltype(delay)::zero()) {
        return peek();
      }
      std::unique_lock ul {_lock};
      return _cv.wait_for(ul, delay, [this] {
        return !_queue.empty() || !_poll_state.running();
      }) && _poll_state.running() &&
             !_queue.empty();
    }

    template<class Rep, class Period>
    status_t pop(std::chrono::duration<Rep, Period> delay) {
      std::unique_lock ul {_lock, std::defer_lock};
      if (delay <= decltype(delay)::zero()) {
        if (!peek() || !ul.try_lock()) {
          return util::false_v<status_t>;
        }
      } else {
        ul.lock();
      }

      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        // A zero-duration condition-variable wait can still yield to the
        // scheduler on Windows. Keep nonblocking capture polls out of it.
        if (delay <= decltype(delay)::zero()) {
          return util::false_v<status_t>;
        }
        if (_cv.wait_for(ul, delay) == std::cv_status::timeout || !_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));
      _poll_state.set_ready(!_queue.empty());

      return val;
    }

    status_t pop() {
      std::unique_lock ul {_lock};

      if (!_poll_state.running()) {
        return util::false_v<status_t>;
      }

      while (_queue.empty()) {
        _cv.wait(ul);

        if (!_poll_state.running()) {
          return util::false_v<status_t>;
        }
      }

      auto val = std::move(_queue.front());
      _queue.erase(std::begin(_queue));
      _poll_state.set_ready(!_queue.empty());

      return val;
    }

    std::vector<T> &unsafe() {
      return _queue;
    }

    void stop() {
      std::lock_guard lg {_lock};

      _poll_state.stop();

      _cv.notify_all();
    }

    void reset() {
      std::lock_guard lg {_lock};

      _queue.clear();
      _poll_state.set_ready(false);
    }

    [[nodiscard]] bool running() const {
      return _poll_state.running();
    }

  private:
    detail::poll_state_t _poll_state;
    std::uint32_t _max_elements;

    std::mutex _lock;
    std::condition_variable _cv;

    std::vector<T> _queue;
  };

  template<class T>
  class shared_t {
  public:
    using element_type = T;

    using construct_f = std::function<int(element_type &)>;
    using destruct_f = std::function<void(element_type &)>;

    struct ptr_t {
      shared_t *owner;

      ptr_t():
          owner {nullptr} {
      }

      explicit ptr_t(shared_t *owner):
          owner {owner} {
      }

      ptr_t(ptr_t &&ptr) noexcept:
          owner {ptr.owner} {
        ptr.owner = nullptr;
      }

      ptr_t(const ptr_t &ptr) noexcept:
          owner {ptr.owner} {
        if (!owner) {
          return;
        }

        auto tmp = ptr.owner->ref();
        tmp.owner = nullptr;
      }

      ptr_t &operator=(const ptr_t &ptr) noexcept {
        if (!ptr.owner) {
          release();

          return *this;
        }

        return *this = std::move(*ptr.owner->ref());
      }

      ptr_t &operator=(ptr_t &&ptr) noexcept {
        if (owner) {
          release();
        }

        std::swap(owner, ptr.owner);

        return *this;
      }

      ~ptr_t() {
        if (owner) {
          release();
        }
      }

      operator bool() const {
        return owner != nullptr;
      }

      void release() {
        std::lock_guard lg {owner->_lock};

        if (!--owner->_count) {
          owner->_destruct(*get());
          (*this)->~element_type();
        }

        owner = nullptr;
      }

      element_type *get() const {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }

      element_type *operator->() {
        return reinterpret_cast<element_type *>(owner->_object_buf.data());
      }
    };

    template<class FC, class FD>
    shared_t(FC &&fc, FD &&fd):
        _construct {std::forward<FC>(fc)},
        _destruct {std::forward<FD>(fd)} {
    }

    [[nodiscard]] ptr_t ref() {
      std::lock_guard lg {_lock};

      if (!_count) {
        new (_object_buf.data()) element_type;
        if (_construct(*reinterpret_cast<element_type *>(_object_buf.data()))) {
          return ptr_t {nullptr};
        }
      }

      ++_count;

      return ptr_t {this};
    }

  private:
    construct_f _construct;
    destruct_f _destruct;

    std::array<std::uint8_t, sizeof(element_type)> _object_buf;

    std::uint32_t _count;
    std::mutex _lock;
  };

  template<class T, class F_Construct, class F_Destruct>
  auto make_shared(F_Construct &&fc, F_Destruct &&fd) {
    return shared_t<T> {
      std::forward<F_Construct>(fc),
      std::forward<F_Destruct>(fd)
    };
  }

  using signal_t = event_t<bool>;

  class mail_raw_t;
  using mail_t = std::shared_ptr<mail_raw_t>;

  void cleanup(mail_raw_t *);

  template<class T>
  class post_t: public T {
  public:
    template<class... Args>
    post_t(mail_t mail, Args &&...args):
        T(std::forward<Args>(args)...),
        mail {std::move(mail)} {
    }

    mail_t mail;

    ~post_t() {
      cleanup(mail.get());
    }
  };

  template<class T>
  inline auto lock(const std::weak_ptr<void> &wp) {
    return std::reinterpret_pointer_cast<typename T::element_type>(wp.lock());
  }

  class mail_raw_t: public std::enable_shared_from_this<mail_raw_t> {
  public:
    template<class T>
    using event_t = std::shared_ptr<post_t<event_t<T>>>;

    template<class T>
    using queue_t = std::shared_ptr<post_t<queue_t<T>>>;

    template<class T>
    event_t<T> event(const std::string_view &id) {
      std::lock_guard lg {mutex};

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        if (auto post = lock<event_t<T>>(it->second)) {
          return post;
        }

        id_to_post.erase(it);
      }

      auto post = std::make_shared<typename event_t<T>::element_type>(shared_from_this());
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> {std::string {id}, post});

      return post;
    }

    template<class T>
    queue_t<T> queue(const std::string_view &id) {
      std::lock_guard lg {mutex};

      auto it = id_to_post.find(id);
      if (it != std::end(id_to_post)) {
        if (auto post = lock<queue_t<T>>(it->second)) {
          return post;
        }

        id_to_post.erase(it);
      }

      auto post = std::make_shared<typename queue_t<T>::element_type>(shared_from_this(), 32);
      id_to_post.emplace(std::pair<std::string, std::weak_ptr<void>> {std::string {id}, post});

      return post;
    }

    void cleanup() {
      std::lock_guard lg {mutex};

      for (auto it = std::begin(id_to_post); it != std::end(id_to_post); ++it) {
        auto &weak = it->second;

        if (weak.expired()) {
          id_to_post.erase(it);

          return;
        }
      }
    }

    std::mutex mutex;

    std::map<std::string, std::weak_ptr<void>, std::less<>> id_to_post;
  };

  inline void cleanup(mail_raw_t *mail) {
    mail->cleanup();
  }
}  // namespace safe
