#pragma once
// The portable Sony report encoder includes the driver's fixed-size ABI.
// These declarations cover that ABI only; no Windows behavior is emulated.
#include <cstdint>

using DWORD = std::uint32_t;

struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2;
  std::uint16_t Data3;
  std::uint8_t Data4[8];
};
