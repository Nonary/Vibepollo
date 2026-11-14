#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

namespace confighttp::policy {
  class LoginAttemptLimiter {
  public:
    using clock = std::chrono::steady_clock;

    struct Decision {
      bool allowed;
      std::chrono::seconds retry_after;
    };

    explicit LoginAttemptLimiter(
      std::size_t max_attempts = 8,
      std::chrono::seconds window = std::chrono::minutes(1),
      std::chrono::seconds block_duration = std::chrono::minutes(1),
      std::size_t max_entries = 1024
    ):
        _max_attempts(std::max<std::size_t>(1, max_attempts)),
        _window(std::max(std::chrono::seconds(1), window)),
        _block_duration(std::max(std::chrono::seconds(1), block_duration)),
        _max_entries(std::max<std::size_t>(1, max_entries)) {}

    Decision consume(const std::string &key, clock::time_point now = clock::now()) {
      std::scoped_lock lock(_mutex);
      prune(now);

      auto it = _entries.find(key);
      if (it != _entries.end() && it->second.blocked_until > now) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(it->second.blocked_until - now);
        return {false, std::max(std::chrono::seconds(1), remaining)};
      }

      if (it == _entries.end()) {
        if (_entries.size() >= _max_entries) {
          evict_oldest();
        }
        it = _entries.emplace(key, Entry {.first_attempt = now, .last_seen = now}).first;
      } else if (now - it->second.first_attempt >= _window) {
        it->second = Entry {.first_attempt = now, .last_seen = now};
      }

      auto &entry = it->second;
      entry.last_seen = now;
      ++entry.attempts;
      if (entry.attempts >= _max_attempts) {
        entry.blocked_until = now + _block_duration;
      }
      return {true, {}};
    }

    void reset(const std::string &key) {
      std::scoped_lock lock(_mutex);
      _entries.erase(key);
    }

    std::size_t size() const {
      std::scoped_lock lock(_mutex);
      return _entries.size();
    }

  private:
    struct Entry {
      std::size_t attempts = 0;
      clock::time_point first_attempt {};
      clock::time_point last_seen {};
      clock::time_point blocked_until {};
    };

    void prune(clock::time_point now) {
      const auto retention = std::max(_window, _block_duration);
      std::erase_if(_entries, [now, retention](const auto &item) {
        const auto &entry = item.second;
        return entry.blocked_until <= now && now - entry.last_seen >= retention;
      });
    }

    void evict_oldest() {
      auto oldest = _entries.end();
      for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (oldest == _entries.end() || it->second.last_seen < oldest->second.last_seen) {
          oldest = it;
        }
      }
      if (oldest != _entries.end()) {
        _entries.erase(oldest);
      }
    }

    const std::size_t _max_attempts;
    const std::chrono::seconds _window;
    const std::chrono::seconds _block_duration;
    const std::size_t _max_entries;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, Entry> _entries;
  };
}  // namespace confighttp::policy
