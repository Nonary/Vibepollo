/**
 * @file src/tablet_posture_policy.h
 * @brief Pure rules for the host's laptop/slate (tablet) state.
 *
 * Windows decides "tablet posture" from a laptop/slate indicator, the way a convertible laptop
 * reports its keyboard being folded back or detached. The host plays that indicator for the
 * streaming clients: while at least one active client is touch-primary (tablet, handheld,
 * phone, or a touch device without a declared posture) the host is a slate; with no such
 * client, or when the client reports a laptop or desktop posture (keyboard attached), it is a
 * laptop. This header holds the bookkeeping so it can be tested without Windows.
 */
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tablet_posture {
  enum class state_e {
    laptop,  ///< Keyboard attached or folded out: Windows desktop behaviour.
    slate,  ///< Keyboard detached or folded back: Windows tablet behaviour.
  };

  /// What a client asks of the host. Tablet and handheld both mean slate; handheld also asks for the gaming posture.
  enum class client_mode_e {
    desktop,
    tablet,
    handheld,
  };

  /// Active clients and the mode each one asks for. A client may hold several sessions.
  class registry_t {
  public:
    /// A session of `client` started. Returns true when the host state changed.
    bool start(const std::string &client, client_mode_e mode) {
      auto &entry = clients_[client];
      ++entry.sessions;
      entry.mode = mode;
      return refresh();
    }

    /// The client's posture changed while it streams. Unknown clients are ignored. Returns true when the host state changed.
    bool update(const std::string &client, client_mode_e mode) {
      const auto it = clients_.find(client);
      if (it == clients_.end()) {
        return false;
      }
      it->second.mode = mode;
      return refresh();
    }

    /// A session of `client` ended. Returns true when the host state changed.
    bool stop(const std::string &client) {
      const auto it = clients_.find(client);
      if (it == clients_.end()) {
        return false;
      }
      if (--it->second.sessions <= 0) {
        clients_.erase(it);
      }
      return refresh();
    }

    bool contains(const std::string &client) const {
      return clients_.find(client) != clients_.end();
    }

    std::size_t size() const {
      return clients_.size();
    }

    /// State the clients ask for, ignoring any override.
    state_e client_state() const {
      return client_mode_ == client_mode_e::desktop ? state_e::laptop : state_e::slate;
    }

    /// Mode the clients ask for (the strongest one: handheld, then tablet, then desktop), ignoring any override.
    client_mode_e client_mode() const {
      return client_mode_;
    }

    /// The mode to apply: the override when set, else what the clients ask for.
    client_mode_e mode() const {
      return override_.value_or(client_mode_);
    }

    /// The state to apply: slate unless the mode is desktop.
    state_e state() const {
      return mode() == client_mode_e::desktop ? state_e::laptop : state_e::slate;
    }

    /// True when the gaming posture (Xbox full screen experience) is wanted.
    bool gaming() const {
      return mode() == client_mode_e::handheld;
    }

    /// Force a mode regardless of the clients (simulates the keyboard being detached or attached). nullopt returns control to the clients.
    void set_override(std::optional<client_mode_e> forced) {
      override_ = forced;
    }

    std::optional<client_mode_e> override_mode() const {
      return override_;
    }

    struct client_info_t {
      std::string id;
      int sessions;
      client_mode_e mode;
    };

    std::vector<client_info_t> clients() const {
      std::vector<client_info_t> out;
      for (const auto &[id, entry] : clients_) {
        out.push_back({id, entry.sessions, entry.mode});
      }
      return out;
    }

  private:
    struct entry_t {
      int sessions = 0;
      client_mode_e mode = client_mode_e::desktop;
    };

    bool refresh() {
      const auto before_mode = mode();
      client_mode_ = client_mode_e::desktop;
      for (const auto &[client, entry] : clients_) {
        if (entry.mode > client_mode_) {
          client_mode_ = entry.mode;
        }
      }
      return mode() != before_mode;
    }

    std::map<std::string, entry_t> clients_;
    client_mode_e client_mode_ = client_mode_e::desktop;
    std::optional<client_mode_e> override_;
  };
}  // namespace tablet_posture
