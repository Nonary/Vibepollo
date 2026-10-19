/**
 * @file src/nvhttp_dispatch.h
 * @brief Bounded dispatch for potentially blocking NVHTTP mutations.
 */
#pragma once

#include "single_flight.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace nvhttp::dispatch {
  // Submit and Response are executor/socket boundaries. Admission and the
  // protocol busy response are shared by the application and component tests.
  template<class Submit, class Response, class Task>
  void run_blocking(single_flight::admission_t &admission, Submit &&submit, Response response, const char *operation, Task &&task) {
    if (admission.try_submit(std::forward<Submit>(submit), std::forward<Task>(task))) {
      return;
    }
    boost::property_tree::ptree tree;
    tree.put(std::string("root.") + operation, 0);
    tree.put("root.<xmlattr>.status_code", 503);
    tree.put("root.<xmlattr>.status_message", "Another stream operation is still running. Retry after it completes; if this persists, check the host GPU and system-sleep logs.");
    std::ostringstream data;
    boost::property_tree::write_xml(data, tree);
    response->close_connection_after_response = true;
    response->write(data.str());
  }
}  // namespace nvhttp::dispatch
