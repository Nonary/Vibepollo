"""Compile the production pipe access policy and accept loop against controlled edges."""
from pathlib import Path
import subprocess, tempfile, argparse
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');a=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'src/platform/windows/ipc/pipes.cpp').read_text()
start=s.index('  std::unique_ptr<INamedPipe> accept_server_pipe_client(')
end=s.index('\n  std::unique_ptr<INamedPipe> NamedPipeFactory::create_client',start)
code=r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <iostream>
using HANDLE=void*;using DWORD=std::uint32_t;
void require(bool b){if(!b)throw std::runtime_error("pipe authority regression");}
struct Edge{bool connected;std::optional<DWORD> peer;};
std::vector<Edge> edges;int opened=0,disconnected=0;bool duplicate_failure=false;
struct INamedPipe{Edge edge;void wait_for_client_connection(int n){require(n>0 && n<=100);}
 bool is_connected(){return edge.connected;}auto peer_process_id(){return edge.peer;}
 void disconnect(){++disconnected;}};
std::unique_ptr<INamedPipe> duplicate_server_pipe(HANDLE){
 ++opened;if(duplicate_failure)return {};
 require(!edges.empty());auto e=edges.front();edges.erase(edges.begin());return std::make_unique<INamedPipe>(e);
}
''' + s[start:end] + r'''
int main(){
 auto deadline=[] {return std::chrono::steady_clock::now()+std::chrono::seconds(1);};
 edges={{true,9},{true,42}};
 require(accept_server_pipe_client(nullptr,42,{},deadline())!=nullptr);
 require(opened==2 && disconnected==1); // hostile client drained before real child
 edges={{false,{}},{true,{}},{true,42}};opened=disconnected=0;
 require(accept_server_pipe_client(nullptr,42,{},deadline())!=nullptr);
 require(opened==3 && disconnected==2); // unconnected and unknown peer are never framed
 opened=0;require(!accept_server_pipe_client(nullptr,0,{},deadline()) && opened==0);
 require(!accept_server_pipe_client(nullptr,42,[]{return true;},deadline()) && opened==0);
 require(!accept_server_pipe_client(nullptr,42,{},std::chrono::steady_clock::now()) && opened==0);
 duplicate_failure=true;require(!accept_server_pipe_client(nullptr,42,{},deadline()) && opened==1);
 duplicate_failure=false;edges={{true,9}};opened=0;
 auto stale=[] {return opened!=0;};
 require(!accept_server_pipe_client(nullptr,42,stale,deadline()) && opened==1); // rotation stops retry
 std::cout<<"production host pipe accept: exact peer, wrong/missing peer, cancellation, deadline, duplicate failure and rotation passed\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'authority.cpp';exe=Path(tmp)/'authority';cpp.write_text(code)
 subprocess.run([a.compiler,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=5)

 # Windows adds FILE_READ_ATTRIBUTES to a pipe client's CreateFile request.
 # Check the production grant against that complete access requirement on every
 # host, including the absence of rights that would change pipe authority.
 policy=(r/'src/platform/windows/ipc/pipe_client_access.h').read_text()
 policy=policy.replace('#pragma once','').replace('#include <windows.h>','')
 source=r'''
#include <cstdint>
#include <iostream>
#include <stdexcept>
using DWORD=std::uint32_t;
constexpr DWORD FILE_READ_DATA=1, FILE_WRITE_DATA=2, FILE_READ_ATTRIBUTES=0x80,
                SYNCHRONIZE=0x100000, FILE_CREATE_PIPE_INSTANCE=4,
                WRITE_DAC=0x40000, WRITE_OWNER=0x80000;
''' + policy + r'''
int main() {
 using namespace platf::dxgi::pipe_security;
 constexpr auto required=FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE;
 if ((client_data_access & required) != required ||
     (client_data_access & (FILE_CREATE_PIPE_INSTANCE|WRITE_DAC|WRITE_OWNER)) != 0 ||
     client_access_sddl(L"S-1-5-21-387") != L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100083;;;S-1-5-21-387)")
   throw std::runtime_error("pipe grant must satisfy CreateFile without granting pipe authority");
 std::cout << "pipe access policy satisfies implicit read-attributes and excludes authority rights\n";
}
'''
 cpp=Path(tmp)/'access.cpp';exe=Path(tmp)/'access';cpp.write_text(source)
 subprocess.run([a.compiler,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=5)
