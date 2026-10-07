/**
 * @file src/session_history_storage_diagnostics.h
 * @brief Runtime-configurable diagnostics boundary for session history storage.
 */
#pragma once

// standard includes
#include <sstream>
#include <string>

namespace session_history::storage::diagnostics {

  enum class level_e {
    debug,
    info,
    warning,
    error,
  };

  using sink_t = void (*)(level_e level, const std::string &message);

  // The session-history runtime installs its Boost.Log adapter before storage
  // opens the database. Component tests deliberately leave this unset.
  void set_sink(sink_t sink);
  void log(level_e level, const std::string &message);

  class message_t {
  public:
    explicit message_t(level_e level):
        level_ {level} {
    }

    message_t(const message_t &) = delete;
    message_t &operator=(const message_t &) = delete;
    message_t(message_t &&) = delete;
    message_t &operator=(message_t &&) = delete;

    ~message_t() {
      log(level_, stream_.str());
    }

    template<typename T>
    message_t &operator<<(const T &value) {
      stream_ << value;
      return *this;
    }

  private:
    level_e level_;
    std::ostringstream stream_;
  };

  inline message_t debug() {
    return message_t {level_e::debug};
  }

  inline message_t info() {
    return message_t {level_e::info};
  }

  inline message_t warning() {
    return message_t {level_e::warning};
  }

  inline message_t error() {
    return message_t {level_e::error};
  }

}  // namespace session_history::storage::diagnostics
