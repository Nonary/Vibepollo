#pragma once

#include <array>
#include <string_view>

// Legacy generated applications retain their established UUIDs and routing.
#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"

namespace proc {
  inline constexpr std::array<std::string_view, 4> legacy_generated_app_uuids {
    VIRTUAL_DISPLAY_UUID, FALLBACK_DESKTOP_UUID, REMOTE_INPUT_UUID, TERMINATE_APP_UUID,
  };
}
