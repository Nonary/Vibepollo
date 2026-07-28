/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Test src/thread_safe.h
 */

#include "../tests_common.h"
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
