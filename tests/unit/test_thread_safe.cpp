/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Test src/thread_safe.h
 */

#include "src/thread_safe.h"

#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace {
  // A producer paused while constructing its payload models a descheduled
  // thread holding the queue/event mutex. Status polls must not wait for it.
  struct paused_payload_t {
    paused_payload_t() = default;

    paused_payload_t(std::promise<void> &entered, std::shared_future<void> resume) {
      entered.set_value();
      resume.wait();
    }
  };

  template<class Mailbox>
  void check_empty_polls_during_publish() {
    using namespace std::chrono_literals;
    Mailbox mailbox;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> resume;
    auto resume_future = resume.get_future().share();
    std::thread producer {[&] {
      mailbox.raise(entered, resume_future);
    }};
    const auto producer_status = entered_future.wait_for(2s);
    auto observer = std::async(std::launch::async, [&] {
      const bool running = mailbox.running();
      const bool ready = mailbox.peek();
      const bool popped = static_cast<bool>(mailbox.pop(0ms));
      return running && !ready && !popped;
    });
    const auto observer_status = observer.wait_for(1s);
    // Always release and join, including when the regression blocks the poll.
    resume.set_value();
    producer.join();
    EXPECT_EQ(producer_status, std::future_status::ready);
    EXPECT_EQ(observer_status, std::future_status::ready);
    if (observer_status == std::future_status::ready) {
      EXPECT_TRUE(observer.get());
      EXPECT_TRUE(mailbox.peek());
      EXPECT_TRUE(mailbox.pop(0ms));
      EXPECT_FALSE(mailbox.peek());
    }
  }

  template<class Mailbox>
  void check_ready_polls_during_publish() {
    using namespace std::chrono_literals;
    Mailbox mailbox;
    mailbox.raise();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> resume;
    auto resume_future = resume.get_future().share();
    std::thread producer {[&] {
      mailbox.raise(entered, resume_future);
    }};
    const auto producer_status = entered_future.wait_for(2s);
    auto observer = std::async(std::launch::async, [&] {
      return mailbox.peek() && !mailbox.pop(0ms);
    });
    const auto observer_status = observer.wait_for(1s);
    // Always release and join, including when the regression blocks the poll.
    resume.set_value();
    producer.join();
    EXPECT_EQ(producer_status, std::future_status::ready);
    EXPECT_EQ(observer_status, std::future_status::ready);
    if (observer_status == std::future_status::ready) {
      EXPECT_TRUE(observer.get());
    }
  }
}  // namespace

TEST(ThreadSafeQueueTests, EmptyCapturePollsDoNotWaitForPayloadConstruction) {
  check_empty_polls_during_publish<safe::queue_t<paused_payload_t>>();
}

TEST(ThreadSafeEventTests, EmptyCapturePollsDoNotWaitForPayloadConstruction) {
  check_empty_polls_during_publish<safe::event_t<paused_payload_t>>();
}

TEST(ThreadSafeQueueTests, ReadyCapturePollsDoNotWaitForPayloadConstruction) {
  check_ready_polls_during_publish<safe::queue_t<paused_payload_t>>();
}

TEST(ThreadSafeEventTests, ReadyCapturePollsDoNotWaitForPayloadConstruction) {
  check_ready_polls_during_publish<safe::event_t<paused_payload_t>>();
}

TEST(ThreadSafeEventTests, UnchangedGenerationDoesNotWaitForPayloadConstruction) {
  using namespace std::chrono_literals;
  safe::event_t<paused_payload_t> event;
  auto generation = event.generation();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> resume;
  auto resume_future = resume.get_future().share();
  std::thread producer {[&] {
    event.raise(entered, resume_future);
  }};
  const auto producer_status = entered_future.wait_for(2s);
  auto observer = std::async(std::launch::async, [&] {
    return !event.view_if_newer(generation);
  });
  const auto observer_status = observer.wait_for(1s);
  resume.set_value();
  producer.join();
  EXPECT_EQ(producer_status, std::future_status::ready);
  EXPECT_EQ(observer_status, std::future_status::ready);
  if (observer_status == std::future_status::ready) {
    EXPECT_TRUE(observer.get());
    EXPECT_TRUE(event.view_if_newer(generation));
    EXPECT_FALSE(event.view_if_newer(generation));
  }
}

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

TEST(ThreadSafeQueueTests, PeekAndRunningTrackPublishedQueueState) {
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

TEST(ThreadSafeQueueTests, NonpositivePollsPreserveQueueStateAndConsumeReadyValues) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue;
  EXPECT_FALSE(queue.pop(0ms));
  EXPECT_FALSE(queue.pop(-1ms));
  EXPECT_TRUE(queue.running());

  queue.raise(7);
  queue.raise(9);
  EXPECT_EQ(queue.pop(-1ms), 7);
  EXPECT_EQ(queue.pop(0ms), 9);
  EXPECT_FALSE(queue.pop(0ms));
}

TEST(ThreadSafeQueueTests, PositiveTimeoutWaitsForProducer) {
  using namespace std::chrono_literals;

  safe::queue_t<int> queue;
  std::thread producer {[&] {
    std::this_thread::sleep_for(10ms);
    queue.raise(9);
  }};

  EXPECT_EQ(queue.pop(1s), 9);
  producer.join();
  EXPECT_FALSE(queue.pop(1ms));
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

TEST(ThreadSafeEventTests, PeekAndRunningTrackPublishedEventState) {
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

TEST(ThreadSafeQueueTests, StopAndResetHideBufferedValuesAndAllowReuse) {
  using namespace std::chrono_literals;
  safe::queue_t<int> queue;
  queue.raise(1);
  queue.stop();
  EXPECT_FALSE(queue.running());
  EXPECT_FALSE(queue.peek());
  EXPECT_FALSE(queue.pop(0ms));
  EXPECT_FALSE(queue.wait_for_data(0ms));
  EXPECT_FALSE(queue.try_raise(2));
  queue.raise(3);
  queue.reset();
  EXPECT_TRUE(queue.running());
  EXPECT_FALSE(queue.peek());
  EXPECT_FALSE(queue.pop(0ms));
  queue.raise(4);
  EXPECT_EQ(queue.pop(), 4);
  EXPECT_FALSE(queue.peek());
}

TEST(ThreadSafeQueueTests, OverflowPublishesReplacementAndFinalDrain) {
  using namespace std::chrono_literals;
  safe::queue_t<int> queue {2};
  queue.raise(1);
  queue.raise(2);
  queue.raise(3);
  EXPECT_TRUE(queue.peek());
  EXPECT_EQ(queue.pop(0ms), 3);
  EXPECT_FALSE(queue.peek());
  EXPECT_FALSE(queue.pop(0ms));
}

TEST(ThreadSafeEventTests, StopAndResetPreserveGenerationAndAllowReuse) {
  using namespace std::chrono_literals;
  safe::event_t<int> event;
  event.raise(1);
  auto generation = event.generation();
  event.stop();
  EXPECT_FALSE(event.peek());
  EXPECT_FALSE(event.pop(0ms));
  EXPECT_FALSE(event.view(0ms));
  event.raise(2);
  EXPECT_EQ(event.generation(), generation);
  event.reset();
  EXPECT_TRUE(event.running());
  EXPECT_FALSE(event.peek());
  EXPECT_FALSE(event.view_if_newer(generation));
  event.raise(3);
  EXPECT_EQ(event.view_if_newer(generation), 3);
  EXPECT_EQ(event.view(0ms), 3);
  EXPECT_TRUE(event.peek());
  EXPECT_EQ(event.pop(0ms), 3);
  EXPECT_FALSE(event.peek());
}

TEST(ThreadSafeEventTests, SignalAndPointerPayloadsPublishTheirActualReadiness) {
  using namespace std::chrono_literals;
  safe::signal_t signal;
  signal.raise(false);
  EXPECT_FALSE(signal.peek());
  EXPECT_FALSE(signal.pop(0ms));
  signal.raise(true);
  EXPECT_TRUE(signal.peek());
  EXPECT_TRUE(signal.pop(0ms));
  EXPECT_FALSE(signal.peek());

  safe::event_t<int *> event;
  int value = 7;
  event.raise(nullptr);
  EXPECT_FALSE(event.peek());
  event.raise(&value);
  EXPECT_TRUE(event.peek());
  EXPECT_EQ(event.pop(0ms), &value);
  EXPECT_FALSE(event.peek());
}

TEST(ThreadSafeQueueTests, ConcurrentPollingDeliversEveryPublishedValueInOrder) {
  using namespace std::chrono_literals;
  constexpr int count = 2000;
  safe::queue_t<int> queue {count};
  std::thread producer {[&] {
    for (int i = 0; i < count; ++i) {
      queue.raise(i);
    }
  }};
  int received = 0;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (received < count && std::chrono::steady_clock::now() < deadline) {
    if (queue.peek()) {
      if (auto value = queue.pop(0ms)) {
        EXPECT_EQ(*value, received);
        ++received;
      }
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_EQ(received, count);
  EXPECT_FALSE(queue.peek());
}

TEST(ThreadSafeEventTests, ConcurrentGenerationViewsMatchPublishedPayloads) {
  using namespace std::chrono_literals;
  safe::event_t<int> event;
  constexpr int count = 2000;
  std::thread producer {[&] {
    for (int i = 1; i <= count; ++i) {
      event.raise(i);
    }
  }};
  std::uint64_t generation = 0;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (generation < count && std::chrono::steady_clock::now() < deadline) {
    if (auto value = event.view_if_newer(generation)) {
      EXPECT_EQ(*value, generation);
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_EQ(generation, count);
}

TEST(ThreadSafeQueueTests, StopWakesTimedConsumerAndReadinessWaiter) {
  using namespace std::chrono_literals;
  safe::queue_t<int> queue;
  auto consumer = std::async(std::launch::async, [&] {
    return queue.pop(2s);
  });
  auto waiter = std::async(std::launch::async, [&] {
    return queue.wait_for_data(2s);
  });
  queue.stop();
  EXPECT_EQ(consumer.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(consumer.get());
  EXPECT_FALSE(waiter.get());
}
