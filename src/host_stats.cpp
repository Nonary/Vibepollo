/**
 * @file src/host_stats.cpp
 * @brief Background sampler for host CPU/GPU/RAM/VRAM/temperature counters.
 */
#include "host_stats.h"

#include "logging.h"
#include "sync.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace host_stats {

  using namespace std::chrono_literals;

  namespace {
    constexpr auto SAMPLE_INTERVAL = 2s;

    sync_util::sync_t<platf::host_stats_t> g_latest;
    platf::host_info_t g_info;

    std::unique_ptr<platf::host_stats_provider_t> g_provider;
    std::thread g_thread;
    std::mutex g_state_mutex;
    std::mutex g_cv_mutex;
    std::condition_variable g_cv;
    std::atomic<bool> g_stop {false};
    std::uint64_t g_owner_generation = 0;
    std::uint64_t g_next_generation = 0;

    void
      sampler_loop() {
      while (!g_stop.load(std::memory_order_acquire)) {
        try {
          auto sample = g_provider->sample();
          {
            auto lg = g_latest.lock();
            g_latest.raw = sample;
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "host_stats: provider sample failed: " << e.what();
        } catch (...) {
          BOOST_LOG(warning) << "host_stats: provider sample failed (unknown exception)";
        }
        std::unique_lock<std::mutex> lk(g_cv_mutex);
        g_cv.wait_for(lk, SAMPLE_INTERVAL, [] {
          return g_stop.load(std::memory_order_acquire);
        });
      }
    }

    class deinit_t: public platf::deinit_t {
    public:
      explicit deinit_t(bool owns_sampler, std::uint64_t generation):
          _owns_sampler(owns_sampler),
          _generation(generation) {
      }

      ~deinit_t() override {
        if (!_owns_sampler) {
          return;
        }
        std::lock_guard<std::mutex> state_lk(g_state_mutex);
        if (g_owner_generation != _generation) {
          return;
        }
        g_stop.store(true, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lk(g_cv_mutex);
          g_cv.notify_all();
        }
        if (g_thread.joinable()) {
          g_thread.join();
        }
        g_provider.reset();
        g_owner_generation = 0;
        BOOST_LOG(::info) << "host_stats: stopped";
      }

    private:
      bool _owns_sampler;
      std::uint64_t _generation;
    };
  }  // namespace

  std::unique_ptr<platf::deinit_t>
    start() {
    std::lock_guard<std::mutex> state_lk(g_state_mutex);
    if (g_provider) {
      BOOST_LOG(warning) << "host_stats: start() called while already running";
      return std::make_unique<deinit_t>(false, 0);
    }
    g_stop.store(false, std::memory_order_release);
    g_provider = platf::create_host_stats_provider();
    if (!g_provider) {
      BOOST_LOG(warning) << "host_stats: no provider for this platform; sampler disabled";
      return {};
    }
    try {
      g_info = g_provider->info();
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "host_stats: provider info() failed: " << e.what();
    }
    try {
      auto first = g_provider->sample();
      auto lg = g_latest.lock();
      g_latest.raw = first;
    } catch (...) {
      BOOST_LOG(warning) << "host_stats: initial sample failed";
    }
    g_thread = std::thread(sampler_loop);
    g_owner_generation = ++g_next_generation;
    BOOST_LOG(::info) << "host_stats: started (cpu='" << g_info.cpu_model
                    << "', gpu='" << g_info.gpu_model << "')";
    return std::make_unique<deinit_t>(true, g_owner_generation);
  }

  platf::host_stats_t
    latest() {
    auto lg = g_latest.lock();
    return g_latest.raw;
  }

  const platf::host_info_t &
    info() {
    return g_info;
  }

}  // namespace host_stats
