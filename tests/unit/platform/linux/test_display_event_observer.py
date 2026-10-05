"""Execute the Linux observer callbacks and production subscription parser.

No live compositor, session broker, or display is changed by these fixtures.
"""
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
generated = pathlib.Path(sys.argv[2]).resolve()
c_compiler, compiler = sys.argv[3:5]


def function(source, signature):
    match = re.search(r"\s*".join(map(re.escape, signature.split())), source)
    assert match, signature
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


c_program = r'''
#define main observer_program_main
#include "packaging/linux/vibepollo-display-observer.c"
#undef main
#include <assert.h>

int main(void) {
  FILE *capture = tmpfile();
  assert(capture && dup2(fileno(capture), STDOUT_FILENO) >= 0);
  struct observer observer = {.watch = true};
  wl_list_init(&observer.outputs);
  struct observed_output *output = calloc(1, sizeof(*output));
  assert(output);
  output->observer = &observer;
  output->registry_name = 42;
  wl_list_insert(&observer.outputs, &output->link);
  observer.output_count = 1;
  output_geometry(output, NULL, 0, 0, 600, 340, 0, "physical", "monitor", 0);
  output_mode(output, NULL, WL_OUTPUT_MODE_CURRENT, 3840, 2160, 120000);
  output_scale(output, NULL, 1);
  output_name(output, NULL, "HDMI-A-1");
  output_done(output, NULL); // Enumeration is a baseline, not a topology event.
  dpms_supported(output, NULL, 1);
  dpms_mode(output, NULL, 3);
  dpms_done(output, NULL);
  observer.ready = true;
  puts("READY");
  output_mode(output, NULL, WL_OUTPUT_MODE_CURRENT, 3840, 2160, 120000);
  output_mode(output, NULL, 0, 800, 600, 60000); // Non-current modes cannot invent a change.
  output_done(output, NULL);
  output_mode(output, NULL, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
  output_done(output, NULL);
  output_done(output, NULL); // No repeated topology traffic while idle.
  dpms_mode(output, NULL, 0);
  dpms_done(output, NULL);
  output_name(output, NULL, "invalid name");
  assert(!strcmp(output->name, "HDMI-A-1"));
  registry_remove(&observer, NULL, 42);
  assert(observer.output_count == 0 && wl_list_empty(&observer.outputs));
  fflush(stdout);
  rewind(capture);
  char content[1024] = {0};
  assert(fread(content, 1, sizeof(content) - 1, capture) > 0);
  assert(!strcmp(content, "S\tHDMI-A-1\t1\t3\nREADY\nT\tHDMI-A-1\nS\tHDMI-A-1\t1\t0\nT\tHDMI-A-1\n"));
  fclose(capture);
  return 0;
}
'''

power = (root / "src/platform/linux/display_power.cpp").read_text()
subscription = function(power, "void observe_events(")
# Advance the clock deterministically between READY and the next real event.
subscription = subscription.replace("std::chrono::steady_clock", "TestClock")
cpp_program = r'''
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <poll.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include "src/platform/linux/display_power.h"

struct TestClock {
  static inline std::chrono::steady_clock::time_point current {};
  static auto now() { return current; }
};
using gchar = char;
enum GSubprocessFlags { G_SUBPROCESS_FLAGS_STDOUT_PIPE = 1, G_SUBPROCESS_FLAGS_STDERR_SILENCE = 2 };
struct GSubprocess { int reader, writer; };
struct GError {};
static std::string initial;
static int starts, stops;
static GSubprocess *running;
static bool expected_broker;
GSubprocess *g_subprocess_newv(const char *const *args, GSubprocessFlags, GError **) {
  ++starts;
  assert(std::string(args[0]) == (expected_broker ? "/usr/libexec/vibeshine/vibepollo-session-exec" : "/usr/libexec/vibeshine/vibepollo-display-observer"));
  assert(std::string(args[1]) == (expected_broker ? "display-observe-events" : "--watch"));
  assert(!args[2]);
  int fds[2]; assert(pipe(fds) == 0);
  running = new GSubprocess {fds[0], fds[1]};
  assert(write(running->writer, initial.data(), initial.size()) == static_cast<ssize_t>(initial.size()));
  return running;
}
void g_clear_error(GError **) {}
GSubprocess *g_subprocess_get_stdout_pipe(GSubprocess *process) { return process; }
#define G_UNIX_INPUT_STREAM(value) (value)
int g_unix_input_stream_get_fd(GSubprocess *stream) { return stream->reader; }
struct lease_t {
  GSubprocess *process = nullptr;
  ~lease_t() {
    if (process) {
      close(process->reader); close(process->writer); delete process;
      running = nullptr; ++stops;
    }
  }
};
namespace platf::display_power {
''' + subscription + r'''
}
void reset(bool broker, const std::string &frames) {
  expected_broker = broker;
  if (broker) setenv("VIBEPOLLO_MACHINE_HOST", "1", 1);
  else unsetenv("VIBEPOLLO_MACHINE_HOST");
  initial = frames; starts = stops = 0; TestClock::current = {};
}
int main() {
  using namespace platf::display_power;
  for (bool broker : {false, true}) {
    reset(broker, "S\tHDMI-A-1\t1\t3\nREADY\n");
    unsigned callbacks = 0;
    observe_events([&](const observation_t &event) {
      ++callbacks;
      if (callbacks == 1) {
        assert(event.kind == observation_t::kind_e::power && event.power.known && !event.power.on);
      } else if (callbacks == 2) {
        assert(event.kind == observation_t::kind_e::baseline_ready);
        TestClock::current += std::chrono::hours {24};
        const std::string next = "S\tHDMI-A-1\t1\t0\nT\tHDMI-A-1\n";
        assert(write(running->writer, next.data(), next.size()) == static_cast<ssize_t>(next.size()));
      } else if (callbacks == 3) {
        assert(event.kind == observation_t::kind_e::power && event.power.known && event.power.on);
      } else {
        assert(callbacks == 4 && event.kind == observation_t::kind_e::topology && event.output == "HDMI-A-1");
        return false;
      }
      return true;
    }, [] { return true; });
    assert(callbacks == 4 && starts == 1 && stops == 1); // One subscription survives a late event.
  }
  for (const auto &invalid : {"T\tinvalid name\n", "S\tHDMI-A-1\t1\t9\n", "READY\nREADY\n"}) {
    reset(true, invalid);
    unsigned callbacks = 0;
    observe_events([&](const auto &) { ++callbacks; return true; }, [] { return true; });
    assert(callbacks <= 1 && starts == 1 && stops == 1);
  }
  reset(true, "READY\n");
  bool current = true;
  observe_events([&](const auto &) { current = false; return true; }, [&] { return current; });
  assert(starts == 1 && stops == 1); // A retired generation closes its subscription.
}
'''

private_display = (root / "src/platform/linux/private_display.cpp").read_text()
event_callback = function(private_display, "display_power::observe_events([&](const display_power::observation_t &event)")
event_callback = event_callback.replace("display_power::observe_events(", "auto on_event = ", 1) + ";"
physical_signature = function(private_display, "json physical_topology_signature(const json &configuration)")
pipeline_program = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/platform/linux/display_power.h"
#include "src/platform/linux/private_display_recovery_policy.h"
using json = nlohmann::json;
namespace recovery_policy = platf::linux_private_display::recovery_policy;
namespace display_power = platf::display_power;
namespace mode_policy { bool managed_connector_name(const std::string &name) { return name.starts_with("Virtual-"); } }
namespace util {
  struct guard { std::function<void()> work; ~guard() { work(); } };
  template<class Work> guard fail_guard(Work work) { return {work}; }
}
enum class recovery_check_e { current, busy, stale };
enum class recovery_enqueue_e { queued, busy, stale };
struct state_t { std::atomic<std::uint64_t> cleanup_generation {9}; };
struct failed_restore_incident_t { recovery_policy::incident_t policy; };
struct restore_context_t { std::chrono::steady_clock::time_point deadline; std::function<bool()> current; };
static const restore_context_t *restore_context;
static constexpr auto helper_reply_timeout = std::chrono::seconds {4};
static constexpr auto output_verification_timeout = std::chrono::seconds {2};
namespace fake {
  recovery_check_e state;
  json topology;
  unsigned queued, reads;
  bool dpms_required;
}
bool process_shutdown_preserve_requested() { return false; }
recovery_check_e recovery_incident_current(state_t &, const failed_restore_incident_t &) { return fake::state; }
std::optional<json> query_configuration() { ++fake::reads; return fake::topology; }
recovery_policy::connector_state_t recovery_connector_state(
  const std::string &name, const std::map<std::string, display_power::dpms_state_t> &samples) {
  if (mode_policy::managed_connector_name(name)) return {};
  recovery_policy::connector_state_t result {.known = true, .connected = true};
  if (const auto sample = samples.find(name); sample != samples.end()) {
    result.dpms_known = sample->second.known;
    result.dpms_on = sample->second.on;
  }
  return result;
}
recovery_enqueue_e enqueue_failed_restore_recovery(state_t &, failed_restore_incident_t &, bool dpms_required) {
  assert(restore_context && restore_context->current() && fake::state == recovery_check_e::current);
  ++fake::queued; fake::dpms_required = dpms_required;
  return recovery_enqueue_e::queued;
}
''' + physical_signature + r'''
using Event = display_power::observation_t;
Event power(bool known, bool on) { return {Event::kind_e::power, "HDMI-A-1", {known, on}}; }
Event ready() { return {Event::kind_e::baseline_ready, {}, {}}; }
Event topology(const std::string &name = "HDMI-A-1") { return {Event::kind_e::topology, name, {}}; }
void reset() {
  fake::state = recovery_check_e::current; fake::queued = fake::reads = 0; fake::dpms_required = false;
  fake::topology = {{"outputs", json::array({
    {{"name", "HDMI-A-1"}, {"connected", true}, {"enabled", true}, {"mode", "A"}},
    {{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}, {"mode", "A"}}
  })}};
}
void run(const std::vector<Event> &script, const std::function<void(unsigned)> &before = {}) {
  state_t manager;
  failed_restore_incident_t incident; incident.policy.cleanup_generation = 9;
  std::stop_token stop;
  recovery_policy::wake_event_tracker_t events;
  recovery_policy::topology_change_tracker_t<json> physical_topology_events;
  std::map<std::string, display_power::dpms_state_t> sampled_dpms;
  bool ready = false, dpms_wake_observed = false;
''' + event_callback + r'''
  unsigned index = 0;
  for (const auto &event : script) {
    if (before) before(index);
    ++index;
    if (!on_event(event)) break;
  }
  assert(!restore_context);
}
int main() {
  reset(); run({power(true, true), ready(), power(true, true)});
  assert(fake::queued == 0 && fake::reads == 1); // Initial On and repeated On are not wake cues.
  reset(); run({power(false, false), ready(), power(true, true)});
  assert(fake::queued == 0 && fake::reads == 1); // Unknown-to-On only establishes a baseline.
  reset(); run({power(true, false), ready(), power(true, true)});
  assert(fake::queued == 1 && fake::dpms_required);
  reset(); run({ready(), topology("Virtual-1")}, [](unsigned index) {
    if (index == 1) fake::topology["outputs"][1]["mode"] = "B";
  });
  assert(fake::queued == 0 && fake::reads == 1); // Own virtual-output events cannot trigger recovery.
  reset(); run({ready(), topology()}, [](unsigned index) {
    if (index == 1) fake::topology["outputs"][0]["mode"] = "B";
  });
  assert(fake::queued == 1 && !fake::dpms_required); // Verified physical topology change can proceed without DPMS.
  reset(); run({power(true, false), ready(), topology()}, [](unsigned index) {
    if (index == 2) fake::topology["outputs"][0]["mode"] = "B";
  });
  assert(fake::queued == 0); // Known Off always vetoes the event handoff.
  reset(); run({power(true, false), ready(), power(true, true), topology()}, [](unsigned index) {
    if (index == 2) fake::state = recovery_check_e::busy;
    if (index == 3) {
      assert(fake::queued == 0);
      fake::state = recovery_check_e::current;
      fake::topology["outputs"][0]["mode"] = "B";
    }
  });
  assert(fake::queued == 1 && fake::dpms_required); // Ownership defers the hint until a later event.
  reset(); run({power(true, false), ready(), power(true, true)}, [](unsigned index) {
    if (index == 2) fake::state = recovery_check_e::stale;
  });
  assert(fake::queued == 0 && fake::reads == 1);
}
'''

with tempfile.TemporaryDirectory(prefix="linux-display-events-") as temporary:
    temporary = pathlib.Path(temporary)
    c_source, c_binary = temporary / "events.c", temporary / "events"
    c_source.write_text(c_program)
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True))
    subprocess.run([c_compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                    "-I", str(generated), str(c_source), str(generated / "dpms.c"), *flags,
                    "-o", str(c_binary)], check=True)
    subprocess.run([str(c_binary)], check=True)
    cpp_source, cpp_binary = temporary / "subscription.cpp", temporary / "subscription"
    cpp_source.write_text(cpp_program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                    str(cpp_source), "-o", str(cpp_binary)], check=True)
    subprocess.run([str(cpp_binary)], check=True, timeout=10)
    pipeline_source, pipeline_binary = temporary / "pipeline.cpp", temporary / "pipeline"
    pipeline_source.write_text(pipeline_program)
    json_include = generated.parent / "_deps/json-src/include"
    json_flags = ["-I", str(json_include)] if json_include.is_dir() else []
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                    *json_flags, str(pipeline_source), "-o", str(pipeline_binary)], check=True)
    subprocess.run([str(pipeline_binary)], check=True, timeout=10)
