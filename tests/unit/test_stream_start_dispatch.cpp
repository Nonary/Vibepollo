#include "src/host_stream_control.h"
#include "src/nvhttp_dispatch.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {
  struct response_t {
    bool close_connection_after_response = false;
    std::string body;

    void write(const std::string &value) {
      body = value;
    }
  };
}  // namespace

TEST(StreamStartDispatch, QueuedNvhttpLaunchAndResumeAreCanceledWithProtocolResponse) {
  for (const auto *operation : {"launch", "resume"}) {
    single_flight::admission_t mutation;
    std::function<void()> queued;
    auto response = std::make_shared<response_t>();
    bool started = false;
    nvhttp::dispatch::run_start(mutation, [&](auto task) {
      queued = std::move(task);
    },
                                response,
                                operation,
                                [&] {
                                  started = true;
                                });
    ASSERT_TRUE(queued);
    EXPECT_TRUE(response->body.empty());
    auto busy = std::make_shared<response_t>();
    nvhttp::dispatch::run_start(mutation, [](auto task) {
      task();
    },
                                busy,
                                operation,
                                [&] {
                                  ADD_FAILURE() << "single-flight bypass";
                                });
    EXPECT_NE(busy->body.find("503"), std::string::npos);
    EXPECT_TRUE(host_stream_control::terminate([] {
      return true;
    },
                                               [] {
                                               }));
    queued();
    EXPECT_FALSE(started);
    EXPECT_TRUE(response->close_connection_after_response);
    EXPECT_NE(response->body.find("503"), std::string::npos);
    EXPECT_NE(response->body.find("canceled"), std::string::npos);
    auto fresh = std::make_shared<response_t>();
    nvhttp::dispatch::run_start(mutation, [](auto task) {
      task();
    },
                                fresh,
                                operation,
                                [&] {
                                  started = true;
                                });
    EXPECT_TRUE(started);
    EXPECT_TRUE(fresh->body.empty());
  }
}

TEST(StreamStartDispatch, ActualWebrtcQueueAdapterCancelsOldAndDuringTerminalIntake) {
  std::function<void()> old, during;
  int started = 0, rejected = 0;
  auto start = [&] {
    ++started;
  };
  auto reject = [&] {
    ++rejected;
  };
  host_stream_control::submit_start([&](auto task) {
    old = std::move(task);
  },
                                    start,
                                    reject);
  ASSERT_TRUE(host_stream_control::terminate([] {
    return true;
  },
                                             [&] {
                                               host_stream_control::submit_start([&](auto task) {
                                                 during = std::move(task);
                                               },
                                                                                 start,
                                                                                 reject);
                                             }));
  old();
  during();
  EXPECT_EQ(started, 0);
  EXPECT_EQ(rejected, 2);
  host_stream_control::submit_start([](auto task) {
    task();
  },
                                    start,
                                    reject);
  EXPECT_EQ(started, 1);
}

TEST(StreamStartDispatch, RejectedTerminalAuthorizationPreservesAcceptedWork) {
  std::function<void()> queued;
  bool started = false;
  host_stream_control::submit_start([&](auto task) {
    queued = std::move(task);
  },
                                    [&] {
                                      started = true;
                                    },
                                    [] {
                                      ADD_FAILURE();
                                    });
  EXPECT_FALSE(host_stream_control::terminate([] {
    return false;
  },
                                              [] {
                                                ADD_FAILURE();
                                              }));
  queued();
  EXPECT_TRUE(started);
}

TEST(StreamStartupDispatch, TerminalRetiresQueuedRuntimeOwnersBeforeFinalRestore) {
  host_stream_control::startup_dispatch_t startup;
  std::function<void()> worker, response;
  bool started = false, canceled = false, armed = true, final_restore = false;
  int ordinary_finalizers = 0;
  startup.submit([&](auto task) {
    worker = std::move(task);
  },
                 [&](bool admitted) {
                   started = admitted;
                   startup.post_response([&](auto task) {
                     response = std::move(task);
                   },
                                         [&] {
                                           canceled = !started;
                                         });
                 },
                 [&] {
                   ++ordinary_finalizers;
                   armed = false;
                 });
  ASSERT_EQ(startup.count(), 1);
  ASSERT_EQ(startup.total_count(), 1);
  ASSERT_TRUE(host_stream_control::terminate([] {
    return true;
  },
                                             [&] {
                                               // Mirrors terminate_sessions/shared-runtime owner lookup, then the explicit
                                               // final idle cleanup before virtual_display_cleanup::terminate_all.
                                               EXPECT_EQ(startup.count(), 0);
                                               if (startup.count() == 0) {
                                                 armed = false;
                                               }
                                               EXPECT_FALSE(armed);
                                               final_restore = true;
                                             }));
  ASSERT_TRUE(final_restore);
  worker();
  EXPECT_EQ(startup.total_count(), 1);  // socket response keeps shutdown alive
  EXPECT_EQ(startup.count(), 0);  // but cannot retain runtime ownership
  response();
  EXPECT_FALSE(started);
  EXPECT_TRUE(canceled);
  EXPECT_EQ(ordinary_finalizers, 0);
  EXPECT_EQ(startup.total_count(), 0);
}

TEST(StreamStartupDispatch, DelayedSocketCompletionCannotFinalizeAfterRestoreOrFreshStart) {
  host_stream_control::startup_dispatch_t startup;
  std::function<void()> response;
  int finalizers = 0, responses = 0;
  startup.submit([](auto worker) {
    worker();
  },
                 [&](bool admitted) {
                   ASSERT_TRUE(admitted);
                   startup.post_response([&](auto task) {
                     response = std::move(task);
                   },
                                         [&] {
                                           ++responses;
                                         });
                 },
                 [&] {
                   EXPECT_EQ(startup.count(), 0);
                   ++finalizers;
                 });
  EXPECT_EQ(finalizers, 1);
  EXPECT_EQ(responses, 0);
  EXPECT_EQ(startup.count(), 0);
  EXPECT_EQ(startup.total_count(), 1);
  bool restored = false;
  ASSERT_TRUE(host_stream_control::terminate([] {
    return true;
  },
                                             [&] {
                                               restored = true;
                                             }));
  bool fresh_started = false;
  host_stream_control::submit_start([](auto task) {
    task();
  },
                                    [&] {
                                      fresh_started = true;
                                    },
                                    [] {
                                      ADD_FAILURE();
                                    });
  response();
  EXPECT_TRUE(restored);
  EXPECT_TRUE(fresh_started);
  EXPECT_EQ(finalizers, 1);
  EXPECT_EQ(responses, 1);
}

TEST(StreamStartupDispatch, TerminalWaitsForWorkerFinalizerWithoutWaitingForIoCompletion) {
  host_stream_control::startup_dispatch_t startup;
  std::promise<void> finalizing, release;
  auto entered = finalizing.get_future();
  auto resume = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
    startup.submit([](auto task) {
      task();
    },
                   [](bool admitted) {
                     EXPECT_TRUE(admitted);
                   },
                   [&] {
                     finalizing.set_value();
                     resume.wait();
                   });
  });
  ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
  std::promise<void> terminal_attempted;
  auto attempted = terminal_attempted.get_future();
  auto terminal = std::async(std::launch::async, [&] {
    terminal_attempted.set_value();
    return host_stream_control::terminate([] {
      return true;
    },
                                          [] {
                                          });
  });
  ASSERT_EQ(attempted.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(terminal.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(worker.wait_for(2s), std::future_status::ready);
  worker.get();
  ASSERT_EQ(terminal.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(terminal.get());
}

TEST(StreamStartupDispatch, WorkerAndSubmissionExceptionsRetireExactlyOnceAndReopenGate) {
  host_stream_control::startup_dispatch_t startup;
  int finalizers = 0;
  EXPECT_THROW(startup.submit([](auto worker) {
    worker();
  },
                              [](bool) {
                                throw std::runtime_error("worker");
                              },
                              [&] {
                                ++finalizers;
                              }),
               std::runtime_error);
  EXPECT_EQ(startup.total_count(), 0);
  EXPECT_EQ(finalizers, 1);
  EXPECT_THROW(startup.submit([](auto) {
    throw std::runtime_error("queue");
  },
                              [](bool) {
                              },
                              [&] {
                                ++finalizers;
                              }),
               std::runtime_error);
  EXPECT_EQ(startup.total_count(), 0);
  EXPECT_EQ(finalizers, 2);
  EXPECT_THROW(host_stream_control::terminate([] {
                 return true;
               },
                                              [] {
                                                throw std::runtime_error("cleanup");
                                              }),
               std::runtime_error);
  host_stream_control::submit_start([](auto task) {
    task();
  },
                                    [] {
                                    },
                                    [] {
                                      ADD_FAILURE() << "gate stayed closed";
                                    });
}
