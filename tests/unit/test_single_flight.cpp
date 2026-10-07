#include "src/single_flight.h"

#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <vector>

TEST(SingleFlight, StalledOperationDoesNotQueueLaterMutations) {
  single_flight::admission_t admission;
  std::vector<std::function<void()>> queue;
  auto submit = [&](auto task) {
    queue.emplace_back(std::move(task));
  };
  std::promise<void> entered, release;
  auto release_future = release.get_future();
  ASSERT_TRUE(admission.try_submit(submit, [&] {
    entered.set_value();
    release_future.wait();
  }));
  int stale_mutations = 0;
  EXPECT_FALSE(admission.try_submit(submit, [&] {
    ++stale_mutations;
  }));
  std::thread worker([&] {
    queue.front()();
  });
  entered.get_future().wait();
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(admission.try_submit(submit, [&] {
      ++stale_mutations;
    }));
  }
  EXPECT_EQ(queue.size(), 1);
  release.set_value();
  worker.join();
  // The executor may retain its completed closure; that cannot retain admission.
  EXPECT_TRUE(admission.try_submit(submit, [&] {
    ++stale_mutations;
  }));
  queue.back()();
  EXPECT_EQ(stale_mutations, 1);
}

TEST(SingleFlight, DiscardedOrThrowingTasksReleaseAdmission) {
  single_flight::admission_t admission;
  std::function<void()> queued;
  auto submit = [&](auto task) {
    queued = std::move(task);
  };
  ASSERT_TRUE(admission.try_submit(submit, [] {
  }));
  queued = {};
  ASSERT_TRUE(admission.try_submit(submit, [] {
    throw std::runtime_error("operation");
  }));
  EXPECT_THROW(queued(), std::runtime_error);
  EXPECT_THROW(admission.try_submit([](auto) {
    throw std::runtime_error("enqueue");
  },
                                    [] {
                                    }),
               std::runtime_error);
  EXPECT_TRUE(admission.try_submit(submit, [] {
  }));
}

TEST(SingleFlight, QueuedWorkCanOutliveTheAdmissionOwner) {
  std::function<void()> queued;
  int calls = 0;
  {
    single_flight::admission_t admission;
    ASSERT_TRUE(admission.try_submit([&](auto task) {
      queued = std::move(task);
    },
                                     [&] {
                                       ++calls;
                                     }));
  }
  queued();
  EXPECT_EQ(calls, 1);
}
