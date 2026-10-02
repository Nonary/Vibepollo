/**
 * @file src/platform/macos/sc_video.m
 * @brief Definitions for ScreenCaptureKit video capture on macOS.
 */
// platform includes
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// local includes
#import "sc_video.h"

/**
 * @brief One capture: a stream feeding a frame callback until the callback returns false.
 * @details ScreenCaptureKit only sends an image when the screen changes, and can go silent on a
 *          static desktop. New images are delivered as soon as they arrive, for the lowest
 *          latency; if nothing new arrives within the keepalive interval, the last one is re-sent.
 *          That matches the async encode path's minimum frame rate: the encoder only runs on
 *          delivered frames, and keyframe requests need a frame to encode. Repeating on a fixed
 *          clock instead would queue new images behind repeats in a busy hardware encoder.
 *          Frames, keepalive ticks, and finishing all run on one serial queue.
 */
@interface SCVideoCapture: NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, strong) SCStream *stream;
@property (nonatomic, copy) FrameCallbackBlock callback;
@property (nonatomic, strong) dispatch_semaphore_t signal;
@property (nonatomic, strong) dispatch_queue_t queue;
- (void)startKeepaliveEvery:(CMTime)interval;
- (void)finish;
@end

@implementation SCVideoCapture {
  CMSampleBufferRef _lastFrame;  ///< Last buffer with an image, re-sent when nothing new arrives.
  dispatch_source_t _keepalive;
  uint64_t _keepalivePeriod;
  uint64_t _lastDelivery;
  BOOL _finished;
}

- (void)startKeepaliveEvery:(CMTime)interval {
  _keepalivePeriod = (uint64_t) (CMTimeGetSeconds(interval) * NSEC_PER_SEC);
  _keepalive = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self.queue);
  dispatch_source_set_timer(_keepalive, dispatch_time(DISPATCH_TIME_NOW, _keepalivePeriod), _keepalivePeriod, _keepalivePeriod / 10);
  __weak SCVideoCapture *weakSelf = self;
  dispatch_source_set_event_handler(_keepalive, ^{
    [weakSelf keepaliveTick];
  });
  dispatch_resume(_keepalive);
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  // Idle and blank status updates carry no image; the keepalive covers those stretches.
  if (type != SCStreamOutputTypeScreen || CMSampleBufferGetImageBuffer(sampleBuffer) == NULL) {
    return;
  }
  if (_lastFrame != NULL) {
    CFRelease(_lastFrame);
  }
  _lastFrame = (CMSampleBufferRef) CFRetain(sampleBuffer);
  [self deliverLastFrame];
}

- (void)keepaliveTick {
  const uint64_t idle = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - _lastDelivery;
  if (idle >= _keepalivePeriod * 9 / 10) {
    [self deliverLastFrame];
  }
}

- (void)deliverLastFrame {
  if (_finished || _lastFrame == NULL) {
    return;
  }
  _lastDelivery = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (!self.callback(_lastFrame)) {
    [self finishOnQueue];
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  [self finish];
}

- (void)finish {
  dispatch_async(self.queue, ^{
    [self finishOnQueue];
  });
}

- (void)finishOnQueue {
  if (_finished) {
    return;
  }
  _finished = YES;
  if (_keepalive != nil) {
    dispatch_source_cancel(_keepalive);
  }
  [self.stream stopCaptureWithCompletionHandler:nil];
  dispatch_semaphore_signal(self.signal);
}

- (void)dealloc {
  if (_keepalive != nil) {
    dispatch_source_cancel(_keepalive);
  }
  if (_lastFrame != NULL) {
    CFRelease(_lastFrame);
  }
}

@end

@implementation SCVideo {
  SCDisplay *_display;
  NSMutableArray<SCVideoCapture *> *_captures;
}

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  // Skips AVVideo's initializer, which would set up AVCaptureScreenInput.
  self = [super init];
  if (!self) {
    return nil;
  }

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
  if (mode == NULL) {
    return nil;
  }
  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;
  self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
  self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
  self.minFrameDuration = CMTimeMake(1, frameRate);
  CGDisplayModeRelease(mode);

  dispatch_semaphore_t found = dispatch_semaphore_create(0);
  __block SCDisplay *display = nil;
  [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
    for (SCDisplay *candidate in content.displays) {
      if (candidate.displayID == displayID) {
        display = candidate;
      }
    }
    dispatch_semaphore_signal(found);
  }];
  if (dispatch_semaphore_wait(found, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0 || display == nil) {
    return nil;
  }

  _display = display;
  _captures = [NSMutableArray array];
  return self;
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
  configuration.width = self.frameWidth;
  configuration.height = self.frameHeight;
  configuration.pixelFormat = self.pixelFormat;
  if (self.hdr && self.pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange) {
    // The stream's HDR color space, so the frames go to the encoder as they are. captureDynamicRange
    // is new in macOS 15; set it by name so builds with an older SDK, such as the Apple Silicon CI's
    // Xcode 15, still capture HDR. 1 is SCCaptureDynamicRangeHDRLocalDisplay.
    if ([configuration respondsToSelector:NSSelectorFromString(@"setCaptureDynamicRange:")]) {
      [configuration setValue:@1 forKey:@"captureDynamicRange"];
    }
    configuration.colorSpaceName = kCGColorSpaceITUR_2100_PQ;
    configuration.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_2020;
  }
  configuration.minimumFrameInterval = self.minFrameDuration;
  configuration.showsCursor = YES;
  // The pipeline holds a few frames and the last one is kept for the keepalive, so leave room.
  configuration.queueDepth = 8;

  SCVideoCapture *capture = [[SCVideoCapture alloc] init];
  capture.callback = frameCallback;
  capture.signal = dispatch_semaphore_create(0);
  dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, DISPATCH_QUEUE_PRIORITY_HIGH);
  capture.queue = dispatch_queue_create("videoCaptureQueue", qos);

  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:_display excludingWindows:@[]];
  capture.stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:capture];

  NSError *error = nil;
  if (![capture.stream addStreamOutput:capture type:SCStreamOutputTypeScreen sampleHandlerQueue:capture.queue error:&error]) {
    return nil;
  }
  [capture.stream startCaptureWithCompletionHandler:^(NSError *startError) {
    if (startError != nil) {
      [capture finish];
    }
  }];
  if (CMTIME_IS_VALID(self.keepaliveInterval)) {
    [capture startKeepaliveEvery:self.keepaliveInterval];
  }

  @synchronized(self) {
    [_captures addObject:capture];
  }
  return capture.signal;
}

- (void)dealloc {
  for (SCVideoCapture *capture in _captures) {
    [capture finish];
  }
}

@end
