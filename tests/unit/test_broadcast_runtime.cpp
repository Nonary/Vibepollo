/**
 * @brief Directly linked broadcast lifecycle/control regression tests.
 */
#include "src/stream_broadcast_runtime.h"
#include "src/video_pacing_policy.h"

#include <array>
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace runtime = stream::broadcast_runtime;

namespace {
  constexpr std::string_view shutdown_id = "broadcast_shutdown";
  constexpr std::array<std::string_view, 2> queue_ids {"video_packets", "audio_packets"};
  using queue_ptr = safe::mail_raw_t::queue_t<int>;

  struct schedule_t {
    explicit schedule_t(int late):
        late {late} {
    }

    int late;
    std::promise<void> release_late;
    std::shared_future<void> late_gate = release_late.get_future().share();
    std::promise<void> receive_join;
    std::future<void> receive_joined = receive_join.get_future();
    std::array<std::promise<queue_ptr>, 2> acquired;
    std::array<std::shared_future<queue_ptr>, 2> queues {
      acquired[0].get_future().share(),
      acquired[1].get_future().share()
    };
    std::array<std::weak_ptr<void>, 2> stopped;
    std::vector<runtime::worker_e> joins;
    bool receiving_stopped = false;
    bool sockets_closed = false;
  };

  // Only the socket/thread boundary is replaced. Workers call the real mail
  // registry and queue pop; their scheduling is controlled by promises.
  class scheduled_transport_t final: public runtime::transport_t {
  public:
    scheduled_transport_t(safe::mail_t mail, std::shared_ptr<schedule_t> schedule):
        _mail {std::move(mail)},
        _schedule {std::move(schedule)} {
      for (int index = 0; index < 2; ++index) {
        _workers[index] = std::thread([this, index] {
          if (index == _schedule->late) {
            _schedule->late_gate.wait();
          }
          auto queue = _mail->queue<int>(queue_ids[index]);
          _schedule->acquired[index].set_value(queue);
          queue->pop();
        });
      }
      _schedule->queues[1 - _schedule->late].wait();
    }

    void stop_receiving() override {
      _schedule->receiving_stopped = true;
    }

    void close_sockets() override {
      _schedule->sockets_closed = true;
      for (int index = 0; index < 2; ++index) {
        _schedule->stopped[index] = _mail->queue<int>(queue_ids[index]);
      }
    }

    void join(runtime::worker_e worker) override {
      _schedule->joins.push_back(worker);
      switch (worker) {
        case runtime::worker_e::receive:
          _schedule->release_late.set_value();
          _schedule->receive_join.set_value();
          break;
        case runtime::worker_e::video:
          _workers[0].join();
          break;
        case runtime::worker_e::audio:
          _workers[1].join();
          break;
        case runtime::worker_e::control:
          break;
      }
    }

  private:
    safe::mail_t _mail;
    std::shared_ptr<schedule_t> _schedule;
    std::array<std::thread, 2> _workers;
  };

  struct context_t {
    std::unique_ptr<scheduled_transport_t> transport;
    std::mutex sessions_mutex;
    std::vector<runtime::session_control_t> sessions;
  };

  struct session_data_t {
    explicit session_data_t(std::string uuid, const safe::mail_t &mail):
        uuid {std::move(uuid)},
        idr {mail->event<bool>(this->uuid + "-idr")},
        bitrate_event {mail->event<int>(this->uuid + "-bitrate")} {
    }

    std::string uuid;
    int bitrate = 1000;
    int requested = 1000;
    std::atomic<int> pacing_bitrate_kbps {1000};
    safe::mail_raw_t::event_t<bool> idr;
    safe::mail_raw_t::event_t<int> bitrate_event;

    runtime::session_control_t view() {
      return {uuid, bitrate, requested, pacing_bitrate_kbps, idr, bitrate_event};
    }
  };

  class BroadcastRuntime: public testing::Test {
  protected:
    safe::mail_t mail = std::make_shared<safe::mail_raw_t>();
    runtime::packet_queues_t queues = runtime::packet_queues<int, int>(mail, shutdown_id, queue_ids[0], queue_ids[1]);
    std::shared_ptr<schedule_t> schedule;
    int starts = 0;
    int ends = 0;
    int admissions = 0;
    bool admit = true;
    bool session_lock_held = false;
    safe::shared_t<context_t> broadcast {
      [&](context_t &ctx) {
        ++starts;
        auto startup = queues.reset_for_start();
        ctx.transport = std::make_unique<scheduled_transport_t>(mail, schedule);
        return 0;
      },
      [&](context_t &ctx) {
        ++ends;
        queues.stop_and_join(*ctx.transport);
      }
    };
    runtime::controls_t<context_t> controls {
      broadcast,
      [&](context_t &ctx, const runtime::session_action_t &action) {
        std::lock_guard lock {ctx.sessions_mutex};
        session_lock_held = true;
        const auto result = action(ctx.sessions);
        session_lock_held = false;
        return result;
      },
      [&](context_t &, const std::string &, int) {
        EXPECT_TRUE(session_lock_held);
        ++admissions;
        return admit;
      }
    };

    void prepare(int late) {
      schedule = std::make_shared<schedule_t>(late);
    }

    // Rescue a known historical deadlock, then join every real worker. Tests
    // observe the lost-stop failure before rescue, without leaving hung jobs.
    void complete(std::future<void> &done, std::thread &caller) {
      EXPECT_EQ(schedule->receive_joined.wait_for(2s), std::future_status::ready);
      const int late = schedule->late;
      const auto acquired = schedule->queues[late].wait_for(2s);
      EXPECT_EQ(acquired, std::future_status::ready);
      if (acquired == std::future_status::ready) {
        const auto queue = schedule->queues[late].get();
        EXPECT_FALSE(queue->running()) << "late worker received a fresh unstopped queue";
        EXPECT_FALSE(schedule->stopped[late].owner_before(queue));
        EXPECT_FALSE(queue.owner_before(schedule->stopped[late]));
      }
      const auto completed = done.wait_for(200ms);
      if (completed != std::future_status::ready) {
        for (const auto id : queue_ids) {
          mail->queue<int>(id)->stop();
        }
      }
      caller.join();
      EXPECT_EQ(completed, std::future_status::ready) << "broadcast teardown blocked in a worker join";
      EXPECT_TRUE(schedule->receiving_stopped);
      EXPECT_TRUE(schedule->sockets_closed);
      EXPECT_EQ(schedule->joins, (std::vector {runtime::worker_e::receive, runtime::worker_e::video, runtime::worker_e::audio, runtime::worker_e::control}));
    }

    void close(safe::shared_t<context_t>::ptr_t &owner) {
      std::promise<void> returned;
      auto done = returned.get_future();
      std::thread caller([&] {
        owner.release();
        returned.set_value();
      });
      complete(done, caller);
    }

    template<class Action>
    void idle_control(Action action) {
      prepare(0);
      const auto previous_starts = starts;
      std::promise<void> returned;
      auto done = returned.get_future();
      std::thread caller([&] {
        action();
        returned.set_value();
      });
      // The fixed path never enters transport startup or teardown. The buggy
      // path does, so let the same bounded rescue observe and clean it up.
      if (done.wait_for(200ms) != std::future_status::ready) {
        complete(done, caller);
      } else {
        caller.join();
      }
      EXPECT_EQ(starts, previous_starts) << "idle control constructed a broadcast";
    }
  };

  TEST_F(BroadcastRuntime, LateVideoLookupUsesStoppedQueue) {
    prepare(0);
    auto owner = broadcast.ref();
    close(owner);
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(ends, 1);
  }

  TEST_F(BroadcastRuntime, LateAudioLookupUsesStoppedQueue) {
    prepare(1);
    auto owner = broadcast.ref();
    close(owner);
  }

  TEST_F(BroadcastRuntime, RestartResetsRetainedProducersAndShutdown) {
    auto video = mail->queue<int>(queue_ids[0]);
    auto audio = mail->queue<int>(queue_ids[1]);
    auto shutdown = mail->event<bool>(shutdown_id);
    for (int generation = 0; generation < 4; ++generation) {
      video->reset();
      audio->reset();
      video->raise(11);
      audio->raise(22);
      video->stop();
      audio->stop();
      shutdown->raise(true);
      prepare(generation % 2);
      auto owner = broadcast.ref();
      EXPECT_TRUE(video->running());
      EXPECT_TRUE(audio->running());
      EXPECT_FALSE(video->peek());
      EXPECT_FALSE(audio->peek());
      EXPECT_FALSE(shutdown->peek());
      close(owner);
      EXPECT_FALSE(video->running());
      EXPECT_FALSE(audio->running());
      EXPECT_FALSE(shutdown->peek());
    }
    EXPECT_EQ(starts, 4);
    EXPECT_EQ(ends, 4);
  }

  TEST_F(BroadcastRuntime, RestartAfterRetainedProducersReleaseUsesStoppedQueues) {
    for (int late = 0; late < 2; ++late) {
      SCOPED_TRACE(late);
      {
        // Model capture producers retaining both real queues for one complete
        // generation, then releasing them before the next broadcast starts.
        auto producers = std::array {mail->queue<int>(queue_ids[0]), mail->queue<int>(queue_ids[1])};
        prepare(1 - late);
        auto owner = broadcast.ref();
        close(owner);
      }

      // Replacing the schedule drops its futures' queue references. The next
      // generation therefore exercises teardown after every prior producer
      // and worker queue reference has expired.
      prepare(late);
      auto owner = broadcast.ref();
      close(owner);
    }
    EXPECT_EQ(starts, 4);
    EXPECT_EQ(ends, 4);
  }

  TEST_F(BroadcastRuntime, IdleBitrateDoesNotConstructBroadcast) {
    idle_control([&] {
      EXPECT_EQ(controls.set_bitrate_for_sessions("A", 12000), 0);
    });
    idle_control([&] {
      EXPECT_EQ(controls.set_bitrate_for_sessions("A", 0), 0);
    });
    EXPECT_EQ(admissions, 0);
  }

  TEST_F(BroadcastRuntime, IdleIdrDoesNotConstructBroadcast) {
    idle_control([&] {
      controls.request_idr_for_all_sessions();
    });
  }

  TEST_F(BroadcastRuntime, ActiveBitrateAndIdrPreserveClientFiltering) {
    // Real capture producers retain these queues independently of controls.
    auto producers = std::array {mail->queue<int>(queue_ids[0]), mail->queue<int>(queue_ids[1])};
    prepare(1);
    auto owner = broadcast.ref();
    session_data_t a {"A", mail}, b {"B", mail}, no_events {"C", mail};
    no_events.bitrate_event.reset();
    no_events.idr.reset();
    owner->sessions = {a.view(), b.view(), no_events.view()};
    EXPECT_EQ(controls.set_bitrate_for_sessions("A", 12000), 1);
    EXPECT_EQ(a.bitrate, 12000);
    EXPECT_EQ(a.requested, 12000);
    EXPECT_EQ(a.pacing_bitrate_kbps.load(std::memory_order_relaxed), 12000);
    EXPECT_EQ(stream::pacing::packets_per_ms(0, a.pacing_bitrate_kbps.load(std::memory_order_relaxed), 1376, 1474, 5, 120, 0), 2u);
    EXPECT_EQ(a.bitrate_event->pop(0ms), 12000);
    EXPECT_EQ(b.bitrate, 1000);
    EXPECT_EQ(b.pacing_bitrate_kbps.load(std::memory_order_relaxed), 1000);
    EXPECT_FALSE(b.bitrate_event->peek());
    EXPECT_EQ(controls.set_bitrate_for_sessions("unknown", 12000), 0);
    EXPECT_EQ(controls.set_bitrate_for_sessions("A", -1), 0);
    EXPECT_EQ(admissions, 1);
    EXPECT_EQ(controls.set_bitrate_for_sessions("", 24000), 2);
    EXPECT_EQ(a.bitrate_event->pop(0ms), 24000);
    EXPECT_EQ(b.bitrate_event->pop(0ms), 24000);
    EXPECT_EQ(a.bitrate, 24000);
    EXPECT_EQ(a.pacing_bitrate_kbps.load(std::memory_order_relaxed), 24000);
    EXPECT_EQ(stream::pacing::packets_per_ms(0, a.pacing_bitrate_kbps.load(std::memory_order_relaxed), 1376, 1474, 5, 120, 0), 3u);
    EXPECT_EQ(b.requested, 24000);
    controls.request_idr_for_all_sessions();
    EXPECT_TRUE(a.idr->pop(0ms));
    EXPECT_TRUE(b.idr->pop(0ms));
    EXPECT_EQ(starts, 1);
    close(owner);
  }

  TEST_F(BroadcastRuntime, AdmissionRejectsBeforeMetadataOrEventsChange) {
    auto producers = std::array {mail->queue<int>(queue_ids[0]), mail->queue<int>(queue_ids[1])};
    prepare(0);
    auto owner = broadcast.ref();
    session_data_t a {"A", mail};
    owner->sessions = {a.view()};
    admit = false;
    EXPECT_EQ(controls.set_bitrate_for_sessions("A", 12000), -1);
    EXPECT_EQ(admissions, 1);
    EXPECT_EQ(a.bitrate, 1000);
    EXPECT_EQ(a.requested, 1000);
    EXPECT_EQ(a.pacing_bitrate_kbps.load(std::memory_order_relaxed), 1000);
    EXPECT_FALSE(a.bitrate_event->peek());
    EXPECT_EQ(controls.set_bitrate_for_sessions("unknown", 12000), 0);
    EXPECT_EQ(admissions, 1);
    close(owner);
  }
}  // namespace
