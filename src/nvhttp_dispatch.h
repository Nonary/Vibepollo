/**
 * @file src/nvhttp_dispatch.h
 * @brief Bounded dispatch for potentially blocking NVHTTP mutations.
 */
#pragma once

#include "single_flight.h"
#include "host_stream_control.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace nvhttp::dispatch {
  template<class Response>
  void canceled_start(Response response, const char *operation) {
    boost::property_tree::ptree tree;
    tree.put(std::string("root.") + operation, 0);
    tree.put("root.<xmlattr>.status_code", 503);
    tree.put("root.<xmlattr>.status_message", "Stream request canceled by the host end-stream shortcut. Submit a new request.");
    std::ostringstream data;
    boost::property_tree::write_xml(data, tree);
    response->close_connection_after_response = true;
    response->write(data.str());
  }

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
  // This is the production launch/resume composition: intake ticket precedes
  // single-flight reservation and enqueueing, and survives both queue layers.
  template<class Submit, class Response, class Task>
  void run_start(single_flight::admission_t &admission, Submit &&submit, Response response, const char *operation, Task &&task) {
    host_stream_control::submit_start([&](auto guarded) {
      run_blocking(admission, std::forward<Submit>(submit), response, operation, std::move(guarded));
    }, std::forward<Task>(task), [response, operation] {
      canceled_start(response, operation);
    });
  }
}  // namespace nvhttp::dispatch
