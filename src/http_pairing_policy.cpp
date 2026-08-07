/**
 * @file src/http_pairing_policy.cpp
 * @brief Data-only pairing protocol decisions shared by NVHTTP and unit tests.
 */

#include "http_pairing_policy.h"

#include <algorithm>
#include <cctype>

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
