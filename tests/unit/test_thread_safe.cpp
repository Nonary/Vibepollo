/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Test src/thread_safe.h
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "src/thread_safe.h"

TEST(MailRegistryTests, QueueLookupReplacesExpiredPost) {
  constexpr auto id = "stale_queue";
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto original = mail->queue<int>(id);
  std::weak_ptr<void> stale = original;

  original.reset();
  ASSERT_TRUE(stale.expired());
  mail->id_to_post.emplace(id, stale);

  auto replacement = mail->queue<int>(id);

  ASSERT_NE(replacement, nullptr);
  EXPECT_FALSE(std::weak_ptr<void> {replacement}.expired());
}

TEST(MailRegistryTests, EventLookupReplacesExpiredPost) {
  constexpr auto id = "stale_event";
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto original = mail->event<bool>(id);
  std::weak_ptr<void> stale = original;

  original.reset();
  ASSERT_TRUE(stale.expired());
  mail->id_to_post.emplace(id, stale);

  auto replacement = mail->event<bool>(id);

  ASSERT_NE(replacement, nullptr);
  EXPECT_FALSE(std::weak_ptr<void> {replacement}.expired());
}

TEST(ThreadSafeQueueTests, PeekAndRunningTrackLockedQueueState) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue;
  EXPECT_TRUE(queue.running());
  EXPECT_FALSE(queue.peek());

  queue.raise(7);
  EXPECT_TRUE(queue.peek());
  ASSERT_EQ(queue.pop(0ms), 7);
  EXPECT_FALSE(queue.peek());

  queue.stop();
  EXPECT_FALSE(queue.running());
  EXPECT_FALSE(queue.pop(0ms));
}

TEST(ThreadSafeQueueTests, WaitForDataDoesNotConsumeTheQueuedValue) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue;
  std::thread producer {[&] {
    std::this_thread::sleep_for(1ms);
    queue.raise(9);
  }};

  EXPECT_TRUE(queue.wait_for_data(100ms));
  ASSERT_EQ(queue.pop(0ms), 9);
  EXPECT_FALSE(queue.wait_for_data(0ms));
  producer.join();
}

TEST(ThreadSafeQueueTests, TryRaiseRejectsOverflowWithoutDiscardingQueuedValues) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue {2};
  EXPECT_TRUE(queue.try_raise(1));
  EXPECT_TRUE(queue.try_raise(2));
  EXPECT_FALSE(queue.try_raise(3));

  EXPECT_EQ(queue.pop(0ms), 1);
  EXPECT_EQ(queue.pop(0ms), 2);
  EXPECT_FALSE(queue.pop(0ms));

  queue.stop();
  EXPECT_FALSE(queue.try_raise(4));
}

TEST(ThreadSafeEventTests, PeekAndRunningTrackLockedEventState) {
  safe::event_t<int> event;
  EXPECT_TRUE(event.running());
  EXPECT_FALSE(event.peek());

  event.raise(7);
  EXPECT_TRUE(event.peek());
  ASSERT_EQ(event.pop(), 7);
  EXPECT_FALSE(event.peek());

  event.stop();
  EXPECT_FALSE(event.running());
  EXPECT_FALSE(event.peek());
}

TEST(ThreadSafeEventTests, ConcurrentRaiseAndStopRemainConsistent) {
  safe::event_t<int> event;
  std::atomic<bool> start {false};

  std::thread producer {[&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int value = 0; value < 1000; ++value) {
      event.raise(value);
    }
  }};
  std::thread observer {[&] {
    start.store(true, std::memory_order_release);
    while (event.running()) {
      static_cast<void>(event.peek());
    }
  }};

  producer.join();
  event.stop();
  observer.join();

  EXPECT_FALSE(event.running());
  EXPECT_FALSE(event.peek());
}

TEST(ThreadSafeEventTests, NonblockingDrainCannotWaitAfterAnotherConsumerWins) {
  using namespace std::chrono_literals;

  safe::event_t<int> event;
  event.raise(7);
  ASSERT_TRUE(event.peek());

  std::thread consumer {[&] {
    EXPECT_EQ(event.pop(), 7);
  }};
  consumer.join();

  const auto drain_started = std::chrono::steady_clock::now();
  EXPECT_FALSE(event.pop(0ms));
  EXPECT_LT(std::chrono::steady_clock::now() - drain_started, 50ms);
}

TEST(ThreadSafeEventTests, GenerationViewsBroadcastOncePerObserver) {
  safe::event_t<int> event;
  std::uint64_t first_observer {};
  std::uint64_t second_observer {};

  event.raise(7);
  EXPECT_EQ(event.view_if_newer(first_observer), 7);
  EXPECT_EQ(event.view_if_newer(second_observer), 7);
  EXPECT_FALSE(event.view_if_newer(first_observer));
  EXPECT_FALSE(event.view_if_newer(second_observer));

  event.raise(9);
  EXPECT_EQ(event.view_if_newer(first_observer), 9);
  EXPECT_EQ(event.view_if_newer(second_observer), 9);
}

TEST(ThreadSafeEventTests, ObserverCanBaselinePastAStickyHistoricalValue) {
  safe::event_t<int> event;
  event.raise(7);

  auto observed_generation = event.generation();
  EXPECT_FALSE(event.view_if_newer(observed_generation));

  event.raise(9);
  EXPECT_EQ(event.view_if_newer(observed_generation), 9);
}

TEST(ThreadSafeEventTests, ConcurrentGenerationObserversBothReceiveTheBroadcast) {
  safe::event_t<int> event;
  std::uint64_t first_generation {};
  std::uint64_t second_generation {};
  std::optional<int> first_value;
  std::optional<int> second_value;
  std::atomic<bool> start {false};

  event.raise(11);
  std::thread first {[&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    first_value = event.view_if_newer(first_generation);
  }};
  std::thread second {[&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    second_value = event.view_if_newer(second_generation);
  }};

  start.store(true, std::memory_order_release);
  first.join();
  second.join();

  EXPECT_EQ(first_value, 11);
  EXPECT_EQ(second_value, 11);
  EXPECT_EQ(first_generation, event.generation());
  EXPECT_EQ(second_generation, event.generation());
}
