/**
 * @file src/platform/macos/sc_video.h
 * @brief Declarations for ScreenCaptureKit video capture on macOS.
 */
#pragma once

// local includes
#import "av_video.h"

/**
 * @brief AVVideo backed by ScreenCaptureKit instead of AVCaptureScreenInput.
 * @details AVCaptureScreenInput delivers no frames from virtual displays (CGVirtualDisplay), so
 *          the per-client virtual display is captured with ScreenCaptureKit. It keeps AVVideo's
 *          interface, so the rest of the capture and encode pipeline is unchanged.
 */
@interface SCVideo: AVVideo

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate;
- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback;

@end
