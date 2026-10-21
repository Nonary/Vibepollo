#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace display_helper::borrow {
  // This is an independent, exact capability handshake. APPLY's v2 reply or
  // successful DISARM dispatch is never evidence that this capability exists.
  inline constexpr std::uint32_t protocol_version = 1;
  inline constexpr std::uint8_t acquire_message = 17;
  inline constexpr std::uint8_t acquire_result_message = 18;
  inline constexpr std::uint8_t release_message = 19;
  inline constexpr std::uint8_t release_result_message = 20;

  struct process_context_t {
    std::uint32_t pid = 0;
    std::uint64_t creation_time = 0;
    bool valid() const { return pid != 0 && creation_time != 0; }
    auto operator<=>(const process_context_t &) const = default;
  };

  struct request_t {
    process_context_t controller;
    std::uint64_t id = 0;
    bool valid() const { return controller.valid() && id != 0; }
    auto operator<=>(const request_t &) const = default;
  };

  struct token_t {
    request_t request;
    process_context_t helper;
    std::uint64_t connection_epoch = 0;
    bool valid() const { return request.valid() && helper.valid() && connection_epoch != 0; }
    auto operator<=>(const token_t &) const = default;
  };

  struct result_t {
    token_t token;
    bool success = false;
    // Handle value is meaningful only in the verified helper process. Host
    // must DuplicateHandle it before publishing a borrow; never use it as a
    // local handle or execute anything from the response.
    std::uint64_t native_keepalive_handle = 0;
  };

  namespace wire {
    inline void append(std::vector<std::uint8_t> &bytes, std::uint64_t value, unsigned count) {
      for (unsigned n = 0; n < count; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (n * 8)));
    }
    inline std::uint64_t read(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count) {
      std::uint64_t value = 0;
      for (unsigned n = 0; n < count; ++n) value |= static_cast<std::uint64_t>(bytes[offset + n]) << (n * 8);
      return value;
    }
    inline void append_request(std::vector<std::uint8_t> &bytes, const request_t &request) {
      append(bytes, protocol_version, 4);
      append(bytes, request.controller.pid, 4);
      append(bytes, request.controller.creation_time, 8);
      append(bytes, request.id, 8);
    }
    inline std::optional<request_t> request_prefix(std::span<const std::uint8_t> bytes) {
      if (bytes.size() < 24 || read(bytes, 0, 4) != protocol_version) return std::nullopt;
      request_t request {{static_cast<std::uint32_t>(read(bytes, 4, 4)), read(bytes, 8, 8)}, read(bytes, 16, 8)};
      return request.valid() ? std::optional<request_t>(request) : std::nullopt;
    }
  }

  inline std::vector<std::uint8_t> encode_acquire(const request_t &request) {
    std::vector<std::uint8_t> bytes;
    wire::append_request(bytes, request);
    return bytes;
  }

  inline std::optional<request_t> decode_acquire(std::span<const std::uint8_t> bytes) {
    return bytes.size() == 24 ? wire::request_prefix(bytes) : std::nullopt;
  }

  inline std::vector<std::uint8_t> encode_release(const token_t &token) {
    auto bytes = encode_acquire(token.request);
    wire::append(bytes, token.helper.pid, 4);
    wire::append(bytes, token.helper.creation_time, 8);
    wire::append(bytes, token.connection_epoch, 8);
    return bytes;
  }

  inline std::optional<token_t> decode_release(std::span<const std::uint8_t> bytes) {
    const auto request = wire::request_prefix(bytes);
    if (bytes.size() != 44 || !request) return std::nullopt;
    token_t token {*request,
      {static_cast<std::uint32_t>(wire::read(bytes, 24, 4)), wire::read(bytes, 28, 8)}, wire::read(bytes, 36, 8)};
    // A zero helper/epoch is the exact-request cancellation sent after an
    // unacknowledged acquire. It cannot release any other request/context.
    const bool cancellation = token.helper.pid == 0 && token.helper.creation_time == 0 && token.connection_epoch == 0;
    return token.valid() || cancellation ? std::optional<token_t>(token) : std::nullopt;
  }

  inline std::vector<std::uint8_t> encode_result(const result_t &result) {
    auto bytes = encode_release(result.token);
    wire::append(bytes, result.native_keepalive_handle, 8);
    bytes.push_back(result.success ? 1 : 0);
    return bytes;
  }

  inline std::optional<result_t> decode_result(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 53 || bytes.back() > 1) return std::nullopt;
    const auto token = decode_release(bytes.first(44));
    if (!token || !token->valid()) return std::nullopt;
    return result_t {*token, bytes.back() != 0, wire::read(bytes, 44, 8)};
  }
}
