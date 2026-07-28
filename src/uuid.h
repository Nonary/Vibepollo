/**
 * @file src/uuid.h
 * @brief Declarations for UUID generation.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

/**
 * @brief UUID utilities.
 */
namespace uuid_util {
  union uuid_t {
    std::uint8_t b8[16];
    std::uint16_t b16[8];
    std::uint32_t b32[4];
    std::uint64_t b64[2];

    static uuid_t generate(std::default_random_engine &engine) {
      std::uniform_int_distribution<std::uint8_t> dist(0, std::numeric_limits<std::uint8_t>::max());

      uuid_t buf;
      for (auto &el : buf.b8) {
        el = dist(engine);
      }

      buf.b8[7] &= (std::uint8_t) 0b00101111;
      buf.b8[9] &= (std::uint8_t) 0b10011111;

      return buf;
    }

    static uuid_t generate() {
      std::random_device r;

      std::default_random_engine engine {r()};

      return generate(engine);
    }

    static uuid_t parse(const std::string &uuid_str) {
      if (uuid_str.length() != 36) {
        throw std::invalid_argument("Invalid UUID string length");
      }

      uuid_t uuid;
      unsigned int temp16_1;
      unsigned int temp32_1, temp32_2;

      unsigned int data1, data2;
      std::sscanf(
        uuid_str.c_str(),
        "%8x-%4x-%4x-%4x-%8x%4x",
        &uuid.b32[0],
        &data1,
        &data2,
        &temp16_1,
        &temp32_1,
        &temp32_2
      );

      uuid.b16[2] = static_cast<std::uint16_t>(data1);
      uuid.b16[3] = static_cast<std::uint16_t>(data2);

      uuid.b8[8] = (temp16_1 >> 8) & 0xFF;
      uuid.b8[9] = temp16_1 & 0xFF;
      uuid.b8[10] = (temp32_1 >> 24) & 0xFF;
      uuid.b8[11] = (temp32_1 >> 16) & 0xFF;
      uuid.b8[12] = (temp32_1 >> 8) & 0xFF;
      uuid.b8[13] = temp32_1 & 0xFF;
      uuid.b8[14] = (temp32_2 >> 8) & 0xFF;
      uuid.b8[15] = temp32_2 & 0xFF;

      return uuid;
    }

    /// Parse the byte-order-preserving representation emitted by string().
    ///
    /// This differs from parse(), which accepts the canonical GUID text format
    /// and therefore normalizes the first three GUID fields to Windows memory
    /// order. Persistent data that was written with string() must use this
    /// parser to recover the original bytes exactly.
    static uuid_t parse_raw(const std::string &uuid_str) {
      if (uuid_str.size() != 36 ||
          uuid_str[8] != '-' || uuid_str[13] != '-' ||
          uuid_str[18] != '-' || uuid_str[23] != '-') {
        throw std::invalid_argument("Invalid raw UUID string format");
      }

      const auto hex_value = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') {
          return static_cast<std::uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
          return static_cast<std::uint8_t>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
          return static_cast<std::uint8_t>(value - 'A' + 10);
        }
        return 0xffu;
      };

      uuid_t uuid {};
      std::size_t byte_index = 0;
      for (std::size_t index = 0; index < uuid_str.size();) {
        if (uuid_str[index] == '-') {
          ++index;
          continue;
        }
        if (index + 1 >= uuid_str.size() || byte_index >= sizeof(uuid.b8)) {
          throw std::invalid_argument("Invalid raw UUID string format");
        }

        const auto high = hex_value(uuid_str[index]);
        const auto low = hex_value(uuid_str[index + 1]);
        if (high == 0xffu || low == 0xffu) {
          throw std::invalid_argument("Invalid raw UUID string format");
        }

        uuid.b8[byte_index++] = static_cast<std::uint8_t>((high << 4u) | low);
        index += 2;
      }

      if (byte_index != sizeof(uuid.b8)) {
        throw std::invalid_argument("Invalid raw UUID string format");
      }
      return uuid;
    }

    [[nodiscard]] std::string string() const {
      std::string result;

      result.reserve(sizeof(uuid_t) * 2 + 4);

      auto hex = util::hex(*this, true);
      auto hex_view = hex.to_string_view();

      std::string_view slices[] = {
        hex_view.substr(0, 8),
        hex_view.substr(8, 4),
        hex_view.substr(12, 4),
        hex_view.substr(16, 4)
      };
      auto last_slice = hex_view.substr(20, 12);

      for (auto &slice : slices) {
        std::copy(std::begin(slice), std::end(slice), std::back_inserter(result));

        result.push_back('-');
      }

      std::copy(std::begin(last_slice), std::end(last_slice), std::back_inserter(result));

      return result;
    }

    constexpr bool operator==(const uuid_t &other) const {
      return b64[0] == other.b64[0] && b64[1] == other.b64[1];
    }

    constexpr bool operator<(const uuid_t &other) const {
      return (b64[0] < other.b64[0] || (b64[0] == other.b64[0] && b64[1] < other.b64[1]));
    }

    constexpr bool operator>(const uuid_t &other) const {
      return (b64[0] > other.b64[0] || (b64[0] == other.b64[0] && b64[1] > other.b64[1]));
    }
  };

  using UUID = uuid_t;
}  // namespace uuid_util
