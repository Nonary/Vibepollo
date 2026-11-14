/** @file src/file_clock.h
 *  @brief Lossless decimal representation of native filesystem clock counts.
 */
#pragma once

#include <algorithm>
#include <limits>
#include <string>

namespace file_clock {
  // libc++ can use a signed 128-bit representation, which std::to_string
  // does not accept. Divide the original value without narrowing or negating
  // it, so even the most negative count retains every digit.
  template<typename Rep>
  std::string count_string(Rep value) {
    static_assert(std::numeric_limits<Rep>::is_integer && std::numeric_limits<Rep>::is_signed);
    const bool negative = value < 0;
    std::string result;
    do {
      const auto digit = static_cast<int>(value % 10);
      result.push_back(static_cast<char>('0' + (digit < 0 ? -digit : digit)));
      value /= 10;
    } while (value != 0);
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
  }
}  // namespace file_clock
