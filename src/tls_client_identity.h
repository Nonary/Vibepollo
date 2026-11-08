/**
 * @file src/tls_client_identity.h
 * @brief Certificate-bound GameStream transport authorization.
 */
#pragma once

#include "crypto.h"
#include "http_pairing_policy.h"
#include "nvhttp_server.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nvhttp::tls_identity {

  using request_t = std::shared_ptr<SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  struct client_record_view_t {
    std::string_view uuid;
    std::string_view name;
    std::string_view certificate;
    bool enabled = false;
    bool allow_sleep = false;
  };

  struct identity_t {
    std::string uuid;
    std::string name;
    bool allow_sleep = false;
  };

  struct authentication_result_t {
    std::optional<identity_t> identity;
    const char *error = nullptr;
  };

  std::optional<std::string> exact_certificate_identity(const crypto::x509_t &certificate);
  bool build_paired_client_records(
    std::span<const client_record_view_t> clients,
    std::vector<std::string> &certificate_identities,
    std::vector<pairing_policy::paired_client_record_view_t> &records,
    std::vector<crypto::x509_t> *parsed_certificates = nullptr
  );
  bool client_enabled(std::span<const client_record_view_t> clients, std::string_view uuid);

  // Callers hold their pairing-registry mutex across authenticate/resolve so
  // the supplied live views and the certificate-chain store remain coherent.
  // The store separately synchronizes transport publication and revocation.
  class store_t {
  public:
    authentication_result_t authenticate(
      const request_t &request,
      SSL *ssl,
      crypto::cert_chain_t &chain,
      std::span<const client_record_view_t> clients,
      pending_peer_factory_t pending_peer = {}
    );
    authentication_result_t authenticate_certificate(
      const request_t &request,
      crypto::x509_t &certificate,
      crypto::cert_chain_t &chain,
      std::span<const client_record_view_t> clients,
      pending_peer_factory_t pending_peer = {}
    );
    std::optional<identity_t> resolve(const request_t &request, std::span<const client_record_view_t> clients);
    pending_peer_factory_t pending_peer(const request_t &request);
    void reset(const request_t &request);
    void revoke(std::string_view uuid);
    void clear();

  private:
    struct authority_t {
      const identity_t identity;
      const std::string certificate;
      const std::string certificate_identity;
      const pending_peer_factory_t pending_peer;
      std::atomic_bool authorized {true};

      authority_t(identity_t value, std::string_view pem, std::string_view der, pending_peer_factory_t peer):
          identity(std::move(value)),
          certificate(pem),
          certificate_identity(der),
          pending_peer(std::move(peer)) {}
    };

    std::mutex mutex;
    // Transports own authority; weak references provide immediate revocation
    // without retaining closed connections or evicting any live transport.
    std::vector<std::weak_ptr<authority_t>> authorities;
  };

}  // namespace nvhttp::tls_identity
