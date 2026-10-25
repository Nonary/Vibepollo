#!/usr/bin/env python3
"""Compile actual RTSP clear/expiry/ANNOUNCE and host terminal callers.

The entire ANNOUNCE parser, natural removal loop, expiry extraction/notification,
WebRTC close body and terminal body are unmodified production fragments. Capture,
peer, executor, socket and finalizer edges are controlled. The topology failure
case links the actual topology coordinator. This is portable source composition,
not native capture, Windows API, or physical-display proof.
"""
import argparse
from pathlib import Path
import subprocess
p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
p.add_argument('--build-dir', type=Path, required=True)
p.add_argument('--json-include', default='')
p.add_argument('--compiler', default='g++')
p.add_argument('--boost-include', default='')
a = p.parse_args()
root=a.source.resolve(); out=a.build_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
rtsp=(root/'src/rtsp.cpp').read_text(); nv=(root/'src/nvhttp.cpp').read_text(); web=(root/'src/webrtc_stream.cpp').read_text()
audio_header=(root/'src/audio.h').read_text(); audio_source=(root/'src/audio.cpp').read_text()
def body(s, signature, indent):
    start=s.index(signature); end=s.index('\n'+indent+'}',start)+len('\n'+indent+'}')
    return s[start:end]
fragments={
'CLAIM_STARTUP':body(rtsp,'    bool claim_launch_startup(','    '),
'RELEASE_CLAIM':body(rtsp,'    void release_launch_startup_claim(','    '),
'AUDIO_PARAMS':body(audio_header,'  struct stream_params_t {','  ')+';',
'AUDIO_CONFIG':body(audio_header,'  struct config_t {','  ')+';',
'DOLBY_CODEC':audio_header[audio_header.index('  enum class dolby_codec_e'):audio_header.index(';',audio_header.index('  enum class dolby_codec_e'))+1],
'DOLBY_CONFIGURE':body(audio_source,'  dolby_codec_e configure_dolby_audio(','  '),
'DOLBY_BITRATE':body(audio_source,'  int dolby_bitrate_kbps(','  '),
'DOLBY_NAME':body(audio_source,'  std::string_view dolby_codec_name(','  '),
'CLEAR':body(rtsp,'    void clear(bool all = true','    '),
'CANCEL_PENDING':body(rtsp,'    void cancel_pending_launches(','    '),
'EXPIRE':body(rtsp,'    void arm_launch_timer()','    '),
'SESSION_CLEAR':body(rtsp,'    void session_clear(','    '),
'DISCONNECT_CLIENT':body(rtsp,'    client_disconnect_result_t disconnect_client(','    '),
'DISCONNECT_ROLE':body(rtsp,'    bool disconnect_remote_role(','    '),
'EXPIRE_LOCKED':body(rtsp,'    std::vector<std::array<std::uint8_t, 16>> expire_launch_sessions_locked(','    '),
'NOTIFY_EXPIRED':body(rtsp,'    void finalize_expired_launch_sessions(','    '),
'RUN_STARTUP':body(rtsp,'    template<class Function>\n    void run_startup(','    '),
'POST':body(rtsp,'    template<class Function>\n    void post(','    '),
'ANNOUNCE':body(rtsp,'  bool cmd_announce(','  '),
'SNAPSHOT':body(rtsp,'  std::shared_ptr<launch_session_t> launch_session_t::clone_for_startup() const','  '),
'CLOSE_WEBRTC':body(web,'  bool close_session(std::string_view id)','  '),
'TERMINAL':body(nv,'  void terminate_all_streams_from_host(','  '),
}
for key, signature in [('RELEASE_HOLDS','    void release_pending_runtime_holds('),('RETAIN_HOLDS','    bool retain_pending_runtime_holds(')]:
    fragments[key]=body(rtsp,signature,'    ') if signature in rtsp else ''
template=Path(__file__).resolve().parents[1]/'support/host_stream_terminal/composition.cpp.in'
s=template.read_text()
for key, fragment in fragments.items(): s=s.replace('@'+key+'@',fragment)
cpp=out/'production-terminal-composition.cpp'; cpp.write_text(s)
exe=out/'production-terminal-composition'
cmd=[a.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-missing-field-initializers','-pthread','-I',str(root)]
for inc in (a.json_include+';'+a.boost_include).split(';'):
    if inc: cmd+=['-I',str(Path(inc).resolve())]
subprocess.run(cmd+[str(cpp),str(root/'src/remote_display_topology.cpp'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=20)
