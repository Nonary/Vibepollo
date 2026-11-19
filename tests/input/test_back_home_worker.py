#!/usr/bin/env python3
"""Exercise Back-to-Home pulses on the production shared input worker paths.

The existing disconnect harness compiles the real packet handlers and reset
functions. Its deterministic timers let other input and disconnects run between
Home press and release, including callbacks already removed from the timer queue.
Use --source with an older input.cpp as a regression control.
"""
import test_disconnect_release as harness

harness.TESTS = r'''
void connect(std::shared_ptr<input_t> &input, int mask, int flags = 0, int x = 0) {
  controller_packet p {}; p.controllerNumber = 0; p.activeGamepadMask = mask;
  p.buttonFlags = flags; p.leftStickX = x; passthrough(input, &p);
}
void key(std::shared_ptr<input_t> &input, bool release = false) {
  keyboard_packet p {{release ? KEY_UP_EVENT_MAGIC : 0}, 0, 124, 0}; passthrough(input, &p);
}
platf::gamepad_state_t state(const std::shared_ptr<input_t> &input) {
  for (auto it = platf::gamepad_events.rbegin(); it != platf::gamepad_events.rend(); ++it)
    if (it->first == input->gamepads[0].id) return it->second;
  std::abort();
}
bool home(const std::shared_ptr<input_t> &input) { return state(input).buttonFlags & platf::HOME; }
void pulse(std::shared_ptr<input_t> &input, int flags = 0) {
  connect(input, 1, platf::BACK | flags); task_pool.fire();
}
int main(int argc, char **argv) {
  assert(argc == 2);
  config::input.guide_button_key = 124;
  auto a = std::make_shared<input_t>();
  const std::string test = argv[1];
  if (test == "pulse_does_not_block_worker") {
    pulse(a); assert(home(a)); assert(task_pool.timers.size() == 1);
    bool serviced = false;
    task_pool.push([&] {
      button_packet p {{0}, BUTTON_RIGHT}; passthrough(a, &p);
      connect(a, 1, 0x1000, 321); serviced = true;
    });
    task_pool.drain();
    assert(serviced && home(a) && state(a).lsX == 321);
    assert((events == std::vector<event_t>{{BUTTON_RIGHT, false}}));
    task_pool.fire(); assert(!home(a));
    assert(state(a).buttonFlags == 0x1000 && state(a).lsX == 321);
  } else if (test == "native_home_survives_release") {
    pulse(a, platf::HOME); assert(home(a));
    task_pool.fire(); assert(home(a));
    connect(a, 1); assert(!home(a));
  } else if (test == "native_home_latest_state") {
    pulse(a); connect(a, 1, platf::HOME); task_pool.fire(); assert(home(a));
    connect(a, 1); assert(!home(a));
  } else if (test == "native_release_during_pulse") {
    pulse(a, platf::HOME); connect(a, 1); assert(home(a));
    task_pool.fire(); assert(!home(a));
  } else if (test == "guide_outlives_synthetic_home") {
    pulse(a); key(a); task_pool.fire(); assert(home(a));
    key(a, true); assert(!home(a));
  } else if (test == "synthetic_home_outlives_guide") {
    connect(a, 1); key(a); pulse(a); key(a, true); assert(home(a));
    task_pool.fire(); assert(!home(a));
  } else if (test == "disconnect_during_pulse") {
    pulse(a); reset(a); task_pool.drain();
    assert(a->gamepads[0].id == -1 && task_pool.timers.empty());
    assert(platf::gamepad_events.back().second.buttonFlags == 0);
  } else if (test == "removed_controller_cancels_release") {
    pulse(a); connect(a, 0);
    assert(task_pool.timers.empty() && a->gamepads[0].id == -1);
    connect(a, 1, platf::HOME); assert(home(a));
  } else if (test == "dequeued_release_cannot_touch_reallocated_slot") {
    pulse(a); auto stale = task_pool.take_timer();
    connect(a, 0); connect(a, 1, platf::HOME);
    auto count = platf::gamepad_events.size(); stale();
    assert(home(a) && count == platf::gamepad_events.size());
  } else if (test == "dequeued_back_cannot_touch_reallocated_slot") {
    connect(a, 1, platf::BACK); auto stale = task_pool.take_timer();
    connect(a, 0); connect(a, 1);
    auto count = platf::gamepad_events.size(); stale();
    assert(!home(a) && count == platf::gamepad_events.size() && task_pool.timers.empty());
  } else if (test == "released_back_cancels_dequeued_timeout") {
    connect(a, 1, platf::BACK); auto stale = task_pool.take_timer();
    connect(a, 1); auto count = platf::gamepad_events.size(); stale();
    assert(!home(a) && count == platf::gamepad_events.size() && task_pool.timers.empty());
  } else if (test == "old_release_cannot_shorten_new_pulse") {
    pulse(a); auto old_release = task_pool.take_timer();
    connect(a, 1); connect(a, 1, platf::BACK); task_pool.fire();
    assert(home(a)); auto count = platf::gamepad_events.size(); old_release();
    assert(home(a) && count == platf::gamepad_events.size());
    task_pool.fire(); assert(!home(a));
  } else if (test == "back_release_still_cancels_long_press") {
    connect(a, 1, platf::BACK); connect(a, 1);
    assert(task_pool.timers.empty() && !home(a));
  } else return 2;
}
'''

if __name__ == '__main__':
    raise SystemExit(harness.main())
