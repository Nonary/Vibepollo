/**
 * @file src/rtsp_launch_registry.h
 * @brief Bookkeeping for launches queued by /launch and awaiting their RTSP handshake.
 */
#pragma once

#include "rtsp_pending_policy.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtsp_stream {
  /**
   * @brief Pending launches keyed by launch ID, with their expiry and startup state.
   *
   * The registry holds no lock and no timer: the RTSP server serializes access
   * with its own mutex and arms its expiry timer from next_expiry(). Keeping
   * time an explicit parameter lets component tests drive the handshake
   * timeline deterministically.
   *
   * @tparam Session Needs `id`, `unique_id`, `rtsp_source_address`, and an
   *                 `rtsp_cipher` that converts to bool when RTSP is encrypted.
   */
  template<class Session>
  class launch_registry_t {
  public:
    using time_point = std::chrono::steady_clock::time_point;
    using session_ptr = std::shared_ptr<Session>;

    struct entry_t {
      session_ptr session;
      time_point expires_at;
      bool accepted = false;
      bool startup_claimed = false;
      // Set while the ANNOUNCE startup worker owns this launch. The entry does
      // not expire meanwhile; see pending_policy::launch_entry_expired().
      bool startup_running = false;
      std::string remote_address;
    };

    enum class add_result_e {
      added,
      full,
      duplicate_id,
      duplicate_plaintext_source,
    };

    static bool is_live(const entry_t &entry, const time_point now) {
      return !pending_policy::launch_entry_expired(entry.startup_running, entry.expires_at, now);
    }

    /**
     * @brief Remove every expired launch.
     * @return The sessions that expired, for the caller's logging and cleanup.
     */
    std::vector<session_ptr> expire(const time_point now) {
      return remove_entries_if([now](const entry_t &entry) {
        return !is_live(entry, now);
      });
    }

    add_result_e add(session_ptr session, const time_point now, const std::chrono::milliseconds ttl, const std::size_t capacity) {
      if (entries_.size() >= capacity) {
        return add_result_e::full;
      }
      const bool duplicate_id = std::any_of(entries_.begin(), entries_.end(), [&session](const entry_t &entry) {
        return entry.session && entry.session->id == session->id;
      });
      if (duplicate_id) {
        return add_result_e::duplicate_id;
      }
      // Plaintext RTSP is routed by source address, so two live plaintext
      // launches from one address would be indistinguishable.
      const bool duplicate_plaintext_source =
        !session->rtsp_cipher &&
        std::any_of(entries_.begin(), entries_.end(), [&session, now](const entry_t &entry) {
          return entry.session &&
                 !entry.session->rtsp_cipher &&
                 is_live(entry, now) &&
                 entry.session->rtsp_source_address == session->rtsp_source_address;
        });
      if (duplicate_plaintext_source) {
        return add_result_e::duplicate_plaintext_source;
      }
      auto &entry = entries_.emplace_back();
      entry.session = std::move(session);
      entry.expires_at = now + ttl;
      return add_result_e::added;
    }

    bool contains_live(const std::uint32_t id, const std::string_view unique_id, const time_point now) const {
      const auto *entry = find(id, unique_id);
      return entry && is_live(*entry, now);
    }

    /**
     * @brief Let the first authenticated ANNOUNCE own a live launch's startup.
     * @return False if the launch is gone, expired, or already claimed.
     */
    bool claim_startup(const std::uint32_t id, const std::string_view unique_id, const time_point now) {
      auto *entry = find(id, unique_id);
      if (!entry || !is_live(*entry, now) || entry->startup_claimed) {
        return false;
      }
      entry->startup_claimed = true;
      entry->startup_running = true;
      return true;
    }

    /**
     * @brief Undo a claim whose startup worker could not be queued.
     */
    void release_startup(const std::uint32_t id, const std::string_view unique_id) {
      if (auto *entry = find(id, unique_id)) {
        entry->startup_claimed = false;
        entry->startup_running = false;
      }
    }

    /**
     * @brief Return a claimed launch to expiry once its startup worker finishes.
     * @param started Whether the stream session started. A started session gets a fresh
     *                ping_timeout for PLAY and the control connection, which clears the launch.
     *                The claim stays held either way, so a repeated ANNOUNCE is still rejected.
     */
    void finish_startup(const std::uint32_t id, const std::string_view unique_id, const bool started, const time_point now, const std::chrono::milliseconds ttl) {
      if (auto *entry = find(id, unique_id)) {
        entry->startup_running = false;
        if (started) {
          entry->expires_at = pending_policy::launch_deadline_after_startup(entry->expires_at, now, ttl);
        }
      }
    }

    /**
     * @brief The earliest deadline among launches that can expire.
     *
     * Running startups are skipped: their deadline may already be in the past,
     * and finish_startup() makes them eligible again.
     */
    std::optional<time_point> next_expiry() const {
      std::optional<time_point> next;
      for (const auto &entry : entries_) {
        if (!entry.startup_running && (!next || entry.expires_at < *next)) {
          next = entry.expires_at;
        }
      }
      return next;
    }

    /**
     * @brief Remove launches whose session matches @p pred, whatever their state.
     * @return The removed sessions.
     */
    template<class Predicate>
    std::vector<session_ptr> remove_if(Predicate pred) {
      return remove_entries_if([&pred](const entry_t &entry) {
        return entry.session && pred(*entry.session);
      });
    }

    std::vector<session_ptr> clear() {
      return remove_entries_if([](const entry_t &) {
        return true;
      });
    }

    bool empty() const {
      return entries_.empty();
    }

    std::size_t size() const {
      return entries_.size();
    }

    // RTSP routing scans and marks entries directly; see rtsp.cpp.
    std::vector<entry_t> &entries() {
      return entries_;
    }

    const std::vector<entry_t> &entries() const {
      return entries_;
    }

  private:
    template<class Predicate>
    std::vector<session_ptr> remove_entries_if(Predicate pred) {
      std::vector<session_ptr> removed;
      for (auto it = entries_.begin(); it != entries_.end();) {
        if (pred(*it)) {
          if (it->session) {
            removed.push_back(it->session);
          }
          it = entries_.erase(it);
        } else {
          ++it;
        }
      }
      return removed;
    }

    entry_t *find(const std::uint32_t id, const std::string_view unique_id) {
      for (auto &entry : entries_) {
        if (entry.session && entry.session->id == id && entry.session->unique_id == unique_id) {
          return &entry;
        }
      }
      return nullptr;
    }

    const entry_t *find(const std::uint32_t id, const std::string_view unique_id) const {
      return const_cast<launch_registry_t *>(this)->find(id, unique_id);
    }

    std::vector<entry_t> entries_;
  };
}  // namespace rtsp_stream
