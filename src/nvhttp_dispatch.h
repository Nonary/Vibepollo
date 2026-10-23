/**
 * @file src/nvhttp_dispatch.h
 * @brief Bounded dispatch for potentially blocking NVHTTP mutations.
 */
#pragma once

#include "single_flight.h"
#include "host_stream_control.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace nvhttp::dispatch {
  enum class trace_stage_e {
    submit_returned,
    worker_started,
    worker_completed,
    worker_failed,
    busy_response_write_returned,
    host_start_canceled_response_write_returned,
  };

  struct trace_event_t {
    trace_stage_e stage;
    std::chrono::steady_clock::duration elapsed;
  };

  inline std::uint64_t next_trace_id() noexcept {
    static std::atomic<std::uint64_t> counter {0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  // The default untraced call path keeps legacy detail logs. Correlated route
  // traces omit request-derived and client-identity fields.
  inline bool include_request_identity_log_fields(std::uint64_t trace_id) noexcept {
    return trace_id == 0;
  }

  inline const char *trace_stage_name(trace_stage_e stage) noexcept {
    switch (stage) {
      case trace_stage_e::submit_returned:
        return "submit-returned";
      case trace_stage_e::worker_started:
        return "worker-started";
      case trace_stage_e::worker_completed:
        return "worker-completed";
      case trace_stage_e::worker_failed:
        return "worker-failed";
      case trace_stage_e::busy_response_write_returned:
        return "busy-503-response-write-returned";
      case trace_stage_e::host_start_canceled_response_write_returned:
        return "host-start-canceled-503-response-write-returned";
    }
    return "unknown";
  }

  struct no_trace_t {
    void operator()(const trace_event_t &) const noexcept {
    }
  };

  template<class Observer>
  void notify_trace(const Observer &observer, const trace_event_t &event) noexcept {
    try {
      observer(event);
    } catch (...) {
      // Diagnostics must never change request admission or handler behavior.
    }
  }

  template<class Response, class Observer = no_trace_t>
  void canceled_start(Response response, const char *operation, Observer observer = {}, std::chrono::steady_clock::time_point request_started = {}) {
    if constexpr (!std::is_same_v<std::remove_cvref_t<Observer>, no_trace_t>) {
      if (request_started == std::chrono::steady_clock::time_point {}) {
        request_started = std::chrono::steady_clock::now();
      }
    }
    boost::property_tree::ptree tree;
    tree.put(std::string("root.") + operation, 0);
    tree.put("root.<xmlattr>.status_code", 503);
    tree.put("root.<xmlattr>.status_message", "Stream request canceled by the host end-stream shortcut. Submit a new request.");
    std::ostringstream data;
    boost::property_tree::write_xml(data, tree);
    response->close_connection_after_response = true;
    response->write(data.str());
    if constexpr (!std::is_same_v<std::remove_cvref_t<Observer>, no_trace_t>) {
      notify_trace(observer, {trace_stage_e::host_start_canceled_response_write_returned, std::chrono::steady_clock::now() - request_started});
    }
  }

  // Submit and Response are executor/socket boundaries. Admission and the
  // protocol busy response are shared by the application and component tests.
  template<class Submit, class Response, class Task, class Observer = no_trace_t>
  bool run_blocking(single_flight::admission_t &admission, Submit &&submit, Response response, const char *operation, Task &&task, Observer observer = {}, std::chrono::steady_clock::time_point request_started = {}) {
    if constexpr (std::is_same_v<std::remove_cvref_t<Observer>, no_trace_t>) {
      if (admission.try_submit(std::forward<Submit>(submit), std::forward<Task>(task))) {
        return true;
      }
    } else {
      if (request_started == std::chrono::steady_clock::time_point {}) {
        request_started = std::chrono::steady_clock::now();
      }
      const auto task_observer = observer;
      if (admission.try_submit(std::forward<Submit>(submit), [task = std::forward<Task>(task), observer = task_observer, request_started]() mutable {
            const auto task_started = std::chrono::steady_clock::now();
            notify_trace(observer, {trace_stage_e::worker_started, task_started - request_started});
            try {
              task();
            } catch (...) {
              notify_trace(observer, {trace_stage_e::worker_failed, std::chrono::steady_clock::now() - task_started});
              throw;
            }
            notify_trace(observer, {trace_stage_e::worker_completed, std::chrono::steady_clock::now() - task_started});
          })) {
        notify_trace(observer, {trace_stage_e::submit_returned, std::chrono::steady_clock::now() - request_started});
        return true;
      }
    }
    boost::property_tree::ptree tree;
    tree.put(std::string("root.") + operation, 0);
    tree.put("root.<xmlattr>.status_code", 503);
    tree.put("root.<xmlattr>.status_message", "Another stream operation is still running. Retry after it completes; if this persists, check the host GPU and system-sleep logs.");
    std::ostringstream data;
    boost::property_tree::write_xml(data, tree);
    response->close_connection_after_response = true;
    response->write(data.str());
    if constexpr (!std::is_same_v<std::remove_cvref_t<Observer>, no_trace_t>) {
      notify_trace(observer, {trace_stage_e::busy_response_write_returned, std::chrono::steady_clock::now() - request_started});
    }
    return false;
  }

  // This is the production launch/resume composition: intake ticket precedes
  // single-flight reservation and enqueueing, and survives both queue layers.
  template<class Submit, class Response, class Task, class Observer = no_trace_t>
  void run_start(single_flight::admission_t &admission, Submit &&submit, Response response, const char *operation, Task &&task, Observer observer = {}) {
    if constexpr (std::is_same_v<std::remove_cvref_t<Observer>, no_trace_t>) {
      host_stream_control::submit_start([&](auto guarded) {
        run_blocking(admission, std::forward<Submit>(submit), response, operation, std::move(guarded));
      }, std::forward<Task>(task), [response, operation] {
        canceled_start(response, operation);
      });
    } else {
      const auto request_started = std::chrono::steady_clock::now();
      const auto task_observer = observer;
      host_stream_control::submit_start([&](auto guarded) {
        run_blocking(admission, std::forward<Submit>(submit), response, operation, std::move(guarded), task_observer, request_started);
      }, std::forward<Task>(task), [response, operation, task_observer, request_started] {
        canceled_start(response, operation, task_observer, request_started);
      });
    }
  }
}  // namespace nvhttp::dispatch
