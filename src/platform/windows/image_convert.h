// Simple WIC-based image conversion helper for Windows
#pragma once

#include <string>

namespace platf::img {
  // Convert source image to PNG at dst path. Preserves pixel dimensions and sets 96 DPI.
  // Returns true on success.
  bool convert_to_png_96dpi(const std::wstring &src_path, const std::wstring &dst_path);

  // Return the largest icon dimension (in pixels) embedded in an executable/DLL, or 0 if none.
  // Cheap: reads only the icon directory resources, not the pixel data.
  int probe_largest_icon_size(const std::wstring &exe_path);

  // Extract the largest icon embedded in an executable/DLL and write it as a 96 DPI PNG.
  // out_native_size receives the icon's native pixel dimension. Returns false when the file
  // has no icon resources or extraction fails.
  bool extract_largest_icon_png(const std::wstring &exe_path, const std::wstring &dst_path, int &out_native_size);

  // Best-effort: return the largest pixel width of an image file via WIC (0 on failure).
  int image_pixel_width(const std::wstring &src_path);

  // Optional diagnostics describing how an icon was resolved (for logging).
  struct IconResolutionInfo {
    std::wstring exe;  ///< Executable chosen for icon extraction (empty if none found).
    int exe_size = 0;  ///< Largest icon dimension embedded in that executable.
    int icon_size = 0;  ///< Pixel width of Playnite's stored icon source.
  };

  // Resolve the highest-quality icon for an application into dst_path (PNG, 96 DPI).
  // Prefers a high-resolution icon extracted from the game executable (exe_hint, or the main
  // executable discovered inside install_dir for store games) over the supplied icon_src.
  // An existing dst that is already at least as large as the best available source is kept.
  // When info is non-null it receives diagnostics about the chosen source.
  // Returns true when dst_path holds a usable icon.
  bool resolve_best_app_icon_png(const std::wstring &icon_src, const std::wstring &exe_hint, const std::wstring &install_dir, const std::wstring &dst_path, IconResolutionInfo *info = nullptr);
}  // namespace platf::img
