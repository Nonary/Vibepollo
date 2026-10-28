/**
 * @file src/tls_client_identity_native.h
 * @brief Native Vibepollo registry adapter for transport authorization.
 */
#pragma once

#include "tls_client_identity.h"

namespace nvhttp::tls_identity {

  // Every caller must hold NVHTTP's client_mutex for the entire operation and
  // while consuming these non-owning views. Null records invalidate the whole
  // registry; only permissions represented in PERM::_all enable a record.
  std::optional<std::vector<client_record_view_t>> native_client_views(
    std::span<const crypto::p_named_cert_t> clients);

  // Final recheck of an already authenticated snapshot, never a UUID-based
  // authentication entry point. Keep the exact native mask, not merely enabled.
  bool current_native_permissions_match(
    std::span<const crypto::p_named_cert_t> clients,
    std::string_view authenticated_uuid,
    crypto::PERM expected_permissions);

  class native_store_t {
  public:
    // The caller holds client_mutex across chain verification, live registry
    // resolution and publication, or across resolution AND native value copy.
    authentication_result_t authenticate_certificate(
      const request_t &request, crypto::x509_t &certificate,
      crypto::cert_chain_t &chain, std::span<const crypto::p_named_cert_t> clients);

    std::optional<crypto::named_cert_t> resolve_snapshot(
      const request_t &request, std::span<const crypto::p_named_cert_t> clients);

    void reset(const request_t &request) { store.reset(request); }
    void revoke(std::string_view uuid) { store.revoke(uuid); }
    void clear() { store.clear(); }

  private:
    store_t store;
  };
}  // namespace nvhttp::tls_identity
