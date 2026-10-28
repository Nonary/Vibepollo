/**
 * @file src/tls_client_identity.cpp
 * @brief Certificate-bound GameStream transport authorization.
 */
#include "tls_client_identity.h"

#include <algorithm>
#include <unordered_set>

namespace nvhttp::tls_identity {

  std::optional<std::string> exact_certificate_identity(const crypto::x509_t &certificate) {
    if (!certificate) {
      return std::nullopt;
    }
    const auto encoded_length = i2d_X509(certificate.get(), nullptr);
    if (encoded_length <= 0 || static_cast<std::size_t>(encoded_length) > pairing_policy::max_paired_certificate_length) {
      return std::nullopt;
    }
    std::string identity(static_cast<std::size_t>(encoded_length), '\0');
    auto *cursor = reinterpret_cast<unsigned char *>(identity.data());
    if (i2d_X509(certificate.get(), &cursor) != encoded_length) {
      return std::nullopt;
    }
    return identity;
  }

  bool build_paired_client_records(
    std::span<const client_record_view_t> clients,
    std::vector<std::string> &certificate_identities,
    std::vector<pairing_policy::paired_client_record_view_t> &records,
    std::vector<crypto::x509_t> *parsed_certificates
  ) {
    certificate_identities.clear();
    records.clear();
    if (clients.size() > pairing_policy::max_paired_clients) {
      return false;
    }
    certificate_identities.reserve(clients.size());
    records.reserve(clients.size());
    if (parsed_certificates) {
      parsed_certificates->clear();
      parsed_certificates->reserve(clients.size());
    }
    for (const auto &client : clients) {
      if (client.name.size() > pairing_policy::max_paired_client_name_length || client.certificate.empty() || client.certificate.size() > pairing_policy::max_paired_certificate_length) {
        return false;
      }
      auto certificate = crypto::x509(client.certificate);
      auto identity = exact_certificate_identity(certificate);
      if (!identity) {
        return false;
      }
      certificate_identities.emplace_back(std::move(*identity));
      records.push_back({client.uuid, certificate_identities.back(), client.enabled});
      if (parsed_certificates) {
        parsed_certificates->emplace_back(std::move(certificate));
      }
    }
    return pairing_policy::paired_client_state_valid(records);
  }

  bool client_enabled(std::span<const client_record_view_t> clients, std::string_view uuid) {
    if (!pairing_policy::valid_paired_client_uuid(uuid) || clients.size() > pairing_policy::max_paired_clients) {
      return false;
    }
    const client_record_view_t *match = nullptr;
    std::unordered_set<std::string_view> seen_uuids;
    seen_uuids.reserve(clients.size());
    for (const auto &client : clients) {
      if (!pairing_policy::valid_paired_client_uuid(client.uuid) || client.name.size() > pairing_policy::max_paired_client_name_length || client.certificate.empty() || client.certificate.size() > pairing_policy::max_paired_certificate_length || !seen_uuids.insert(client.uuid).second) {
        return false;
      }
      if (client.uuid == uuid) {
        match = &client;
      }
    }
    return match && match->enabled;
  }

  authentication_result_t store_t::authenticate(
    const request_t &request,
    SSL *ssl,
    crypto::cert_chain_t &chain,
    std::span<const client_record_view_t> clients
  ) {
    crypto::x509_t certificate {
#if OPENSSL_VERSION_MAJOR >= 3
      ssl ? SSL_get1_peer_certificate(ssl) : nullptr
#else
      ssl ? SSL_get_peer_certificate(ssl) : nullptr
#endif
    };
    return authenticate_certificate(request, certificate, chain, clients);
  }

  authentication_result_t store_t::authenticate_certificate(
    const request_t &request,
    crypto::x509_t &certificate,
    crypto::cert_chain_t &chain,
    std::span<const client_record_view_t> clients
  ) {
    reset(request);
    if (!request || !certificate) {
      return {{}, "No client certificate available"};
    }
    // Native chain verification also returns a record. A trusted/stale chain
    // record is not authority: resolve the exact certificate against the live
    // registry below, without using this output's UUID or permissions.
    crypto::p_named_cert_t chain_record;
    if (const auto error = chain.verify(certificate.get(), chain_record)) {
      return {{}, error};
    }
    const auto presented = exact_certificate_identity(certificate);
    std::vector<std::string> identities;
    std::vector<pairing_policy::paired_client_record_view_t> records;
    if (!presented || !build_paired_client_records(clients, identities, records)) {
      return {{}, "Pairing state is invalid or ambiguous"};
    }
    const auto resolution = pairing_policy::resolve_paired_client(records, *presented);
    if (resolution.status != pairing_policy::paired_client_resolution_e::authorized) {
      return {{}, "Client certificate is not one exact enabled pairing record"};
    }
    const auto &client = clients[resolution.index];
    identity_t identity {std::string(client.uuid), std::string(client.name), client.allow_sleep};
    auto authority = std::make_shared<authority_t>(identity, client.certificate, *presented);
    {
      std::lock_guard lock(mutex);
      std::erase_if(authorities, [](const auto &entry) {
        return entry.expired();
      });
      authorities.emplace_back(authority);
      request->set_connection_context(authority);
      if (request->get_connection_context() != authority) {
        return {{}, "TLS connection no longer exists"};
      }
    }
    return {std::move(identity), nullptr};
  }

  std::optional<identity_t> store_t::resolve(const request_t &request, std::span<const client_record_view_t> clients) {
    if (!request) {
      return std::nullopt;
    }
    auto authority = std::static_pointer_cast<authority_t>(request->get_connection_context());
    if (!authority || !authority->authorized.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    if (!client_enabled(clients, authority->identity.uuid)) {
      authority->authorized.store(false, std::memory_order_release);
      return std::nullopt;
    }
    const auto client = std::find_if(clients.begin(), clients.end(), [&](const auto &record) {
      return record.uuid == authority->identity.uuid;
    });
    if (client->certificate != authority->certificate) {
      auto certificate = crypto::x509(client->certificate);
      const auto identity = exact_certificate_identity(certificate);
      if (!identity || *identity != authority->certificate_identity) {
        authority->authorized.store(false, std::memory_order_release);
        return std::nullopt;
      }
    }
    return identity_t {authority->identity.uuid, std::string(client->name), client->allow_sleep};
  }

  void store_t::reset(const request_t &request) {
    if (request) {
      if (auto previous = std::static_pointer_cast<authority_t>(request->get_connection_context())) {
        previous->authorized.store(false, std::memory_order_release);
      }
      request->set_connection_context({});
    }
  }

  void store_t::revoke(std::string_view uuid) {
    std::lock_guard lock(mutex);
    std::erase_if(authorities, [&](const auto &entry) {
      if (auto authority = entry.lock()) {
        if (authority->identity.uuid != uuid) {
          return false;
        }
        authority->authorized.store(false, std::memory_order_release);
      }
      return true;
    });
  }

  void store_t::clear() {
    std::lock_guard lock(mutex);
    for (const auto &entry : authorities) {
      if (auto authority = entry.lock()) {
        authority->authorized.store(false, std::memory_order_release);
      }
    }
    authorities.clear();
  }

}  // namespace nvhttp::tls_identity
