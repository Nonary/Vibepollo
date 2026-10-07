#include "src/platform/windows/display_helper_session_deferral.h"

#include "src/rtsp.h"

namespace display_helper_integration {
  void SessionDeferralManager::set_pending(const DisplayApplyRequest &request) {
    PendingSessionSnapshot snapshot;
    std::uint32_t session_id = 0;
    const bool has_session = request.session != nullptr;

    if (request.session) {
      const auto &session = *request.session;
      session_id = session.id;
      snapshot.width = session.width;
      snapshot.height = session.height;
      snapshot.fps = session.fps;
      snapshot.client_display_mode_override = session.client_display_mode_override;
      snapshot.client_display_refresh_millihz = session.client_display_refresh_millihz;
      snapshot.enable_hdr = rtsp_stream::effective_hdr_requested(session);
      snapshot.enable_sops = session.enable_sops;
      snapshot.virtual_display = session.virtual_display;
      snapshot.virtual_display_device_id = session.virtual_display_device_id;
      snapshot.virtual_display_ready_since = session.virtual_display_ready_since;
      snapshot.framegen_refresh_rate = session.framegen_refresh_rate;
      snapshot.framegen_refresh_millihz = session.framegen_refresh_millihz;
      snapshot.framegen_refresh_multiplier = session.framegen_refresh_multiplier;
      snapshot.gen1_framegen_fix = session.gen1_framegen_fix;
      snapshot.gen2_framegen_fix = session.gen2_framegen_fix;
    }

    set_pending(request, std::move(snapshot), session_id, has_session);
  }
}  // namespace display_helper_integration
