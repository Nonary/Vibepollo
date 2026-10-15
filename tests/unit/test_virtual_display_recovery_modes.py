"""Compile the production refresh-support helper against fake display APIs.

The fake Windows enumerator can be unavailable while the driver's securely
queried temporary-display descriptor is still present. This fixture exercises
the extracted production helper body and the production reclaimed-display
policy; it deliberately does not reimplement either decision.
"""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/platform/windows/virtual_display_sunshine.cpp").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("(", start)
    parens, closing = 0, opening
    while True:
        if text[closing] == "(":
            parens += 1
        elif text[closing] == ")":
            parens -= 1
            if parens == 0:
                break
        closing += 1
    brace = text.index("{", closing)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


production_helper = extract_function(
    SOURCE,
    "bool existing_virtual_display_supports_refresh(",
)
helper_takes_client = "ControlClient" in production_helper.split("{", 1)[0]


PROGRAM = r'''
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>
#include "src/platform/windows/virtual_display_policy.h"

namespace display_device {
  enum class DeviceEnumerationDetail { Minimal, Full };
  struct Rational {
    std::uint32_t m_numerator = 0;
    std::uint32_t m_denominator = 1;
  };
  struct EnumeratedDevice {
    bool virtual_display = false;
    std::uint64_t display_id = 0;
    std::vector<Rational> m_supported_refresh_rates;
  };
  using EnumeratedDeviceList = std::vector<EnumeratedDevice>;
}

namespace sunshine_driver {
  inline constexpr std::uint32_t kMaxDisplayStateEntries = 16;
  inline constexpr std::uint32_t kDisplayStateKindTemporary = 2;
  inline constexpr std::uint32_t kDisplayStateKindPermanent = 1;
  struct DisplayStateEntry {
    std::uint32_t kind = 0;
    std::uint64_t display_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_rate_millihz = 0;
  };
  struct QueryDisplayStateResult {
    std::uint32_t entry_count = 0;
    DisplayStateEntry entries[16] {};
  };
  template<class T> struct ControlResult {
    bool success = true;
    T value {};
    int status = 0;
    unsigned long native_error = 0;
    bool ok() const { return success; }
  };
  struct ControlClient {
    bool query_succeeds = true;
    QueryDisplayStateResult state {};
    ControlResult<QueryDisplayStateResult> query_display_state() {
      return {query_succeeds, state};
    }
  };
}

void log_control_failure(const char *, int, unsigned long) {}

namespace platf::display_helper {
  struct Coordinator {
    std::optional<display_device::EnumeratedDeviceList> devices;
    static Coordinator &instance() { static Coordinator value; return value; }
    std::optional<display_device::EnumeratedDeviceList> enumerate_devices(
      display_device::DeviceEnumerationDetail
    ) { return devices; }
  };
}

bool is_virtual_display_device(const display_device::EnumeratedDevice &device) {
  return device.virtual_display;
}
bool matches_virtual_display_id_edid(
  const display_device::EnumeratedDevice &device,
  std::uint64_t display_id
) {
  return device.display_id == display_id;
}

namespace VDISPLAY_SUNSHINE {
'''

PROGRAM += production_helper
PROGRAM += r'''

  bool invoke(
    sunshine_driver::ControlClient &client,
    std::uint64_t display_id,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh
  ) {
#if HELPER_TAKES_CLIENT
    return existing_virtual_display_supports_refresh(client, display_id, width, height, refresh);
#else
    (void) client;
    (void) width;
    (void) height;
    return existing_virtual_display_supports_refresh(display_id, refresh);
#endif
  }
}

int main() {
  constexpr std::uint64_t id = 0x617;
  constexpr std::uint32_t width = 1920;
  constexpr std::uint32_t height = 1280;
  constexpr std::uint32_t refresh = 1'000'000;
  int failures = 0;
  const auto expect = [&](bool actual, bool expected, const char *name) {
    if (actual != expected) {
      std::cerr << "FAIL: " << name << " expected=" << expected << " actual=" << actual << '\n';
      ++failures;
    }
  };
  auto &coordinator = platf::display_helper::Coordinator::instance();
  sunshine_driver::ControlClient client;
  client.state.entry_count = 1;
  client.state.entries[0] = {
    sunshine_driver::kDisplayStateKindTemporary, id, width, height, refresh
  };

  // The temporary driver's preferred descriptor is positive evidence when Full
  // Windows enumeration has no result or has no usable inactive-target modes.
  coordinator.devices = std::nullopt;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), true,
         "exact driver descriptor survives unavailable Full enumeration");
  coordinator.devices = display_device::EnumeratedDeviceList {
    display_device::EnumeratedDevice {.virtual_display = true, .display_id = id}
  };
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), true,
         "exact driver descriptor survives empty Full mode list");

  coordinator.devices = std::nullopt;
#if HELPER_TAKES_CLIENT
  client.state.entries[0].display_id = id + 1;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "wrong driver display ID is rejected");
  client.state.entries[0].display_id = id;
  client.state.entries[0].kind = sunshine_driver::kDisplayStateKindPermanent;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "permanent driver display is rejected");
  client.state.entries[0].kind = sunshine_driver::kDisplayStateKindTemporary;
  client.state.entries[0].width = width + 1;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "wrong driver descriptor width is rejected");
  client.state.entries[0].width = width;
  client.state.entries[0].height = height + 1;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "wrong driver descriptor height is rejected");
  client.state.entries[0].height = height;
  client.state.entries[0].refresh_rate_millihz = refresh - 1'000;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "wrong driver descriptor refresh is rejected");
  client.state.entries[0].refresh_rate_millihz = refresh;
  client.query_succeeds = false;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "failed driver state query is rejected");
  client.query_succeeds = true;
  client.state.entry_count = 0;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "omitted driver state entry does not certify a mode");
  client.state.entries[0].display_id = id + 1;
  client.state.entry_count = sunshine_driver::kMaxDisplayStateEntries + 1;
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "truncated response without a match is bounded and rejected");
  client.state.entry_count = 1;
  client.state.entries[0].display_id = id;
#else
  std::cout << "BASELINE: descriptor-field rejection cases await the production ControlClient overload.\n";
#endif

  // A nonempty helper result is a definitive mode list: a driver descriptor
  // match must not override its explicit exclusion.
  coordinator.devices = display_device::EnumeratedDeviceList {
    display_device::EnumeratedDevice {
      .virtual_display = true,
      .display_id = id,
      .m_supported_refresh_rates = {{60, 1}}
    }
  };
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), false,
         "nonempty helper list excluding requested refresh stays false");

  coordinator.devices = display_device::EnumeratedDeviceList {
    display_device::EnumeratedDevice {
      .virtual_display = true,
      .display_id = id,
      .m_supported_refresh_rates = {{1'000, 1}}
    }
  };
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh), true,
         "positive helper rate remains supported");

  coordinator.devices = display_device::EnumeratedDeviceList {
    display_device::EnumeratedDevice {.virtual_display = true, .display_id = id}
  };
  expect(VDISPLAY_SUNSHINE::invoke(client, id, width, height, 2'000'000), false,
         "no inferred 2x or 4x driver descriptor mode");

  const auto supported = VDISPLAY_SUNSHINE::invoke(client, id, width, height, refresh);
  const auto matching_provenance = VDISPLAY::policy::reclaimed_display_plan_for_session(
    true, true, supported, true
  );
  const auto stale_provenance = VDISPLAY::policy::reclaimed_display_plan_for_session(
    true, true, supported, false
  );
  expect(matching_provenance.action == VDISPLAY::policy::reclaimed_display_action::reuse,
         true, "descriptor-supported inactive plan reuses with matching provenance");
  expect(stale_provenance.action == VDISPLAY::policy::reclaimed_display_action::recreate,
         true, "descriptor-supported inactive plan recreates with mismatched provenance");

  std::cout << "Recovery mode fixture failures=" << failures << '\n';

  return failures == 0 ? 0 : 1;
}
'''


with tempfile.TemporaryDirectory(prefix="virtual-display-recovery-modes-") as temporary:
    directory = pathlib.Path(temporary)
    cpp = directory / "test.cpp"
    binary = directory / "test-recovery-modes"
    cpp.write_text(PROGRAM)
    subprocess.run(
        [
            "g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-Wno-missing-field-initializers",
            f"-DHELPER_TAKES_CLIENT={int(helper_takes_client)}",
            "-I", str(ROOT), str(cpp), "-o", str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
