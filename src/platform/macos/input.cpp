/**
 * @file src/platform/macos/input.cpp
 * @brief Definitions for macOS input handling.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <thread>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <mach/mach.h>

// local includes
#include "src/config.h"
#include "src/display_device.h"
#include "src/input.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/virtual_display.h"
#include "src/utility.h"

/**
 * @brief Delay for a double click, in milliseconds.
 * @todo Make this configurable.
 */
constexpr std::chrono::milliseconds MULTICLICK_DELAY_MS(500);

namespace platf {
  using namespace std::literals;

  constexpr int WHEEL_DELTA = 120;
  constexpr double DEFAULT_SCROLLWHEEL_SCALING = 0.3125;
  constexpr int DEFAULT_SCROLL_LINES_PER_DETENT = 5;

  struct macos_input_t {
  public:
    CGDirectDisplayID configured_display {};  ///< Chosen at startup from output_name.
    CGDirectDisplayID display {};  ///< Display input currently targets; see current_display().
    CGFloat displayScaling {};
    CGEventSourceRef source {};

    // keyboard related stuff
    CGEventSourceRef keyboard_source {};
    CGEventFlags kb_flags {};

    // mouse related stuff
    CGEventRef mouse_event {};  // mouse event source
    double scrollwheel_scaling {DEFAULT_SCROLLWHEEL_SCALING};
    int scroll_lines_per_detent {DEFAULT_SCROLL_LINES_PER_DETENT};
    bool mouse_down[3] {};  // mouse button status
    std::chrono::steady_clock::steady_clock::time_point last_mouse_event[3][2];  // timestamp of last mouse events
  };

  // Input coordinates are based on the display's points, not its pixels (Retina is 2 pixels per point).
  CGFloat display_scaling(const CGDirectDisplayID display) {
    const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) {
      return 1;
    }
    const auto pixel_width = CGDisplayModeGetPixelWidth(mode);
    CGDisplayModeRelease(mode);
    return pixel_width ? ((CGFloat) CGDisplayPixelsWide(display)) / ((CGFloat) pixel_width) : 1;
  }

  // The streamed display can change while input is running (the per-client virtual display comes
  // and goes with streams), so resolve it per event and refresh the scaling when it changes.
  CGDirectDisplayID current_display(macos_input_t *macos_input) {
    const CGDirectDisplayID display = macos_virtual_display::active_display_id().value_or(macos_input->configured_display);
    if (display != macos_input->display) {
      macos_input->display = display;
      macos_input->displayScaling = display_scaling(display);
    }
    return display;
  }

  // A Remote Monitor stream's pointer belongs to its own display, which display() records by the
  // stream's touch port offset. Every other stream's goes to the streamed display.
  CGDirectDisplayID pointer_display(macos_input_t *macos_input, const touch_port_t &touch_port) {
    static std::atomic<CGDirectDisplayID> last_remote {kCGNullDirectDisplay};
    if (const auto remote = macos_virtual_display::remote_display_captured_at(touch_port.offset_x, touch_port.offset_y)) {
      if (last_remote.exchange(*remote) != *remote) {
        BOOST_LOG(info) << "Remote Monitor pointer: moving on display "sv << *remote;
      }
      return *remote;
    }
    return current_display(macos_input);
  }

  // A struct to hold a Windows keycode to Mac virtual keycode mapping.
  struct KeyCodeMap {
    int win_keycode;
    int mac_keycode;
  };

  // Customized less operator for using std::lower_bound() on a KeyCodeMap array.
  bool operator<(const KeyCodeMap &a, const KeyCodeMap &b) {
    return a.win_keycode < b.win_keycode;
  }

  // clang-format off
const KeyCodeMap kKeyCodesMap[] = {
  { 0x08 /* VKEY_BACK */,                      kVK_Delete              },
  { 0x09 /* VKEY_TAB */,                       kVK_Tab                 },
  { 0x0A /* VKEY_BACKTAB */,                   0x21E4                  },
  { 0x0C /* VKEY_CLEAR */,                     kVK_ANSI_KeypadClear    },
  { 0x0D /* VKEY_RETURN */,                    kVK_Return              },
  { 0x10 /* VKEY_SHIFT */,                     kVK_Shift               },
  { 0x11 /* VKEY_CONTROL */,                   kVK_Control             },
  { 0x12 /* VKEY_MENU */,                      kVK_Option              },
  { 0x13 /* VKEY_PAUSE */,                     -1                      },
  { 0x14 /* VKEY_CAPITAL */,                   kVK_CapsLock            },
  { 0x15 /* VKEY_KANA */,                      kVK_JIS_Kana            },
  { 0x15 /* VKEY_HANGUL */,                    -1                      },
  { 0x17 /* VKEY_JUNJA */,                     -1                      },
  { 0x18 /* VKEY_FINAL */,                     -1                      },
  { 0x19 /* VKEY_HANJA */,                     -1                      },
  { 0x19 /* VKEY_KANJI */,                     -1                      },
  { 0x1B /* VKEY_ESCAPE */,                    kVK_Escape              },
  { 0x1C /* VKEY_CONVERT */,                   -1                      },
  { 0x1D /* VKEY_NONCONVERT */,                -1                      },
  { 0x1E /* VKEY_ACCEPT */,                    -1                      },
  { 0x1F /* VKEY_MODECHANGE */,                -1                      },
  { 0x20 /* VKEY_SPACE */,                     kVK_Space               },
  { 0x21 /* VKEY_PRIOR */,                     kVK_PageUp              },
  { 0x22 /* VKEY_NEXT */,                      kVK_PageDown            },
  { 0x23 /* VKEY_END */,                       kVK_End                 },
  { 0x24 /* VKEY_HOME */,                      kVK_Home                },
  { 0x25 /* VKEY_LEFT */,                      kVK_LeftArrow           },
  { 0x26 /* VKEY_UP */,                        kVK_UpArrow             },
  { 0x27 /* VKEY_RIGHT */,                     kVK_RightArrow          },
  { 0x28 /* VKEY_DOWN */,                      kVK_DownArrow           },
  { 0x29 /* VKEY_SELECT */,                    -1                      },
  { 0x2A /* VKEY_PRINT */,                     -1                      },
  { 0x2B /* VKEY_EXECUTE */,                   -1                      },
  { 0x2C /* VKEY_SNAPSHOT */,                  -1                      },
  { 0x2D /* VKEY_INSERT */,                    kVK_Help                },
  { 0x2E /* VKEY_DELETE */,                    kVK_ForwardDelete       },
  { 0x2F /* VKEY_HELP */,                      kVK_Help                },
  { 0x30 /* VKEY_0 */,                         kVK_ANSI_0              },
  { 0x31 /* VKEY_1 */,                         kVK_ANSI_1              },
  { 0x32 /* VKEY_2 */,                         kVK_ANSI_2              },
  { 0x33 /* VKEY_3 */,                         kVK_ANSI_3              },
  { 0x34 /* VKEY_4 */,                         kVK_ANSI_4              },
  { 0x35 /* VKEY_5 */,                         kVK_ANSI_5              },
  { 0x36 /* VKEY_6 */,                         kVK_ANSI_6              },
  { 0x37 /* VKEY_7 */,                         kVK_ANSI_7              },
  { 0x38 /* VKEY_8 */,                         kVK_ANSI_8              },
  { 0x39 /* VKEY_9 */,                         kVK_ANSI_9              },
  { 0x41 /* VKEY_A */,                         kVK_ANSI_A              },
  { 0x42 /* VKEY_B */,                         kVK_ANSI_B              },
  { 0x43 /* VKEY_C */,                         kVK_ANSI_C              },
  { 0x44 /* VKEY_D */,                         kVK_ANSI_D              },
  { 0x45 /* VKEY_E */,                         kVK_ANSI_E              },
  { 0x46 /* VKEY_F */,                         kVK_ANSI_F              },
  { 0x47 /* VKEY_G */,                         kVK_ANSI_G              },
  { 0x48 /* VKEY_H */,                         kVK_ANSI_H              },
  { 0x49 /* VKEY_I */,                         kVK_ANSI_I              },
  { 0x4A /* VKEY_J */,                         kVK_ANSI_J              },
  { 0x4B /* VKEY_K */,                         kVK_ANSI_K              },
  { 0x4C /* VKEY_L */,                         kVK_ANSI_L              },
  { 0x4D /* VKEY_M */,                         kVK_ANSI_M              },
  { 0x4E /* VKEY_N */,                         kVK_ANSI_N              },
  { 0x4F /* VKEY_O */,                         kVK_ANSI_O              },
  { 0x50 /* VKEY_P */,                         kVK_ANSI_P              },
  { 0x51 /* VKEY_Q */,                         kVK_ANSI_Q              },
  { 0x52 /* VKEY_R */,                         kVK_ANSI_R              },
  { 0x53 /* VKEY_S */,                         kVK_ANSI_S              },
  { 0x54 /* VKEY_T */,                         kVK_ANSI_T              },
  { 0x55 /* VKEY_U */,                         kVK_ANSI_U              },
  { 0x56 /* VKEY_V */,                         kVK_ANSI_V              },
  { 0x57 /* VKEY_W */,                         kVK_ANSI_W              },
  { 0x58 /* VKEY_X */,                         kVK_ANSI_X              },
  { 0x59 /* VKEY_Y */,                         kVK_ANSI_Y              },
  { 0x5A /* VKEY_Z */,                         kVK_ANSI_Z              },
  { 0x5B /* VKEY_LWIN */,                      kVK_Command             },
  { 0x5C /* VKEY_RWIN */,                      kVK_RightCommand        },
  { 0x5D /* VKEY_APPS */,                      kVK_RightCommand        },
  { 0x5F /* VKEY_SLEEP */,                     -1                      },
  { 0x60 /* VKEY_NUMPAD0 */,                   kVK_ANSI_Keypad0        },
  { 0x61 /* VKEY_NUMPAD1 */,                   kVK_ANSI_Keypad1        },
  { 0x62 /* VKEY_NUMPAD2 */,                   kVK_ANSI_Keypad2        },
  { 0x63 /* VKEY_NUMPAD3 */,                   kVK_ANSI_Keypad3        },
  { 0x64 /* VKEY_NUMPAD4 */,                   kVK_ANSI_Keypad4        },
  { 0x65 /* VKEY_NUMPAD5 */,                   kVK_ANSI_Keypad5        },
  { 0x66 /* VKEY_NUMPAD6 */,                   kVK_ANSI_Keypad6        },
  { 0x67 /* VKEY_NUMPAD7 */,                   kVK_ANSI_Keypad7        },
  { 0x68 /* VKEY_NUMPAD8 */,                   kVK_ANSI_Keypad8        },
  { 0x69 /* VKEY_NUMPAD9 */,                   kVK_ANSI_Keypad9        },
  { 0x6A /* VKEY_MULTIPLY */,                  kVK_ANSI_KeypadMultiply },
  { 0x6B /* VKEY_ADD */,                       kVK_ANSI_KeypadPlus     },
  { 0x6C /* VKEY_SEPARATOR */,                 -1                      },
  { 0x6D /* VKEY_SUBTRACT */,                  kVK_ANSI_KeypadMinus    },
  { 0x6E /* VKEY_DECIMAL */,                   kVK_ANSI_KeypadDecimal  },
  { 0x6F /* VKEY_DIVIDE */,                    kVK_ANSI_KeypadDivide   },
  { 0x70 /* VKEY_F1 */,                        kVK_F1                  },
  { 0x71 /* VKEY_F2 */,                        kVK_F2                  },
  { 0x72 /* VKEY_F3 */,                        kVK_F3                  },
  { 0x73 /* VKEY_F4 */,                        kVK_F4                  },
  { 0x74 /* VKEY_F5 */,                        kVK_F5                  },
  { 0x75 /* VKEY_F6 */,                        kVK_F6                  },
  { 0x76 /* VKEY_F7 */,                        kVK_F7                  },
  { 0x77 /* VKEY_F8 */,                        kVK_F8                  },
  { 0x78 /* VKEY_F9 */,                        kVK_F9                  },
  { 0x79 /* VKEY_F10 */,                       kVK_F10                 },
  { 0x7A /* VKEY_F11 */,                       kVK_F11                 },
  { 0x7B /* VKEY_F12 */,                       kVK_F12                 },
  { 0x7C /* VKEY_F13 */,                       kVK_F13                 },
  { 0x7D /* VKEY_F14 */,                       kVK_F14                 },
  { 0x7E /* VKEY_F15 */,                       kVK_F15                 },
  { 0x7F /* VKEY_F16 */,                       kVK_F16                 },
  { 0x80 /* VKEY_F17 */,                       kVK_F17                 },
  { 0x81 /* VKEY_F18 */,                       kVK_F18                 },
  { 0x82 /* VKEY_F19 */,                       kVK_F19                 },
  { 0x83 /* VKEY_F20 */,                       kVK_F20                 },
  { 0x84 /* VKEY_F21 */,                       -1                      },
  { 0x85 /* VKEY_F22 */,                       -1                      },
  { 0x86 /* VKEY_F23 */,                       -1                      },
  { 0x87 /* VKEY_F24 */,                       -1                      },
  { 0x90 /* VKEY_NUMLOCK */,                   -1                      },
  { 0x91 /* VKEY_SCROLL */,                    -1                      },
  { 0xA0 /* VKEY_LSHIFT */,                    kVK_Shift               },
  { 0xA1 /* VKEY_RSHIFT */,                    kVK_RightShift          },
  { 0xA2 /* VKEY_LCONTROL */,                  kVK_Control             },
  { 0xA3 /* VKEY_RCONTROL */,                  kVK_RightControl        },
  { 0xA4 /* VKEY_LMENU */,                     kVK_Option              },
  { 0xA5 /* VKEY_RMENU */,                     kVK_RightOption         },
  { 0xA6 /* VKEY_BROWSER_BACK */,              -1                      },
  { 0xA7 /* VKEY_BROWSER_FORWARD */,           -1                      },
  { 0xA8 /* VKEY_BROWSER_REFRESH */,           -1                      },
  { 0xA9 /* VKEY_BROWSER_STOP */,              -1                      },
  { 0xAA /* VKEY_BROWSER_SEARCH */,            -1                      },
  { 0xAB /* VKEY_BROWSER_FAVORITES */,         -1                      },
  { 0xAC /* VKEY_BROWSER_HOME */,              -1                      },
  { 0xAD /* VKEY_VOLUME_MUTE */,               -1                      },
  { 0xAE /* VKEY_VOLUME_DOWN */,               -1                      },
  { 0xAF /* VKEY_VOLUME_UP */,                 -1                      },
  { 0xB0 /* VKEY_MEDIA_NEXT_TRACK */,          -1                      },
  { 0xB1 /* VKEY_MEDIA_PREV_TRACK */,          -1                      },
  { 0xB2 /* VKEY_MEDIA_STOP */,                -1                      },
  { 0xB3 /* VKEY_MEDIA_PLAY_PAUSE */,          -1                      },
  { 0xB4 /* VKEY_MEDIA_LAUNCH_MAIL */,         -1                      },
  { 0xB5 /* VKEY_MEDIA_LAUNCH_MEDIA_SELECT */, -1                      },
  { 0xB6 /* VKEY_MEDIA_LAUNCH_APP1 */,         -1                      },
  { 0xB7 /* VKEY_MEDIA_LAUNCH_APP2 */,         -1                      },
  { 0xBA /* VKEY_OEM_1 */,                     kVK_ANSI_Semicolon      },
  { 0xBB /* VKEY_OEM_PLUS */,                  kVK_ANSI_Equal          },
  { 0xBC /* VKEY_OEM_COMMA */,                 kVK_ANSI_Comma          },
  { 0xBD /* VKEY_OEM_MINUS */,                 kVK_ANSI_Minus          },
  { 0xBE /* VKEY_OEM_PERIOD */,                kVK_ANSI_Period         },
  { 0xBF /* VKEY_OEM_2 */,                     kVK_ANSI_Slash          },
  { 0xC0 /* VKEY_OEM_3 */,                     kVK_ANSI_Grave          },
  { 0xDB /* VKEY_OEM_4 */,                     kVK_ANSI_LeftBracket    },
  { 0xDC /* VKEY_OEM_5 */,                     kVK_ANSI_Backslash      },
  { 0xDD /* VKEY_OEM_6 */,                     kVK_ANSI_RightBracket   },
  { 0xDE /* VKEY_OEM_7 */,                     kVK_ANSI_Quote          },
  { 0xDF /* VKEY_OEM_8 */,                     -1                      },
  { 0xE2 /* VKEY_OEM_102 */,                   -1                      },
  { 0xE5 /* VKEY_PROCESSKEY */,                -1                      },
  { 0xE7 /* VKEY_PACKET */,                    -1                      },
  { 0xF6 /* VKEY_ATTN */,                      -1                      },
  { 0xF7 /* VKEY_CRSEL */,                     -1                      },
  { 0xF8 /* VKEY_EXSEL */,                     -1                      },
  { 0xF9 /* VKEY_EREOF */,                     -1                      },
  { 0xFA /* VKEY_PLAY */,                      -1                      },
  { 0xFB /* VKEY_ZOOM */,                      -1                      },
  { 0xFC /* VKEY_NONAME */,                    -1                      },
  { 0xFD /* VKEY_PA1 */,                       -1                      },
  { 0xFE /* VKEY_OEM_CLEAR */,                 kVK_ANSI_KeypadClear    }
};
  // clang-format on

  int keysym(int keycode) {
    KeyCodeMap key_map {};

    key_map.win_keycode = keycode;
    const KeyCodeMap *temp_map = std::lower_bound(
      kKeyCodesMap,
      kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]),
      key_map
    );

    if (temp_map >= kKeyCodesMap + sizeof(kKeyCodesMap) / sizeof(kKeyCodesMap[0]) ||
        temp_map->win_keycode != keycode || temp_map->mac_keycode == -1) {
      return -1;
    }

    return temp_map->mac_keycode;
  }

  struct modifier_flags_t {
    CGEventFlags generic {};
    CGEventFlags device {};
    CGEventFlags all_devices {};
  };

  bool modifier_flags_for_key(int key, modifier_flags_t &flags) {
    switch (key) {
      case kVK_Shift:
        flags = {kCGEventFlagMaskShift, NX_DEVICELSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK | NX_DEVICERSHIFTKEYMASK};
        return true;
      case kVK_RightShift:
        flags = {kCGEventFlagMaskShift, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK | NX_DEVICERSHIFTKEYMASK};
        return true;
      case kVK_Command:
        flags = {kCGEventFlagMaskCommand, NX_DEVICELCMDKEYMASK, NX_DEVICELCMDKEYMASK | NX_DEVICERCMDKEYMASK};
        return true;
      case kVK_RightCommand:
        flags = {kCGEventFlagMaskCommand, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK | NX_DEVICERCMDKEYMASK};
        return true;
      case kVK_Option:
        flags = {kCGEventFlagMaskAlternate, NX_DEVICELALTKEYMASK, NX_DEVICELALTKEYMASK | NX_DEVICERALTKEYMASK};
        return true;
      case kVK_RightOption:
        flags = {kCGEventFlagMaskAlternate, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK | NX_DEVICERALTKEYMASK};
        return true;
      case kVK_Control:
        flags = {kCGEventFlagMaskControl, NX_DEVICELCTLKEYMASK, NX_DEVICELCTLKEYMASK | NX_DEVICERCTLKEYMASK};
        return true;
      case kVK_RightControl:
        flags = {kCGEventFlagMaskControl, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK | NX_DEVICERCTLKEYMASK};
        return true;
      default:
        return false;
    }
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags) {
    auto key = keysym(modcode);

    BOOST_LOG(debug) << "got keycode: 0x"sv << std::hex << modcode << ", translated to: 0x" << std::hex << key << ", release:" << release;

    if (key < 0) {
      return;
    }

    auto macos_input = ((macos_input_t *) input.get());
    CGEventRef event = nullptr;
    modifier_flags_t modifier_flags;

    if (modifier_flags_for_key(key, modifier_flags)) {
      event = CGEventCreateKeyboardEvent(macos_input->keyboard_source, key, !release);
      if (!event) {
        return;
      }

      CGEventSetIntegerValueField(event, kCGKeyboardEventKeycode, key);

      if (release) {
        macos_input->kb_flags &= ~modifier_flags.device;
        if ((macos_input->kb_flags & modifier_flags.all_devices) == 0) {
          macos_input->kb_flags &= ~modifier_flags.generic;
        }
      } else {
        macos_input->kb_flags |= modifier_flags.generic | modifier_flags.device;
      }

      CGEventSetType(event, kCGEventFlagsChanged);
    } else {
      event = CGEventCreateKeyboardEvent(macos_input->keyboard_source, key, !release);
      if (!event) {
        return;
      }

      CGEventSetType(event, release ? kCGEventKeyUp : kCGEventKeyDown);
    }

    CGEventSetFlags(event, macos_input->kb_flags);
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
  }

  void unicode(input_t &input, char *utf8, int size) {
    BOOST_LOG(info) << "unicode: Unicode input not yet implemented for MacOS."sv;
  }

  int alloc_gamepad(input_t &input, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    BOOST_LOG(info) << "alloc_gamepad: Gamepad not yet implemented for MacOS."sv;
    return -1;
  }

  void free_gamepad(input_t &input, int nr) {
    BOOST_LOG(info) << "free_gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  void gamepad_update(input_t &input, int nr, const gamepad_state_t &gamepad_state) {
    BOOST_LOG(info) << "gamepad: Gamepad not yet implemented for MacOS."sv;
  }

  // returns current mouse location:
  util::point_t get_mouse_loc(input_t &input) {
    // Creating a new event every time to avoid any reuse risk
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto snapshot_event = CGEventCreate(macos_input->source);
    const auto current = CGEventGetLocation(snapshot_event);
    CFRelease(snapshot_event);
    return util::point_t {
      current.x,
      current.y
    };
  }

  void post_mouse(
    input_t &input,
    const CGMouseButton button,
    const CGEventType type,
    const util::point_t raw_location,
    const util::point_t previous_location,
    const int click_count
  ) {
    BOOST_LOG(debug) << "mouse_event: "sv << button << ", type: "sv << type << ", location:"sv << raw_location.x << ":"sv << raw_location.y << " click_count: "sv << click_count;

    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto event = macos_input->mouse_event;

    // Keep the pointer on the streamed display, or on a Remote Monitor display, which someone is
    // also watching.
    auto display = current_display(macos_input);
    CGDirectDisplayID display_at_point = kCGNullDirectDisplay;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(raw_location.x, raw_location.y), 1, &display_at_point, &count) == kCGErrorSuccess && count == 1 &&
        macos_virtual_display::is_remote_display(display_at_point)) {
      display = display_at_point;
    }
    const CGRect display_bounds = CGDisplayBounds(display);
    const auto location = CGPoint {
      std::clamp(raw_location.x, display_bounds.origin.x, display_bounds.origin.x + display_bounds.size.width - 1),
      std::clamp(raw_location.y, display_bounds.origin.y, display_bounds.origin.y + display_bounds.size.height - 1)
    };

    CGEventSetType(event, type);
    CGEventSetLocation(event, location);
    CGEventSetIntegerValueField(event, kCGMouseEventButtonNumber, button);
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);

    // Include deltas so some 3D applications can consume changes (game cameras, etc)
    const double deltaX = raw_location.x - previous_location.x;
    const double deltaY = raw_location.y - previous_location.y;
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaX);
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaY);

    // Inject modifier flags into mouse events so that shift+click and similar combinations work correctly.
    CGEventSetFlags(event, macos_input->kb_flags);
    CGEventPost(kCGHIDEventTap, event);
    // For why this is here, see:
    // https://stackoverflow.com/questions/15194409/simulated-mouseevent-not-working-properly-osx
    CGWarpMouseCursorPosition(location);
  }

  inline CGEventType event_type_mouse(input_t &input) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());

    if (macos_input->mouse_down[0]) {
      return kCGEventLeftMouseDragged;
    }
    if (macos_input->mouse_down[1]) {
      return kCGEventOtherMouseDragged;
    }
    if (macos_input->mouse_down[2]) {
      return kCGEventRightMouseDragged;
    }
    return kCGEventMouseMoved;
  }

  void move_mouse(
    input_t &input,
    const int deltaX,
    const int deltaY
  ) {
    const auto current = get_mouse_loc(input);

    const auto location = util::point_t {current.x + deltaX, current.y + deltaY};
    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), location, current, 0);
  }

  // Maps a point in the stream's touch port to a global screen point on the display it belongs to.
  CGPoint stream_location(macos_input_t *macos_input, const touch_port_t &touch_port, const float x, const float y) {
    const auto display = pointer_display(macos_input, touch_port);
    const auto scaling = display == macos_input->display ? macos_input->displayScaling : display_scaling(display);

    // in order to get the correct mouse location for capturing display , we need to add the display bounds to the location
    const CGRect display_bounds = CGDisplayBounds(display);
    return CGPointMake(x * scaling + display_bounds.origin.x, y * scaling + display_bounds.origin.y);
  }

  void abs_mouse(
    input_t &input,
    const touch_port_t &touch_port,
    const float x,
    const float y
  ) {
    const auto macos_input = static_cast<macos_input_t *>(input.get());
    const auto location = stream_location(macos_input, touch_port, x, y);

    post_mouse(input, kCGMouseButtonLeft, event_type_mouse(input), util::point_t {location.x, location.y}, get_mouse_loc(input), 0);
  }

  void button_mouse(input_t &input, const int button, const bool release) {
    CGMouseButton mac_button;
    CGEventType event;

    const auto macos_input = static_cast<macos_input_t *>(input.get());

    switch (button) {
      case 1:
        mac_button = kCGMouseButtonLeft;
        event = release ? kCGEventLeftMouseUp : kCGEventLeftMouseDown;
        break;
      case 2:
        mac_button = kCGMouseButtonCenter;
        event = release ? kCGEventOtherMouseUp : kCGEventOtherMouseDown;
        break;
      case 3:
        mac_button = kCGMouseButtonRight;
        event = release ? kCGEventRightMouseUp : kCGEventRightMouseDown;
        break;
      default:
        BOOST_LOG(warning) << "Unsupported mouse button for MacOS: "sv << button;
        return;
    }

    macos_input->mouse_down[mac_button] = !release;

    // if the last mouse down was less than MULTICLICK_DELAY_MS, we send a double click event
    const auto now = std::chrono::steady_clock::now();
    const auto mouse_position = get_mouse_loc(input);

    if (now < macos_input->last_mouse_event[mac_button][release] + MULTICLICK_DELAY_MS) {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 2);
    } else {
      post_mouse(input, mac_button, event, mouse_position, mouse_position, 1);
    }

    macos_input->last_mouse_event[mac_button][release] = now;
  }

  int get_scroll_lines_per_detent(double &scrollwheel_scaling) {
    double scale = DEFAULT_SCROLLWHEEL_SCALING;
    const auto value = CFPreferencesCopyValue(CFSTR("com.apple.scrollwheel.scaling"), kCFPreferencesAnyApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (value) {
      if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &scale);
      } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        scale = CFStringGetDoubleValue(static_cast<CFStringRef>(value));
      }
      CFRelease(value);
    }

    if (!std::isfinite(scale)) {
      scale = DEFAULT_SCROLLWHEEL_SCALING;
    }

    scrollwheel_scaling = scale;

    // com.apple.scrollwheel.scaling stores the Mouse scroll speed slider position, not
    // the scroll multiplier itself. The slider is 0..1 and Apple's default is 0.3125,
    // so anchor 0 at one line per wheel detent and 0.3125 at five lines.
    const auto scroll_scale = std::clamp(scale, 0.0, 1.0);
    constexpr double lines_per_scroll_scale = (DEFAULT_SCROLL_LINES_PER_DETENT - 1.0) / DEFAULT_SCROLLWHEEL_SCALING;

    return std::max(1, static_cast<int>(std::ceil(1.0 + scroll_scale * lines_per_scroll_scale)));
  }

  int scroll_pixels(const macos_input_t *macos_input, const int high_res_distance) {
    const auto source_pixels_per_line = CGEventSourceGetPixelsPerLine(macos_input->source);
    const auto pixels_per_line = source_pixels_per_line > 0 ? static_cast<int>(source_pixels_per_line + 0.5) : 10;
    const auto scaled_pixels = static_cast<int64_t>(high_res_distance) * std::max(1, pixels_per_line) * std::max(1, macos_input->scroll_lines_per_detent);

    return static_cast<int>(scaled_pixels / WHEEL_DELTA);
  }

  void post_scroll(macos_input_t *macos_input, const int wheelY, const int wheelX) {
    if (wheelY == 0 && wheelX == 0) {
      return;
    }

    CGEventRef event = CGEventCreateScrollWheelEvent(macos_input->source, kCGScrollEventUnitPixel, 2, wheelY, wheelX);
    if (!event) {
      return;
    }

    CGEventSetIntegerValueField(event, kCGScrollWheelEventIsContinuous, 1);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
  }

  void post_scroll(input_t &input, const int wheelY, const int wheelX) {
    post_scroll(static_cast<macos_input_t *>(input.get()), wheelY, wheelX);
  }

  void scroll(input_t &input, const int high_res_distance) {
    post_scroll(input, scroll_pixels(static_cast<macos_input_t *>(input.get()), high_res_distance), 0);
  }

  void hscroll(input_t &input, int high_res_distance) {
    post_scroll(input, 0, scroll_pixels(static_cast<macos_input_t *>(input.get()), high_res_distance));
  }

  /**
   * @brief Per-client pen and touch state.
   *
   * The pen is posted as a tablet device: proximity events as it enters, leaves, or changes tool,
   * and mouse events carrying the tablet fields (pressure, tilt, rotation, buttons) while it is
   * in range. Touch has no public injection API on macOS, so one finger scrolls (hold, then
   * move, to drag), a tap clicks, and two fingers scroll.
   */
  class macos_client_input_t: public client_input_t {
  public:
    explicit macos_client_input_t(macos_input_t *macos_input):
        macos_input {macos_input} {
    }

    ~macos_client_input_t() override {
      pen_release();
      touch_release();
    }

    void pen_update(const touch_port_t &touch_port, const pen_input_t &pen);
    void touch_update(const touch_port_t &touch_port, const touch_input_t &touch);

  private:
    // Pen buttons, in the order used for the pressed state: tip, then the barrel buttons.
    enum pen_button_e {
      PEN_TIP,
      PEN_PRIMARY,
      PEN_SECONDARY,
      PEN_TERTIARY,
      PEN_BUTTON_COUNT
    };

    struct touch_point_t {
      CGPoint start;
      CGPoint last;
      std::chrono::steady_clock::time_point down_time;
    };

    // A tap (down and up with little movement) clicks; moving past this many points drags.
    static constexpr double TOUCH_SLOP = 6;
    // Holding one finger still this long makes the next movement drag instead of scroll.
    static constexpr std::chrono::milliseconds LONG_PRESS {350};
    static constexpr double MULTICLICK_DISTANCE = 6;
    static constexpr int POINTER_PEN = 1;  // NX_TABLET_POINTER_PEN
    static constexpr int POINTER_ERASER = 3;  // NX_TABLET_POINTER_ERASER
    // NX_TABLET_CAPABILITY_ DEVICEID, BUTTONS, TILTX, TILTY, PRESSURE, and ROTATION.
    static constexpr int TABLET_CAPABILITIES = 0x0001 | 0x0040 | 0x0080 | 0x0100 | 0x0400 | 0x2000;

    static CGMouseButton mouse_button(const int pen_button) {
      switch (pen_button) {
        case PEN_TIP:
          return kCGMouseButtonLeft;
        case PEN_PRIMARY:
          return kCGMouseButtonRight;
        case PEN_SECONDARY:
          return kCGMouseButtonCenter;
        default:
          return static_cast<CGMouseButton>(3);
      }
    }

    static CGEventType button_event(const int pen_button, const bool down, const bool dragged) {
      switch (pen_button) {
        case PEN_TIP:
          return dragged ? kCGEventLeftMouseDragged : (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp);
        case PEN_PRIMARY:
          return dragged ? kCGEventRightMouseDragged : (down ? kCGEventRightMouseDown : kCGEventRightMouseUp);
        default:
          return dragged ? kCGEventOtherMouseDragged : (down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp);
      }
    }

    CGPoint clamp_to_display(const CGPoint &point) const {
      CGDirectDisplayID display = current_display(macos_input);
      CGDirectDisplayID display_at_point = kCGNullDirectDisplay;
      uint32_t count = 0;
      if (CGGetDisplaysWithPoint(point, 1, &display_at_point, &count) == kCGErrorSuccess && count == 1 &&
          macos_virtual_display::is_remote_display(display_at_point)) {
        display = display_at_point;
      }
      const CGRect bounds = CGDisplayBounds(display);
      return CGPointMake(
        std::clamp(point.x, bounds.origin.x, bounds.origin.x + bounds.size.width - 1),
        std::clamp(point.y, bounds.origin.y, bounds.origin.y + bounds.size.height - 1)
      );
    }

    void post_event(CGEventRef event) const {
      CGEventSetFlags(event, macos_input->kb_flags);
      CGEventPost(kCGHIDEventTap, event);
      CFRelease(event);
    }

    // Posts a mouse event at location; pressure and tilt are set for pen events only.
    void post_mouse_event(const CGEventType type, const CGMouseButton button, const CGPoint &location, const int click_count, const pen_input_t *pen = nullptr) {
      const auto event = CGEventCreateMouseEvent(macos_input->source, type, location, button);
      if (!event) {
        return;
      }

      CGEventSetIntegerValueField(event, kCGMouseEventClickState, click_count);
      if (have_location) {
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, location.x - last_location.x);
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, location.y - last_location.y);
      }

      if (pen) {
        double tilt_x = 0;
        double tilt_y = 0;
        // Convert the tilt from vertical and the direction it leans into X and Y tilt, from -1 to 1.
        if (pen->tilt != LI_TILT_UNKNOWN && pen->rotation != LI_ROT_UNKNOWN) {
          const auto rotation = pen->rotation * (M_PI / 180);
          const auto tilt = pen->tilt * (M_PI / 180);
          const auto r = std::sin(tilt);
          const auto z = std::cos(tilt);
          tilt_x = std::atan2(std::sin(-rotation) * r, z) / (M_PI / 2);
          tilt_y = std::atan2(std::cos(-rotation) * r, z) / (M_PI / 2);
        }

        int buttons = 0;
        for (int i = 0; i < PEN_BUTTON_COUNT; ++i) {
          if (pen_down[i]) {
            buttons |= 1 << i;
          }
        }

        // The subtype has to be set before the other tablet fields.
        CGEventSetIntegerValueField(event, kCGMouseEventSubtype, kCGEventMouseSubtypeTabletPoint);
        CGEventSetIntegerValueField(event, kCGTabletEventPointButtons, buttons);
        const double pressure = pen_down[PEN_TIP] ? pen->pressureOrDistance : 0.0;
        CGEventSetDoubleValueField(event, kCGTabletEventPointPressure, pressure);
        // NSEvent.pressure on mouse events reads this field instead of the tablet one.
        CGEventSetDoubleValueField(event, kCGMouseEventPressure, pressure);
        CGEventSetDoubleValueField(event, kCGTabletEventTiltX, tilt_x);
        CGEventSetDoubleValueField(event, kCGTabletEventTiltY, tilt_y);
        CGEventSetDoubleValueField(event, kCGTabletEventRotation, pen->rotation == LI_ROT_UNKNOWN ? 0.0 : pen->rotation);
        CGEventSetIntegerValueField(event, kCGTabletEventDeviceID, pen_pointer);
      }

      post_event(event);
      // See post_mouse(): keeps the cursor where the event was posted.
      CGWarpMouseCursorPosition(location);
      last_location = location;
      have_location = true;
    }

    void post_proximity(const bool enter) const {
      const auto event = CGEventCreate(macos_input->source);
      if (!event) {
        return;
      }

      CGEventSetType(event, kCGEventTabletProximity);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventEnterProximity, enter);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventPointerType, pen_pointer);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventDeviceID, pen_pointer);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventPointerID, pen_pointer);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventSystemTabletID, 1);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventTabletID, 1);
      CGEventSetIntegerValueField(event, kCGTabletProximityEventCapabilityMask, TABLET_CAPABILITIES);
      post_event(event);
    }

    // Lifts every pen button and leaves proximity, so nothing stays pressed.
    void pen_release() {
      if (!pen_in_proximity) {
        return;
      }

      pen_input_t pen {};
      pen.tilt = LI_TILT_UNKNOWN;
      pen.rotation = LI_ROT_UNKNOWN;
      for (int i = PEN_BUTTON_COUNT - 1; i >= 0; --i) {
        if (pen_down[i]) {
          pen_down[i] = false;
          post_mouse_event(button_event(i, false, false), mouse_button(i), last_location, i == PEN_TIP ? pen_click_count : 1, &pen);
        }
      }
      post_proximity(false);
      pen_in_proximity = false;
    }

    void touch_right_click(const CGPoint &location) {
      post_mouse_event(kCGEventRightMouseDown, kCGMouseButtonRight, location, 1);
      post_mouse_event(kCGEventRightMouseUp, kCGMouseButtonRight, location, 1);
    }

    void touch_click(const CGPoint &location) {
      // Taps close together in time and place are a double or triple click.
      const auto now = std::chrono::steady_clock::now();
      const bool near = std::hypot(location.x - last_tap.x, location.y - last_tap.y) <= MULTICLICK_DISTANCE;
      tap_count = (now - last_tap_time < MULTICLICK_DELAY_MS && near) ? tap_count + 1 : 1;
      last_tap = location;
      last_tap_time = now;

      post_mouse_event(kCGEventLeftMouseDown, kCGMouseButtonLeft, location, tap_count);
      post_mouse_event(kCGEventLeftMouseUp, kCGMouseButtonLeft, location, tap_count);
    }

    // Scrolls by the whole pixels gathered so far and keeps the fraction for the next event.
    void scroll_by_remainder() {
      const auto whole_x = std::trunc(scroll_remainder_x);
      const auto whole_y = std::trunc(scroll_remainder_y);
      scroll_remainder_x -= whole_x;
      scroll_remainder_y -= whole_y;
      post_scroll(macos_input, static_cast<int>(whole_y), static_cast<int>(whole_x));
    }

    void touch_release() {
      if (touch_dragging) {
        post_mouse_event(kCGEventLeftMouseUp, kCGMouseButtonLeft, last_location, 1);
      }
      touch_dragging = false;
      touch_points.clear();
    two_finger_tap = false;
    }

    macos_input_t *macos_input;
    CGPoint last_location {};
    bool have_location {};

    bool pen_in_proximity {};
    int pen_pointer {POINTER_PEN};
    bool pen_down[PEN_BUTTON_COUNT] {};
    int pen_click_count {1};
    CGPoint last_pen_down {};
    std::chrono::steady_clock::time_point last_pen_down_time {};

    std::map<std::uint32_t, touch_point_t> touch_points;
    bool touch_dragging {};  ///< The left button is held for a one-finger drag.
    bool touch_scrolling {};  ///< Two fingers have been down since the last finger lifted.
    bool two_finger_tap {};  ///< Two fingers are down and neither has moved yet, so lifting them right-clicks.
    std::chrono::steady_clock::time_point two_finger_down_time {};
    double scroll_remainder_x {};  ///< Movement too small to scroll a whole pixel yet.
    double scroll_remainder_y {};
    int tap_count {1};
    CGPoint last_tap {};
    std::chrono::steady_clock::time_point last_tap_time {};
  };

  void macos_client_input_t::pen_update(const touch_port_t &touch_port, const pen_input_t &pen) {
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        pen_release();
        return;
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_MOVE:
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        break;
      default:
        return;
    }

    // Pen and touch coordinates are 0..1 across the stream; touch_port gives its size.
    const auto location = clamp_to_display(stream_location(macos_input, touch_port, pen.x * touch_port.width, pen.y * touch_port.height));

    // A tool change (pen to eraser) is a new device coming into range.
    const int pointer = pen.toolType == LI_TOOL_TYPE_ERASER ? POINTER_ERASER : POINTER_PEN;
    if (pen_in_proximity && pointer != pen_pointer) {
      pen_release();
    }
    if (!pen_in_proximity) {
      pen_pointer = pointer;
      pen_in_proximity = true;
      post_proximity(true);
    }

    bool wanted[PEN_BUTTON_COUNT] {};
    switch (pen.eventType) {
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        wanted[PEN_TIP] = pen.eventType == LI_TOUCH_EVENT_DOWN || pen_down[PEN_TIP];
        break;
      default:
        break;  // Hover and up leave the tip off the surface.
    }
    wanted[PEN_PRIMARY] = pen.penButtons & LI_PEN_BUTTON_PRIMARY;
    wanted[PEN_SECONDARY] = pen.penButtons & LI_PEN_BUTTON_SECONDARY;
    wanted[PEN_TERTIARY] = pen.penButtons & LI_PEN_BUTTON_TERTIARY;

    // Move first, so a press lands where the pen is.
    int held = -1;
    for (int i = 0; i < PEN_BUTTON_COUNT && held < 0; ++i) {
      if (pen_down[i]) {
        held = i;
      }
    }
    // A move is posted even when the pen holds still, since pressure and tilt can change.
    const bool moved = !have_location || location.x != last_location.x || location.y != last_location.y;
    if (pen.eventType == LI_TOUCH_EVENT_MOVE || (pen.eventType != LI_TOUCH_EVENT_BUTTON_ONLY && moved)) {
      post_mouse_event(held < 0 ? kCGEventMouseMoved : button_event(held, true, true), held < 0 ? kCGMouseButtonLeft : mouse_button(held), location, 1, &pen);
    }

    for (int i = 0; i < PEN_BUTTON_COUNT; ++i) {
      if (wanted[i] == pen_down[i]) {
        continue;
      }
      pen_down[i] = wanted[i];

      int clicks = 1;
      if (i == PEN_TIP && wanted[i]) {
        // Taps close together in time and place are a double or triple click.
        const auto now = std::chrono::steady_clock::now();
        const bool near = std::hypot(location.x - last_pen_down.x, location.y - last_pen_down.y) <= MULTICLICK_DISTANCE;
        pen_click_count = (now - last_pen_down_time < MULTICLICK_DELAY_MS && near) ? pen_click_count + 1 : 1;
        last_pen_down = location;
        last_pen_down_time = now;
      }
      if (i == PEN_TIP) {
        clicks = pen_click_count;
      }
      post_mouse_event(button_event(i, wanted[i], false), mouse_button(i), location, clicks, &pen);
    }
  }

  void macos_client_input_t::touch_update(const touch_port_t &touch_port, const touch_input_t &touch) {
    switch (touch.eventType) {
      case LI_TOUCH_EVENT_CANCEL:
        touch_points.erase(touch.pointerId);
        if (touch_points.empty()) {
          touch_release();
          touch_scrolling = false;
        }
        return;
      case LI_TOUCH_EVENT_CANCEL_ALL:
        touch_release();
        touch_scrolling = false;
        return;
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_MOVE:
        break;
      default:
        return;  // Hover and button-only events have no touch equivalent.
    }

    const auto location = clamp_to_display(stream_location(macos_input, touch_port, touch.x * touch_port.width, touch.y * touch_port.height));

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_DOWN:
        touch_points[touch.pointerId] = {location, location, std::chrono::steady_clock::now()};
        if (touch_points.size() >= 2) {
          // The first finger was waiting to become a click or drag; it is a scroll now.
          if (touch_dragging) {
            post_mouse_event(kCGEventLeftMouseUp, kCGMouseButtonLeft, last_location, 1);
            touch_dragging = false;
          }
          touch_scrolling = true;
          two_finger_tap = touch_points.size() == 2;
          two_finger_down_time = std::chrono::steady_clock::now();
        } else {
          touch_scrolling = false;
          two_finger_tap = false;
          post_mouse_event(kCGEventMouseMoved, kCGMouseButtonLeft, location, 0);
        }
        break;
      case LI_TOUCH_EVENT_MOVE: {
        const auto point = touch_points.find(touch.pointerId);
        if (point == touch_points.end()) {
          return;
        }

        const auto delta_x = location.x - point->second.last.x;
        const auto delta_y = location.y - point->second.last.y;
        point->second.last = location;

        if (two_finger_tap) {
          // Small movement still counts as a tap; moving past the slop turns it into a scroll.
          if (std::hypot(location.x - point->second.start.x, location.y - point->second.start.y) <= TOUCH_SLOP) {
            return;
          }
          two_finger_tap = false;
        }

        if (touch_scrolling) {
          // Two fingers moving together scroll by their average movement; each finger reports
          // its own move, so take half of each.
          const auto fingers = static_cast<double>(std::max<std::size_t>(touch_points.size(), 1));
          scroll_remainder_x += delta_x / fingers;
          scroll_remainder_y += delta_y / fingers;
          scroll_by_remainder();
        } else if (touch_dragging) {
          post_mouse_event(kCGEventLeftMouseDragged, kCGMouseButtonLeft, location, 1);
        } else if (touch_points.size() == 1 && std::hypot(location.x - point->second.start.x, location.y - point->second.start.y) > TOUCH_SLOP) {
          if (std::chrono::steady_clock::now() - point->second.down_time >= LONG_PRESS) {
            // Held still, then moved: press at the start of the touch and drag from there.
            post_mouse_event(kCGEventLeftMouseDown, kCGMouseButtonLeft, point->second.start, 1);
            touch_dragging = true;
            post_mouse_event(kCGEventLeftMouseDragged, kCGMouseButtonLeft, location, 1);
          } else {
            // Moved right away: scroll, counting from where the finger landed.
            touch_scrolling = true;
            scroll_remainder_x += location.x - point->second.start.x;
            scroll_remainder_y += location.y - point->second.start.y;
            scroll_by_remainder();
          }
        }
        break;
      }
      case LI_TOUCH_EVENT_UP: {
        const auto point = touch_points.find(touch.pointerId);
        if (point == touch_points.end()) {
          return;
        }

        const bool single = touch_points.size() == 1 && !touch_dragging && !touch_scrolling;
        const bool held = std::chrono::steady_clock::now() - point->second.down_time >= LONG_PRESS;
        const auto start = point->second.start;
        if (two_finger_tap) {
          // Two fingers tapped together: right-click where the first one landed.
          two_finger_tap = false;
          if (std::chrono::steady_clock::now() - two_finger_down_time < LONG_PRESS) {
            touch_right_click(start);
          }
        }
        touch_points.erase(point);
        if (touch_dragging) {
          post_mouse_event(kCGEventLeftMouseUp, kCGMouseButtonLeft, location, 1);
          touch_dragging = false;
        } else if (single) {
          // A finger held still and lifted is a right-click, like a long press on an iPad.
          if (held) {
            touch_right_click(start);
          } else {
            touch_click(start);
          }
        }
        if (touch_points.empty()) {
          touch_scrolling = false;
        }
        break;
      }
    }
  }

  /**
   * @brief Allocates a context to store per-client input data.
   * @param input The global input context.
   * @return A unique pointer to a per-client input data context.
   */
  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<macos_client_input_t>(static_cast<macos_input_t *>(input.get()));
  }

  /**
   * @brief Sends a touch event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param touch The touch event.
   */
  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    if (const auto client = static_cast<macos_client_input_t *>(input)) {
      client->touch_update(touch_port, touch);
    }
  }

  /**
   * @brief Sends a pen event to the OS.
   * @param input The client-specific input context.
   * @param touch_port The current viewport for translating to screen coordinates.
   * @param pen The pen event.
   */
  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    if (const auto client = static_cast<macos_client_input_t *>(input)) {
      client->pen_update(touch_port, pen);
    }
  }

  /**
   * @brief Sends a gamepad touch event to the OS.
   * @param input The global input context.
   * @param touch The touch event.
   */
  void gamepad_touch(input_t &input, const gamepad_touch_t &touch) {
    // Unimplemented feature - platform_caps::controller_touch
  }

  /**
   * @brief Sends a gamepad motion event to the OS.
   * @param input The global input context.
   * @param motion The motion event.
   */
  void gamepad_motion(input_t &input, const gamepad_motion_t &motion) {
    // Unimplemented
  }

  /**
   * @brief Sends a gamepad battery event to the OS.
   * @param input The global input context.
   * @param battery The battery event.
   */
  void gamepad_battery(input_t &input, const gamepad_battery_t &battery) {
    // Unimplemented
  }

  input_t input() {
    input_t result {new macos_input_t()};

    const auto macos_input = static_cast<macos_input_t *>(result.get());

    // Default to main display
    macos_input->display = CGMainDisplayID();

    auto output_name = config::get_active_output_name();
    // If output_name is set, try to find the display with that display id
    if (!output_name.empty()) {
      const int MAX_DISPLAYS = 32;
      uint32_t max_display = MAX_DISPLAYS;
      uint32_t display_count;
      CGDirectDisplayID displays[MAX_DISPLAYS];
      if (CGGetActiveDisplayList(max_display, displays, &display_count) != kCGErrorSuccess) {
        BOOST_LOG(error) << "Unable to get active display list , error: "sv << std::endl;
      } else {
        for (int i = 0; i < display_count; i++) {
          CGDirectDisplayID display_id = displays[i];
          if (display_id == std::atoi(output_name.c_str())) {
            macos_input->display = display_id;
          }
        }
      }
    }

    macos_input->configured_display = macos_input->display;
    macos_input->displayScaling = display_scaling(macos_input->display);

    macos_input->source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    macos_input->keyboard_source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    macos_input->scroll_lines_per_detent = get_scroll_lines_per_detent(macos_input->scrollwheel_scaling);

    macos_input->kb_flags = 0;

    macos_input->mouse_event = CGEventCreate(macos_input->source);
    macos_input->mouse_down[0] = false;
    macos_input->mouse_down[1] = false;
    macos_input->mouse_down[2] = false;

    BOOST_LOG(debug) << "macOS scroll speed: com.apple.scrollwheel.scaling="sv << macos_input->scrollwheel_scaling << ", lines per detent="sv << macos_input->scroll_lines_per_detent << ", pixels per line="sv << CGEventSourceGetPixelsPerLine(macos_input->source);
    BOOST_LOG(debug) << "Display "sv << macos_input->display << ", pixel dimension: " << CGDisplayPixelsWide(macos_input->display) << "x"sv << CGDisplayPixelsHigh(macos_input->display);

    return result;
  }

  void freeInput(void *p) {
    const auto *input = static_cast<macos_input_t *>(p);

    CFRelease(input->source);
    CFRelease(input->keyboard_source);
    CFRelease(input->mouse_event);

    delete input;
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector gamepads {
      supported_gamepad_t {"", false, "gamepads.macos_not_implemented"}
    };

    return gamepads;
  }

  /**
   * @brief Returns the supported platform capabilities to advertise to the client.
   * @return Capability flags.
   */
  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;

    // Pen and touch are posted as ordinary input events, so they need only the permission that
    // mouse and keyboard input already do.
    if (config::input.native_pen_touch) {
      caps |= platform_caps::pen_touch;
    }

    return caps;
  }
}  // namespace platf
