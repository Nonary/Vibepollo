#include "../tests_common.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <src/hardware_metrics.h>
#include <src/hardware_metrics_config.h>
#include <src/hardware_metrics_gpu.h>
#include <src/hardware_metrics_session.h>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace {
  struct state_t {
    std::atomic<int> calls {0};
    std::atomic<int> factories {0};
    bool block = false;
    bool release = false;
    bool entered = false;
    bool fail = false;
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void()> on_sample;
  };

  class provider_t: public platf::host_stats_provider_t {
  public:
    explicit provider_t(state_t &state):
        state(state) {}

    platf::host_stats_t sample() override {
      ++state.calls;
      if (state.on_sample) {
        state.on_sample();
      }
      std::unique_lock lock(state.mutex);
      state.entered = true;
      state.cv.notify_all();
      state.cv.wait(lock, [&] {
        return !state.block || state.release;
      });
      if (state.fail) {
        throw std::runtime_error("read failed");
      }
      platf::host_stats_t stats;
      stats.cpu_percent = 37;
      return stats;
    }

    platf::host_info_t info() override {
      return {};
    }

    state_t &state;
  };

  auto factory(state_t &state) {
    return [&state] {
      ++state.factories;
      return std::make_unique<provider_t>(state);
    };
  }

  auto allowed = [] {
    return hardware_metrics::access_t {true, true, 500};
  };
  auto video = [] {
    return true;
  };
}  // namespace

TEST(HardwareMetrics, DenialsAndDiscoveryDoNotStartCollector) {
  state_t state;
  hardware_metrics::service_t service(factory(state));
  EXPECT_FALSE(hardware_metrics::advertised({false, true}));
  EXPECT_FALSE(hardware_metrics::advertised({true, false}));
  EXPECT_TRUE(hardware_metrics::advertised({true, true}));
  EXPECT_EQ(hardware_metrics::query(service, [] {
              return hardware_metrics::access_t {false, true};
            },
                                    video)
              .status,
            403);
  EXPECT_EQ(hardware_metrics::query(service, [] {
              return hardware_metrics::access_t {true, false};
            },
                                    video)
              .status,
            403);
  EXPECT_EQ(hardware_metrics::query(service, allowed, [] {
              return false;
            })
              .status,
            409);
  EXPECT_EQ(state.factories, 0);
  EXPECT_EQ(state.calls, 0);
}

TEST(HardwareMetrics, RequestsShareCompletedTimestampAndNeverPollInBackground) {
  state_t state;
  hardware_metrics::service_t service(factory(state));
  const auto first = hardware_metrics::query(service, allowed, video);
  ASSERT_EQ(first.status, 200);
  EXPECT_GT(first.body["sampled_at_unix_ms"].get<std::uint64_t>(), 0u);
  const auto second = hardware_metrics::query(service, allowed, video);
  EXPECT_EQ(first.body, second.body);
  EXPECT_EQ(state.calls, 1);
  std::this_thread::sleep_for(550ms);
  EXPECT_EQ(state.calls, 1);
  const auto third = hardware_metrics::query(service, allowed, video);
  EXPECT_EQ(third.status, 200);
  EXPECT_GT(third.body["sampled_at_unix_ms"], first.body["sampled_at_unix_ms"]);
  EXPECT_EQ(state.calls, 2);
}

TEST(HardwareMetrics, UnsupportedAndInvalidMetricsAreOmittedButZeroSurvives) {
  hardware_metrics::sample_t sample;
  sample.sampled_at_unix_ms = 123;
  sample.stats.cpu_percent = 0;
  // Legacy flat readings have no GPU identity and cannot become a GPU entry.
  sample.stats.gpu_percent = 90;
  auto result = hardware_metrics::serialize(sample, 1000);
  EXPECT_EQ(result["cpu"]["cpu_usage_percent"], 0);
  EXPECT_TRUE(result["gpus"].empty());
  platf::client_gpu_stats_t gpu;
  gpu.id = 7;
  gpu.name = "Identified device";
  gpu.usage_percent = std::numeric_limits<float>::infinity();
  gpu.encoder_percent = 0;
  gpu.temperature_c = std::numeric_limits<float>::quiet_NaN();
  gpu.vram_used_bytes = 0;
  sample.stats.client_gpus.push_back(gpu);
  result = hardware_metrics::serialize(sample, 1000);
  const auto metrics = result["gpus"][0]["metrics"];
  EXPECT_FALSE(metrics.contains("gpu_usage_percent"));
  EXPECT_FALSE(metrics.contains("gpu_temperature_c"));
  EXPECT_FALSE(metrics.contains("gpu_vram_usage_percent"));
  EXPECT_EQ(metrics["gpu_encoder_percent"], 0);
  EXPECT_EQ(metrics["gpu_vram_used_mb"], 0);
  EXPECT_EQ(result["gpus"][0]["id"], 7);
  sample.stats.client_gpus[0].vram_total_bytes = 4 * 1024 * 1024;
  result = hardware_metrics::serialize(sample, 1000);
  EXPECT_EQ(result["gpus"][0]["metrics"]["gpu_vram_used_mb"], 0);
  EXPECT_EQ(result["gpus"][0]["metrics"]["gpu_vram_usage_percent"], 0);
  sample.stats.cpu_percent = -1;
  sample.stats.client_gpus[0].vram_used_bytes = 1024 * 1024;
  sample.stats.client_gpus[0].vram_total_bytes = 4 * 1024 * 1024;
  result = hardware_metrics::serialize(sample, 1000);
  EXPECT_TRUE(result["cpu"].empty());
  EXPECT_EQ(result["gpus"][0]["metrics"]["gpu_vram_used_mb"], 1);
  EXPECT_EQ(result["gpus"][0]["metrics"]["gpu_vram_usage_percent"], 25);
  sample.stats.client_gpus[0].vram_used_bytes = 8 * 1024 * 1024;
  result = hardware_metrics::serialize(sample, 1000);
  EXPECT_FALSE(result["gpus"][0]["metrics"].contains("gpu_vram_used_mb"));
  EXPECT_FALSE(result["gpus"][0]["metrics"].contains("gpu_vram_usage_percent"));
}

TEST(HardwareMetrics, MissingOrFailedProviderDoesNotReturnStaleSample) {
  hardware_metrics::service_t missing([] {
    return std::unique_ptr<platf::host_stats_provider_t> {};
  });
  EXPECT_EQ(hardware_metrics::query(missing, allowed, video).status, 503);
  state_t state;
  hardware_metrics::service_t service(factory(state));
  EXPECT_EQ(hardware_metrics::query(service, allowed, video).status, 200);
  std::this_thread::sleep_for(510ms);
  state.fail = true;
  EXPECT_EQ(hardware_metrics::query(service, allowed, video).status, 503);
  EXPECT_EQ(hardware_metrics::query(service, allowed, video).status, 503);
  EXPECT_EQ(state.calls, 2);
}

TEST(HardwareMetrics, ConcurrentConsumersCoalesceAndTimestampAfterSampleCompletion) {
  state_t state;
  state.block = true;
  hardware_metrics::service_t service(factory(state));
  auto first = std::async(std::launch::async, [&] {
    return service.request(1000ms);
  });
  {
    std::unique_lock lock(state.mutex);
    if (!state.cv.wait_for(lock, 1s, [&] {
          return state.entered;
        })) {
      state.release = true;
      state.cv.notify_all();
      FAIL() << "provider did not start";
    }
  }
  auto second = std::async(std::launch::async, [&] {
    return service.request(1000ms);
  });
  const auto before_completion = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  {
    const std::lock_guard lock(state.mutex);
    state.release = true;
    state.cv.notify_all();
  }
  const auto a = first.get();
  const auto b = second.get();
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_GE(a->sampled_at_unix_ms, static_cast<std::uint64_t>(before_completion));
  EXPECT_EQ(a->sampled_at_unix_ms, b->sampled_at_unix_ms);
  EXPECT_EQ(state.calls, 1);
}

TEST(HardwareMetrics, RevocationDisableAndDisconnectDuringCollectionAreRechecked) {
  for (int scenario = 0; scenario < 3; ++scenario) {
    state_t state;
    hardware_metrics::access_t access {true, true};
    bool stream_running = true;
    state.on_sample = [&] {
      if (scenario == 0) {
        access.authenticated_viewer = false;
      }
      if (scenario == 1) {
        access.enabled = false;
      }
      if (scenario == 2) {
        stream_running = false;
      }
    };
    hardware_metrics::service_t service(factory(state));
    const auto result = hardware_metrics::query(service, [&] {
      return access;
    },
                                                [&] {
                                                  return stream_running;
                                                });
    EXPECT_EQ(result.status, scenario == 2 ? 409 : 403);
  }
}

TEST(HardwareMetrics, SlowProviderTimesOutWithoutSpawningAnotherCollector) {
  state_t state;
  state.block = true;
  hardware_metrics::service_t service(factory(state));
  const auto begin = std::chrono::steady_clock::now();
  auto request = std::async(std::launch::async, [&] {
    return service.request(500ms);
  });
  const auto result = request.get();
  {
    const std::lock_guard lock(state.mutex);
    state.release = true;
    state.cv.notify_all();
  }
  EXPECT_FALSE(result);
  EXPECT_GE(std::chrono::steady_clock::now() - begin, 1900ms);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 5s);
  EXPECT_EQ(state.calls, 1);
}

TEST(HardwareMetrics, CompletedSettingsAreRecheckedAfterProviderWait) {
  for (int scenario = 0; scenario != 3; ++scenario) {
    config::hardware_metrics_config_t settings;
    settings.publish({true, 10000});
    state_t state;
    state.block = true;
    hardware_metrics::service_t service(factory(state));
    auto request = std::async(std::launch::async, [&] {
      return hardware_metrics::query(service, [&] {
        const auto current = settings.snapshot();
        return hardware_metrics::access_t {current.enabled, true, current.interval_ms};
      },
                                     video);
    });
    {
      std::unique_lock lock(state.mutex);
      if (!state.cv.wait_for(lock, 1s, [&] {
            return state.entered;
          })) {
        state.release = true;
        state.cv.notify_all();
        FAIL() << "provider did not start";
      }
    }
    if (scenario != 2) {
      // A successful apply publishes disable/interval together. A failed apply
      // (scenario 2) publishes nothing and retains the completed generation.
      settings.publish({scenario == 1, 500});
    }
    {
      const std::lock_guard lock(state.mutex);
      state.release = true;
      state.cv.notify_all();
    }
    const auto result = request.get();
    EXPECT_EQ(result.status, scenario == 0 ? 403 : 200);
    if (result.status == 200) {
      EXPECT_EQ(result.body["interval_ms"], scenario == 1 ? 500 : 10000);
    }
    EXPECT_EQ(state.calls, 1);
  }
}

TEST(HardwareMetrics, SessionObservationUsesExactPairedOwnerRunningVideoAndNeverCleans) {
  struct session_t {
    bool running;
    bool input;
  };

  auto session = std::make_shared<session_t>(session_t {true, false});
  std::vector sessions {session};
  std::unordered_map<const session_t *, std::string> owners {{session.get(), "paired-a"}};
  auto matches = [&](std::string_view uuid) {
    return hardware_metrics::has_running_video_for_client(sessions, owners, uuid, [](const session_t &s) {
      return hardware_metrics::running_video(s.running, s.input);
    });
  };
  EXPECT_TRUE(matches("paired-a"));  // Normal or Remote Monitor video.
  EXPECT_FALSE(matches("paired-b"));
  EXPECT_FALSE(matches(""));
  session->input = true;
  EXPECT_FALSE(matches("paired-a"));
  session->input = false;
  session->running = false;  // Pending, STOPPING or STOPPED all fail RUNNING.
  EXPECT_FALSE(matches("paired-a"));
  EXPECT_EQ(sessions.size(), 1u);
  EXPECT_EQ(owners.size(), 1u);
  EXPECT_FALSE(session->running);
  owners.clear();
  session->running = true;
  EXPECT_FALSE(matches("paired-a"));
}

TEST(HardwareMetrics, QualifiedCountersNeverAttributeOtherAdaptersOrMissingValues) {
  hardware_metrics::qualified_counter_t counters {L"luid_0x00000000_0x00000001"};
  counters.add(L"pid_7_luid_0x00000000_0x00000002_phys_0_eng_0_engtype_3D", 80);
  EXPECT_EQ(counters.value(100), -1);
  counters.add(L"pid_7_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D", 0);
  EXPECT_EQ(counters.value(100), 0);
  counters.add(L"pid_8_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D", 30);
  EXPECT_EQ(counters.value(100), 30);
  counters.add(L"pid_9_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D", std::numeric_limits<double>::quiet_NaN());
  counters.add(L"pid_9_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D", -1);
  EXPECT_EQ(counters.value(100), 30);
  counters.add(L"pid_9_luid_0x00000000_0x00000001_phys_0_eng_0_engtype_3D", 80);
  EXPECT_EQ(counters.value(100), 100);
  hardware_metrics::qualified_counter_t unidentified {L""};
  unidentified.add(L"anything", 50);
  EXPECT_EQ(unidentified.value(100), -1);
}
