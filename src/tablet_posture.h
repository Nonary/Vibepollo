/**
 * @file src/tablet_posture.h
 * @brief Host laptop/slate state driven by the streaming clients' posture.
 *
 * Windows only switches to tablet behaviour (touch keyboard on text focus, tablet taskbar) when it
 * believes the machine is a convertible whose keyboard is folded back or detached. A virtual
 * display host is neither, so the host reports that state itself, through Windows' own laptop/slate
 * indicator interface, whenever a touch-primary client streams. Clients drive it: `clientPosture`
 * at launch and resume, and the `/posture` endpoint while streaming (keyboard attached, detached,
 * flipped, flipped back). Controlled by the `touch_tablet_mode` option. Other platforms: no-ops.
 *
 * Switching to slate happens at once; switching back to laptop waits a few seconds so a resume or
 * reconnect (which ends one session and starts the next) does not flicker the shell. All Windows work
 * happens on a worker thread, never on a streaming or HTTP thread.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "posture_size.h"
#include "tablet_posture_policy.h"

namespace tablet_posture {
  /// The mode a client's host mode asks of the posture backend.
  inline client_mode_e to_client_mode(posture::host_mode_e mode) {
    switch (mode) {
      case posture::host_mode_e::tablet:
        return client_mode_e::tablet;
      case posture::host_mode_e::handheld:
        return client_mode_e::handheld;
      case posture::host_mode_e::desktop:
        break;
    }
    return client_mode_e::desktop;
  }

  /// Host mode for these hints, with the configured `handheld_mode` policy applied. Hints must already include any per-client override.
  posture::host_mode_e host_mode_for(const posture::hints_t &hints);

  /// Delay before the host returns to laptop after the last touch-primary client left.
  inline constexpr std::chrono::milliseconds laptop_grace {3000};

  /// A session of `client` started. `mode` is the client's current host mode.
  void client_started(const std::string &client, client_mode_e mode);

  /// The client's posture changed during a session. Ignored when the client has no active session.
  void client_updated(const std::string &client, client_mode_e mode);

  /// A session of `client` ended. The last touch-primary client leaving puts the host back to laptop.
  void client_stopped(const std::string &client);

  /// Force the state regardless of clients (a simulated keyboard detach/attach); nullopt gives control back to the clients.
  void set_override(std::optional<client_mode_e> forced);

  namespace backend {
    /// Outcome of a request to enter or leave the gaming posture.
    enum class gaming_result_e {
      switched,  ///< The host made the switch.
      already,  ///< The host was in the wanted state before; nothing was switched.
      failed,
    };
  }  // namespace backend

  struct status_t {
    state_e applied;  ///< What Windows was last told.
    state_e wanted;  ///< What the clients (or the override) ask for.
    client_mode_e wanted_mode;  ///< The wanted state as a mode: desktop, tablet or handheld.
    bool gaming_applied;  ///< The host entered the gaming posture (Xbox full screen experience) and will leave it.
    std::optional<state_e> override_state;
    std::optional<client_mode_e> override_mode;
    bool enabled;  ///< touch_tablet_mode
    std::vector<registry_t::client_info_t> clients;
  };

  status_t status();

  /// First argument of the instance a Windows host service starts as the logged-on user to switch the
  /// gaming posture; followed by `enter` or `leave`. Handled before any host initialization.
  inline constexpr std::string_view gaming_switch_arg = "--gaming-posture-switch";

  /// Windows: make the switch `argv[2]` asks for and return 0 when the host is in the wanted state.
  int run_gaming_switch(int argc, char **argv);

  /// At host start: undo whatever a previous host process left behind after a crash.
  void recover();

  /// At host stop: put the host back to laptop and stop the worker.
  void shutdown();
}  // namespace tablet_posture
