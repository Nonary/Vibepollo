/**
 * @file src/http_pairing_policy.h
 * @brief Data-only pairing protocol decisions shared by NVHTTP and unit tests.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nvhttp::pairing_policy {

  // The legacy GameStream PIN flow does not carry a server-generated request
  // identity into the administrator approval action. Exactly one immutable
  // pending session is therefore the only unambiguous safe admission model.
  inline constexpr std::size_t max_pending_sessions = 1;
  inline constexpr std::size_t max_unique_id_length = 128;
  inline constexpr std::size_t max_paired_clients = 256;
  inline constexpr std::size_t max_paired_client_name_length = 1024;
  inline constexpr std::size_t max_paired_certificate_length = 65536;
  inline constexpr std::size_t max_pairing_hex_field_length = 65536;

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

  struct paired_client_record_view_t {
    std::string_view uuid;
    std::string_view certificate_identity;
    bool enabled = false;
  };

  enum class paired_client_resolution_e {
    authorized,
    unknown_certificate,
    disabled,
    invalid_state,
  };

  struct paired_client_resolution_t {
    paired_client_resolution_e status = paired_client_resolution_e::unknown_certificate;
    std::size_t index = 0;
  };

  struct admission_decision_t {
    bool accepted = false;
    std::string_view failure_message;
  };

  /**
   * Preserve the certificate subject string across the crypto/HTTP boundary.
   * A missing certificate subject is an empty identity, rather than an error.
   */
  std::string certificate_subject_name(std::optional<std::string_view> subject_name);

  bool is_placeholder_client_name(std::string_view name);
  std::string display_client_name(std::string_view paired_name, std::string_view device_name, std::string_view host_name);

  bool valid_unique_id(std::string_view value);
  bool valid_hex_field(std::string_view value, std::size_t minimum, std::size_t maximum = max_pairing_hex_field_length);
  bool valid_paired_client_uuid(std::string_view value);
  bool paired_client_state_valid(std::span<const paired_client_record_view_t> clients);
  paired_client_resolution_t resolve_paired_client(
    std::span<const paired_client_record_view_t> clients,
    std::string_view presented_certificate_identity
  );
  admission_decision_t admit_pending_session(
    std::string_view unique_id,
    std::string_view client_certificate_hex,
    std::string_view salt_hex,
    std::size_t pending_sessions,
    bool replacing_existing
  );

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
