/**
 * @file tests/unit/test_rtsp_startup_snapshot.cpp
 * @brief Regression guard for launch_session_t::clone_for_startup().
 *
 * The background RTSP ANNOUNCE startup worker operates on a clone of the launch
 * session, because the io_context thread still owns and reuses the original. The
 * clone must carry every field that stream::session::alloc() and the cmd_announce()
 * startup lambda consume. A previous revision dropped `perm`, which left streaming
 * sessions at PERM::_no and made input::passthrough() silently discard all mouse,
 * keyboard, and controller input (Vibepollo #280). The same omission also dropped
 * the per-client do/undo command lists. This test fails if any consumed field is
 * dropped again.
 */
#include "../tests_common.h"

#include <src/crypto.h>
#include <src/rtsp.h>

namespace {

  rtsp_stream::launch_session_t make_populated_launch_session() {
    rtsp_stream::launch_session_t ls {};

    ls.id = 0x1234u;
    ls.gcm_key = crypto::aes_t {0x01, 0x02, 0x03, 0x04};
    ls.iv = crypto::aes_t {0x05, 0x06, 0x07, 0x08};
    ls.av_ping_payload = "ping-payload";
    ls.control_connect_data = 0xABCDu;
    ls.unique_id = "unique-id";
    ls.client_uuid = "client-uuid";
    ls.device_name = "device-name";
    ls.perm = crypto::PERM::_all;
    ls.fps = 120;
    ls.client_do_cmds.push_back(crypto::command_entry_t {"do-cmd", true});
    ls.client_undo_cmds.push_back(crypto::command_entry_t {"undo-cmd-1", false});
    ls.client_undo_cmds.push_back(crypto::command_entry_t {"undo-cmd-2", true});
    ls.virtual_display = true;
    ls.virtual_display_guid_bytes = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
    ls.gen1_framegen_fix = true;
    ls.gen2_framegen_fix = true;
    ls.lossless_scaling_framegen = true;
    ls.frame_generation_provider = "provider";
    ls.lossless_scaling_target_fps = 144.0;
    ls.lossless_scaling_rtss_limit = 90;

    return ls;
  }

}  // namespace

// Guards the exact field that caused Vibepollo #280: a dropped perm leaves the
// streaming session at PERM::_no, which makes input::passthrough() discard every
// mouse/keyboard/controller packet while video keeps flowing.
TEST(RtspStartupSnapshot, PreservesPermission) {
  const auto source = make_populated_launch_session();
  const auto clone = source.clone_for_startup();

  ASSERT_TRUE(clone != nullptr);
  EXPECT_EQ(clone->perm, crypto::PERM::_all);
  EXPECT_NE(clone->perm, crypto::PERM::_no);
  EXPECT_TRUE(!!(clone->perm & crypto::PERM::_all_inputs));
}

// The clone must carry every field that stream::session::alloc() / the cmd_announce()
// startup lambda read. If you add such a field to launch_session_t, copy it in
// clone_for_startup() and assert it here.
TEST(RtspStartupSnapshot, CopiesAllConsumedFields) {
  const auto source = make_populated_launch_session();
  const auto clone = source.clone_for_startup();
  ASSERT_TRUE(clone != nullptr);

  EXPECT_EQ(clone->id, source.id);
  EXPECT_EQ(clone->gcm_key, source.gcm_key);
  EXPECT_EQ(clone->iv, source.iv);
  EXPECT_EQ(clone->av_ping_payload, source.av_ping_payload);
  EXPECT_EQ(clone->control_connect_data, source.control_connect_data);
  EXPECT_EQ(clone->unique_id, source.unique_id);
  EXPECT_EQ(clone->client_uuid, source.client_uuid);
  EXPECT_EQ(clone->device_name, source.device_name);
  EXPECT_EQ(clone->perm, source.perm);
  EXPECT_EQ(clone->fps, source.fps);

  ASSERT_EQ(clone->client_do_cmds.size(), source.client_do_cmds.size());
  EXPECT_EQ(clone->client_do_cmds.front().cmd, "do-cmd");
  EXPECT_EQ(clone->client_do_cmds.front().elevated, true);
  ASSERT_EQ(clone->client_undo_cmds.size(), source.client_undo_cmds.size());
  EXPECT_EQ(clone->client_undo_cmds.back().cmd, "undo-cmd-2");

  EXPECT_EQ(clone->virtual_display, source.virtual_display);
  EXPECT_EQ(clone->virtual_display_guid_bytes, source.virtual_display_guid_bytes);
  EXPECT_EQ(clone->gen1_framegen_fix, source.gen1_framegen_fix);
  EXPECT_EQ(clone->gen2_framegen_fix, source.gen2_framegen_fix);
  EXPECT_EQ(clone->lossless_scaling_framegen, source.lossless_scaling_framegen);
  EXPECT_EQ(clone->frame_generation_provider, source.frame_generation_provider);
  EXPECT_EQ(clone->lossless_scaling_target_fps, source.lossless_scaling_target_fps);
  EXPECT_EQ(clone->lossless_scaling_rtss_limit, source.lossless_scaling_rtss_limit);

  // The clone intentionally does NOT copy rtsp_cipher: it is move-only and only the
  // io_context respond() path uses it, never the startup worker.
  EXPECT_FALSE(clone->rtsp_cipher.has_value());
}
