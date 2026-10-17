#!/usr/bin/env python3
"""Replay production cursor forwarding, helper dispatch/WinRT calls, and IPC framing.

Only native IPC and WinRT objects are modeled. The fixture extracts production
method bodies unchanged; it does not run capture or synthesize input. Use
--source-tree for negative controls and --emit-only while CPU slots are held.
The Windows wire layout is checked with GCC/Clang's -fshort-wchar.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, marker):
    start = source.index(marker)
    # All selected definitions end at their original indentation.
    indent = re.match(r" *", marker).group()
    return source[start:source.index("\n" + indent + "}", start) + len(indent) + 2]


def declaration(source, marker):
    start = source.index(marker)
    indent = re.match(r" *", marker).group()
    return source[start:source.index("\n" + indent + "};", start) + len(indent) + 3]


def generate(root):
    read = lambda path: (root / path).read_text()
    header = read("src/platform/windows/ipc/pipes.h")
    pipes = read("src/platform/windows/ipc/pipes.cpp")
    ipc = read("src/platform/windows/ipc/ipc_session.cpp")
    display = read("src/platform/windows/display_wgc.cpp")
    helper = read("tools/sunshine_wgc_capture.cpp")
    wire = declaration(header, "  enum wgc_ipc_config_flags_e")
    if "WGC_IPC_FLAG_HIDE_CURSOR" not in wire:
        wire += "\nconstexpr uint32_t WGC_IPC_FLAG_HIDE_CURSOR = 1u << 3;"
    wire += "\n" + declaration(header, "  struct config_data_t")
    wire += "\n" + header[header.index("  constexpr uint32_t WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC"):header.index("  /**\n   * @brief Result codes")]
    if "struct cursor_visibility_data_t" not in wire:
        wire += "\nconstexpr uint32_t WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC=0x57474343; struct cursor_visibility_data_t {uint32_t magic;uint32_t visible;};"
    setter = function(ipc, "  bool ipc_session_t::set_cursor_visible(") if "ipc_session_t::set_cursor_visible(" in ipc else "bool ipc_session_t::set_cursor_visible(bool){return true;}"
    admission_setter = function(ipc, "  bool ipc_session_t::set_activity_admission_fps(") if "ipc_session_t::set_activity_admission_fps(" in ipc else "bool ipc_session_t::set_activity_admission_fps(int){return true;}"
    send_marker = "  bool AsyncNamedPipe::send(" if "  bool AsyncNamedPipe::send(" in pipes else "  void AsyncNamedPipe::send("
    send = function(pipes, send_marker)
    # Keep historical void send callable in the baseline host fixture. Its
    # inability to expose a failed write remains visible as a behavioral failure.
    old_send = send_marker.startswith("  void")
    if old_send:
        send = send.replace("void AsyncNamedPipe::send", "void AsyncNamedPipe::legacy_send", 1)
        send += "\nbool AsyncNamedPipe::send(std::span<const uint8_t> data){legacy_send(data);return true;}"
    flag_start = ipc.index("    config_data.flags = wgc_ipc_flags(_config);")
    flags = ipc[flag_start:ipc.index("    config_data.initial_frame_buffer_size", flag_start)]
    reconcile_start = ipc.index("    _sent_cursor_visible = (config_data.flags")
    reconcile_end = ipc.index("\n  }\n\n  bool ipc_session_t::set_cursor_visible", reconcile_start)
    reconcile = ipc[reconcile_start:reconcile_end]
    reset = "_sent_cursor_visible.reset();" if "_sent_cursor_visible.reset();" in ipc else ""
    snapshots = []
    for cls in ("display_wgc_ipc_vram_t", "display_wgc_ipc_ram_t"):
        start = display.index("  capture_e " + cls + "::snapshot(")
        boundary = "    timeout = effective_wgc_timeout" if "vram" in cls else "    winrt::com_ptr<ID3D11Texture2D> gpu_tex"
        snapshots.append(display[start:display.index(boundary, start)] + "    return capture_e::ok;\n  }")
    start = display.index("  capture_e display_wgc_ipc_vram_t::acquire_next_frame(")
    snapshots.append(display[start:display.index("    winrt::com_ptr<ID3D11Texture2D> gpu_tex", start)] + "    return capture_e::ok;\n  }")
    update = function(helper, "  void update_cursor_capture()") if "  void update_cursor_capture()" in helper else "void update_cursor_capture() {}"
    dispatch = function(helper, "void handle_ipc_message(")
    stream = declaration(helper, "class WgcControlStream") if "class WgcControlStream" in helper else ""
    callbacks = function(helper, "bool setup_pipe_callbacks(")
    # Creation and live-loop hooks are tested as production source boundaries;
    # session creation itself requires WinRT/D3D and is outside this fixture.
    creation = function(helper, "  bool create_capture_session()")
    loop = helper[helper.index("  // Main message loop"):]
    hooks = ('update_cursor_capture();' in creation and
             creation.index('update_cursor_capture();') > creation.index('_frame_pool.CreateCaptureSession') and
             'wgc_capture_manager.update_cursor_capture();' in loop)
    code = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
struct LUID {uint32_t LowPart; int32_t HighPart;};
struct Log {template<class T> Log& operator<<(const T&){return *this;}};
#define BOOST_LOG(level) Log{}
namespace winrt {
struct hresult_error {int code() const{return -1;} std::string message() const{return "modeled failure";}};
template<class T> std::string to_string(const T&){return "";}
namespace Windows::Graphics::Capture {struct IGraphicsCaptureSession2 {};}
}
namespace boost::log {struct core {static core* get(){static core c;return &c;}void set_filter(bool){}};}
int severity=0; constexpr int INITIAL_LOG_LEVEL=2;
namespace platf {struct img_t{};namespace dxgi {
// WIRE
// INTERFACE
struct NativeState {bool connected=true,success=true,throws=false;int sends=0;std::function<void()> before_send;std::vector<uint8_t> wire;std::deque<std::vector<uint8_t>> chunks;};
struct NativePipe: INamedPipe {
 std::shared_ptr<NativeState> state;explicit NativePipe(std::shared_ptr<NativeState> s):state(std::move(s)){}
 bool send(std::span<const uint8_t> bytes,int) override {++state->sends;if(state->before_send)state->before_send();if(state->throws)throw std::runtime_error("write");if(!state->success)return false;state->wire.insert(state->wire.end(),bytes.begin(),bytes.end());return true;}
 PipeResult receive(std::span<uint8_t> dst,size_t& size,int) override {size=0;if(state->chunks.empty())return PipeResult::Timeout;auto bytes=std::move(state->chunks.front());state->chunks.pop_front();size=bytes.size();std::copy(bytes.begin(),bytes.end(),dst.begin());return PipeResult::Success;}
 PipeResult receive_latest(std::span<uint8_t> dst,size_t& size,int ms) override{return receive(dst,size,ms);}
 void wait_for_client_connection(int) override{} void disconnect() override{state->connected=false;}bool is_connected() override{return state->connected;}
};
class AsyncNamedPipe {
public:
 using MessageCallback=std::function<void(std::span<const uint8_t>)>;using ErrorCallback=std::function<void(const std::string&)>;
 MessageCallback on_message;
 bool start(const MessageCallback& message,const ErrorCallback&){on_message=message;return true;}
 std::unique_ptr<INamedPipe> _pipe;std::mutex _send_mutex;
 bool send(std::span<const uint8_t>);
 // LEGACY_DECL
 void safe_execute_operation(const std::string&,const std::function<void()>& operation) const noexcept {try{operation();}catch(...){}}
};
// SEND
std::unique_ptr<AsyncNamedPipe> channel(std::shared_ptr<NativeState> state){auto out=std::make_unique<AsyncNamedPipe>();out->_pipe=std::make_unique<NativePipe>(state);return out;}
struct ipc_session_t {
 bool _initialized=false,_force_reinit=false,_stop_requested=false,_cursor_visible=true;
 std::optional<bool> _sent_cursor_visible;std::unique_ptr<AsyncNamedPipe> _pipe;std::mutex _pipe_mutex;int _config=0;
 std::atomic<int> _activity_admission_fps{120};int _startup_activity_admission_fps=120;
 bool stop_requested()const{return _stop_requested;}
 bool should_swap_to_dxgi()const{return false;}bool should_reinit()const{return _force_reinit;}bool is_initialized()const{return _initialized;}
 bool set_cursor_visible(bool);
 bool set_activity_admission_fps(int);
 uint32_t wgc_ipc_flags(int)const{return WGC_IPC_FLAG_DRAIN_TO_LATEST|WGC_IPC_FLAG_ALLOW_BUFFER_DECREASE;}
 config_data_t startup_config(){config_data_t config_data{};config_data.activity_admission_fps=_startup_activity_admission_fps;
 // FLAGS
 return config_data;}
 void initialize_if_needed(){if(_initialized)return;auto config_data=startup_config();(void)config_data;
 // RESET
 // RECONCILE
 }
};
// SETTER
// ADMISSION_SETTER
enum class capture_e {ok,error,reinit,interrupted};using pull_free_image_cb_t=int;using texture2d_t=int;
struct display_wgc_ipc_vram_t {std::shared_ptr<ipc_session_t> _ipc_session;capture_e snapshot(const pull_free_image_cb_t&,std::shared_ptr<platf::img_t>&,std::chrono::milliseconds,bool);capture_e acquire_next_frame(std::chrono::milliseconds,texture2d_t&,uint64_t&,bool);};
struct display_wgc_ipc_ram_t {std::shared_ptr<ipc_session_t> _ipc_session;capture_e snapshot(const pull_free_image_cb_t&,std::shared_ptr<platf::img_t>&,std::chrono::milliseconds,bool);};
// SNAPSHOTS
}}
using namespace platf::dxgi;
static config_data_t g_config{};static bool g_config_received=false;static std::mutex g_config_mutex;static std::condition_variable g_config_cv;
static std::atomic<int32_t> g_activity_admission_fps{120};static std::atomic<uint32_t> g_activity_admission_generation{0};static std::atomic<bool> g_cursor_visible{true};
// DISPATCH
// CONTROL_CALLBACK
struct CaptureState {bool supported=true,throws=false,unknown=false,visible=true;int queries=0,calls=0;};
struct NativeSession {
 std::shared_ptr<CaptureState> state;explicit operator bool() const{return bool(state);}
 struct Session2 {std::shared_ptr<CaptureState> state;explicit operator bool()const{return state&&state->supported;}
 void IsCursorCaptureEnabled(bool visible){++state->calls;if(state->throws)throw winrt::hresult_error{};if(state->unknown)throw std::runtime_error("unknown");state->visible=visible;}};
 template<class T> Session2 try_as(){++state->queries;return {state};}
};
struct WgcCaptureManager {NativeSession _capture_session;std::optional<bool> _last_cursor_capture_request;
 // UPDATE
};
int failures=0,checks=0;void check(bool ok,const char* message){++checks;if(!ok){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
template<class T> auto bytes(const T& x){return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&x),sizeof(x));}
void reset_helper(){g_config_received=false;g_config={};g_cursor_visible=true;g_activity_admission_fps=120;g_activity_admission_generation=0;}
std::unique_ptr<AsyncNamedPipe> receiver(){auto pipe=channel(std::make_shared<NativeState>());check(setup_pipe_callbacks(*pipe),"production callback starts");return pipe;}
void deliver(const std::vector<uint8_t>& wire,size_t split=0){
 auto pipe=receiver();auto initial=g_config;if(initial.activity_admission_fps<=0)initial.activity_admission_fps=120;
 pipe->on_message(bytes(initial));
 if(!split){pipe->on_message(wire);return;}
 for(size_t at=0;at<wire.size();at+=split)pipe->on_message({wire.data()+at,std::min(split,wire.size()-at)});
}
void test_control_stream(){
 config_data_t initial{};initial.activity_admission_fps=120;
 cursor_visibility_data_t hide{WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC,0},show{WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC,1},invalid{WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC,2},unknown{0xabcdef,0};
 activity_admission_data_t a{WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,240},b{WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,360},zero{WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,0},negative{WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,-1};
 std::vector<uint8_t> wire;
 auto append=[&](const auto& value){auto data=bytes(value);wire.insert(wire.end(),data.begin(),data.end());};
 append(initial);append(hide);append(a);append(show);append(b);append(hide);
 for(size_t split=1;split<wire.size();++split){reset_helper();auto pipe=receiver();pipe->on_message({wire.data(),split});pipe->on_message({wire.data()+split,wire.size()-split});check(g_config_received&&!g_cursor_visible&&g_activity_admission_fps==360&&g_activity_admission_generation==6,"every two-read split preserves config, ordered cursor and activity updates");}
 for(size_t chunk: {size_t(1),size_t(2),size_t(3),size_t(5),size_t(7),size_t(8),size_t(11),size_t(120),size_t(128),size_t(65536)}){reset_helper();auto pipe=receiver();for(size_t at=0;at<wire.size();at+=chunk)pipe->on_message({wire.data()+at,std::min(chunk,wire.size()-at)});check(g_config_received&&!g_cursor_visible&&g_activity_admission_fps==360&&g_activity_admission_generation==6,"raw coalesced and fragmented interleaved updates lose no activity messages");}
 reset_helper();auto pipe=receiver();pipe->on_message(bytes(initial).first(sizeof(initial)-1));check(!g_config_received,"incomplete startup is retained");pipe->on_message(bytes(initial).last(1));check(g_config_received,"last startup byte completes config");
 pipe->on_message(bytes(hide).first(3));pipe->on_message({});check(g_cursor_visible,"empty read cannot flush incomplete cursor data");
 pipe->on_message(bytes(hide).subspan(3));pipe->on_message(bytes(a).first(6));check(!g_cursor_visible&&g_activity_admission_fps==120&&g_activity_admission_generation==2,"complete cursor applies while incomplete trailing activity remains buffered");
 pipe->on_message(bytes(a).subspan(6));pipe->on_message(bytes(show));check(g_cursor_visible&&g_activity_admission_fps==240&&g_activity_admission_generation==4,"trailing activity completes without losing next cursor");
 wire.clear();append(invalid);append(a);append(unknown);append(zero);append(negative);append(hide);append(b);pipe->on_message(wire);check(!g_cursor_visible&&g_activity_admission_fps==360&&g_activity_admission_generation==8,"invalid runtime payloads consume their record without losing valid neighbors");
 reset_helper();auto old=receiver();old->on_message(bytes(initial));old->on_message(bytes(hide).first(3));old.reset();reset_helper();auto fresh=receiver();fresh->on_message(bytes(initial));fresh->on_message(bytes(b));check(g_cursor_visible&&g_activity_admission_fps==360&&g_activity_admission_generation==4,"fresh helper connection has no retired partial cursor prefix");fresh->on_message(bytes(hide));check(!g_cursor_visible,"reconnected helper accepts cursor update normally");
 reset_helper();auto state=std::make_shared<NativeState>();ipc_session_t host;host._pipe=channel(state);host.initialize_if_needed();auto live=receiver();live->on_message(bytes(initial));host.set_cursor_visible(false);host._pipe->send(bytes(a));check(state->wire.size()==16,"production sender preserves two unframed eight-byte records");live->on_message(state->wire);auto writes=state->sends;check(host.set_cursor_visible(false)&&state->sends==writes&&!g_cursor_visible&&g_activity_admission_fps==240&&g_activity_admission_generation==4,"coalesced hide plus activity applies even when host suppresses unchanged hide resend");
 reset_helper();auto legacy=receiver();legacy->on_message(bytes(initial));legacy->on_message(bytes(b));check(g_cursor_visible&&g_activity_admission_fps==360,"old host raw config and activity wire remains compatible");
}

int main(){
 check(sizeof(wchar_t)==2&&sizeof(config_data_t)==120&&offsetof(config_data_t,flags)==108&&offsetof(config_data_t,activity_admission_fps)==112,"Windows config ABI remains 120 bytes with original offsets");
 check(sizeof(cursor_visibility_data_t)==8&&sizeof(activity_admission_data_t)==8&&WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC!=WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,"runtime messages have distinct magic");
 check(HOOKS,"helper creation and owner loop apply cursor inclusion");
 auto state=std::make_shared<NativeState>();auto session=std::make_shared<ipc_session_t>();session->_pipe=channel(state);
 check(session->set_cursor_visible(false),"hidden request before helper startup is accepted");auto config=session->startup_config();
 check((config.flags&WGC_IPC_FLAG_HIDE_CURSOR)!=0&&state->sends==0,"first startup carries hidden without a premature live write");
 reset_helper();handle_ipc_message(bytes(config));check(!g_cursor_visible,"helper applies startup hide bit");
 auto native=std::make_shared<CaptureState>();WgcCaptureManager manager{{native},{}};manager.update_cursor_capture();check(!native->visible&&native->calls==1,"startup applies hidden to session2");
 session->initialize_if_needed();check(session->set_cursor_visible(false)&&state->sends==0,"startup state is not redundantly sent every frame");
 check(session->set_cursor_visible(true)&&state->sends==1,"live visible change writes once");deliver(state->wire);manager.update_cursor_capture();check(native->visible&&native->calls==2,"live state reaches native session");
 check(session->set_cursor_visible(true)&&state->sends==1,"duplicate live state is coalesced");
 state->wire.clear();check(session->set_cursor_visible(false),"live hide request succeeds");deliver(state->wire);manager.update_cursor_capture();check(!native->visible&&native->calls==3,"live hide reaches the native session");
 state->wire.clear();activity_admission_data_t activity{WGC_ACTIVITY_ADMISSION_MESSAGE_MAGIC,240};session->_pipe->send(bytes(activity));session->set_cursor_visible(true);session->_pipe->send(bytes(activity));session->set_cursor_visible(false);session->set_cursor_visible(true);
 auto mixed=state->wire;for(size_t split: {size_t(0),size_t(1),size_t(3),size_t(7)}){reset_helper();deliver(mixed,split);check(g_cursor_visible&&g_activity_admission_fps==240&&g_activity_admission_generation==6,"coalesced/split raw stream keeps cursor and activity independent");}
 cursor_visibility_data_t invalid{WGC_CURSOR_VISIBILITY_MESSAGE_MAGIC,2};reset_helper();handle_ipc_message(bytes(invalid));check(g_cursor_visible&&g_activity_admission_generation==0,"invalid cursor state is ignored");
 cursor_visibility_data_t unknown{0xabcdef,0};handle_ipc_message(bytes(unknown));check(g_cursor_visible&&g_activity_admission_fps==120,"unknown magic changes neither state");
 auto peer=std::make_shared<ipc_session_t>();peer->_pipe=channel(std::make_shared<NativeState>());peer->initialize_if_needed();session->set_cursor_visible(false);check(peer->_cursor_visible&&!(peer->startup_config().flags&WGC_IPC_FLAG_HIDE_CURSOR),"capture session cursor state is isolated");
 for(int mode=0;mode<3;++mode){auto failed=std::make_shared<ipc_session_t>();auto bad=std::make_shared<NativeState>();failed->_pipe=channel(bad);failed->initialize_if_needed();bad->success=mode!=0;bad->connected=mode!=1;bad->throws=mode==2;
 check(!failed->set_cursor_visible(false)&&failed->_force_reinit,"failed/disconnected/throwing write requires reinit");check(failed->_sent_cursor_visible==true&&!failed->_cursor_visible,"failed write retains desired state without committing delivery");
 failed->_initialized=false;failed->_force_reinit=false;bad->success=true;bad->connected=true;bad->throws=false;failed->initialize_if_needed();check(failed->startup_config().flags&WGC_IPC_FLAG_HIDE_CURSOR,"restart keeps requested visibility");}
 reset_helper();config_data_t legacy{};handle_ipc_message(bytes(legacy));check(g_cursor_visible,"old host with no hide bit retains inclusion");
 for(int mode=0;mode<3;++mode){auto fallback=std::make_shared<CaptureState>();fallback->supported=mode!=0;fallback->throws=mode==1;fallback->unknown=mode==2;WgcCaptureManager optional{{fallback},{}};g_cursor_visible=false;optional.update_cursor_capture();optional.update_cursor_capture();check(fallback->visible&&fallback->queries==1,"missing/throwing optional API continues capture without repeated native calls");}
 for(bool visible: {false,true}){auto capture=std::make_shared<ipc_session_t>();capture->_pipe=channel(std::make_shared<NativeState>());display_wgc_ipc_vram_t v{capture};display_wgc_ipc_ram_t r{capture};std::shared_ptr<platf::img_t> image;
 check(v.snapshot(0,image,std::chrono::milliseconds(0),visible)==capture_e::ok&&capture->_cursor_visible==visible,"VRAM forwards requested visibility before helper startup");
 capture->_initialized=false;capture->_cursor_visible=!visible;check(r.snapshot(0,image,std::chrono::milliseconds(0),visible)==capture_e::ok&&capture->_cursor_visible==visible,"RAM forwards requested visibility before helper startup");
 capture->_initialized=false;capture->_cursor_visible=!visible;texture2d_t tex{};uint64_t qpc{};check(v.acquire_next_frame(std::chrono::milliseconds(0),tex,qpc,visible)==capture_e::ok&&capture->_cursor_visible==visible,"VRAM acquire path forwards requested visibility");}
 auto stopped=std::make_shared<ipc_session_t>();stopped->_stop_requested=true;check(!stopped->set_cursor_visible(false),"retired capture generation cannot send a cursor update");
 check(!stopped->set_activity_admission_fps(240),"retired capture generation cannot send an activity update");
 auto activity_session=std::make_shared<ipc_session_t>();activity_session->_pipe=channel(std::make_shared<NativeState>());
 check(activity_session->set_activity_admission_fps(240)&&activity_session->_activity_admission_fps==240,"pre-init activity update is retained for startup");
 activity_session->_initialized=true;auto activity_state=static_cast<NativePipe*>(activity_session->_pipe->_pipe.get())->state;
 check(activity_session->set_activity_admission_fps(360)&&activity_state->sends==1,"live activity update reports a completed pipe send");
 activity_state->success=false;check(!activity_session->set_activity_admission_fps(480)&&activity_session->_force_reinit,"failed activity update forces reinit and reports failure");
 auto reconcile_session=std::make_shared<ipc_session_t>();auto reconcile_state=std::make_shared<NativeState>();reconcile_session->_pipe=channel(reconcile_state);reconcile_session->_activity_admission_fps=240;
 std::atomic<bool> reconcile_send_entered=false;reconcile_state->before_send=[&]{if(reconcile_state->sends==1){reconcile_send_entered=true;while(reconcile_session->_activity_admission_fps!=360)std::this_thread::yield();}};
 std::jthread transition([&]{while(!reconcile_send_entered)std::this_thread::yield();check(reconcile_session->set_activity_admission_fps(360),"concurrent startup transition sends after reconciliation");});
 reconcile_session->initialize_if_needed();transition.join();check(reconcile_session->_activity_admission_fps==360&&reconcile_state->sends==2,"startup reconciliation cannot overwrite newer desired activity rate");
 activity_admission_data_t final_activity{};memcpy(&final_activity,reconcile_state->wire.data()+reconcile_state->wire.size()-sizeof(final_activity),sizeof(final_activity));check(final_activity.admission_fps==360,"newer activity rate is the final helper update");
 display_wgc_ipc_vram_t cancelled_v{stopped};display_wgc_ipc_ram_t cancelled_r{stopped};std::shared_ptr<platf::img_t> empty;check(cancelled_v.snapshot(0,empty,std::chrono::milliseconds(0),false)==capture_e::interrupted&&cancelled_r.snapshot(0,empty,std::chrono::milliseconds(0),false)==capture_e::interrupted,"cancelled snapshots preserve interrupted status");
 test_control_stream();
 std::cout<<"Production cursor ownership/startup/live/restart/IPC/optional-API contract: failures="<<failures<<", checks="<<checks<<'\n';return failures?1:0;
}
'''
    replacements = {
        "WIRE": wire,
        "INTERFACE": declaration(header, "  enum class PipeResult") + "\n" + declaration(header, "  class INamedPipe"),
        "SEND": send,
        "LEGACY_DECL": "void legacy_send(std::span<const uint8_t>);" if old_send else "",
        "FLAGS": flags,
        "RESET": reset,
        "RECONCILE": reconcile,
        "SETTER": setter,
        "ADMISSION_SETTER": admission_setter,
        "SNAPSHOTS": "\n".join(snapshots),
        "DISPATCH": dispatch,
        "CONTROL_CALLBACK": stream + "\n" + callbacks,
        "UPDATE": update,
    }
    for name, value in replacements.items():
        code = code.replace("// " + name + "\n", value + "\n")
    return code.replace("HOOKS", "true" if hooks else "false")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-tree", type=Path)
    parser.add_argument("--emit-only", type=Path)
    args = parser.parse_args()
    code = generate(args.source_tree or args.repository)
    if args.emit_only:
        args.emit_only.write_text(code)
        print("Prepared production source replay:", args.emit_only)
        return
    with tempfile.TemporaryDirectory(prefix="wgc-cursor-replay-") as temp:
        source = Path(temp) / "test.cpp"
        exe = Path(temp) / "test"
        source.write_text(code)
        subprocess.run([args.compiler, "-std=c++23", "-O0", "-fshort-wchar", "-pthread", "-Wall", "-Wextra", str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=8)


if __name__ == "__main__":
    main()
