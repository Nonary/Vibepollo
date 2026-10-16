#pragma once

#include "thread_safe.h"
#include "video_policy.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace video {
  enum class capture_source_e : std::uint8_t {
    active_output,
    exact_output,
    synthetic_black,
  };

  struct capture_pacing_context_t {
    policy::rational_t display_refresh {0, 1};
    int rounded_refresh = 0;
    bool adjust_to_display = false;
  };

  template<class Config>
  policy::rational_t capture_pacing(const Config &config, const capture_pacing_context_t &context = {}) {
    if (config.framerateX100 > 0) {
      return policy::framerate_x100_to_rational(config.framerateX100);
    }
    const auto requested = std::max(0, config.framerate);
    // Match Windows display_base_t::capture(): automatic integer requests can
    // decrease slightly to follow a fractional desktop; explicit X100 cannot.
    // The source-derived fixture checks this against that actual implementation.
    if (context.adjust_to_display && requested > 0 && context.display_refresh.numerator > 0 && context.display_refresh.denominator > 1 && context.rounded_refresh > 0) {
      auto candidate = context.display_refresh;
      if (requested % context.rounded_refresh == 0) {
        candidate.numerator *= requested / context.rounded_refresh;
      } else if (context.rounded_refresh % requested == 0) {
        candidate.denominator *= context.rounded_refresh / requested;
      }
      const auto rate = static_cast<double>(candidate.numerator) / candidate.denominator;
      if (requested > rate && rate / requested > 0.99) {
        return candidate;
      }
    }
    return {requested, 1};
  }

  template<class Config>
  bool same_capture_pacing(const Config &left, const Config &right, const capture_pacing_context_t &context = {}) {
    const auto lhs = capture_pacing(left, context);
    const auto rhs = capture_pacing(right, context);
    return std::int64_t {lhs.numerator} * rhs.denominator ==
           std::int64_t {rhs.numerator} * lhs.denominator;
  }

  // A shared producer must service the fastest live consumer. Copy only its
  // capture-rate fields; each encoder keeps its own negotiated configuration.
  // Compare exact fractional rates, including equivalent X100 aliases, so
  // equal demand does not needlessly tear down a display generation.
  template<class Contexts>
  auto capture_config_for_consumers(const Contexts &contexts, const capture_pacing_context_t &pacing = {}) {
    using config_t = std::decay_t<decltype(contexts.front().config)>;
    std::optional<config_t> result;
    for (const auto &context : contexts) {
      if (!context.images->running()) {
        continue;
      }
      if (!result) {
        result = context.config;
        continue;
      }
      const auto current = capture_pacing(*result, pacing);
      const auto candidate = capture_pacing(context.config, pacing);
      const auto lhs = std::int64_t {candidate.numerator} * current.denominator;
      const auto rhs = std::int64_t {current.numerator} * candidate.denominator;
      if (lhs > rhs || (lhs == rhs && context.config.framerateX100 > 0 && result->framerateX100 <= 0)) {
        result->framerate = context.config.framerate;
        result->framerateX100 = context.config.framerateX100;
      }
    }
    return result;
  }

  // A stopped dequeued subscriber does not end a shared worker while another
  // lease is still admitting its context. Drain pending joins first, then wait
  // for a live subscriber or queue shutdown rather than stopping their images.
  template<class Contexts, class Queue>
  auto await_capture_config(Contexts &contexts, Queue &queue, const capture_pacing_context_t &pacing = {}) {
    using result_t = decltype(capture_config_for_consumers(contexts, pacing));
    while (queue.running()) {
      while (auto pending = queue.pop(std::chrono::milliseconds {0})) {
        contexts.emplace_back(std::move(*pending));
      }
      if (!queue.running()) {
        break;
      }
      std::erase_if(contexts, [](const auto &context) {
        return !context.images->running();
      });
      if (auto demand = capture_config_for_consumers(contexts, pacing)) {
        return demand;
      }
      auto pending = queue.pop();
      if (!pending) {
        break;
      }
      contexts.emplace_back(std::move(*pending));
    }
    return result_t {};
  }

  /**
   * Normal game transports mirror one capture producer. Remote Monitor and
   * Remote Input retain private producers so they cannot inherit that output.
   * Keep the shared lease until the encoder has released its display/images;
   * only its final consumer may stop capture (including WGC helper handshakes).
   */
  template<class Context>
  class capture_source_lease_t {
  public:
    template<class Start, class Stop>
    capture_source_lease_t(capture_source_e source, safe::shared_t<Context> &shared, Start start, Stop stop):
        stop_private_ {std::move(stop)} {
      if (source == capture_source_e::active_output) {
        shared_ = shared.ref();
      } else {
        private_started_ = start(private_) == 0;
      }
    }

    capture_source_lease_t(const capture_source_lease_t &) = delete;
    capture_source_lease_t &operator=(const capture_source_lease_t &) = delete;

    ~capture_source_lease_t() {
      if (private_started_) {
        stop_private_(private_);
      }
    }

    Context *get() {
      if (shared_) {
        return shared_.get();
      }
      return private_started_ ? &private_ : nullptr;
    }

  private:
    typename safe::shared_t<Context>::ptr_t shared_;
    Context private_ {};
    bool private_started_ = false;
    std::function<void(Context &)> stop_private_;
  };
}  // namespace video
