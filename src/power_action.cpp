#include "power_action.h"

#include <array>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <openssl/rand.h>
#include <sstream>

namespace nvhttp::power_action {
  namespace {
    bool valid_token(std::string_view token) {
      return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    std::string new_token() {
      std::array<unsigned char, 32> bytes;
      if (RAND_bytes(bytes.data(), bytes.size()) != 1) {
        return {};
      }
      std::string value;
      value.reserve(64);
      for (const auto byte : bytes) {
        value += "0123456789abcdef"[byte >> 4];
        value += "0123456789abcdef"[byte & 15];
      }
      return value;
    }
  }  // namespace

  bool activity_t::busy() const {
    return app || pending || rtsp || running || teardown || webrtc || capture || remote_owner;
  }

  service_t::service_t(std::mutex &gate):
      lifecycle_gate(gate) {}

  response_t service_t::capabilities(const hooks_t &hooks, clock::time_point now) {
    const auto caller = hooks.authority();
    if (caller.uuid.empty()) {
      return {401, "A paired TLS client identity is required"};
    }
    response_t result;
    result.supported = hooks.supported();
    result.allowed = caller.allow_sleep;
    if (!result.supported || !result.allowed) {
      return result;
    }
    std::unique_lock gate(lifecycle_gate, std::try_to_lock);
    if (!gate.owns_lock() || hooks.activity().busy()) {
      return result;
    }
    std::lock_guard lock(token_mutex);
    std::erase_if(tokens, [now](const auto &item) {
      return item.second.expires <= now;
    });
    if (!tokens.contains(caller.uuid) && tokens.size() >= max_tokens) {
      return result;
    }
    result.token = new_token();
    if (result.token.empty()) {
      return {503, "Could not create a sleep request token"};
    }
    tokens.insert_or_assign(caller.uuid, token_t {result.token, now + token_lifetime});
    result.available = true;
    return result;
  }

  result_e service_t::sleep(const request_t &request, const hooks_t &hooks, clock::time_point now) {
    const auto refuse = [&](int status, const char *message) {
      hooks.respond({status, message});
      return result_e::refused;
    };
    if (request.method != "POST") {
      return refuse(405, "Use POST for a sleep request");
    }
    if (!request.valid_shape || !valid_token(request.token)) {
      return refuse(400, "Expected one sleep token and an empty body; browser origins are not accepted");
    }
    const auto caller = hooks.authority();
    if (caller.uuid.empty()) {
      return refuse(401, "A paired TLS client identity is required");
    }
    if (!caller.allow_sleep) {
      return refuse(403, "Sleep control is disabled for this paired client");
    }
    if (!hooks.supported()) {
      return refuse(501, "Native sleep control is unavailable on this host");
    }
    std::unique_lock gate(lifecycle_gate, std::try_to_lock);
    if (!gate.owns_lock() || hooks.activity().busy()) {
      return refuse(409, "End all applications and sessions before requesting sleep");
    }
    {
      std::lock_guard lock(token_mutex);
      const auto it = tokens.find(caller.uuid);
      if (it == tokens.end() || it->second.expires <= now || it->second.value != request.token) {
        return refuse(409, "Sleep token expired or already used; confirm the action again");
      }
      // Consume before publishing acceptance. Replays after wake or a failed
      // response cannot suspend again. A new GET invalidates an older token.
      tokens.erase(it);
    }
    if (!hooks.respond({202, "Sleep request accepted; native completion is not confirmed"})) {
      return result_e::response_failed;
    }
    // The lifecycle gate spans response flush and native invocation. A racing
    // launch cannot publish a stream between the idle check and suspend.
    return hooks.suspend_if_authorized() ? result_e::requested : result_e::native_failed;
  }

  std::string response_xml(const response_t &response) {
    boost::property_tree::ptree tree;
    tree.put("root.<xmlattr>.status_code", response.status);
    tree.put("root.<xmlattr>.status_message", response.message);
    tree.put("root.PowerActionVersion", 1);
    tree.put("root.SleepSupported", response.supported ? 1 : 0);
    tree.put("root.SleepAllowed", response.allowed ? 1 : 0);
    tree.put("root.SleepAvailable", response.available ? 1 : 0);
    tree.put("root.SleepActiveSessionPolicy", "idle-only");
    tree.put("root.SleepToken", response.token);
    tree.put("root.SleepTokenLifetimeSeconds", service_t::token_lifetime.count());
    tree.put("root.SleepAccepted", response.status == 202 ? 1 : 0);
    std::ostringstream xml;
    boost::property_tree::write_xml(xml, tree);
    return xml.str();
  }
}  // namespace nvhttp::power_action
