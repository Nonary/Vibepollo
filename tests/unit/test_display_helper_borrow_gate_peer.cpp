#include "src/platform/windows/display_helper_borrow_gate.h"

display_helper::borrow::gate_t::scope_t enter_display_helper_borrow_gate_from_peer(
  display_helper::borrow::gate_t &gate) {
  return gate.try_enter();
}
