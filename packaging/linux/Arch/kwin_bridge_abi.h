#pragma once

#include <type_traits>

#include <core/gpumanager.h>

// The bridge interposes these symbols using their ELF names. Check the
// declarations too, since the mangled names do not encode their return types.
static_assert(std::is_same_v<decltype(&KWin::GpuManager::findCompatibleRenderDevice),
  KWin::RenderDevice *(KWin::GpuManager::*)(drmDevicePtr) const>);
