/**
 * @file src/http_pairing_policy.cpp
 * @brief Data-only pairing protocol decisions shared by NVHTTP and unit tests.
 */

#include "http_pairing_policy.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace nvhttp::pairing_policy {

  namespace {
    constexpr std::size_t salt_hex_length = 32;
    constexpr std::size_t pairing_secret_length = 16;

    std::string_view trim(std::string_view value) {
      const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
      };

      while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      return value;
    }

    bool equals_ignore_case(std::string_view left, std::string_view right) {
      return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](unsigned char lhs, unsigned char rhs) {
        return std::tolower(lhs) == std::tolower(rhs);
      });
    }

    decision_t accepted(phase_e next_phase) {
      return {true, next_phase, {}};
    }

    decision_t rejected(phase_e next_phase, std::string_view failure_message) {
      return {false, next_phase, failure_message};
    }
  }  // namespace

  std::string certificate_subject_name(const std::optional<std::string_view> subject_name) {
    return subject_name ? std::string {*subject_name} : std::string {};
  }

  bool is_placeholder_client_name(const std::string_view name) {
    return equals_ignore_case(trim(name), "self");
  }

  std::string display_client_name(const std::string_view paired_name, const std::string_view device_name, const std::string_view host_name) {
    for (const auto name: {paired_name, device_name, host_name}) {
      const auto trimmed_name = trim(name);
      if (!trimmed_name.empty() && !is_placeholder_client_name(trimmed_name)) {
        return std::string {trimmed_name};
      }
    }
    return "Sunshine";
  }

  bool valid_unique_id(const std::string_view value) {
    return !value.empty() && value.size() <= max_unique_id_length &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
             return std::isalnum(character) || character == '-' || character == '_' || character == '.';
           });
  }

  bool valid_hex_field(const std::string_view value, const std::size_t minimum, const std::size_t maximum) {
    return value.size() >= minimum && value.size() <= maximum && value.size() % 2 == 0 &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
             return std::isxdigit(character) != 0;
           });
  }

  bool valid_paired_client_uuid(const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
      return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (index == 8 || index == 13 || index == 18 || index == 23) {
        continue;
      }
      if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
        return false;
      }
    }
    return true;
  }

  bool paired_client_state_valid(const std::span<const paired_client_record_view_t> clients) {
    if (clients.size() > max_paired_clients) {
      return false;
    }

    std::unordered_set<std::string_view> uuids;
    std::unordered_set<std::string_view> certificate_identities;
    uuids.reserve(clients.size());
    certificate_identities.reserve(clients.size());
    for (const auto &client : clients) {
      if (!valid_paired_client_uuid(client.uuid) ||
          client.certificate_identity.empty() ||
          client.certificate_identity.size() > max_paired_certificate_length ||
          !uuids.insert(client.uuid).second ||
          !certificate_identities.insert(client.certificate_identity).second) {
        return false;
      }
    }
    return true;
  }

  paired_client_resolution_t resolve_paired_client(
    const std::span<const paired_client_record_view_t> clients,
    const std::string_view presented_certificate_identity
  ) {
    if (!paired_client_state_valid(clients)) {
      return {paired_client_resolution_e::invalid_state, 0};
    }
    if (presented_certificate_identity.empty() ||
        presented_certificate_identity.size() > max_paired_certificate_length) {
      return {paired_client_resolution_e::unknown_certificate, 0};
    }
    for (std::size_t index = 0; index < clients.size(); ++index) {
      if (clients[index].certificate_identity != presented_certificate_identity) {
        continue;
      }
      return {
        clients[index].enabled ? paired_client_resolution_e::authorized : paired_client_resolution_e::disabled,
        index,
      };
    }
    return {paired_client_resolution_e::unknown_certificate, 0};
  }

  admission_decision_t admit_pending_session(
    const std::string_view unique_id,
    const std::string_view client_certificate_hex,
    const std::string_view salt_hex,
    const std::size_t pending_sessions,
    const bool replacing_existing
  ) {
    // Pairing is accepted at the login screen too. Completing it still
    // requires the Web UI login to submit the PIN, so the greeter gains no
    // unauthenticated path; refusing here was a GameStream habit only.
    if (!valid_unique_id(unique_id)) {
      return {false, "Invalid uniqueid"};
    }
    if (!valid_hex_field(client_certificate_hex, 2)) {
      return {false, "Invalid client certificate"};
    }
    if (!valid_hex_field(salt_hex, salt_hex_length, 128)) {
      return {false, "Invalid pairing salt"};
    }
    // A same-uniqueid request is not a continuation: it supplies a new
    // certificate and salt and could replace the identity that the operator
    // intended to approve. Never replace or multiplex a pending request.
    if (pending_sessions >= max_pending_sessions) {
      return {false, "Too many pending pairing sessions"};
    }
    (void) replacing_existing;
    return {true, {}};
  }

  decision_t begin_get_server_certificate(const session_state_t state, const std::size_t salt_size) {
    if (state.phase != phase_e::none) {
      return rejected(state.phase, "Out of order call to getservercert");
    }
    if (salt_size < salt_hex_length) {
      return rejected(phase_e::get_server_cert, "Salt too short");
    }
    return accepted(phase_e::get_server_cert);
  }

  decision_t begin_client_challenge(const session_state_t state) {
    if (state.phase != phase_e::get_server_cert) {
      return rejected(state.phase, "Out of order call to clientchallenge");
    }
    if (!state.has_cipher_key) {
      return rejected(phase_e::client_challenge, "Cipher key not set");
    }
    return accepted(phase_e::client_challenge);
  }

  decision_t begin_server_challenge_response(const session_state_t state) {
    if (state.phase != phase_e::client_challenge) {
      return rejected(state.phase, "Out of order call to serverchallengeresp");
    }
    if (!state.has_cipher_key || !state.has_server_secret) {
      return rejected(phase_e::server_challenge_response, "Cipher key or serversecret not set");
    }
    return accepted(phase_e::server_challenge_response);
  }

  decision_t begin_client_pairing_secret(const session_state_t state, const std::size_t payload_size) {
    if (state.phase != phase_e::server_challenge_response) {
      return rejected(state.phase, "Out of order call to clientpairingsecret");
    }
    if (payload_size <= pairing_secret_length) {
      return rejected(phase_e::client_pairing_secret, "Client pairing secret too short");
    }
    return accepted(phase_e::client_pairing_secret);
  }

  decision_t decide_client_pairing_secret(
    const session_state_t state,
    const bool client_certificate_valid,
    const bool client_hash_matches,
    const bool client_secret_signature_valid
  ) {
    if (state.phase != phase_e::client_pairing_secret) {
      return rejected(state.phase, "Out of order call to clientpairingsecret");
    }
    if (!client_certificate_valid) {
      return rejected(phase_e::client_pairing_secret, "Invalid client certificate");
    }
    if (!client_hash_matches || !client_secret_signature_valid) {
      return rejected(phase_e::client_pairing_secret, {});
    }
    return accepted(phase_e::client_pairing_secret);
  }

}  // namespace nvhttp::pairing_policy
