"""Exercise actual Avahi start/teardown with fake API lifetime recording."""
from pathlib import Path
import argparse, subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2]);a=p.parse_args()
s=(a.repo/'src/platform/linux/publish.cpp').read_text();start=s.index('  class deinit_t: public ::platf::deinit_t {');body=s[start:s.index('\n}  // namespace platf::publish',start)]
program=r'''
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using namespace std::literals;
struct log_t{template<class T>log_t &operator<<(const T &){return *this;}} logger;
#define BOOST_LOG(level) logger
std::mutex gate;
std::condition_variable condition;
bool loop_started=false,loop_can_return=false,client_freed=false;
std::vector<std::string> events;
namespace avahi{
 struct Client{};struct EntryGroup{};struct SimplePoll{};enum ClientFlags{none=0};
 int init_client(){return 0;}
 SimplePoll *simple_poll_new(){return new SimplePoll;}
 SimplePoll *simple_poll_get(SimplePoll *p){return p;}
 int simple_poll_loop(SimplePoll *){std::unique_lock lock(gate);loop_started=true;condition.notify_all();condition.wait(lock,[]{return loop_can_return;});events.push_back("loop-return");return 0;}
 void quit_impl(SimplePoll *){std::lock_guard lock(gate);loop_can_return=true;condition.notify_all();}
 void (*simple_poll_quit)(SimplePoll *)=quit_impl;
 void free_client(Client *p){std::lock_guard lock(gate);events.push_back("client-free");client_freed=true;delete p;condition.notify_all();}
 void free_poll(SimplePoll *p){std::lock_guard lock(gate);events.push_back("poll-free");delete p;}
 char *strdup(const char *s){return ::strdup(s);}
 const char *strerror(int){return "fake";}
 template<class Callback>Client *client_new(SimplePoll *,ClientFlags,Callback,std::nullptr_t,int *){return new Client;}
}
namespace net{std::string mdns_instance_name(const std::string &s){return s;}}
namespace platf{
 struct deinit_t{virtual ~deinit_t()=default;};
 void set_thread_name(const char *){} std::string get_host_name(){return "fake-host";}
 namespace publish{
 std::unique_ptr<avahi::SimplePoll,decltype(&avahi::free_poll)> poll{nullptr,avahi::free_poll};
 std::unique_ptr<avahi::Client,decltype(&avahi::free_client)> client{nullptr,avahi::free_client};
 std::unique_ptr<char,decltype(&std::free)> name{nullptr,std::free};
 avahi::EntryGroup *group=nullptr;
 void client_callback(){}
@BODY@
 }}
int main(){
 auto owner=platf::publish::start();assert(owner);
 {std::unique_lock lock(gate);assert(condition.wait_for(lock,2s,[]{return loop_started;}));
  platf::publish::group=reinterpret_cast<avahi::EntryGroup *>(1);loop_can_return=true;condition.notify_all();
  assert(condition.wait_for(lock,2s,[]{return client_freed;}));}
 owner.reset();
 assert(platf::publish::group==nullptr);assert(!platf::publish::client);assert(!platf::publish::poll);
 assert((events==std::vector<std::string>{"loop-return","client-free","poll-free"}));
}
'''.replace('@BODY@',body)
with tempfile.TemporaryDirectory(prefix='avahi-owner-') as d:
 cpp=Path(d)/'avahi.cpp';cpp.write_text(program);bin=Path(d)/'avahi'
 subprocess.run(['clang++','-std=c++23','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',str(cpp),'-o',str(bin)],check=True)
 subprocess.run([str(bin)],check=True)
 print('PASS: actual Avahi loop exit releases client/group before owner reset, then poll teardown after join')
