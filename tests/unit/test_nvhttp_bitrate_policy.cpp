/**
 * @brief Actual native bitrate permission/dispatch policy with native records.
 * Registry lookup, socket output and executor scheduling are the boundaries.
 * The complete NVHTTP handler, TLS transport and native pacing adapter are not
 * linked here; existing component/pacing tests cover their stated boundaries.
 */
#include "src/nvhttp_bitrate_policy.h"
#include "src/nvhttp_dispatch.h"
#include "src/stream_broadcast_runtime.h"

#include <atomic>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <vector>

namespace policy = nvhttp::bitrate_policy;
namespace runtime = stream::broadcast_runtime;
using crypto::PERM;

namespace {
  constexpr std::string_view client_a = "11111111-1111-4111-8111-111111111111";
  constexpr std::string_view client_b = "22222222-2222-4222-8222-222222222222";
  constexpr std::string_view denied_status =
    "Permission denied: this device lacks the \"View stream\" permission. Enable it on the host in the Vibepollo Web UI under Client Management.";
  constexpr std::string_view denied_log =
    "Permission SetBitrate denied for client [client-a]: it lacks the \"View stream\" permission (current permission mask 0x1000000). Grant it in the Web UI under Client Management.";
  constexpr std::string_view unknown_status =
    "Permission denied: this device's certificate is not recognized by the host. Unpair and pair this device again.";
  constexpr std::string_view unknown_log =
    "Permission SetBitrate denied: HTTPS client certificate not recognized. The device may need to be re-paired.";

  struct response_t {
    bool close_connection_after_response = false;
    bool closed_at_write = false;
    int writes = 0;
    std::string body;

    void write(const std::string &data) {
      closed_at_write = close_connection_after_response;
      ++writes;
      body += data;
    }
  };

  struct request_t {
    int verified_connection = 17;
    std::string untrusted_query_uuid {client_b};
  };

  boost::property_tree::ptree read_response(const response_t &response) {
    std::istringstream input {response.body};
    boost::property_tree::ptree tree;
    boost::property_tree::read_xml(input, tree);
    return tree;
  }

  class NativeBitratePolicy: public testing::Test {
  protected:
    // Native registry records and permission bits are production types. The
    // resolver stands in only for the locked, connection-owned TLS registry.
    policy::verified_client_t record {crypto::named_cert_t {}};
    int resolves = 0;
    int submits = 0;
    int handler_calls = 0;
    int result = -99;
    std::vector<std::string> logs;
    std::function<void()> on_denial_log;
    single_flight::admission_t admission;
    std::vector<std::function<void()>> pending;
    std::shared_ptr<response_t> response = std::make_shared<response_t>();
    std::shared_ptr<request_t> request = std::make_shared<request_t>();

    struct context_t {
      std::mutex mutex;
      std::vector<runtime::session_control_t> sessions;
    };

    safe::shared_t<context_t> broadcast {[](context_t &) {
                                           return 0;
                                         },
                                         [](context_t &) {
                                         }};
    safe::shared_t<context_t>::ptr_t owner;
    safe::mail_t mail = std::make_shared<safe::mail_raw_t>();
    safe::mail_raw_t::event_t<int> event_a = mail->event<int>("bitrate-a");
    safe::mail_raw_t::event_t<int> event_b = mail->event<int>("bitrate-b");
    int bitrate_a = 1000, requested_a = 1000, bitrate_b = 2000, requested_b = 2000;
    std::atomic<int> pacing_bitrate_a {1000};
    std::atomic<int> pacing_bitrate_b {2000};
    runtime::controls_t<context_t> controls {
      broadcast,
      [](context_t &ctx, const runtime::session_action_t &action) {
        std::lock_guard lock {ctx.mutex};
        return action(ctx.sessions);
      }
    };

    void SetUp() override {
      record->name = "client-a";
      record->uuid = client_a;
      record->perm = PERM::view;
      owner = broadcast.ref();
      ASSERT_TRUE(owner);
      owner->sessions = {
        {client_a, bitrate_a, requested_a, pacing_bitrate_a, {}, event_a},
        {client_b, bitrate_b, requested_b, pacing_bitrate_b, {}, event_b},
      };
    }

    template<class Task>
    void blocking(std::shared_ptr<response_t> resp, const char *operation, Task task) {
      nvhttp::dispatch::run_blocking(admission, [&](auto admitted) {
        ++submits;
        pending.emplace_back(std::move(admitted));
      },
                                     std::move(resp),
                                     operation,
                                     std::move(task));
    }

    void occupy_with_launch() {
      blocking(std::make_shared<response_t>(), "launch", [] {
      });
      ASSERT_EQ(submits, 1);
      ASSERT_EQ(pending.size(), 1);
    }

    void dispatch() {
      policy::run(
        response,
        request,
        [&](const auto &req) -> policy::verified_client_t {
          ++resolves;
          // The real native adapter obtains identity from the TLS connection;
          // request query UUID is intentionally not consulted by this boundary.
          EXPECT_EQ(req->verified_connection, 17);
          return record;
        },
        [&](auto resp, const char *operation, auto task) {
          blocking(std::move(resp), operation, std::move(task));
        },
        [&](auto, auto, const policy::verified_client_t &client) {
          ++handler_calls;
          // Exercise the actual existing-session component, not a copied
          // mutation. Its session adapter is an in-memory registry boundary.
          result = controls.set_bitrate_for_sessions(client->uuid, 12000);
        },
        [&](const std::string &message) {
          logs.push_back(message);
          if (on_denial_log) {
            on_denial_log();
          }
        }
      );
    }

    void expect_denied(std::string_view status = denied_status, std::string_view log = denied_log) {
      ASSERT_FALSE(response->body.empty());
      const auto tree = read_response(*response);
      EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 403);
      EXPECT_EQ(tree.get<int>("root.bitrate"), 0);
      EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), status);
      EXPECT_EQ(response->writes, 1);
      EXPECT_TRUE(response->close_connection_after_response);
      EXPECT_FALSE(response->closed_at_write) << "preserve native denial's write-then-close ordering";
      ASSERT_EQ(logs.size(), 1);
      EXPECT_EQ(logs.front(), log);
    }

    void expect_no_mutation() {
      EXPECT_EQ(handler_calls, 0);
      EXPECT_EQ(result, -99);
      EXPECT_EQ(bitrate_a, 1000);
      EXPECT_EQ(requested_a, 1000);
      EXPECT_EQ(pacing_bitrate_a.load(std::memory_order_relaxed), 1000);
      EXPECT_EQ(bitrate_b, 2000);
      EXPECT_EQ(requested_b, 2000);
      EXPECT_EQ(pacing_bitrate_b.load(std::memory_order_relaxed), 2000);
      EXPECT_FALSE(event_a->peek());
      EXPECT_FALSE(event_b->peek());
    }
  };

  TEST_F(NativeBitratePolicy, PairedNoViewGets403BeforeOccupiedAdmission) {
    record->perm = PERM::list;
    occupy_with_launch();
    dispatch();
    expect_denied();
    EXPECT_EQ(resolves, 1);
    EXPECT_EQ(submits, 1) << "only the original launch was submitted";
    EXPECT_EQ(pending.size(), 1);
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, AuthorizedOccupiedAdmissionGets503WithoutSubmission) {
    occupy_with_launch();
    dispatch();
    const auto tree = read_response(*response);
    EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(tree.get<int>("root.bitrate"), 0);
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Another stream operation is still running. Retry after it completes; if this persists, check the host GPU and system-sleep logs.");
    EXPECT_TRUE(response->close_connection_after_response);
    EXPECT_EQ(submits, 1);
    EXPECT_EQ(pending.size(), 1);
    EXPECT_EQ(resolves, 1);
    EXPECT_TRUE(logs.empty());
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, AuthorizedIdleRunsOnWorkerAndFiltersByVerifiedUuid) {
    ASSERT_EQ(request->untrusted_query_uuid, client_b);
    dispatch();
    EXPECT_EQ(submits, 1);
    ASSERT_EQ(pending.size(), 1);
    EXPECT_EQ(resolves, 1);
    EXPECT_TRUE(response->body.empty());
    expect_no_mutation();
    pending.front()();
    EXPECT_EQ(resolves, 2);
    EXPECT_EQ(handler_calls, 1);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(bitrate_a, 12000);
    EXPECT_EQ(requested_a, 12000);
    EXPECT_EQ(pacing_bitrate_a.load(std::memory_order_relaxed), 12000);
    EXPECT_EQ(event_a->pop(std::chrono::milliseconds {0}), 12000);
    EXPECT_EQ(bitrate_b, 2000);
    EXPECT_EQ(requested_b, 2000);
    EXPECT_EQ(pacing_bitrate_b.load(std::memory_order_relaxed), 2000);
    EXPECT_FALSE(event_b->peek());
    EXPECT_TRUE(logs.empty());
  }

  TEST_F(NativeBitratePolicy, PermissionRevocationBeforeWorkerGets403WithoutMutation) {
    dispatch();
    ASSERT_EQ(pending.size(), 1);
    record->perm = PERM::list;
    pending.front()();
    expect_denied();
    EXPECT_EQ(resolves, 2);
    EXPECT_EQ(submits, 1);
    expect_no_mutation();
    // A completed denial releases the real admission lease even if its closure
    // remains retained by the executor.
    record->perm = PERM::view;
    response = std::make_shared<response_t>();
    dispatch();
    EXPECT_EQ(submits, 2);
  }

  TEST_F(NativeBitratePolicy, PairingRemovalBeforeWorkerGets403WithoutMutation) {
    dispatch();
    ASSERT_EQ(pending.size(), 1);
    record.reset();
    pending.front()();
    expect_denied(unknown_status, unknown_log);
    EXPECT_EQ(resolves, 2);
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, GrantDuringListenerDenialCannotInvokeHandlerInline) {
    record->perm = PERM::list;
    on_denial_log = [&] {
      record->perm = PERM::view;
    };
    dispatch();
    expect_denied();
    EXPECT_EQ(record->perm, PERM::view) << "the grant occurred during the denial path";
    EXPECT_EQ(submits, 0);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(resolves, 1);
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, GrantDuringWorkerDenialCannotInvokeMutation) {
    dispatch();
    ASSERT_EQ(pending.size(), 1);
    record->perm = PERM::list;
    on_denial_log = [&] {
      record->perm = PERM::view;
    };
    pending.front()();
    expect_denied();
    EXPECT_EQ(record->perm, PERM::view);
    EXPECT_EQ(resolves, 2);
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, NativeLaunchPermissionAlsoAllowsView) {
    record->perm = PERM::launch;
    dispatch();
    ASSERT_EQ(pending.size(), 1);
    pending.front()();
    EXPECT_EQ(handler_calls, 1);
    EXPECT_EQ(result, 1);
    EXPECT_TRUE(logs.empty());
  }

  TEST_F(NativeBitratePolicy, RequestUuidCannotGrantMissingNativePermission) {
    record->perm = PERM::list;
    request->untrusted_query_uuid = client_b;
    dispatch();
    expect_denied();
    EXPECT_EQ(submits, 0);
    EXPECT_TRUE(pending.empty());
    expect_no_mutation();
  }

  TEST_F(NativeBitratePolicy, UnrecognizedConnectionDenialAlsoPrecedesBusyAdmission) {
    record.reset();
    occupy_with_launch();
    dispatch();
    expect_denied(unknown_status, unknown_log);
    EXPECT_EQ(submits, 1);
    EXPECT_EQ(resolves, 1);
    expect_no_mutation();
  }
}  // namespace
