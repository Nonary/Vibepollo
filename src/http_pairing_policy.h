/**
 * @file src/http_pairing_policy.h
 * @brief Data-only pairing protocol decisions shared by NVHTTP and unit tests.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace nvhttp::pairing_policy {

  enum class phase_e {
    none,
    get_server_cert,
    client_challenge,
    server_challenge_response,
    client_pairing_secret,
  };

  struct session_state_t {
    phase_e phase = phase_e::none;
    bool has_cipher_key = false;
    bool has_server_secret = false;
  };

  struct decision_t {
    bool accepted = false;
    phase_e next_phase = phase_e::none;
    std::string_view failure_message;
  };

  /**
   * Preserve the certificate subject string across the crypto/HTTP boundary.
   * A missing certificate subject is an empty identity, rather than an error.
   */
  std::string certificate_subject_name(std::optional<std::string_view> subject_name);

  bool is_placeholder_client_name(std::string_view name);
  std::string display_client_name(std::string_view paired_name, std::string_view device_name, std::string_view host_name);

  decision_t begin_get_server_certificate(session_state_t state, std::size_t salt_size);
  decision_t begin_client_challenge(session_state_t state);
  decision_t begin_server_challenge_response(session_state_t state);
  decision_t begin_client_pairing_secret(session_state_t state, std::size_t payload_size);
  decision_t decide_client_pairing_secret(
    session_state_t state,
    bool client_certificate_valid,
    bool client_hash_matches,
    bool client_secret_signature_valid
  );

}  // namespace nvhttp::pairing_policy
