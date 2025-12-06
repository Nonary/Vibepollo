/**
 * @file src/boost_process_shim.cpp
 * @brief Explicit instantiations required for Boost.Process v2 on Windows.
 */

#ifdef _WIN32
  #include "boost_process_shim.h"

  #include <boost/asio/any_io_executor.hpp>
  #include <boost/process/v2/detail/process_handle_windows.hpp>

namespace boost::process::v2::detail {
  template struct basic_process_handle_win<boost::asio::any_io_executor>;
}  // namespace boost::process::v2::detail

#endif
