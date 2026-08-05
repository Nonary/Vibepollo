/**
 * @file tests/tests_common.h
 * @brief Lightweight assertions shared by standalone test components.
 *
 * This header deliberately has no Sunshine runtime dependencies.  In
 * particular it must not initialize logging, the global mail bus, or a live
 * platform backend.  Tests needing an operating-system boundary provide a
 * deterministic adapter in their own component.
 */
#pragma once

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace test_utils {
  struct XFailMarker {
    bool should_xfail;
    std::string reason;

    XFailMarker(bool condition, std::string reason):
        should_xfail(condition),
        reason(std::move(reason)) {}
  };

  inline void handleXFail(const XFailMarker &marker, bool test_passed) {
    if (!marker.should_xfail) {
      return;
    }
    if (test_passed) {
      GTEST_SKIP() << "XPASS: unexpectedly passed (expected failure: " << marker.reason << ")";
    }
    GTEST_SKIP() << "XFAIL: " << marker.reason;
  }

  template<typename T1, typename T2>
  inline bool checkEqual(const T1 &actual, const T2 &expected, const std::string & = {}) {
    return actual == expected;
  }

  template<typename T1, typename T2>
  inline bool checkNotEqual(const T1 &actual, const T2 &expected, const std::string & = {}) {
    return actual != expected;
  }
}  // namespace test_utils

#define XFAIL_IF(condition, reason) \
  test_utils::XFailMarker xfail_marker((condition), (reason))

#define HANDLE_XFAIL_ASSERT_EQ(actual, expected, message) \
  do { \
    if (xfail_marker.should_xfail) { \
      test_utils::handleXFail(xfail_marker, test_utils::checkEqual((actual), (expected), (message))); \
    } else { \
      EXPECT_EQ((actual), (expected)) << (message); \
    } \
  } while (0)

#define HANDLE_XFAIL_ASSERT_NE(actual, expected, message) \
  do { \
    if (xfail_marker.should_xfail) { \
      test_utils::handleXFail(xfail_marker, test_utils::checkNotEqual((actual), (expected), (message))); \
    } else { \
      EXPECT_NE((actual), (expected)) << (message); \
    } \
  } while (0)

#ifdef _WIN32
  #define IS_WINDOWS true
#else
  #define IS_WINDOWS false
#endif
#ifdef __linux__
  #define IS_LINUX true
#else
  #define IS_LINUX false
#endif
#ifdef __APPLE__
  #define IS_MACOS true
#else
  #define IS_MACOS false
#endif
#ifdef __FreeBSD__
  #define IS_FREEBSD true
#else
  #define IS_FREEBSD false
#endif

// Compatibility base for tests not yet converted to a local provider fixture.
// It intentionally performs no platform initialization.
struct PlatformTestSuite: testing::Test {};
