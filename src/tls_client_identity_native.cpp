/**
 * @file src/tls_client_identity_native.cpp
 * @brief Native Vibepollo registry adapter for transport authorization.
 */
#include "tls_client_identity_native.h"

#include <algorithm>

namespace nvhttp::tls_identity {
  std::optional<std::vector<client_record_view_t>> native_client_views(
    std::span<const crypto::p_named_cert_t> clients
  ) {
    if (clients.size() > pairing_policy::max_paired_clients) {
      return std::nullopt;
    }
    std::vector<client_record_view_t> views;
    views.reserve(clients.size());
    for (const auto &client : clients) {
      if (!client) {
        return std::nullopt;
      }
      views.push_back({client->uuid, client->name, client->cert, !!(client->perm & crypto::PERM::_all), client->allow_sleep});
    }
    return views;
  }

  bool current_native_permissions_match(
    std::span<const crypto::p_named_cert_t> clients,
    std::string_view authenticated_uuid,
    crypto::PERM expected_permissions
  ) {
    const auto views = native_client_views(clients);
    if (!views || !client_enabled(*views, authenticated_uuid)) {
      return false;
    }
    const auto record = std::find_if(clients.begin(), clients.end(), [&](const auto &client) {
      return client->uuid == authenticated_uuid;
    });
    return record != clients.end() && (*record)->perm == expected_permissions;
  }

  authentication_result_t native_store_t::authenticate_certificate(
    const request_t &request, crypto::x509_t &certificate,
    crypto::cert_chain_t &chain, std::span<const crypto::p_named_cert_t> clients
  ) {
    const auto views = native_client_views(clients);
    if (!views) {
      store.reset(request);
      return {{}, "Pairing state contains an invalid native record"};
    }
    return store.authenticate_certificate(request, certificate, chain, *views);
  }

  std::optional<crypto::named_cert_t> native_store_t::resolve_snapshot(
    const request_t &request, std::span<const crypto::p_named_cert_t> clients
  ) {
    const auto views = native_client_views(clients);
    if (!views) {
      store.reset(request);
      return std::nullopt;
    }
    const auto identity = store.resolve(request, *views);
    if (!identity) {
      return std::nullopt;
    }
    const auto record = std::find_if(clients.begin(), clients.end(), [&](const auto &client) {
      return client->uuid == identity->uuid;
    });
    // Copy the WHOLE native record while the caller still holds client_mutex.
    // No cached permissions/preferences, field reconstruction or request UUID.
    return record != clients.end() ? std::optional<crypto::named_cert_t>(**record) : std::nullopt;
  }
}  // namespace nvhttp::tls_identity
