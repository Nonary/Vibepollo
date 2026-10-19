/**
 * @brief Linked NVHTTP dispatch/admission tests with real broadcast controls.
 * TLS and route registration are outside this component test.
 */
#include "src/http_pairing_policy.h"
#include "src/nvhttp_dispatch.h"
#include "src/stream_broadcast_runtime.h"

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace runtime = stream::broadcast_runtime;
namespace pairing = nvhttp::pairing_policy;

namespace {
  constexpr std::string_view client_a = "11111111-1111-4111-8111-111111111111";
  constexpr std::string_view client_b = "22222222-2222-4222-8222-222222222222";
  constexpr std::array<pairing::paired_client_record_view_t, 2> paired_clients {{
    {client_a, "certificate-a", true},
    {client_b, "certificate-b", true},
  }};

  // Only socket output and executor scheduling are replaced. The response XML,
  // single-flight lease, pairing decision, shared_t and controls are production.
  struct response_t {
    bool close_connection_after_response = false;
    std::string body;

    void write(const std::string &value) {
      body += value;
    }
  };

  boost::property_tree::ptree read_response(const response_t &response) {
    std::istringstream input {response.body};
    boost::property_tree::ptree tree;
    boost::property_tree::read_xml(input, tree);
    return tree;
  }

  class NvhttpDispatch: public testing::Test {
  protected:
    struct context_t {
      std::mutex lock;
      std::vector<runtime::session_control_t> sessions;
    };

    struct join_boundary_t final: runtime::transport_t {
      bool pause = false;
      std::promise<void> entered;
      std::promise<void> release;
      std::future<void> released = release.get_future();

      void stop_receiving() override {
      }

      void close_sockets() override {
      }

      void join(runtime::worker_e worker) override {
        if (pause && worker == runtime::worker_e::receive) {
          entered.set_value();
          released.wait();
        }
      }
    } transport;

    safe::mail_t mail = std::make_shared<safe::mail_raw_t>();
    runtime::packet_queues_t queues = runtime::packet_queues<int, int>(mail, "shutdown", "video", "audio");
    safe::shared_t<context_t> broadcast {
      [&](context_t &) {
        auto startup = queues.reset_for_start();
        return 0;
      },
      [&](context_t &) {
        queues.stop_and_join(transport);
      }
    };
    bool admit = true;
    runtime::controls_t<context_t> controls {
      broadcast,
      [](context_t &ctx, const runtime::session_action_t &action) {
        std::lock_guard lock {ctx.lock};
        return action(ctx.sessions);
      },
      [&](context_t &, const std::string &, int) {
        return admit;
      }
    };
    single_flight::admission_t admission;
    std::vector<std::function<void()>> pending;

    template<class Task>
    void dispatch(const std::shared_ptr<response_t> &response, const char *operation, Task &&task) {
      nvhttp::dispatch::run_blocking(admission, [&](auto admitted) {
        pending.emplace_back(std::move(admitted));
      },
                                     response,
                                     operation,
                                     std::forward<Task>(task));
    }
  };

  TEST_F(NvhttpDispatch, PairedBitrateReturnsFromDispatchWhileFinalReleaseOwnsLifecycleMutex) {
    const auto identity = pairing::resolve_paired_client(paired_clients, "certificate-a");
    ASSERT_EQ(identity.status, pairing::paired_client_resolution_e::authorized);
    const std::string uuid {paired_clients[identity.index].uuid};
    auto owner = broadcast.ref();
    ASSERT_TRUE(owner);
    transport.pause = true;
    auto teardown_entered = transport.entered.get_future();
    std::promise<void> handler_entered, handler_done;
    auto entered = handler_entered.get_future();
    auto done = handler_done.get_future();
    std::atomic<int> result {-99};
    auto response = std::make_shared<response_t>();
    std::thread worker;
    std::thread teardown([&] {
      owner.release();
    });
    auto cleanup = util::fail_guard([&] {
      transport.release.set_value();
      teardown.join();
      if (worker.joinable()) {
        worker.join();
      }
      transport.pause = false;
    });
    EXPECT_EQ(teardown_entered.wait_for(2s), std::future_status::ready);

    dispatch(response, "bitrate", [&, uuid] {
      handler_entered.set_value();
      result = controls.set_bitrate_for_sessions(uuid, 12000);
      handler_done.set_value();
    });
    // Dispatch already returned. The admitted handler has not run inline.
    EXPECT_EQ(pending.size(), 1);
    EXPECT_EQ(result.load(), -99);
    worker = std::thread([&] {
      pending.front()();
    });
    EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(done.wait_for(50ms), std::future_status::timeout);

    int rejected_calls = 0;
    for (int request = 0; request < 100; ++request) {
      auto busy = std::make_shared<response_t>();
      dispatch(busy, "bitrate", [&] {
        ++rejected_calls;
      });
      const auto tree = read_response(*busy);
      EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
      EXPECT_EQ(tree.get<int>("root.bitrate"), 0);
      EXPECT_TRUE(busy->close_connection_after_response);
    }
    EXPECT_EQ(pending.size(), 1);
    EXPECT_EQ(rejected_calls, 0);

    // Release the scheduling boundary and join every real test thread.
    transport.release.set_value();
    teardown.join();
    worker.join();
    transport.pause = false;
    cleanup.disable();
    EXPECT_EQ(done.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(result.load(), 0);

    dispatch(response, "bitrate", [] {
    });
    EXPECT_EQ(pending.size(), 2) << "a retained completed closure must release admission";
    pending.back()();
  }

  TEST_F(NvhttpDispatch, PairedClientFilteringAndPolicyRejectionSurviveDispatch) {
    const auto identity = pairing::resolve_paired_client(paired_clients, "certificate-a");
    ASSERT_EQ(identity.status, pairing::paired_client_resolution_e::authorized);
    const std::string uuid {paired_clients[identity.index].uuid};
    auto owner = broadcast.ref();
    ASSERT_TRUE(owner);
    int bitrate_a = 1000, requested_a = 1000, bitrate_b = 2000, requested_b = 2000;
    auto event_a = mail->event<int>("bitrate-a");
    auto event_b = mail->event<int>("bitrate-b");
    owner->sessions = {
      {client_a, bitrate_a, requested_a, {}, event_a},
      {client_b, bitrate_b, requested_b, {}, event_b},
    };
    auto response = std::make_shared<response_t>();
    int result = -99;
    dispatch(response, "bitrate", [&] {
      result = controls.set_bitrate_for_sessions(uuid, 12000);
    });
    EXPECT_EQ(result, -99);
    pending.back()();
    EXPECT_EQ(result, 1);
    EXPECT_EQ(bitrate_a, 12000);
    EXPECT_EQ(requested_a, 12000);
    EXPECT_EQ(event_a->pop(0ms), 12000);
    EXPECT_EQ(bitrate_b, 2000);
    EXPECT_EQ(requested_b, 2000);
    EXPECT_FALSE(event_b->peek());

    admit = false;
    dispatch(response, "bitrate", [&] {
      result = controls.set_bitrate_for_sessions(uuid, 24000);
    });
    pending.back()();
    EXPECT_EQ(result, -1);
    EXPECT_EQ(bitrate_a, 12000);
    EXPECT_EQ(requested_a, 12000);
    EXPECT_FALSE(event_a->peek());
    EXPECT_TRUE(response->body.empty()) << "accepted dispatch leaves the handler responsible for its response";
    owner.release();
  }

  TEST_F(NvhttpDispatch, OtherMutationOccupiesTheSameAdmissionSlot) {
    auto launch_response = std::make_shared<response_t>();
    dispatch(launch_response, "launch", [] {
    });
    auto bitrate_response = std::make_shared<response_t>();
    bool stale_bitrate = false;
    dispatch(bitrate_response, "bitrate", [&] {
      stale_bitrate = true;
    });
    EXPECT_EQ(read_response(*bitrate_response).get<int>("root.<xmlattr>.status_code"), 503);
    EXPECT_EQ(pending.size(), 1);
    pending.front()();
    EXPECT_FALSE(stale_bitrate);
  }
}  // namespace
