/**
 * @file tests/tests_main.cpp
 * @brief Minimal GTest entry point for self-contained test targets.
 */
#include <gtest/gtest.h>

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
