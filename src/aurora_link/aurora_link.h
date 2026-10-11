/**
 * @file src/aurora_link/aurora_link.h
 * @brief Aurora Link — the compatibility-named host companion stack, built and installed with the host.
 *
 * WHY THIS LIVES INSIDE THE HOST
 * ----------------------------
 * The pieces this replaces grew up as loose scripts in a local xlink-build folder:
 * a pad host exe, three Python scripts, a .vbs launcher and two .cmd wrappers, all
 * referenced by absolute path from the host's prep-cmd. That works on the one machine
 * it was hand-built on and nowhere else, and nothing ties their lifetime to the host's:
 * uninstalling the host would leave every one of them behind, and a second PC needs the
 * whole tree copied and its paths rewritten.
 *
 * Building it as a host target instead means:
 *   - one executable, installed next to sunshine.exe by the host's CMake install
 *     rules, so uninstalling the host removes it too;
 *   - the host's prep-cmd starts and stops it, so ending a session ends everything;
 *   - a second PC gets it by installing the host, with no hand-copied tree.
 *
 * Third-party DRIVERS cannot live inside the exe, so they follow the pattern the host
 * already uses for ViGEmBus (cmake/packaging/windows.cmake downloads the signed
 * installer and ships it under scripts/ as its own install component). VB-CABLE is
 * added the same way, which keeps the "uninstalling the host removes it" property.
 *
 * SUBCOMMANDS
 * -----------
 *   aurora-link cable on|off     show/hide the virtual audio cable (needs admin)
 *   aurora-link run              pad host: UDP pad state -> ViGEm, rumble back
 *   aurora-link mic              mic host: Opus/RTP -> the virtual cable
 *   aurora-link audio-default save|restore   the default sink, around a session
 *   aurora-link session-start    everything a stream needs, as one verb
 *   aurora-link session-end      ...and put it all back
 */
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace aurora_link {

  // Cooperative helper shutdown for host exit, session-end and console stop.
  bool helper_stopping();
  void request_helper_stop();

  /** Ports are fixed by the Pi-side sender; see xlink protocol.h. */
  constexpr unsigned short pad_port = 48200;
  constexpr unsigned short mic_port = 48210;

  /**
   * Enable or disable the virtual audio cable's PnP device.
   *
   * The cable's endpoints are a permanent fixture in Windows' sound UI even though
   * they are only meaningful while streaming -- they exist so the Pi's mic RTP has
   * somewhere to land that a game can select as a microphone. Disabling the device
   * drops every endpoint it publishes to DEVICE_STATE_NOTPRESENT; enabling restores
   * them. Endpoint renames survive, because those are keyed by endpoint GUID.
   *
   * @param on true to show the cable, false to hide it.
   * @return true on success.
   */
  bool set_cable_enabled(bool on);

  /**
   * Rename audio endpoints whose current name CONTAINS `first`, to `second`.
   *
   * Used to give the virtual audio cable names that say what it is for, at install
   * time rather than by hand -- a fork cannot ship "rename these two devices
   * yourself" as a setup step. Goes through IPolicyConfig, the interface the Sound
   * control panel uses, so it needs no registry ACL changes.
   *
   * @return false if the rename mechanism was unavailable; never fatal to a session.
   */
  bool rename_endpoints(const std::vector<std::pair<std::wstring, std::wstring>> &rules);

  /**
   * Run the pad host until stopped: receive xlink pad-state packets on
   * `pad_port` and drive a ViGEm virtual Xbox 360 pad, forwarding rumble back to
   * the Pi so it reaches the real controller's motors.
   *
   * The virtual pad is created only while the real controller is talking -- plugged
   * in on the first packet, unplugged on BYE or after idle silence. The Pi sends
   * ~1 Hz keepalives while a controller is connected, so silence means it is gone.
   *
   * @return process exit code.
   */
  int run_pad_host();

  /**
   * Run the mic host until stopped: receive the Pi's Opus/RTP voice stream on
   * `mic_port` and render it into the virtual audio cable, so a game sees it as a
   * microphone.
   *
   * Replaces mic_receiver.py + ffmpeg + sounddevice + numpy -- that chain was the only
   * thing keeping the host stack from being a single executable, and it pulled in a
   * per-machine Python install and a borrowed ffmpeg.exe.
   *
   * @return process exit code.
   */
  int run_mic_host();

  /**
   * Save / restore the Windows default PLAYBACK endpoint around a session.
   *
   * The host parks the machine on its own virtual sink for the duration of a stream
   * and does not reliably put the previous device back. Replaces audio_default.py,
   * the last Python dependency in the host stack.
   *
   * The snapshot holds the endpoint id AND its friendly name, and the name is the
   * common path rather than a fallback: this stack changes display topology on every
   * session, and NVIDIA re-enumerates its audio endpoints with fresh GUIDs when it
   * does, so the saved id has usually expired by the time we restore.
   *
   * @return true on success; restoring with no snapshot present succeeds.
   */
  bool save_default_endpoint(const std::wstring &path);
  bool restore_default_endpoint(const std::wstring &path);

  /** Path of the per-session audio snapshot, so the CLI and the hooks agree. */
  std::wstring session_audio_snapshot();

  /** UTF-8/ANSI argv to wide, for paths given on the command line. */
  std::wstring widen(const char *s);

  /**
   * One whole stream session, as the two verbs the host's global_prep_cmd names.
   *
   * start: name the cable's endpoints, snapshot the default sink, then launch the pad
   * and mic hosts hidden.
   * end: stop those, put the default sink back, and relight the
   * physical panel via dispwake -- which is the ONLY hook covering a full quit,
   * because the host does not run its pause command when a session terminates.
   *
   * @return always 0. A non-zero prep-cmd aborts the stream, and nothing here is
   *         worth refusing to play a game over.
   */
  int session_start();
  int session_end();

}  // namespace aurora_link
