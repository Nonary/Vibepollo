// Compare the host's keyboard-page usages with Windows' own HID translator.
// This is a pure translation test: it does not install a device or send input.
#include <windows.h>
#include <hidusage.h>
#include <hidpi.h>

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "src/platform/windows/vhf_keyboard_mouse_policy.h"

using namespace platf::vhf_keyboard_mouse;

static BOOLEAN collect(PVOID context, PCHAR codes, ULONG length) {
  auto &output = *static_cast<std::vector<unsigned char> *>(context);
  for (ULONG i = 0; i < length; ++i) output.push_back(static_cast<unsigned char>(codes[i]));
  return TRUE;
}

static bool check_translation(unsigned int scan, USAGE usage) {
  if (!usage) {
    std::printf("Missing usage for scan %04x\n", scan);
    return false;
  }
  HIDP_KEYBOARD_MODIFIER_STATE state {};
  std::vector<unsigned char> expected_make;
  std::vector<unsigned char> expected_break;
  if (scan == 0xe037) {
    // The Windows HID parser synthesizes shift around Print Screen.
    expected_make = {0xe0, 0x2a, 0xe0, 0x37};
    expected_break = {0xe0, 0xb7, 0xe0, 0xaa};
  } else {
    if (scan & 0xff00) {
      expected_make.push_back(0xe0);
      expected_break.push_back(0xe0);
    }
    expected_make.push_back(static_cast<unsigned char>(scan));
    expected_break.push_back(static_cast<unsigned char>(scan | 0x80));
  }
  bool success = true;
  for (const auto &test : {std::pair {HidP_Keyboard_Make, &expected_make},
                           std::pair {HidP_Keyboard_Break, &expected_break}}) {
    std::vector<unsigned char> actual;
    const auto status = HidP_TranslateUsagesToI8042ScanCodes(&usage, 1, test.first, &state, collect, &actual);
    if (status != HIDP_STATUS_SUCCESS || actual != *test.second) {
      std::printf("Windows HID translation failed: scan %04x, usage %04x, direction %d, status %08lx; got",
                  scan, usage, static_cast<int>(test.first), static_cast<unsigned long>(status));
      for (const auto byte : actual) std::printf(" %02x", byte);
      std::printf("\n");
      success = false;
    }
  }
  return success;
}

int main() {
  unsigned checked = 0, failures = 0;
  for (const auto prefix : {0u, 0xe000u}) {
    for (unsigned code = 0; code < 128; ++code) {
      const auto scan = prefix | code;
      const auto usage = scan_usage(static_cast<std::uint16_t>(scan));
      if (!usage) continue;
      ++checked;
      if (!check_translation(scan, usage)) ++failures;
    }
  }
  // These explicit VK-to-physical-key checks cover the US key layout and
  // distinguish keypad, extended, modifier, and OEM keys independently of
  // the layout installed on the machine running the test.
  constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 30> keys {{
    {'A', 0x1e}, {'Z', 0x2c}, {'Q', 0x10}, {'1', 0x02}, {'0', 0x0b},
    {0x08, 0x0e}, {0x09, 0x0f}, {0x0d, 0x1c}, {0x010d, 0xe01c},
    {0xa0, 0x2a}, {0xa1, 0x36}, {0xa2, 0x1d}, {0xa3, 0xe01d},
    {0xa4, 0x38}, {0xa5, 0xe038}, {0x5b, 0xe05b}, {0x5c, 0xe05c},
    {0x25, 0xe04b}, {0x26, 0xe048}, {0x2c, 0xe037}, {0x2d, 0xe052},
    {0x60, 0x52}, {0x61, 0x4f}, {0x6f, 0xe035}, {0x70, 0x3b},
    {0x7b, 0x58}, {0x90, 0x45}, {0xba, 0x27}, {0xdc, 0x2b}, {0xe2, 0x56},
  }};
  for (const auto [key, scan] : keys) {
    ++checked;
    if (!check_translation(scan, key_usage(key))) {
      std::printf("Virtual key %04x\n", key);
      ++failures;
    }
  }
  if (checked < 134) {
    std::printf("Unexpectedly few keyboard mappings checked: %u\n", checked);
    ++failures;
  }
  std::printf("Checked %u keyboard mappings against Windows HID, %u failures\n", checked, failures);
  return failures ? 1 : 0;
}
