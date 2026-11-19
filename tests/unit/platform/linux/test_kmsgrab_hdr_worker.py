"""Run the unchanged capture worker, KMS capture loop/getters and encoder setup.

Only OS/codec/queue/display boundaries are deterministic fakes. This executes
actual worker generation replacement and image delivery, with a bounded factory
so a no-frame reinit storm fails rather than hanging. --source also accepts the
preserved FIX5 tree as a failure control; no source-presence assertions are used.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile


def function(source, marker, last=False):
    start = source.rindex(marker) if last else source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PRELUDE = r'''
#include "src/platform/linux/kmsgrab_hdr.h"
#include "src/platform/linux/capture_target.h"
#include "src/platform/linux/session_backend.h"
#include "src/capture_recovery.h"
#include "src/video_policy.h"
#include "src/input_geometry_policy.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <thread>
#include <vector>
using namespace std::literals;
struct NullLog { template<class T> NullLog &operator<<(const T&) { return *this; } };
#define BOOST_LOG(x) NullLog{}
#define KITTY_WHILE_LOOP(x,y,...) { x; while(y) { __VA_ARGS__ } }
struct SS_HDR_METADATA {
  struct XY { unsigned short x, y; } displayPrimaries[3], whitePoint;
  unsigned short maxDisplayLuminance{}, minDisplayLuminance{}, maxContentLightLevel{}, maxFrameAverageLightLevel{};
};
namespace probe {
  unsigned generations, frames, deliveries, restarts, metadata_commits, selected_hdr, selected_sdr, init_reads, total_reads;
  bool initial_phase, fail_initial, fail_selected, fail_current;
  unsigned eotf=2, cll=900;
  std::string scenario;
  unsigned enumerations = 0, wrong_frames = 0;
  std::vector<std::string> opened_outputs;
  std::optional<int> switch_request;
  unsigned frames_per_generation = 8, wanted_frames = 24, forced_restarts = 2;
  std::optional<uint64_t> current_id = 10;
  unsigned luminance = 1000, connector_id=91;
  std::function<void()> on_frame, on_reinit, on_pop;
  bool reinitializing, old_encoder_live, sync_mode;
  int sync_live_sessions;
  bool encoder_hdr; unsigned encoder_luminance;
}
extern "C" drmModePropertyBlobPtr drmModeGetPropertyBlob(int, uint32_t) {
  ++probe::total_reads;
  if (probe::initial_phase) ++probe::init_reads;
  const bool strict_bad=probe::scenario.starts_with("hyprland-second-") && probe::total_reads>=3;
  if(strict_bad && probe::scenario=="hyprland-second-unknown") return nullptr;
  if ((probe::initial_phase && probe::fail_initial) || probe::fail_selected || probe::fail_current) return nullptr;
  hdr_output_metadata raw {};
  raw.hdmi_metadata_type1.eotf = probe::eotf;
  if(strict_bad) raw.hdmi_metadata_type1.eotf=probe::scenario=="hyprland-second-pq"?2:probe::scenario=="hyprland-second-gamma"?1:3;
  raw.hdmi_metadata_type1.max_display_mastering_luminance = probe::luminance;
  raw.hdmi_metadata_type1.max_cll = probe::cll;
  auto *blob = new drmModePropertyBlobRes{};
  blob->length = sizeof(raw); blob->data = new hdr_output_metadata(raw);
  return blob;
}
extern "C" void drmModeFreePropertyBlob(drmModePropertyBlobPtr blob) {
  delete static_cast<hdr_output_metadata *>(blob->data); delete blob;
}
namespace util {
  template<class F> struct guard_t { F f; ~guard_t() { f(); } };
  template<class F> auto fail_guard(F f) { return guard_t<F>{f}; }
}
namespace safe {
  template<class T> struct queue_t {
    bool active = true; std::deque<T> items;
    bool running() const { return active; }
    bool peek() const { return !items.empty(); }
    void stop() { active = false; }
    auto &unsafe() { return items; }
    std::optional<T> pop() { if (probe::on_pop) probe::on_pop(); if (items.empty()) return {}; auto t=std::move(items.front()); items.pop_front(); return t; }
    template<class D> std::optional<T> pop(D) { return pop(); }
    std::vector<T> drain() {
      std::vector<T> result;
      result.reserve(items.size());
      std::move(items.begin(), items.end(), std::back_inserter(result));
      items.clear();
      return result;
    }
    void raise(const T &) { ++probe::deliveries; if (probe::on_frame) probe::on_frame(); }
  };
  template<class T> struct event_t {
    uint64_t generation() const { return 0; }
    std::optional<T> view_if_newer(uint64_t&) { return std::exchange(probe::switch_request, std::nullopt); }
  };
  struct signal_t { void raise(bool) { probe::reinitializing=true; if (probe::on_reinit) probe::on_reinit(); } void reset() { probe::reinitializing=false; } };
  class mail_raw_t { public: template<class T> class event_t {}; };
}
namespace config {
  std::optional<std::string> exclusive_private_capture_output() {
    return probe::scenario.starts_with("private-") ? std::optional<std::string>{"card2:Virtual-1"} : std::nullopt;
  }
}
namespace input { struct touch_port_t {}; }
namespace platf {
  struct img_t {
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp, host_processing_timestamp, capture_pacing_timestamp;
  };
  enum class thread_priority_e { critical };
  void set_thread_name(const char *) {} void adjust_thread_priority(thread_priority_e) {}
  enum class pix_fmt_e { any };
  using mem_type_e = int; struct adapter_id_t {};
  std::vector<std::string> display_names(int);
  using push_captured_image_cb_t = std::function<bool(std::shared_ptr<img_t>&&, bool)>;
  using pull_free_image_cb_t = std::function<bool(std::shared_ptr<img_t>&)>;
}
namespace video {
@@COLOR_TYPES@@
  enum class capture_source_e { active_output, exact_output, synthetic_black };
  struct config_t {
    bool prefer_sdr_10bit=false, force_sdr=false, rtx_hdr_active=false;
    int dynamicRange=1, encoderCscMode=2, chromaSamplingType=0;
    capture_source_e capture_source=capture_source_e::active_output;
    std::optional<std::string> capture_output;
    std::function<bool()> capture_target_valid;
  };
@@COLOR_FUNCTIONS@@
}
namespace platf {
  struct encode_device_t {
    virtual ~encode_device_t() = default;
    virtual bool supports_rtx_hdr() const { return false; }
    video::sunshine_colorspace_t colorspace;
    bool rtx_hdr_active{}, hdr_metadata_valid{};
    SS_HDR_METADATA hdr_metadata{};
  };
  struct display_t {
    std::uint64_t recovery_capture_instance = 0;
    std::string captured_connector;
    virtual ~display_t() = default;
    virtual bool is_hdr() = 0;
    virtual bool get_hdr_metadata(SS_HDR_METADATA&) = 0;
    virtual capture_e capture(const push_captured_image_cb_t&, const pull_free_image_cb_t&, bool*) = 0;
    virtual std::shared_ptr<img_t> alloc_img() { return std::make_shared<img_t>(); }
    bool is_event_driven_capture() { return false; }
    void request_refresh() {} void prepare_for_reinit() { ++probe::restarts; }
    virtual bool commit_pending_capture_metadata() { return false; }
    std::unique_ptr<encode_device_t> make_avcodec_encode_device(pix_fmt_e) { return std::make_unique<encode_device_t>(); }
    std::unique_ptr<encode_device_t> make_deferred_avcodec_encode_device(pix_fmt_e p) { return make_avcodec_encode_device(p); }
    std::unique_ptr<encode_device_t> make_nvenc_encode_device(pix_fmt_e p) { return make_avcodec_encode_device(p); }
    std::unique_ptr<encode_device_t> make_amf_encode_device(pix_fmt_e p) { return make_avcodec_encode_device(p); }
  };
}
void raise_touch_port_if_geometry_changed(
  const safe::mail_raw_t::event_t<input::touch_port_t> &,
  platf::display_t *,
  const video::config_t &
) {}
namespace platf {
  namespace kms {
    struct prop_blob_t {
      drmModePropertyBlobPtr p; prop_blob_t(drmModePropertyBlobPtr p):p(p) {}
      ~prop_blob_t() { if (p) drmModeFreePropertyBlob(p); }
      bool operator==(std::nullptr_t) const { return p == nullptr; }
      explicit operator bool() const { return p != nullptr; }
      auto operator->() const { return p; }
    };
    struct card_t {
      struct { int el=7; } fd;
      struct connector_t { int crtc_id=1; unsigned connector_id=91; bool connected=true; std::string name="Virtual-1"; int connection=DRM_MODE_CONNECTED; unsigned encoder_id=4; };
      std::vector<connector_t> monitors() { return {{1,probe::connector_id}}; }
      using properties_t=std::vector<std::pair<std::shared_ptr<int>,uint64_t>>;
      std::optional<properties_t> routed_connector_props(unsigned id, unsigned) { return connector_props(id); }
      properties_t connector_props(unsigned) { return probe::current_id ? properties_t{{std::make_shared<int>(1),*probe::current_id}} : properties_t{}; }
      std::optional<uint64_t> prop_value_by_name(const properties_t& properties, std::string_view) { return properties.empty()? std::nullopt : std::optional<uint64_t>(properties.front().second); }
      std::optional<connector_t> connector(unsigned) { return connector_t{}; }
      struct encoder_t { unsigned crtc_id=1; };
      std::optional<encoder_t> encoder(unsigned) { return encoder_t{}; }
    };
    struct display_probe_t: platf::display_t {
      int crtc_id=1; card_t card;
      struct selected_t { std::string connector_name="Virtual-1"; };
      std::optional<selected_t> selected_monitor=selected_t{};
@@KMS_MEMBERS@@
      std::chrono::nanoseconds delay{};
      struct { void reset(){} void first_point(auto){} void second_point_now_and_log(){} } sleep_overshoot_logger;
      unsigned delivered{};
      struct { void reset() { assert(!probe::old_encoder_live && probe::sync_live_sessions == 0); ++probe::metadata_commits; } } pending_exported_frame;
      struct { void reset() {} } presentation_rate_limiter;
      display_probe_t() { probe::initial_phase=true; initialize(); probe::initial_phase=false; }
      void initialize() {
@@KMS_INITIALIZE@@
      }
@@KMS_STRICT@@
@@KMS_GETTERS@@
@@KMS_COMMIT@@
      capture_e metadata_status() {
@@KMS_REFRESH@@
        return capture_e::ok;
      }
      capture_e snapshot(const pull_free_image_cb_t &pull, std::shared_ptr<platf::img_t> &out, std::chrono::milliseconds, bool) {
        // The real KMS HDR guard executes before the frame-device boundary.
        const auto result = metadata_status();
        if (result != capture_e::ok) return result;
        if (delivered >= probe::frames_per_generation && probe::restarts < probe::forced_restarts) return capture_e::reinit;
        if (!probe::fail_current) {
          assert(probe::encoder_hdr == (probe::eotf == 2));
          if (probe::encoder_hdr) assert(probe::encoder_luminance == probe::luminance);
        }
        if (!pull(out)) return capture_e::interrupted;
        ++delivered; ++probe::frames;
        if (captured_connector == "DP-3") ++probe::wrong_frames;
        return capture_e::ok;
      }
@@KMS_FIXED_CAPTURE@@
      capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *cursor) override {
        const auto status = capture_fixed_rate(push, pull, cursor);
        if (probe::sync_mode && status == capture_e::reinit && probe::on_reinit) probe::on_reinit();
        return status;
      }
    };
  }
}
namespace video {
  struct formats_t {
    virtual ~formats_t()=default; int dev_type{};
    platf::pix_fmt_e pix_fmt_yuv444_10bit{}, pix_fmt_yuv444_8bit{}, pix_fmt_10bit{}, pix_fmt_8bit{};
  };
  struct encoder_platform_formats_avcodec: formats_t {};
  struct encoder_platform_formats_nvenc: formats_t {};
  struct encoder_platform_formats_amf: formats_t {};
  struct encoder_t {
    std::shared_ptr<formats_t> platform_formats=std::make_shared<encoder_platform_formats_avcodec>();
    unsigned flags{};
    struct codec_t { const char *name="mock"; };
    codec_t codec_from_config(const config_t&) const { return {}; }
  };
  constexpr unsigned YUV444_SUPPORT=1;
  namespace logging { auto bracket(auto value) { return value; } }
@@MAKE_ENCODE_DEVICE@@
  struct capture_ctx_t {
    config_t config;
    std::shared_ptr<safe::queue_t<std::shared_ptr<platf::img_t>>> images;
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;
  };
  struct capture_pacing_context_t {};
  bool display_cursor=false;
  std::shared_ptr<safe::queue_t<capture_ctx_t>> test_queue;
  config_t test_config;
  struct observed_selection_t { bool hdr; unsigned luminance; };
  std::vector<observed_selection_t> selections;
  std::shared_ptr<platf::display_t> old_encoder_display;
  void select_encoder(const std::shared_ptr<platf::display_t>& display) {
    // Production make_encode_device includes the actual is_hdr/colorspace/
    // get_hdr_metadata order, with only codec allocation replaced by a fake.
    probe::fail_selected = (probe::scenario == "selected-recovery" && probe::generations == 1) || probe::scenario == "persistent-selected-faults";
    auto device=make_encode_device(*display, encoder_t{}, test_config, false);
    probe::fail_selected = false;
    if (probe::scenario == "unreadable-recovery" && probe::generations == 1) probe::fail_current=true;
    const bool hdr=colorspace_is_hdr(device->colorspace);
    probe::encoder_hdr=hdr; probe::encoder_luminance=device->hdr_metadata.maxDisplayLuminance;
    selections.push_back({hdr, device->hdr_metadata.maxDisplayLuminance});
    if (!probe::sync_mode) { old_encoder_display=display; probe::old_encoder_live=true; }
    if (hdr) { ++probe::selected_hdr; assert(device->hdr_metadata_valid); }
    else ++probe::selected_sdr;
  }
  auto await_capture_config(auto&,auto&,auto) { return std::optional<config_t>(test_config); }
  auto capture_config_for_consumers(auto&,auto) { return std::optional<config_t>(test_config); }
  bool same_capture_pacing(auto&,auto&,auto) { return true; }
  bool same_capture_demand(auto&,auto&,auto) { return true; }
  template<class... T> void refresh_displays(int, std::vector<std::string> &names, int &index, T&&...) { names=probe::scenario.starts_with("private-") ? platf::display_names(0) : std::vector<std::string>{"Virtual-1"}; index=0; }
  bool ensure_virtual_display_ready(auto&,int,bool) { return true; }
  void wait_before_display_retry(size_t&) { test_queue->stop(); }
  auto make_black_display(const config_t&) { return std::shared_ptr<platf::display_t>{}; }
}
namespace platf {
  std::vector<std::string> kms_capture_targets(int, const capture_deadline_t &limits = {}) {
    const auto attempt = ++probe::enumerations;
    if (probe::scenario == "private-slow-discovery" && attempt == 1) {
      assert(limits.until != capture_deadline_t::clock::time_point::max());
      std::this_thread::sleep_until(limits.until);
      return {"card2:Virtual-1"};
    }
    if (probe::scenario == "private-other-card" && attempt == 1) return {"card0:Virtual-1"};
    if (probe::scenario == "private-shortcut") {
      if (attempt == 1) probe::switch_request = 7;  // An absent monitor does not authorize HDMI.
      if (attempt == 2) probe::switch_request = 0;  // The user selected the available HDMI monitor.
      return {"card0:HDMI-A-1"};
    }
    if (probe::scenario == "private-missing" || probe::scenario == "private-cancel" ||
        (probe::scenario == "private-reconnect" && attempt == 1) ||
        (probe::scenario == "private-reinit" && attempt == 2)) {
      if (probe::scenario == "private-cancel") video::test_queue->stop();
      return {"card0:HDMI-A-1"};
    }
    if (probe::scenario == "private-manual" && attempt >= 2) return {"card0:HDMI-A-1"};
    if (probe::scenario == "private-manual") return {"card2:Virtual-1", "card0:HDMI-A-1"};
    return {"card0:HDMI-A-1", "card2:Virtual-1"};
  }
  std::vector<std::string> display_names(int) {
    if (probe::scenario == "private-backend") return {"0", "1"};
    const auto targets = kms_capture_targets(0);
    std::vector<std::string> names;
    for (const auto &target : targets) names.push_back(target.substr(target.find(':') + 1));
    return names;
  }
  std::vector<std::string> display_switch_targets(int, display_switch_source_e &source, const capture_deadline_t &) {
    assert(probe::scenario != "private-backend");
    if (probe::scenario.starts_with("private-public-")) {
      source = probe::scenario == "private-public-indexed" ? display_switch_source_e::indexed : display_switch_source_e::named;
      // KWin's F1 selects Virtual-1, where the KMS list puts HDMI first.
      return {"Virtual-1", "HDMI-A-1"};
    }
    source = display_switch_source_e::kms;
    if (probe::scenario == "private-shortcut") return {"card0:HDMI-A-1"};
    return {"card2:Virtual-1", "card0:HDMI-A-1"};
  }
  std::shared_ptr<display_t> make_test_display(const std::string &name) {
    probe::opened_outputs.push_back(name);
    if (probe::scenario == "private-open-retry" && probe::opened_outputs.size() == 1) return {};
    if (probe::scenario == "private-public-open-failed" && name == "HDMI-A-1") return {};
    if (++probe::generations > 8) { video::test_queue->stop(); return {}; }
    if (probe::generations == 2 && probe::scenario == "new-identity") { probe::connector_id=92; probe::luminance=1500; }
    if (probe::generations == 2 && probe::scenario == "recycled-identity") { probe::connector_id=91; probe::luminance=1600; }
    auto result=std::make_shared<kms::display_probe_t>();
    result->captured_connector = name;
    if (name == "HDMI-A-1" && probe::scenario == "private-public-wrong") result->captured_connector = "DP-3";
    if (name == "1" && probe::scenario == "private-public-indexed") result->captured_connector = "HDMI-A-1";
    if(!result->sdr_capture_allowed(true)) return {};
    return result;
  }
  std::shared_ptr<display_t> display(int,const std::string& name,const video::config_t&, const std::optional<adapter_id_t>& = std::nullopt) {
    assert(probe::scenario != "private-backend");
    return make_test_display(name);
  }
  std::shared_ptr<display_t> kms_display(int,const std::string& name,const video::config_t&, const capture_deadline_t & = {}) {
    return make_test_display(name);
  }
}
namespace video {
@@RESET_DISPLAY@@
}
namespace sync_util {
  template<class T> struct sync_t {
    T value;
    sync_t &operator=(const std::shared_ptr<platf::display_t> &p) { value=p; video::select_encoder(p); return *this; }
    auto operator->() { return &value; }
  };
}
namespace mail {
  using event_t = safe::event_t<int>;
  struct man_t { template<class T> auto event(int) { return std::make_shared<event_t>(); } };
  auto man=std::make_shared<man_t>(); int switch_display{};
}
namespace proc { struct proc_t { std::string display_name; }; inline proc_t proc; }
namespace video {
@@PRIVATE_CAPTURE@@
@@CAPTURE_THREAD@@
  using encode_e = platf::capture_e;
@@SYNC_OPEN_FIXTURE@@
  struct sync_context_t { config_t config; safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events; };
  struct sync_lifetime_t { sync_lifetime_t() { ++probe::sync_live_sessions; } ~sync_lifetime_t() { --probe::sync_live_sessions; } };
  struct sync_session_t { std::shared_ptr<platf::display_t> display; std::shared_ptr<sync_lifetime_t> lifetime; };
  std::optional<sync_session_t> make_synced_session(std::shared_ptr<platf::display_t> disp, const encoder_t&, platf::img_t&, sync_context_t&) {
    select_encoder(disp);
    return sync_session_t{disp,std::make_shared<sync_lifetime_t>()};
  }
  encode_e run_sync_recovery(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder) {
    std::vector<std::unique_ptr<sync_context_t>> synced_session_ctxs;
    synced_session_ctxs.emplace_back(std::make_unique<sync_context_t>());
    auto img=disp->alloc_img();
    std::vector<sync_session_t> synced_sessions;
    synced_sessions.push_back(std::move(*make_synced_session(disp,encoder,*img,*synced_session_ctxs.front())));
    const auto ec=platf::capture_e::ok;
    while (test_queue->running()) {
      auto push_captured_image_callback=[&](std::shared_ptr<platf::img_t>&& frame,bool captured) {
        if (captured) { ++probe::deliveries; probe::on_frame(); }
        return test_queue->running();
      };
      auto pull_free_image_callback=[&](std::shared_ptr<platf::img_t> &out) { out=img; return true; };
@@SYNC_RECOVERY@@
    }
    return encode_e::ok;
  }
}
int main(int argc,char **argv) {
  probe::scenario = argc > 1 ? argv[1] : "initial-faults";
  unsetenv("VIBEPOLLO_MACHINE_HOST");
  if(probe::scenario.starts_with("hyprland-")) {
    setenv("VIBEPOLLO_MACHINE_HOST","1",1);
    setenv("VIBEPOLLO_SESSION_BACKEND","hyprland-kms",1);
    probe::eotf=0;
  }
  probe::sync_mode=argc > 2;
  probe::forced_restarts=0;
  unsigned expected_generations=1, expected_hdr=1, expected_sdr=0;
  if (probe::scenario == "private-reinit" || probe::scenario == "private-manual" || probe::scenario.starts_with("private-public-")) {
    probe::forced_restarts = 1;
    expected_generations = expected_hdr = 2;
  } else if (probe::scenario == "initial-faults") {
    probe::fail_initial=true;
    probe::forced_restarts=2;
    expected_generations=expected_hdr=3;
  } else if (probe::scenario == "selected-recovery") {
    expected_sdr=1;
  } else if (probe::scenario == "unreadable-recovery" || probe::scenario == "replacement-aba" || probe::scenario == "initial-aba") {
    expected_hdr=2;
  } else if (probe::scenario == "persistent-selected-faults") {
    expected_sdr=1;
  } else if (probe::scenario == "changed-during-drain") {
    expected_hdr=3;
  } else if (probe::scenario == "new-identity" || probe::scenario == "recycled-identity") {
    probe::forced_restarts=1; expected_generations=expected_hdr=2;
  } else if (probe::scenario == "sdr" || probe::scenario == "hyprland-sdr" || probe::scenario.starts_with("hyprland-second-")) {
    probe::eotf=0; expected_hdr=0; expected_sdr=1;
  } else if (probe::scenario.starts_with("nonpq-churn-")) {
    probe::eotf=std::stoul(probe::scenario.substr(12)); expected_hdr=0; expected_sdr=1;
  } else if (probe::scenario == "nonpq-hdr-transitions") {
    probe::eotf=0; expected_hdr=expected_sdr=2;
  }
  video::test_queue=std::make_shared<safe::queue_t<video::capture_ctx_t>>();
  auto images=std::make_shared<safe::queue_t<std::shared_ptr<platf::img_t>>>();
  video::test_queue->items.push_back({video::test_config,images});
  probe::on_pop=[] {
    if (probe::reinitializing) { video::old_encoder_display.reset(); probe::old_encoder_live=false; }
  };
  probe::on_reinit=[] {
    if (probe::scenario == "changed-during-drain" && probe::restarts == 0) {
      probe::current_id=10; probe::luminance=1300;
    }
  };
  probe::on_frame=[] {
    if (probe::scenario == "private-manual" && probe::frames == 4) probe::switch_request = 1;
    if (probe::scenario == "private-public-reserved" && probe::frames == 4) probe::switch_request = 0;
    if (probe::scenario.starts_with("private-public-") && probe::scenario != "private-public-reserved" && probe::frames == 4) probe::switch_request = 1;
    if (probe::scenario.starts_with("nonpq-churn-")) {
      // Keep the same numeric ID while its unused SDR metadata changes.
      ++probe::cll; ++probe::luminance;
    }
    if (probe::scenario == "nonpq-hdr-transitions") {
      if (probe::eotf != 2) ++probe::cll;
      if (probe::frames == 8) { probe::eotf=2; probe::luminance=1200; }
      if (probe::frames == 16) probe::eotf=3;
      if (probe::frames == 20) { probe::eotf=2; probe::luminance=1300; }
    }
    if (probe::scenario == "equivalent-churn") probe::current_id=probe::frames % 2 ? 11 : 10;
    if (probe::scenario == "replacement-aba" || probe::scenario == "initial-aba") {
      if (probe::frames == 1) probe::current_id=11;
      if (probe::frames == 2) {
        probe::current_id=probe::scenario == "replacement-aba" ? 11 : 10;
        probe::luminance=1200;
      }
    }
    if (probe::scenario == "changed-during-drain" && probe::frames == 2) {
      probe::current_id=11; probe::luminance=1200;
    }
    if (probe::scenario == "unreadable-recovery" && probe::frames == 4) {
      probe::fail_current=false; probe::luminance=1200;
    }
    if (probe::frames >= probe::wanted_frames) video::test_queue->stop();
  };
  sync_util::sync_t<std::weak_ptr<platf::display_t>> display;
  safe::signal_t reinit;
  const auto started = std::chrono::steady_clock::now();
  if (probe::sync_mode) {
    @@SYNC_OPEN_STATE@@
    while (video::test_queue->running()) {
      auto disp = @@SYNC_OPEN_CALL@@;
      if (!disp) break;
      auto status=video::run_sync_recovery(disp,video::encoder_t{});
      if (status != platf::capture_e::reinit) break;
      ++probe::restarts;
    }
  } else {
    video::captureThread(video::test_queue,display,reinit,video::encoder_t{});
  }
  video::old_encoder_display.reset();
  std::cout << (probe::sync_mode ? "sync " : "async ") << probe::scenario << ": generations=" << probe::generations << " frames=" << probe::frames
            << " restarts=" << probe::restarts << " handoffs=" << probe::metadata_commits << " init_reads=" << probe::init_reads
            << " hdr=" << probe::selected_hdr << " sdr=" << probe::selected_sdr << '\n';
  if (probe::scenario.starts_with("private-")) {
    if (probe::scenario == "private-missing" || probe::scenario == "private-cancel" || probe::scenario == "private-slow-discovery") {
      const auto elapsed = std::chrono::steady_clock::now() - started;
      const bool timed_out = probe::scenario != "private-cancel";
      return !probe::opened_outputs.empty() || probe::frames != 0 || (!probe::sync_mode && images->running()) ||
             (timed_out ? (elapsed < 2900ms || elapsed > 6s) : elapsed > 1s);
    }
    std::vector<std::string> expected {"card2:Virtual-1"};
    if (probe::scenario == "private-open-retry" || probe::scenario == "private-reinit") expected.push_back("card2:Virtual-1");
    if (probe::scenario == "private-manual") expected.push_back("card0:HDMI-A-1");
    if (probe::scenario == "private-public-reserved") expected.push_back("card2:Virtual-1");
    if (probe::scenario == "private-public-physical" || probe::scenario == "private-public-open-failed") expected.push_back("HDMI-A-1");
    if (probe::scenario == "private-public-open-failed") expected.push_back("card2:Virtual-1");
    if (probe::scenario == "private-public-wrong") {
      expected.insert(expected.end(), {"HDMI-A-1", "card2:Virtual-1"});
      // The mismatched display was created, but it must never reach the encoder.
      if (probe::wrong_frames) return 12;
    }
    if (probe::scenario == "private-public-indexed") expected.push_back("1");
    if (probe::scenario == "private-shortcut") {
      expected = {"card0:HDMI-A-1"};
      if (probe::enumerations != 2) return 10;
    }
    if (probe::opened_outputs != expected) return 7;
    if (probe::scenario == "private-healthy" && probe::enumerations != 1) return 8;
    if ((probe::scenario == "private-reconnect" || probe::scenario == "private-other-card") && probe::enumerations != 2) return 9;
  }
  if(probe::scenario.starts_with("hyprland-second-")) {
    return probe::frames!=0 || probe::deliveries!=0 || probe::generations!=(probe::sync_mode?2u:3u) || probe::selected_hdr!=0 || probe::selected_sdr!=1 || probe::metadata_commits!=0;
  }
  if (probe::frames != 24 || probe::deliveries != 24 || probe::generations != expected_generations + (probe::scenario == "private-public-wrong") ||
      probe::selected_hdr != expected_hdr || probe::selected_sdr != expected_sdr ||
      probe::metadata_commits != expected_hdr + expected_sdr - expected_generations ||
      probe::restarts != (probe::sync_mode ? expected_generations - 1 : expected_hdr + expected_sdr - 1)) return 1;
  for (size_t i=0; i<video::selections.size(); ++i) {
    auto selection=video::selections[i];
    const bool expect_hdr=probe::scenario == "nonpq-hdr-transitions" ? i % 2 == 1 : probe::scenario != "sdr" && probe::scenario != "hyprland-sdr" && !probe::scenario.starts_with("nonpq-churn-") && !((probe::scenario == "selected-recovery" || probe::scenario == "persistent-selected-faults") && i == 0);
    if (selection.hdr != expect_hdr) return 2;
    if (expect_hdr) {
      if (probe::scenario == "nonpq-hdr-transitions") {
        if (selection.luminance != (i == 1 ? 1200 : 1300)) return 6;
        continue;
      }
      if ((probe::scenario == "new-identity" || probe::scenario == "recycled-identity") && i>0) {
        if (selection.luminance != (probe::scenario == "new-identity" ? 1500 : 1600)) return 5;
        continue;
      }
      if (probe::scenario == "changed-during-drain") {
        if (selection.luminance != (i == 0 ? 1000 : i == 1 ? 1200 : 1300)) return 4;
        continue;
      }
      const auto expected_luminance=i>0 && (probe::scenario == "unreadable-recovery" || probe::scenario == "replacement-aba" || probe::scenario == "initial-aba") ? 1200 : 1000;
      if (selection.luminance != expected_luminance) return 3;
    }
  }
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('compiler', nargs='?', default='c++')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--sanitizers', action='store_true')
    parser.add_argument('--nonpq-controls', action='store_true', help='Run ignored-field churn and genuine HDR transition controls alone')
    parser.add_argument('--persistent-selected-faults', action='store_true', help='Run the repeated real-selection fault schedule alone')
    args = parser.parse_args()
    root = args.source.resolve()
    kms = (root / 'src/platform/linux/kmsgrab.cpp').read_text()
    video = (root / 'src/video.cpp').read_text()
    color = (root / 'src/video_colorspace.cpp').read_text()
    types = (root / 'src/video_colorspace.h').read_text()
    init = function(kms, '            for (auto &connector : card.monitors()) {')
    after_label = kms.index('      break_loop:') + len('      break_loop:')
    init += kms[after_label:kms.index('        // Look for the cursor plane', after_label)]
    members_start = kms.index('      std::optional<uint32_t> connector_id;')
    members = kms[members_start:kms.index('      bool direct_import_required', members_start)]
    refresh = function(kms, '      inline capture_e refresh(')
    refresh = refresh[refresh.index('        // Recheck current connector state'):refresh.index('        if (pending_exported_frame)')]
    sync_function = function(video, '  encode_e encode_run_sync(')
    sync_start = sync_function.index('      auto status = disp->capture(')
    sync_recovery = sync_function[sync_start:sync_function.rindex('    return encode_e::ok;')]
    sync_recovery = sync_recovery[:sync_recovery.rfind('    }')]
    # Use the production synchronous selection and retry branch, retaining its worker state across reopen.
    sync_open, sync_state, sync_call = '', '', 'platf::display(0,"Virtual-1",video::test_config)'
    if '  struct private_capture_t {' in video:
        branch = function(sync_function, '      if (private_capture.output) {')
        sync_state = 'video::private_capture_t private_capture {video::capture_source_e::active_output, [] { return video::test_queue->running(); }};'
        sync_call = 'video::open_sync_display(private_capture)'
        sync_open = r'''
  std::shared_ptr<platf::display_t> open_sync_display(private_capture_t &private_capture) {
    const encoder_t encoder;
    struct session_t { config_t config; } session {test_config};
    std::vector<session_t *> synced_session_ctxs {&session};
    auto switch_display_event = mail::man->event<int>(mail::switch_display);
    std::uint64_t switch_display_generation = 0;
    std::optional<std::string> pending_switch_output;
    std::size_t display_retry_failures = 0;
    std::vector<std::string> display_names;
    int display_p = -1;
    std::shared_ptr<platf::display_t> disp;
    while (test_queue->running()) {
''' + branch + r'''
      return platf::display(0, "Virtual-1", test_config);
    }
    return disp;
  }
'''
    replacements = {
        '@@SYNC_OPEN_FIXTURE@@': sync_open,
        '@@SYNC_OPEN_STATE@@': sync_state,
        '@@SYNC_OPEN_CALL@@': sync_call,
        '@@COLOR_TYPES@@': function(types, '  enum class colorspace_e {') + ';\n' + function(types, '  struct sunshine_colorspace_t {') + ';',
        '@@COLOR_FUNCTIONS@@': function(color, '  bool colorspace_is_hdr(') + '\n' + function(color, '  sunshine_colorspace_t colorspace_from_client_config('),
        '@@KMS_MEMBERS@@': members,
        '@@KMS_STRICT@@': function(kms, '      bool sdr_capture_allowed('),
        '@@KMS_INITIALIZE@@': init,
        '@@KMS_GETTERS@@': function(kms, '      bool is_hdr() {') + '\n' + function(kms, '      bool get_hdr_metadata(SS_HDR_METADATA &metadata) {'),
        '@@KMS_REFRESH@@': refresh,
        '@@KMS_COMMIT@@': function(kms, '      bool commit_pending_capture_metadata() override {') if '      bool commit_pending_capture_metadata() override {' in kms else '',
        '@@KMS_FIXED_CAPTURE@@': function(kms, '      capture_e capture_fixed_rate('),
        '@@MAKE_ENCODE_DEVICE@@': function(video, '  std::unique_ptr<platf::encode_device_t> make_encode_device(', last=True),
        '@@RESET_DISPLAY@@': function(video, '  void reset_display('),
        '@@PRIVATE_CAPTURE@@': function(video, '  struct private_capture_t {') + ';' if '  struct private_capture_t {' in video else '',
        '@@CAPTURE_THREAD@@': function(video, '  void captureThread('),
        '@@SYNC_RECOVERY@@': sync_recovery,
    }
    program = PRELUDE
    for marker, body in replacements.items():
        program = program.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='kms-hdr-worker-') as temporary:
        output = args.output_dir.resolve() if args.output_dir else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        cpp, binary = output / 'worker.cpp', output / 'worker'
        cpp.write_text(program)
        flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libdrm'], text=True))
        command = [args.compiler, '-std=c++23', '-DSUNSHINE_BUILD_DRM', '-pthread', '-I', str(root), str(cpp), str(root / 'src/video_policy.cpp'), *flags, '-o', str(binary)]
        if args.sanitizers:
            command[1:1] = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        compiled = subprocess.run(command, capture_output=True, text=True)
        (output / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            return compiled.returncode
        failed = False
        results = []
        nonpq = ['nonpq-churn-0', 'nonpq-churn-1', 'nonpq-churn-3', 'nonpq-hdr-transitions']
        scenarios = nonpq if args.nonpq_controls else ['persistent-selected-faults'] if args.persistent_selected_faults else ['initial-faults', 'selected-recovery', 'persistent-selected-faults', 'unreadable-recovery', 'equivalent-churn', 'replacement-aba', 'initial-aba', 'changed-during-drain', 'new-identity', 'recycled-identity', 'sdr', *nonpq, 'hyprland-sdr', 'hyprland-second-pq', 'hyprland-second-gamma', 'hyprland-second-hlg', 'hyprland-second-unknown']
        if not args.nonpq_controls and not args.persistent_selected_faults:
            scenarios += ['private-healthy', 'private-missing', 'private-open-retry', 'private-reconnect',
                          'private-reinit', 'private-manual', 'private-shortcut', 'private-cancel', 'private-backend', 'private-other-card',
                          'private-public-reserved', 'private-public-physical', 'private-public-open-failed',
                          'private-public-wrong', 'private-public-indexed', 'private-slow-discovery']
        for scenario in scenarios:
            for mode in [[], ["sync"]]:
                result = subprocess.run([str(binary), scenario, *mode], capture_output=True, text=True, timeout=10, env=os.environ.copy())
                results.append(result.stdout + result.stderr)
                failed |= result.returncode != 0
        (output / 'result.log').write_text(''.join(results))
        print(''.join(results), end='')
        return int(failed)

if __name__ == '__main__':
    raise SystemExit(main())
