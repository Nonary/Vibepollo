#!/usr/bin/env python3
"""Check the production WebRTC state callback bridge and unregister synchronization."""
from pathlib import Path
import subprocess
import tempfile

from test_delayed_mouse_release import ROOT, function

PRELUDE = r'''
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using lwrtc_peer_state_cb = void (*)(void *, int);
struct lwrtc_peer {
  std::mutex state_callback_mutex;
  lwrtc_peer_state_cb state_callback = nullptr;
  void *state_callback_user = nullptr;
};
using lwrtc_peer_t = lwrtc_peer;
'''
TESTS = r'''
int main() {
  lwrtc_peer peer;
  observer_t observer {&peer};
  std::vector<int> seen;
  lwrtc_peer_register_state_callback(&peer, [](void *user, int state) {
    static_cast<std::vector<int> *>(user)->push_back(state);
  }, &seen);
  for (auto state : {libwebrtc::RTCPeerConnectionStateNew,
                     libwebrtc::RTCPeerConnectionStateConnecting,
                     libwebrtc::RTCPeerConnectionStateConnected,
                     libwebrtc::RTCPeerConnectionStateDisconnected,
                     libwebrtc::RTCPeerConnectionStateFailed,
                     libwebrtc::RTCPeerConnectionStateClosed}) {
    observer.OnPeerConnectionState(state);
  }
  assert(seen == (std::vector<int>{LWRTC_PEER_NEW, LWRTC_PEER_CONNECTING,
      LWRTC_PEER_CONNECTED, LWRTC_PEER_DISCONNECTED, LWRTC_PEER_FAILED, LWRTC_PEER_CLOSED}));
  lwrtc_peer_register_state_callback(&peer, nullptr, nullptr);
  observer.OnPeerConnectionState(libwebrtc::RTCPeerConnectionStateClosed);
  assert(seen.size() == 6);
  struct blocker_t { std::promise<void> entered, release; } blocker;
  lwrtc_peer_register_state_callback(&peer, [](void *user, int) {
    auto &blocker = *static_cast<blocker_t *>(user);
    blocker.entered.set_value(); blocker.release.get_future().wait();
  }, &blocker);
  std::thread callback([&] { observer.OnPeerConnectionState(libwebrtc::RTCPeerConnectionStateDisconnected); });
  blocker.entered.get_future().wait();
  auto unregister = std::async(std::launch::async, [&] { lwrtc_peer_register_state_callback(&peer, nullptr, nullptr); });
  assert(unregister.wait_for(0ms) == std::future_status::timeout);
  blocker.release.set_value(); callback.join(); unregister.get();
  observer.OnPeerConnectionState(libwebrtc::RTCPeerConnectionStateClosed);
  assert(!peer.state_callback && !peer.state_callback_user);
  std::cout << "PASS: all peer states, callback removal and in-flight callback synchronization\n";
}
'''


def main():
    bridge = (ROOT / 'third-party/libwebrtc/src/libwebrtc_c.cc').read_text()
    header = (ROOT / 'third-party/libwebrtc/include/libwebrtc_c.h').read_text()
    peer_header = (ROOT / 'third-party/libwebrtc/include/rtc_peerconnection.h').read_text()
    peer_enum = header[header.index('typedef enum lwrtc_peer_state {'):header.index('} lwrtc_peer_state_t;') + len('} lwrtc_peer_state_t;')]
    enum_start = peer_header.index('enum RTCPeerConnectionState {')
    rtc_enum = peer_header[enum_start:peer_header.index('};', enum_start) + 2]
    callback = function(bridge, 'void OnPeerConnectionState(')
    callback = callback.replace(' override', '')
    setter = function(bridge, 'void lwrtc_peer_register_state_callback(')
    with tempfile.TemporaryDirectory(prefix='peer-state-') as temp:
        cpp, binary = Path(temp) / 'test.cpp', Path(temp) / 'test'
        cpp.write_text(PRELUDE + peer_enum + '\nnamespace libwebrtc {\n' + rtc_enum + '\n}\n' +
                       'struct observer_t { lwrtc_peer *peer_;\n' + callback + '\n};\n' + setter + TESTS)
        subprocess.run(['c++', '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
