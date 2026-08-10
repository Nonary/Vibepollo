#include "src/logging.h"

#define DISPLAY_HELPER_V2_RUNTIME_BOOST_ADAPTER
#include "src/platform/windows/display_helper_v2/diagnostics.h"

namespace display_helper::v2::diagnostics {
  namespace {
    void boost_sink(Level level, const std::string &message) {
      switch (level) {
        case Level::debug: BOOST_LOG(debug) << message; break;
        case Level::info: BOOST_LOG(info) << message; break;
        case Level::warning: BOOST_LOG(warning) << message; break;
        case Level::error: BOOST_LOG(error) << message; break;
      }
    }

    struct InstallBoostSink {
      InstallBoostSink() { set_sink(boost_sink); }
    } install_boost_sink;
  }  // namespace
}  // namespace display_helper::v2::diagnostics
