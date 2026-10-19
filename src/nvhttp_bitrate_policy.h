/**
 * @file src/nvhttp_bitrate_policy.h
 * @brief Native bitrate permission checks around blocking dispatch.
 */
#pragma once

#include "crypto.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace nvhttp::bitrate_policy {
  using verified_client_t = std::optional<crypto::named_cert_t>;

  // Keep the native _allow_view rule: either view or launch permits viewing.
  template<class LogDenied>
  bool authorize(const verified_client_t &client, boost::property_tree::ptree &tree, LogDenied &&log_denied) {
    if (client && !!(client->perm & crypto::PERM::_allow_view)) {
      return true;
    }

    std::ostringstream message;
    if (client) {
      message << "Permission SetBitrate denied for client [" << client->name
              << "]: it lacks the \"View stream\" permission (current permission mask 0x"
              << std::hex << static_cast<uint32_t>(client->perm) << std::dec
              << "). Grant it in the Web UI under Client Management.";
    } else {
      message << "Permission SetBitrate denied: HTTPS client certificate not recognized. The device may need to be re-paired.";
    }
    log_denied(message.str());
    tree.put("root.bitrate", 0);
    tree.put("root.<xmlattr>.status_code", 403);
    tree.put("root.<xmlattr>.status_message", client ? "Permission denied: this device lacks the \"View stream\" permission. Enable it on the host in the Vibepollo Web UI under Client Management." : "Permission denied: this device's certificate is not recognized by the host. Unpair and pair this device again.");
    return false;
  }

  template<class Response, class LogDenied>
  bool authorize_response(const verified_client_t &client, const Response &response, LogDenied &&log_denied) {
    boost::property_tree::ptree tree;
    if (authorize(client, tree, std::forward<LogDenied>(log_denied))) {
      return true;
    }
    std::ostringstream data;
    boost::property_tree::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
    return false;
  }

  // resolve must return the current native record for the verified connection,
  // never a record selected by the request's UUID. The application supplies its
  // client_mutex-protected TLS identity lookup and shared blocking admission.
  template<class Response, class Request, class Resolve, class Blocking, class Handler, class LogDenied>
  void run(Response response, Request request, Resolve resolve, Blocking blocking, Handler handler, LogDenied log_denied) {
    const auto client = resolve(request);
    if (!authorize_response(client, response, log_denied)) {
      return;
    }

    blocking(response, "bitrate", [response, request = std::move(request), resolve = std::move(resolve), handler = std::move(handler), log_denied = std::move(log_denied)]() mutable {
      // Permissions/pairing can change while queued. Pass only this freshly
      // checked snapshot to the worker handler, never the listener snapshot.
      const auto current_client = resolve(request);
      if (!authorize_response(current_client, response, log_denied)) {
        return;
      }
      handler(std::move(response), std::move(request), current_client);
    });
  }
}  // namespace nvhttp::bitrate_policy
