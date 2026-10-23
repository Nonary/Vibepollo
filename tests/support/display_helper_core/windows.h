// Data declarations only: the portable helper-core tests never call Windows.
#pragma once
#include <cstdint>
using UINT32 = std::uint32_t;

struct LUID {
  std::uint32_t LowPart {};
  std::int32_t HighPart {};
};

struct DISPLAYCONFIG_PATH_INFO {};

struct DISPLAYCONFIG_MODE_INFO {};
