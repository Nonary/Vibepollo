/**
 * @file src/wired_pin.h
 * @brief Eclipse: keep a client's stream on its WIRED network card for the session.
 *
 * WHY (measured on an LG TV, 2026-08-22). A TV with a USB-Ethernet adapter AND Wi-Fi on the same subnet
 * answers ARP for either address out of either card, so the host can latch the TV's wired IP to its Wi-Fi
 * MAC: a 150 MB push to the wired IP arrived 157 MB on wlan0 and 9 KB on the adapter. The TV cannot fix
 * that itself (no root, no policy routing), so the HOST pins the client's addresses to the wired MAC with
 * permanent neighbour entries for the session: the same push then arrived 156 MB on the adapter.
 *
 * The client asks for it at launch (auroraWiredMac / auroraOwnMacs / auroraOwnIps -- Eclipse on webOS,
 * when its setting is on), reporting its own cards. Only addresses the host already maps to one of THOSE
 * MACs (or does not know yet) are touched, only on-link ones, and everything is reverted when the session
 * ends -- or at the next start after a crash (a state file lists what was pinned).
 */
#pragma once

#include <string>
#include <vector>

namespace wired_pin {
  /** serverinfo `UmbraCaps` bit: this build can pin (Windows). */
  constexpr int CAP_WIRED_PIN = 1;

  /** Can this platform pin at all? */
  bool supported();

  /** Pin @p own_ips to @p wired_mac (see the file comment for what is refused). @p client_ip is the address
   *  the launch came from; it must be one of @p own_ips. No-op when @p wired_mac is empty. */
  void apply(const std::string &client_ip, const std::string &wired_mac,
             const std::vector<std::string> &own_macs, const std::vector<std::string> &own_ips);

  /** Remove every entry this process pinned (session end). */
  void revert();

  /** Remove entries a previous run pinned and never reverted (call once at startup). */
  void revert_leftovers();
}  // namespace wired_pin
