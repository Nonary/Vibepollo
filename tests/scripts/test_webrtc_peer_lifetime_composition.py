#!/usr/bin/env python3
"""Compile unchanged offer/ICE/SDP/close/terminal methods against a controlled C SDK.

The fake C wrapper records use after release and exposes completion boundaries;
this proves production ownership/order, not libwebrtc threading or native devices.
The same template deliberately compiles the immutable V3 source as a red control.
"""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--compiler',default='g++');p.add_argument('--full-drain',action='store_true');a=p.parse_args()
root=a.source.resolve();out=a.build_dir.resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'src/webrtc_stream.cpp').read_text();nv=(root/'src/nvhttp.cpp').read_text()
def body(text,signature,indent):
 start=text.index(signature)
 while ';' in text[start:text.index('\n',start)]:start=text.index(signature,start+len(signature))
 end=text.index('\n'+indent+'}',start)+len('\n'+indent+'}')
 return text[start:end]
start=s.index('    struct SessionIceContext {');end=s.index('\n#endif',start)
fragments={'CONTEXTS':s[start:end], 'OFFER':body(s,'  bool set_remote_offer(','  '),'ICE':body(s,'  bool add_ice_candidate(','  '),'CLOSE':body(s,'  bool close_session(','  '),'LOCAL_ANSWER':body(s,'  bool set_local_answer(','  '),'TERMINAL':body(nv,'  void terminate_all_streams_from_host(','  ')}
for key,name in [('REMOTE_OK','on_set_remote_success'),('REMOTE_FAIL','on_set_remote_failure'),('ANSWER_OK','on_create_answer_success'),('ANSWER_FAIL','on_create_answer_failure'),('LOCAL_OK','on_set_local_success'),('LOCAL_FAIL','on_set_local_failure'),('STORE_ERROR','store_negotiation_error')]:fragments[key]=body(s,'    void '+name+'(','    ')
fragments['DATA_CHANNEL']=body(s,'    void on_data_channel(void *user, lwrtc_data_channel_t *channel)','    ')
fragments['HAS_LIFETIME']='1' if 'struct SessionPeerLifetime {' in s else '0'
template=Path(__file__).resolve().parents[1]/'support/webrtc_peer_lifetime/composition.cpp.in'
text=template.read_text()
for key,value in fragments.items():text=text.replace('@'+key+'@',value)
cpp=out/'production-peer-lifetime.cpp';cpp.write_text(text);exe=out/'production-peer-lifetime'
subprocess.run([a.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread','-I',str(root),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)]+(['--full-drain'] if a.full_drain else []),check=True,timeout=70)
