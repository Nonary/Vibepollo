/**
 * @file src/platform/windows/capture_gpu_policy.h
 * @brief GPU submission ordering for cursor-composited desktop capture.
 */
#pragma once

namespace platf::dxgi::capture_policy {

  // On fresh desktop frames, copy directly from the acquired desktop into the
  // shared encoder image. Release that image before saving the cursor-free
  // desktop for subsequent mouse-only updates. This keeps the background copy
  // out of the encoder image's GPU dependency chain. Both copies are submitted
  // before releasing the acquired desktop, preserving D3D resource ordering.
  template<class CopyFrame, class BlendCursor, class ReleaseFrame, class CacheDesktop>
  bool submit_cursor_frame(
    bool desktop_updated,
    CopyFrame copy_frame,
    BlendCursor blend_cursor,
    ReleaseFrame release_frame,
    CacheDesktop cache_desktop) {
    copy_frame(desktop_updated);
    blend_cursor();
    if (!release_frame()) {
      return false;
    }
    if (desktop_updated) {
      cache_desktop();
    }
    return true;
  }

}  // namespace platf::dxgi::capture_policy
