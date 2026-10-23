/**
 * @file tests/unit/test_nvenc_event_wait.cpp
 * @brief Exercise the production NVENC wait component through its OS boundary.
 */
#include "src/nvenc/nvenc_event_wait.h"

#include <gmock/gmock.h>

namespace {
  namespace wait = nvenc::event_wait;
  using testing::InSequence;
  using testing::Return;
  using testing::StrictMock;

  class wait_api_t: public wait::api_t {
  public:
    MOCK_METHOD(std::uint32_t, wait, (void *event, std::uint32_t timeout_ms), (override));
    MOCK_METHOD(std::uint32_t, last_error, (), (override));
  };

  TEST(NvencEventWait, SignaledEventPreservesSuccessWithoutReadingLastError) {
    StrictMock<wait_api_t> api;
    int event;
    EXPECT_CALL(api, wait(&event, 100)).WillOnce(Return(wait::signaled));
    EXPECT_CALL(api, last_error()).Times(0);

    const auto result = wait::wait(&event, 100, api);

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(result.code, wait::signaled);
    EXPECT_EQ(result.error, 0);
  }

  TEST(NvencEventWait, TimeoutKeepsDeadlineAndDoesNotReadStaleLastError) {
    StrictMock<wait_api_t> api;
    int event;
    EXPECT_CALL(api, wait(&event, 100)).WillOnce(Return(wait::timed_out));
    EXPECT_CALL(api, last_error()).Times(0);

    const auto result = wait::wait(&event, 100, api);

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.code, wait::timed_out);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(wait::describe(result, 100), "encode completion wait timed out (wait_result=258, timeout_ms=100)");
  }

  TEST(NvencEventWait, FailedWaitCapturesErrorBeforeDiagnosticWork) {
    StrictMock<wait_api_t> api;
    int event;
    std::uint32_t thread_error = 1234;
    {
      InSequence sequence;
      EXPECT_CALL(api, wait(&event, 100)).WillOnce([&](void *, std::uint32_t) {
        thread_error = 6;
        return wait::failed;
      });
      EXPECT_CALL(api, last_error()).WillOnce([&] {
        return thread_error;
      });
    }

    const auto result = wait::wait(&event, 100, api);
    // Subsequent diagnostic work must use the captured error, not thread state.
    thread_error = 999;

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.code, 0xffffffffU);
    EXPECT_EQ(result.error, 6);
    EXPECT_EQ(wait::describe(result, 100), "encode completion wait failed (wait_result=4294967295, winerr=6)");
  }

  TEST(NvencEventWait, EachFailedWaitCapturesItsOwnErrorIncludingZero) {
    StrictMock<wait_api_t> api;
    int event;
    InSequence sequence;
    EXPECT_CALL(api, wait(&event, 100)).WillOnce(Return(wait::failed));
    EXPECT_CALL(api, last_error()).WillOnce(Return(6));
    EXPECT_CALL(api, wait(&event, 100)).WillOnce(Return(wait::failed));
    EXPECT_CALL(api, last_error()).WillOnce(Return(0));

    const auto first = wait::wait(&event, 100, api);
    const auto second = wait::wait(&event, 100, api);

    EXPECT_FALSE(first.succeeded());
    EXPECT_FALSE(second.succeeded());
    EXPECT_EQ(first.error, 6);
    EXPECT_EQ(second.error, 0);
    EXPECT_EQ(wait::describe(second, 100), "encode completion wait failed (wait_result=4294967295, winerr=0)");
  }

  TEST(NvencEventWait, UnexpectedResultRetainsRawCodeAndFailureContract) {
    StrictMock<wait_api_t> api;
    int event;
    EXPECT_CALL(api, wait(&event, 100)).WillOnce(Return(0x80));
    EXPECT_CALL(api, last_error()).Times(0);

    const auto result = wait::wait(&event, 100, api);

    EXPECT_FALSE(result.succeeded());
    EXPECT_EQ(result.code, 0x80);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(wait::describe(result, 100), "encode completion wait returned an unexpected result (wait_result=128)");
  }
}  // namespace
