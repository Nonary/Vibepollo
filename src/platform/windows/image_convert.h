// Simple WIC-based image conversion helper for Windows
#pragma once

#include <string>

namespace platf::img {
  // Convert source image to PNG at dst path. Preserves pixel dimensions and sets 96 DPI.
  // Returns true on success.
  bool convert_to_png_96dpi(const std::wstring &src_path, const std::wstring &dst_path);
}  // namespace platf::img
