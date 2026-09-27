/**
 * @file src/pyrowave_host.cpp
 * @brief PyroWave host encoder for builds and platforms without an implementation.
 *
 * Windows builds with SUNSHINE_ENABLE_PYROWAVE use
 * src/platform/windows/pyrowave_encode.cpp instead.
 *
 * Linux builds use src/platform/linux/pyrowave_encode.cpp.
 */
#include "pyrowave_host.h"

#if !((defined(_WIN32) || defined(__linux__)) && defined(SUNSHINE_ENABLE_PYROWAVE))

  #include "platform/common.h"

namespace pyrowave::host {
  std::unique_ptr<encoder_t> make_encoder(const session_params_t &, std::shared_ptr<platf::display_t>) {
    return nullptr;
  }

  bool probe(const std::optional<platf::adapter_id_t> &, std::string &detail) {
  #if defined(SUNSHINE_ENABLE_PYROWAVE)
    detail = "PyroWave encoding is not implemented on this platform yet";
  #else
    detail = "this build has PyroWave disabled (SUNSHINE_ENABLE_PYROWAVE=OFF)";
  #endif
    return false;
  }
}  // namespace pyrowave::host

#endif
