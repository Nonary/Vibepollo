/**
 * @file tests/unit/test_http_pairing.cpp
 * @brief Test the data-only HTTP pairing policy used by src/nvhttp.cpp.
 */

#include "../tests_common.h"

#include <src/http_pairing_policy.h>

namespace pairing_policy = nvhttp::pairing_policy;

TEST(HttpPairingCertLogging, MissingSubjectNameIsEmptyInsteadOfCrashing) {
  ASSERT_EQ(pairing_policy::certificate_subject_name(std::nullopt), "");

  const std::optional<std::string_view> empty_subject {std::string_view {}};
  ASSERT_TRUE(empty_subject);
  ASSERT_EQ(pairing_policy::certificate_subject_name(empty_subject), "");
}

TEST(HttpPairingClientNames, DisplayClientNameSkipsSelfPlaceholder) {
  ASSERT_EQ(pairing_policy::display_client_name("Living Room", "TTV", "Vibeshine"), "Living Room");
  ASSERT_EQ(pairing_policy::display_client_name(" self ", "TTV", "Vibeshine"), "TTV");
  ASSERT_EQ(pairing_policy::display_client_name("", " self ", "Vibeshine"), "Vibeshine");
  ASSERT_EQ(pairing_policy::display_client_name("self", "", ""), "Sunshine");
}

struct pairing_input {
  pairing_policy::session_state_t session;
  std::size_t salt_size = 0;
  std::size_t pairing_secret_size = 0;
  bool client_certificate_valid = false;
  bool client_hash_matches = false;
  bool client_secret_signature_valid = false;
  std::string client_certificate_subject;
};

struct pairing_output {
  bool phase_1_success = false;
  bool phase_2_success = false;
  bool phase_3_success = false;
  bool phase_4_success = false;
};

struct PairingTest: testing::TestWithParam<std::tuple<pairing_input, pairing_output>> {};

TEST_P(PairingTest, Run) {
  auto [input, expected] = GetParam();

  // phase 1: accept a new pairing request only with a complete hex salt.
  auto phase_1 = pairing_policy::begin_get_server_certificate(input.session, input.salt_size);
  ASSERT_EQ(phase_1.accepted, expected.phase_1_success);
  if (!expected.phase_1_success) {
    return;
  }
  input.session.phase = phase_1.next_phase;
  input.session.has_cipher_key = true;

  // phase 2: the crypto adapter has a cipher key after phase 1.
  auto phase_2 = pairing_policy::begin_client_challenge(input.session);
  ASSERT_EQ(phase_2.accepted, expected.phase_2_success);
  if (!expected.phase_2_success) {
    return;
  }
  input.session.phase = phase_2.next_phase;
  input.session.has_server_secret = true;

  // phase 3: the crypto adapter has generated a server secret after phase 2.
  auto phase_3 = pairing_policy::begin_server_challenge_response(input.session);
  ASSERT_EQ(phase_3.accepted, expected.phase_3_success);
  if (!expected.phase_3_success) {
    return;
  }
  input.session.phase = phase_3.next_phase;

  // phase 4: crypto reports certificate parsing, hash comparison, and signature verification.
  auto phase_4_begin = pairing_policy::begin_client_pairing_secret(input.session, input.pairing_secret_size);
  pairing_policy::decision_t phase_4 = phase_4_begin;
  if (phase_4_begin.accepted) {
    input.session.phase = phase_4_begin.next_phase;
    phase_4 = pairing_policy::decide_client_pairing_secret(
      input.session,
      input.client_certificate_valid,
      input.client_hash_matches,
      input.client_secret_signature_valid
    );
  }
  ASSERT_EQ(phase_4.accepted, expected.phase_4_success);

  // A successful policy decision retains the identity supplied by the crypto adapter.
  if (expected.phase_4_success) {
    const auto added_certificate_subject = pairing_policy::certificate_subject_name(input.client_certificate_subject);
    ASSERT_EQ(!added_certificate_subject.empty(), true);
    ASSERT_EQ(added_certificate_subject, input.client_certificate_subject);
  }
}

constexpr std::size_t valid_salt_size = 32;
constexpr std::size_t valid_pairing_secret_size = 17;

pairing_input valid_pairing_input() {
  return {
    .session = {},
    .salt_size = valid_salt_size,
    .pairing_secret_size = valid_pairing_secret_size,
    .client_certificate_valid = true,
    .client_hash_matches = true,
    .client_secret_signature_valid = true,
    .client_certificate_subject = "test",
  };
}

INSTANTIATE_TEST_SUITE_P(
  TestWorkingPairing,
  PairingTest,
  testing::Values(
    std::make_tuple(valid_pairing_input(), pairing_output {true, true, true, true}),
    // Empty client identity data reaches phase 4 but cannot authorize a certificate.
    std::make_tuple(
      pairing_input {.salt_size = valid_salt_size, .pairing_secret_size = valid_pairing_secret_size},
      pairing_output {true, true, true, false}
    ),
    // A parsed certificate without an authenticated challenge must not pair.
    std::make_tuple(
      pairing_input {
        .salt_size = valid_salt_size,
        .pairing_secret_size = valid_pairing_secret_size,
        .client_certificate_valid = true,
      },
      pairing_output {true, true, true, false}
    )
  )
);

INSTANTIATE_TEST_SUITE_P(
  TestFailingPairing,
  PairingTest,
  testing::Values(
    // Wrong PIN: the crypto adapter reports that the client hash does not match.
    std::make_tuple(
      pairing_input {
        .salt_size = valid_salt_size,
        .pairing_secret_size = valid_pairing_secret_size,
        .client_certificate_valid = true,
        .client_secret_signature_valid = true,
      },
      pairing_output {true, true, true, false}
    ),
    // Wrong client challenge: the decrypted challenge produces a different hash.
    std::make_tuple(
      pairing_input {
        .salt_size = valid_salt_size,
        .pairing_secret_size = valid_pairing_secret_size,
        .client_certificate_valid = true,
        .client_secret_signature_valid = true,
      },
      pairing_output {true, true, true, false}
    ),
    // Wrong signature: the signed client secret does not verify.
    std::make_tuple(
      pairing_input {
        .salt_size = valid_salt_size,
        .pairing_secret_size = valid_pairing_secret_size,
        .client_certificate_valid = true,
        .client_hash_matches = true,
      },
      pairing_output {true, true, true, false}
    ),
    // Null values at phase 1: a pairing request cannot start without a full salt.
    std::make_tuple(pairing_input {}, pairing_output {false}),
    // Null client values reach phase 4 but fail certificate validation.
    std::make_tuple(
      pairing_input {.salt_size = valid_salt_size, .pairing_secret_size = valid_pairing_secret_size},
      pairing_output {true, true, true, false}
    )
  )
);

TEST(PairingTest, OutOfOrderCalls) {
  pairing_policy::session_state_t session {};

  auto client_challenge = pairing_policy::begin_client_challenge(session);
  ASSERT_FALSE(client_challenge.accepted);

  auto server_challenge_response = pairing_policy::begin_server_challenge_response(session);
  ASSERT_FALSE(server_challenge_response.accepted);

  auto client_pairing_secret = pairing_policy::begin_client_pairing_secret(session, valid_pairing_secret_size);
  ASSERT_FALSE(client_pairing_secret.accepted);

  // This should work, it's the first time we call it.
  auto server_certificate = pairing_policy::begin_get_server_certificate(session, valid_salt_size);
  ASSERT_TRUE(server_certificate.accepted);
  session.phase = server_certificate.next_phase;

  // Calling it again should fail.
  server_certificate = pairing_policy::begin_get_server_certificate(session, valid_salt_size);
  ASSERT_FALSE(server_certificate.accepted);
}
