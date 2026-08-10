#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace display_helper::v2::diagnostics {
  enum class Level { debug, info, warning, error };

  using Sink = std::function<void(Level, const std::string &)>;

  /// Installs the process-level runtime adapter.  The core deliberately has no
  /// dependency on Boost.Log (or any other logging implementation).
  void set_sink(Sink sink);
  void emit(Level level, std::string message);

  class Stream {
  public:
    explicit Stream(Level level) : level_(level) {}
    Stream(Stream &&) = default;
    Stream(const Stream &) = delete;
    ~Stream() { emit(level_, stream_.str()); }

    template<class T>
    Stream &operator<<(const T &value) {
      stream_ << value;
      return *this;
    }

  private:
    Level level_;
    std::ostringstream stream_;
  };

  inline Stream log(Level level) { return Stream {level}; }
}  // namespace display_helper::v2::diagnostics

// Existing V2 call sites use streaming log expressions.  Retaining that
// spelling keeps the policy code readable while routing it through the
// portable diagnostic boundary above; production installs the Boost adapter.
#ifndef DISPLAY_HELPER_V2_RUNTIME_BOOST_ADAPTER
  #define BOOST_LOG(level) ::display_helper::v2::diagnostics::log(::display_helper::v2::diagnostics::Level::level)
#endif
