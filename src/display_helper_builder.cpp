/**
 * @file src/display_helper_builder.cpp
 */

#include "display_helper_builder.h"

#include "rtsp.h"

namespace display_helper_integration {

DisplayApplyBuilder &DisplayApplyBuilder::set_session(const rtsp_stream::launch_session_t &session) {
  session_ = &session;
  return *this;
}

DisplayApplyBuilder &DisplayApplyBuilder::set_action(const DisplayApplyAction action) {
  action_ = action;
  return *this;
}

DisplayApplyBuilder &DisplayApplyBuilder::set_configuration(const display_device::SingleDisplayConfiguration &config) {
  configuration_ = config;
  return *this;
}

DisplayApplyBuilder &DisplayApplyBuilder::clear_configuration() {
  configuration_.reset();
  return *this;
}

DisplayApplyBuilder &DisplayApplyBuilder::set_virtual_display_watchdog(const bool enable) {
  enable_virtual_display_watchdog_ = enable;
  return *this;
}

DisplayApplyBuilder &DisplayApplyBuilder::set_topology(const DisplayTopologyDefinition &topology) {
  topology_ = topology;
  return *this;
}

DisplayTopologyDefinition &DisplayApplyBuilder::mutable_topology() {
  return topology_;
}

ActiveSessionState &DisplayApplyBuilder::mutable_session_overrides() {
  return session_overrides_;
}

DisplayApplyBuilder &DisplayApplyBuilder::set_virtual_display_arrangement(std::optional<VirtualDisplayArrangement> arrangement) {
  virtual_display_arrangement_ = arrangement;
  return *this;
}

DisplayApplyRequest DisplayApplyBuilder::build() const {
  DisplayApplyRequest request;
  request.action = action_;
  request.configuration = configuration_;
  request.session_overrides = session_overrides_;
  request.enable_virtual_display_watchdog = enable_virtual_display_watchdog_;
  request.topology = topology_;
  request.session = session_;
  request.virtual_display_arrangement = virtual_display_arrangement_;
  return request;
}

}  // namespace display_helper_integration
