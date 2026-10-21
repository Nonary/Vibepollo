// Controlled CPU witnesses. Actual protocol/control/gate/consumer headers and
// extracted real IPC/dispatcher bodies are compiled unchanged. Only pipe,
// process liveness, kernel handle ownership and native observations are leaves.
#include "src/platform/windows/display_helper_borrow_control.h"
#include "src/platform/windows/display_helper_borrow_consumer.h"

#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <thread>
#include <future>

using namespace std::chrono_literals;
using namespace display_helper::borrow;
static int checks=0;
static void check(bool value) { ++checks; if(!value) throw std::runtime_error("borrow witness check "+std::to_string(checks)); }
template<class F> static void eventually(F ready) {
  const auto end=std::chrono::steady_clock::now()+2s;
  while(!ready() && std::chrono::steady_clock::now()<end) std::this_thread::yield();
  check(ready());
}

namespace actual_worker {
  class AsyncDispatcher {
  public:
    AsyncDispatcher():worker_([this](std::stop_token st){worker_loop(st);}){}
    ~AsyncDispatcher(){worker_.request_stop();cv_.notify_all();if(worker_.joinable())worker_.join();}
    void enqueue_task(std::function<void()>);
  private:
    void worker_loop(std::stop_token);
    std::mutex mutex_;std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;std::jthread worker_;
  };
  #include "actual-worker.inc"
}

namespace platf::dxgi { enum class PipeResult {Success,Timeout,Failed}; }
namespace actual_client {
  enum class MsgType:std::uint8_t { BorrowAcquire=17,BorrowAcquireResult=18,BorrowRelease=19,BorrowReleaseResult=20,Ping=254 };
  struct pipe_t {
    std::deque<std::vector<std::uint8_t>> replies;
    std::optional<std::uint32_t> peer {20};
    std::optional<std::uint32_t> peer_process_id() {return peer;}
    std::function<void(MsgType,const std::vector<std::uint8_t>&)> dispatch;
    platf::dxgi::PipeResult receive(std::span<std::uint8_t> buffer,std::size_t &read,int timeout){
      if(replies.empty()){std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout,2)));read=0;return platf::dxgi::PipeResult::Timeout;}
      auto bytes=std::move(replies.front());replies.pop_front();read=bytes.size();std::copy(bytes.begin(),bytes.end(),buffer.begin());return platf::dxgi::PipeResult::Success;
    }
  };
  struct session_t {std::shared_ptr<pipe_t> pipe=std::make_shared<pipe_t>();std::timed_mutex reader;bool current=true;std::deque<std::vector<std::uint8_t>> inbox;};
  using SessionPtr=std::shared_ptr<session_t>;
  static SessionPtr active;
  static bool reject_before_write=false;
  static constexpr int kConnectTimeoutMs=2000;
  static bool session_is_current(const SessionPtr &s){return s && s==active && s->current;}
  static int remaining_timeout_ms(auto deadline){return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count());}
  static SessionPtr connected_session_within(auto,int,const std::function<bool()> &cancelled){return cancelled&&cancelled()?nullptr:active;}
  static bool lock_response_reader_until(const SessionPtr &s,std::unique_lock<std::timed_mutex> &lock,auto,const std::function<bool()> &cancelled){if(cancelled())return false;lock=std::unique_lock(s->reader);return true;}
  static bool send_serialized_within(const SessionPtr &s,MsgType type,const std::vector<std::uint8_t> &payload,auto,std::optional<std::uint64_t>,std::optional<int>,std::uint64_t*,bool,const std::function<bool()> &cancelled,bool *attempted=nullptr){if(attempted)*attempted=false;if(cancelled()||reject_before_write)return false;if(attempted)*attempted=true;s->pipe->dispatch(type,payload);return true;}
  static std::optional<std::vector<std::uint8_t>> take_buffered_response(const SessionPtr&s,MsgType type,const std::function<bool(std::span<const std::uint8_t>)>&match){for(auto i=s->inbox.begin();i!=s->inbox.end();++i)if(!i->empty()&&i->front()==static_cast<std::uint8_t>(type)&&match(*i)){auto v=std::move(*i);s->inbox.erase(i);return v;}return std::nullopt;}
  static void buffer_response(const SessionPtr&s,std::span<const std::uint8_t> bytes){s->inbox.emplace_back(bytes.begin(),bytes.end());}
  #include "actual-client.inc"
}

struct producer_fixture {
  gate_t gate;
  process_context_t controller{10,100},helper{20,200};
  std::optional<bool> alive=true;
  control_t control;
  mailbox_t mailbox;
  std::uint64_t epoch=1;
  bool old_version=false,lose_release=false,wrong_reply=false,retire_after_write=false,cancel_after_write=false;
  std::function<void()> acquired_hook;
  producer_fixture():control(gate,helper,[this](auto){return alive;},[]{return 77;}),mailbox(control){
    actual_client::reject_before_write=false;
    actual_client::active=std::make_shared<actual_client::session_t>();
    auto session=actual_client::active;
    session->pipe->dispatch=[this,session](auto type,const auto &payload){
      if(old_version || (lose_release&&type==actual_client::MsgType::BorrowRelease))return;
      mailbox.post(static_cast<std::uint8_t>(type),payload,epoch,controller.pid);
      mailbox.service([&](auto response,const result_t&r,auto){auto result=r;if(wrong_reply)++result.token.request.id;auto bytes=encode_result(result);bytes.insert(bytes.begin(),response);session->pipe->replies.push_back(std::move(bytes));});
      if(type==actual_client::MsgType::BorrowAcquire){if(acquired_hook)acquired_hook();if(retire_after_write)session->current=false;}
    };
  }
  consumer_ports_t ports(std::vector<int> &order,bool &fenced,bool &pin_ok,bool &kernel_ok,int &observations){
    return {
      .context=[this]{return controller;},
      .acquire=[](auto request,auto deadline){bool unknown=false;auto value=actual_client::acquire_borrow_quiescence(request,deadline,{},unknown);return acquire_outcome_t{value,unknown};},
      .release=[](auto token,auto deadline){return actual_client::release_borrow_quiescence(token,deadline);},
      .pin_native_resource=[&](auto)->std::optional<std::shared_ptr<void>>{if(!pin_ok)return std::nullopt;return std::shared_ptr<void>(new int(1),[&](void*p){order.push_back(1);delete static_cast<int*>(p);});},
      .inhibit_kernel=[&](auto,auto)->std::shared_ptr<void>{if(!kernel_ok)return {};return std::shared_ptr<void>(new int(2),[&](void*p){order.push_back(2);delete static_cast<int*>(p);});},
      .alive=[this](auto){return alive;},
      .observe=[&](auto target)->std::optional<display_target::observation_t>{++observations;return display_target::observation_t{target,{1920,1080,60000,false},"DXGI-exact",{}};},
      .fence_unknown=[&]{fenced=true;},
    };
  }
};

int main(){
  check(std::atexit([] {
    if (!process_teardown_started().load(std::memory_order_acquire)) std::abort();
    std::cout<<"PASS actual late-registered atexit guard precedes earlier owner teardown.\n";
  })==0);
  const process_context_t host{10,100},helper{20,200};
  const request_t a{host,1},b{host,2};
  auto bytes=encode_acquire(a);check(decode_acquire(bytes)==a);
  bytes[0]=2;check(!decode_acquire(bytes));bytes=encode_acquire(a);bytes.push_back(0);check(!decode_acquire(bytes));
  token_t ta{a,helper,1};auto reply=encode_result({ta,true,77});check(decode_result(reply)->native_keepalive_handle==77);
  reply.pop_back();check(!decode_result(reply));
  {
    gate_t gate;control_t producer(gate,helper,[](auto){return std::optional<bool>(true);},[]{return 77;});
    auto ra=producer.acquire(a,1,std::chrono::steady_clock::now()+1s);check(ra.success);check(gate.inhibited());
    auto rb=producer.acquire(b,2,std::chrono::steady_clock::now()+1s);check(rb.success);
    check(producer.release(ra.token,3).success);check(gate.inhibited());
    check(!producer.release(ra.token,3).success);check(gate.inhibited());
    auto stale=rb.token;++stale.helper.creation_time;check(!producer.release(stale,3).success);check(gate.inhibited());
    const auto duplicate=producer.acquire(b,4,std::chrono::steady_clock::now()+1s);check(duplicate.success);check(duplicate.token==rb.token);
    check(producer.release(rb.token,3).success);check(!gate.inhibited());
    check(!producer.acquire(a,4,std::chrono::steady_clock::now()+1s).success);
  }
  {
    gate_t gate;std::optional<bool> live=true;control_t producer(gate,helper,[&](auto){return live;},[]{return 0;});
    auto value=producer.acquire(a,1,std::chrono::steady_clock::now()+1s);check(value.success);
    live=std::nullopt;producer.reap_dead_controllers();check(gate.inhibited());
    live=false;producer.reap_dead_controllers();check(!gate.inhibited());
    control_t restarted(gate,{20,201},[](auto){return std::optional<bool>(true);},[]{return 0;});
    check(!restarted.release(value.token,2).success);
  }
  {
    auto &gate=process_gate();actual_worker::AsyncDispatcher worker;
    std::atomic<bool> entered=false,finish=false,queued_restore=false,ack=false;
    worker.enqueue_task([&]{entered=true;while(!finish.load())std::this_thread::yield();});eventually([&]{return entered.load();});
    control_t control(gate,helper,[](auto){return std::optional<bool>(true);},[]{return 77;});mailbox_t mailbox(control);
    check(mailbox.post(acquire_message,encode_acquire({host,10}),1,host.pid));result_t token;
    std::thread acquiring([&]{mailbox.service([&](auto,const auto&r,auto){token=r;ack=true;});});
    eventually([&]{return gate.inhibited();});check(!ack.load());
    worker.enqueue_task([&]{queued_restore=true;});check(!queued_restore.load());
    finish=true;acquiring.join();check(ack&&token.success);check(!queued_restore.load());check(gate.active()==0);
    check(mailbox.post(release_message,encode_release(token.token),1,host.pid));mailbox.service([](auto,const auto&,auto){});
    eventually([&]{return queued_restore.load();});
  }
  {
    gate_t gate;control_t producer(gate,helper,[](auto){return std::optional<bool>(true);},[]{return 0;});mailbox_t mailbox(producer);
    check(!mailbox.post(acquire_message,encode_acquire(a),1,std::nullopt));
    check(!mailbox.post(acquire_message,encode_acquire(a),1,11));
    check(!mailbox.post(acquire_message,encode_acquire(a),0,host.pid));
    auto unsupported=encode_acquire(a);unsupported[0]=2;check(!mailbox.post(acquire_message,unsupported,1,host.pid));
    check(!gate.inhibited());
    const auto acquired=producer.acquire(a,1,std::chrono::steady_clock::now()+1s);check(acquired.success);
    check(!producer.release({a,{},0},2).success);check(gate.inhibited());
    check(producer.release(acquired.token,2).success);check(!gate.inhibited());
    const request_t new_process{{host.pid,host.creation_time+1},1};
    const auto next=producer.acquire(new_process,3,std::chrono::steady_clock::now()+1s);check(next.success);
    check(!producer.release(acquired.token,3).success);check(gate.inhibited());
    check(producer.release(next.token,3).success);check(!gate.inhibited());
  }
  {
    gate_t gate;auto active=gate.try_enter();std::atomic<bool> closed=false;
    bool acquired=true;std::thread pending([&]{closed=true;acquired=gate.acquire(a,std::chrono::steady_clock::now()+15ms);});
    eventually([&]{return closed.load();});pending.join();check(!acquired);check(!gate.inhibited());active.reset();
    check(!gate.acquire(a,std::chrono::steady_clock::now()+1s));
    check(gate.acquire(b,std::chrono::steady_clock::now()+1s));check(gate.release(b));
  }
  const display_target::descriptor_t target{display_target::platform_e::windows_device,display_target::kind_e::physical,"native-exact"};
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=true;int reads=0;
    consumer_t client(host_gate,f.ports(order,fenced,pin,kernel,reads));
    auto first=client.observe(target);check(first.has_value());check(reads==1);check(order.empty());
    auto pending=first->runtime_hold;auto capture=client.observe(target);check(capture.has_value());check(reads==2);check(host_gate.inhibited());
    first.reset();pending.reset();check(order.empty());check(!host_gate.try_enter());capture.reset();
    check(order==std::vector<int>({1,2}));check(!host_gate.inhibited());check(!f.gate.inhibited());check(!fenced);
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=false,kernel=true;int reads=0;
    consumer_t client(host_gate,f.ports(order,fenced,pin,kernel,reads));check(!client.observe(target));check(reads==0);check(!host_gate.inhibited());check(!f.gate.inhibited());
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=false;int reads=0;
    consumer_t client(host_gate,f.ports(order,fenced,pin,kernel,reads));check(!client.observe(target));check(reads==0);check(order==std::vector<int>({1}));check(!host_gate.inhibited());
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=true;int reads=0;
    consumer_t client(host_gate,f.ports(order,fenced,pin,kernel,reads));auto value=client.observe(target);check(value.has_value());f.lose_release=true;value.reset();
    check(fenced);check(host_gate.permanently_inhibited());check(order.empty());check(!host_gate.try_enter());check(!client.observe(target));
  }
  {
    producer_fixture f;f.old_version=true;bool unknown=false;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+10ms,{},unknown));check(unknown);
  }
  {
    producer_fixture f;bool unknown=false;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+100ms,[]{return true;},unknown));check(!unknown);check(!f.gate.inhibited());
  }
  {
    producer_fixture f;actual_client::reject_before_write=true;bool unknown=true;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+100ms,{},unknown));check(!unknown);check(!f.gate.inhibited());
  }
  {
    producer_fixture f;f.alive=false;bool unknown=true;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+100ms,{},unknown));check(!unknown);check(!f.gate.inhibited());
  }
  {
    producer_fixture f;bool cancelled=false,unknown=true;f.acquired_hook=[&]{cancelled=true;};
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+100ms,[&]{return cancelled;},unknown));
    check(!unknown);check(!f.gate.inhibited());
  }
  {
    producer_fixture f;f.retire_after_write=true;bool unknown=false;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+20ms,{},unknown));check(unknown);check(f.gate.inhibited());
  }
  {
    producer_fixture f;actual_client::active->pipe->peer=21;bool unknown=false;
    check(!actual_client::acquire_borrow_quiescence(a,std::chrono::steady_clock::now()+10ms,{},unknown));check(unknown);
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=true;int reads=0;
    auto ports=f.ports(order,fenced,pin,kernel,reads);ports.observe=[&](auto)->std::optional<display_target::observation_t>{++reads;return std::nullopt;};
    consumer_t client(host_gate,ports);check(!client.observe(target));check(reads==1);check(order==std::vector<int>({1,2}));check(!host_gate.inhibited());check(!f.gate.inhibited());
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=true;int reads=0;
    auto ports=f.ports(order,fenced,pin,kernel,reads);ports.alive=[&](auto)->std::optional<bool>{return std::nullopt;};
    consumer_t client(host_gate,ports);check(!client.observe(target));check(fenced);check(host_gate.permanently_inhibited());check(!client.observe(target));
  }
  {
    producer_fixture f;gate_t host_gate;std::vector<int> order;bool fenced=false,pin=true,kernel=true;int reads=0,releases=0;
    auto ports=f.ports(order,fenced,pin,kernel,reads);auto release=ports.release;
    ports.release=[&](auto token,auto deadline){++releases;return release(token,deadline);};
    consumer_t client(host_gate,ports);auto value=client.observe(target);check(value.has_value());
    process_teardown_started().store(true,std::memory_order_release);value.reset();
    check(releases==0);check(order.empty());check(host_gate.inhibited());check(f.gate.inhibited());check(!fenced);
  }
  // The real production atexit callback must set this again before the
  // earlier owner teardown assertion runs. This does not execute native APIs.
  process_teardown_started().store(false,std::memory_order_release);
  std::cout<<"PASS "<<checks<<" actual protocol/mailbox/dispatcher/host-caller/consumer checks; native leaves controlled, no Windows process/device/display execution.\n";
}
