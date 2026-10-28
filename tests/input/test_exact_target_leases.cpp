#include "src/display_target_lease.h"
#include "src/remote_display_topology.h"
#include "src/remote_monitor_profile_policy.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

using display_target::descriptor_t;
using display_target::observation_t;
const descriptor_t physical {display_target::platform_e::linux_drm, display_target::kind_e::physical, "/sys/devices/pci/drm/card1/card1-DP-1"};
const descriptor_t shared {display_target::platform_e::linux_drm, display_target::kind_e::shared_virtual, "/sys/devices/faux/vibeshine/drm/card2/card2-Virtual-1"};
const display_target::mode_t mode {1920,1080,59940,false};
auto ready = [](const descriptor_t &target) -> std::optional<observation_t> { return observation_t {target,mode,"DP-1"}; };
int cases = 0;
void passed(const char *name) { ++cases; std::cout << "PASS " << name << '\n'; }
int main() {
  for (const bool reverse : {false,true}) {
    display_target::registry_t r;
    auto a=r.acquire("A",1,shared,mode,ready), b=r.acquire("B",2,shared,mode,ready);
    assert(a && b && !r.begin_mutation());
    auto pending=a.hold; auto teardown=b.hold;
    r.release(reverse?"B":"A",reverse?2:1); (reverse?b:a).hold.reset();
    assert(!r.begin_mutation());
    r.release(reverse?"A":"B",reverse?1:2); a.hold.reset();b.hold.reset();
    assert(!r.begin_mutation()); pending.reset();assert(!r.begin_mutation());
    teardown.reset(); assert(r.begin_mutation());
    passed(reverse?"two clients reverse release, pending+teardown protect":"two clients forward release, pending+teardown protect");
  }
  {
    display_target::registry_t r;
    { auto native=r.begin_mutation(); assert(native); assert(!r.acquire("A",1,physical,mode,ready)); }
    auto a=r.acquire("A",2,physical,mode,ready); assert(a);r.release("A",2);a.hold.reset();
    { auto native=r.begin_mutation();native.completion_unknown(); }
    assert(!r.acquire("A",3,physical,mode,ready) && !r.begin_mutation() && r.protected_output());
    passed("in-flight and unconfirmed helper completion forbid borrow");
  }
  {
    display_target::registry_t r;
    auto outer=r.begin_mutation();assert(outer);
    auto nested=r.begin_mutation();assert(nested); // synchronous callbacks may enter the same gate.
    auto concurrent=std::async(std::launch::async,[&]{return bool(r.begin_mutation());});
    assert(!concurrent.get()); // a second thread cannot enter before the first mutation drains.
    passed("same-thread nested mutation, concurrent cross-thread mutation rejected");
  }
  {
    for(int operation=0;operation<3;++operation) {
      display_target::registry_t r;
      std::thread callback_probe;
      std::atomic_bool probe_started{false};
      std::atomic_bool probe_finished{false};
      std::atomic_int probe_admitted{-1};
      bool probe_finished_before_destructor_return=false;
      auto lease=r.acquire("A",1,physical,mode,[&](const descriptor_t &target)->std::optional<observation_t>{
        auto runtime_hold=std::shared_ptr<void>(new int(0),[&](void *value){
          callback_probe=std::thread([&]{
            probe_started.store(true,std::memory_order_release);
            auto mutation=r.begin_mutation();
            probe_admitted.store(bool(mutation),std::memory_order_release);
            probe_finished.store(true,std::memory_order_release);
          });
          for(unsigned wait=0;wait<500 && !probe_started.load(std::memory_order_acquire);++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          for(unsigned wait=0;wait<500 && probe_started.load(std::memory_order_acquire) &&
              !probe_finished.load(std::memory_order_acquire);++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          probe_finished_before_destructor_return=probe_finished.load(std::memory_order_acquire);
          delete static_cast<int *>(value);
        });
        return observation_t {target,mode,"DP-1",std::move(runtime_hold)};
      });
      assert(lease);
      lease.hold.reset(); // make the registry map the final hold reference.
      if(operation==0)assert(r.release("A",1));
      else if(operation==1)r.revoke("A");
      else r.shutdown();
      callback_probe.join();
      assert(probe_finished_before_destructor_return && probe_admitted.load(std::memory_order_acquire)==0);
    }
    passed("release/revoke/shutdown destroy native holds outside the registry mutex");
  }
  {
    display_target::registry_t r;
    std::thread callback_probe;
    std::atomic_bool probe_started{false};
    std::atomic_bool probe_finished{false};
    std::atomic_int probe_admitted{-1};
    bool probe_finished_before_return=false;
    auto mismatch=mode;++mismatch.width;
    auto rejected=r.acquire("A",1,physical,mode,[&](const descriptor_t &target)->std::optional<observation_t>{
      auto runtime_hold=std::shared_ptr<void>(new int(0),[&](void *value){
        callback_probe=std::thread([&]{
          probe_started.store(true,std::memory_order_release);
          auto mutation=r.begin_mutation();
          probe_admitted.store(bool(mutation),std::memory_order_release);
          probe_finished.store(true,std::memory_order_release);
        });
        for(unsigned wait=0;wait<500 && !probe_started.load(std::memory_order_acquire);++wait)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for(unsigned wait=0;wait<500 && probe_started.load(std::memory_order_acquire) &&
            !probe_finished.load(std::memory_order_acquire);++wait)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        probe_finished_before_return=probe_finished.load(std::memory_order_acquire);
        delete static_cast<int *>(value);
      });
      return observation_t {target,mismatch,"DP-1",std::move(runtime_hold)};
    });
    assert(!rejected);
    callback_probe.join();
    assert(probe_finished_before_return && probe_admitted.load(std::memory_order_acquire)==0);
    assert(r.begin_mutation());
    passed("rejected probe destroys temporary native hold after unlocking admission");
  }
  {
    display_target::registry_t r;
    int mutations=0;
    for (int which=0;which<4;++which) {
      auto mismatch=mode;
      if(which==0)++mismatch.width;if(which==1)++mismatch.height;if(which==2)++mismatch.refresh_millihz;if(which==3)mismatch.hdr=true;
      auto probe=[&](const descriptor_t &target)->std::optional<observation_t>{ assert(!r.begin_mutation()); return observation_t {target,mismatch,"DP-1"}; };
      assert(!r.acquire("A",which+1,physical,mode,probe));assert(!r.protected_output());assert(mutations==0);
    }
    assert(!r.acquire("A",5,physical,mode,{}));
    auto renamed=physical;renamed.exact_id+="-renamed";
    assert(!r.acquire("A",6,physical,mode,[&](const descriptor_t &)->std::optional<observation_t>{return observation_t {renamed,mode,"DP-1"};}));
    assert(r.begin_mutation());passed("width height fractional refresh HDR missing renamed conflicts, no mutation");
  }
  {
    display_target::registry_t r;
    auto a=r.acquire("A",10,physical,mode,ready);assert(a);
    assert(!r.acquire("A",11,shared,mode,ready));
    auto resumed=r.acquire("A",12,physical,mode,ready);assert(resumed && resumed.hold==a.hold);
    assert(r.release("A",10));assert(r.snapshot("A",12));
    assert(!r.acquire("A",9,physical,mode,ready));
    r.release("A",12);a.hold.reset();assert(!r.begin_mutation());resumed.hold.reset();assert(r.begin_mutation());
    assert(!r.acquire("A",12,physical,mode,ready));
    passed("immutable resume binding and stale events");
    auto current=r.acquire("B",20,physical,mode,ready);assert(current);
    assert(r.revalidate(current.hold,ready));
    assert(!r.revalidate(current.hold,[&](const descriptor_t &target)->std::optional<observation_t>{return observation_t {target,mode,"DP-2"};}));
    assert(!r.revalidate(current.hold,{}));
    passed("native capture reopen requires unchanged exact target and output");
  }
  for(const bool shutdown:{false,true}) {
    display_target::registry_t r;std::promise<void> entered,finish;
    auto release=finish.get_future();
    auto work=std::async(std::launch::async,[&]{return r.acquire("A",1,physical,mode,[&](const descriptor_t &target)->std::optional<observation_t>{entered.set_value();release.wait();return observation_t {target,mode,"DP-1"};});});
    entered.get_future().wait();assert(!r.begin_mutation());
    if(shutdown)r.shutdown();else r.revoke("A");finish.set_value();assert(!work.get());assert(r.begin_mutation());
    passed(shutdown?"shutdown races an uncommitted probe":"unpair races an uncommitted probe");
  }
  {
    display_target::registry_t r;auto a=r.acquire("A",1,physical,mode,ready);auto pending=a.hold;
    r.revoke("A");a.hold.reset();assert(!r.begin_mutation());pending.reset();assert(r.begin_mutation());
    auto b=r.acquire("B",1,physical,mode,ready);auto capture=b.hold;r.shutdown();b.hold.reset();assert(!r.begin_mutation());capture.reset();assert(r.begin_mutation());
    passed("revoke and shutdown retain pending and capture references until drain");
  }
  {
    // Actual coordinator: owner exit must preserve the creator identity and
    // native callbacks while a cross-client borrower is pending/tearing down.
    remote_display_topology::coordinator_t coordinator;
    int creates=0,applies=0,removes=0;
    coordinator.set_runtime_callbacks({.create_or_reclaim=[&](auto &,auto &,auto &){++creates;return true;},
      .apply_composed_topology=[&](auto &){++applies;return true;},
      .exact_target_has_current_mode_and_dxgi=[](auto &,auto &)->std::optional<std::string>{return "Virtual-1";},
      .remove_owned_display=[&](auto &){++removes;return true;},
      .terminate_owned_display=[&](auto &){++removes;return true;}});
    auto normal=coordinator.reserve_normal_game_identity("creator","Game",{});assert(normal.accepted);
    auto normal_capture=coordinator.retain_normal_game_capture("creator",normal.token);assert(normal_capture);
    auto borrowed=display_target::leases().acquire("borrower",100,shared,mode,ready);assert(borrowed);
    auto pending=borrowed.hold;
    coordinator.release_normal_game_identity("creator",normal.token);
    normal_capture.reset();coordinator.release_drained_normal_game_identities();
    assert(coordinator.managed_client_identity_count()==1 && removes==0 && applies==0 && creates==0);
    assert(!coordinator.generic_virtual_display_cleanup_allowed() && !coordinator.physical_display_restore_allowed());
    display_target::leases().release("borrower",100);borrowed.hold.reset();coordinator.release_drained_normal_game_identities();assert(removes==0);
    pending.reset();coordinator.release_drained_normal_game_identities();assert(removes==1 && applies==1);
    const auto legacy=coordinator.activate_or_resume("builtin","Builtin",{},200);assert(legacy.ready && creates==1);
    auto physical_borrow=display_target::leases().acquire("physical-borrower",201,physical,mode,ready);assert(physical_borrow);
    auto teardown_hold=physical_borrow.hold;
    assert(!coordinator.explicit_release("builtin",200,"end") && removes==1);
    assert(coordinator.snapshot("builtin",200).accepted);
    display_target::leases().release("physical-borrower",201);physical_borrow.hold.reset();
    coordinator.release_drained_normal_game_identities();assert(removes==1);
    teardown_hold.reset();coordinator.release_drained_normal_game_identities();assert(removes==2);
    passed("actual normal owner cleanup waits for borrower pending/teardown; builtin create/remove retained");
  }
  {
    // Actual public monitor entrypoints, not a parallel selector model.
    int legacy=0;
    remote_session::register_exact_target_probe(ready);
    remote_session::register_monitor_runtime_hooks({.activate_or_resume=[&](auto,auto,auto,bool,auto){++legacy;return remote_session::monitor_runtime_state_t{.accepted=true,.ready=true,.output="builtin"};}});
    auto borrowed=remote_session::borrow_monitor_target("http-client",300,physical,mode);
#ifdef SUNSHINE_EXPERIMENTAL_TARGET_LEASES
    assert(borrowed.accepted && borrowed.ready && borrowed.target_hold && borrowed.exact_target==physical && legacy==0);
    assert(!remote_session::borrow_monitor_target("other-output",301,physical,mode,"DP-2").accepted);
    const auto snapshot=remote_session::monitor_runtime_snapshot("http-client",300);assert(snapshot.output=="DP-1" && snapshot.target_hold);
    remote_session::notify_monitor_transport_lost("http-client",300);
    assert(remote_session::release_monitor("http-client",299,"stale"));assert(remote_session::monitor_runtime_snapshot("http-client",300).accepted);
    assert(remote_session::release_monitor("http-client",300,"release"));
#else
    assert(!borrowed.accepted && borrowed.conflict && !borrowed.target_hold);
#endif
    assert(remote_session::activate_or_resume_monitor("builtin-http","name","1920x1080@60",false,1).ready && legacy==1);
    assert(remote_session::capture_plan(remote_session::role_e::monitor,"DP-1").source==remote_session::capture_source_e::exact_output);
    passed("actual monitor entrypoint exact hold and default-off/builtin controls");
  }
  {
    const nlohmann::json row={{"remote-monitor-profile",true},{"uuid","d71f7d92-62e9-4c54-b07d-9f0a665a581b"},
      {"remote-monitor-target",{{"platform","linux-drm"},{"kind","physical"},{"id",physical.exact_id}}}};
    assert(remote_session::monitor_target_from_json(row)==physical);
    auto bad=row;bad["remote-monitor-target"]["fallback"]="primary";assert(!remote_session::monitor_target_from_json(bad));
    bad=row;bad["remote-monitor-target"]["platform"]=1;assert(!remote_session::monitor_target_from_json(bad));
    const auto retained=remote_session::monitor_profile_t{"profile",{},physical};
    const auto selected=remote_session::select_monitor_profile({"profile",{},shared},retained);assert(selected && selected->target==physical);
    passed("strict typed schema and retained profile target resists edits");
  }
  std::cout << cases << " bounded behavior cases passed\n";
}
