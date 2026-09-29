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
 */
@interface SCVideoCapture: NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, strong) SCStream *stream;
@property (nonatomic, copy) FrameCallbackBlock callback;
@property (nonatomic, strong) dispatch_semaphore_t signal;
- (void)finish;
@end

@implementation SCVideoCapture {
  CMSampleBufferRef _lastFrame;  ///< Last buffer with an image, re-sent on idle ticks.
  BOOL _finished;
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || _finished) {
    return;
  }

  CMSampleBufferRef frame = sampleBuffer;
  if (CMSampleBufferGetImageBuffer(sampleBuffer) == NULL) {
    // ScreenCaptureKit only sends images when the screen changes, plus image-less idle ticks.
    // Re-send the last image on those, keeping the steady cadence AVCaptureScreenInput had:
    // keyframe requests from the client need a frame to encode even on a static desktop.
    if (_lastFrame == NULL) {
      return;
    }
    frame = _lastFrame;
  } else {
    if (_lastFrame != NULL) {
      CFRelease(_lastFrame);
    }
    _lastFrame = (CMSampleBufferRef) CFRetain(sampleBuffer);
  }

  if (!self.callback(frame)) {
    [self finish];
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  [self finish];
}

- (void)finish {
  @synchronized(self) {
    if (_finished) {
      return;
    }
    _finished = YES;
  }
  [self.stream stopCaptureWithCompletionHandler:nil];
  dispatch_semaphore_signal(self.signal);
}

- (void)dealloc {
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
  configuration.minimumFrameInterval = self.minFrameDuration;
  configuration.showsCursor = YES;
  // The pipeline holds a few frames and the last one is kept for idle ticks, so leave room.
  configuration.queueDepth = 8;

  SCVideoCapture *capture = [[SCVideoCapture alloc] init];
  capture.callback = frameCallback;
  capture.signal = dispatch_semaphore_create(0);

  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:_display excludingWindows:@[]];
  capture.stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:capture];

  dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, DISPATCH_QUEUE_PRIORITY_HIGH);
  NSError *error = nil;
  if (![capture.stream addStreamOutput:capture type:SCStreamOutputTypeScreen sampleHandlerQueue:dispatch_queue_create("videoCaptureQueue", qos) error:&error]) {
    return nil;
  }
  [capture.stream startCaptureWithCompletionHandler:^(NSError *startError) {
    if (startError != nil) {
      [capture finish];
    }
  }];

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
