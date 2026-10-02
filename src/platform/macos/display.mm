/**
 * @file src/platform/macos/display.mm
 * @brief Definitions for display capture on macOS.
 */
// standard includes
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

// platform includes
#import <AppKit/AppKit.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/av_video.h"
#include "src/platform/macos/misc.h"
#include "src/platform/macos/nv12_zero_device.h"
#include "src/platform/macos/sc_video.h"
#include "src/platform/macos/virtual_display.h"

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "src/video.h"
#undef AVMediaType

namespace fs = std::filesystem;

namespace platf {
  using namespace std::literals;

  namespace {
    /**
     * @brief State shared by a capture call and its frame callback.
     * @details The callback runs on the capture queue and can fire after the call has timed out and
     *          returned, so it checks `stopped` under the lock before touching anything the call owns.
     */
    struct capture_state_t {
      std::mutex mutex;
      bool stopped = false;
      bool delivered = false;
      std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    };

    // SDR white in ScreenCaptureKit's HDR frames, measured from an HDR virtual display on macOS 27.
    constexpr double sdr_white_nits = 140.0;
    // macOS 27 gives an HDR virtual display this much EDR headroom.
    constexpr double virtual_display_headroom = 5.0;
    // The brightest Apple displays peak at 1600 nits.
    constexpr double max_peak_nits = 1600.0;

    /**
     * @brief How far above SDR white a display can go: more than 1 for an HDR display.
     */
    double edr_headroom(const CGDirectDisplayID display_id) {
      @autoreleasepool {
        for (NSScreen *screen in NSScreen.screens) {
          if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == display_id) {
            return screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
          }
        }
      }
      return 1.0;
    }
  }  // namespace

  struct av_display_t: public display_t {
    AVVideo *av_capture {};
    CGDirectDisplayID display_id {};
    bool hdr = false;
    double headroom = 1.0;  ///< The display's EDR headroom, while hdr.

    ~av_display_t() override {
      [av_capture release];
    }

    bool is_hdr() override {
      return hdr;
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      std::memset(&metadata, 0, sizeof(metadata));
      if (!hdr) {
        return false;
      }
      // Display P3 primaries (red, green, blue) and a D65 white point, normalized to 50,000: the
      // gamut of Apple's HDR displays and of Vibepollo's HDR virtual display.
      metadata.displayPrimaries[0] = {34000, 16000};
      metadata.displayPrimaries[1] = {13250, 34500};
      metadata.displayPrimaries[2] = {7500, 3000};
      metadata.whitePoint = {15635, 16450};
      metadata.maxDisplayLuminance = peak_nits();
      metadata.minDisplayLuminance = 50;  // 0.005 nits
      metadata.maxContentLightLevel = metadata.maxDisplayLuminance;
      metadata.maxFrameAverageLightLevel = static_cast<std::uint16_t>(sdr_white_nits);
      return true;
    }

    /// The display's peak brightness in the captured frames: its EDR headroom over SDR white.
    std::uint16_t peak_nits() const {
      return static_cast<std::uint16_t>(std::lround(std::min(headroom * sdr_white_nits, max_peak_nits)));
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      // Keep the display awake while capturing, as the Windows capture does: a sleeping display
      // sends no frames.
      IOPMAssertionID keep_awake = kIOPMNullAssertionID;
      IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, CFSTR("Vibepollo is streaming the display"), &keep_awake);
      auto release_keep_awake = util::fail_guard([keep_awake]() {
        IOPMAssertionRelease(keep_awake);
      });

      auto state = std::make_shared<capture_state_t>();
      // Copied to the heap: the capture can hold on to it after this function returns.
      FrameCallbackBlock on_frame = [^bool(CMSampleBufferRef sampleBuffer) {
        std::lock_guard lock {state->mutex};
        if (state->stopped) {
          return false;
        }

        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          // got interrupt signal
          // returning false here stops capture backend
          state->stopped = true;
          return false;
        }
        auto av_img = std::static_pointer_cast<av_img_t>(img_out);

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img_out->data
        );

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img_out->data = new_pixel_buffer->data();

        img_out->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img_out->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img_out->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img_out->pixel_pitch = img_out->row_pitch / img_out->width;

        old_data_retainer = nullptr;

        if (!push_captured_image_cb(std::move(img_out), true)) {
          // got interrupt signal
          // returning false here stops capture backend
          state->stopped = true;
          return false;
        }

        state->last_frame = std::chrono::steady_clock::now();
        return true;
      } copy];
      auto signal = [av_capture capture:on_frame];
      [on_frame release];  // the capture keeps its own reference
      if (signal == nil) {
        return capture_e::error;
      }

      // Frames stop while the display sleeps. After a second without one, re-send the last image
      // every second, as the Linux captures do on a timeout: the stream stays up, and a stop
      // request is still seen.
      for (;;) {
        std::chrono::nanoseconds until_resend;
        {
          std::lock_guard lock {state->mutex};
          until_resend = state->last_frame + 1s - std::chrono::steady_clock::now();
        }
        if (until_resend > 0ns && dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW, until_resend.count())) == 0) {
          break;
        }

        std::lock_guard lock {state->mutex};
        if (state->stopped) {
          return capture_e::ok;
        }
        if (std::chrono::steady_clock::now() - state->last_frame < 1s) {
          continue;  // a frame arrived while waiting
        }
        std::shared_ptr<img_t> img_out;
        if (!pull_free_image_cb(img_out) || !push_captured_image_cb(std::move(img_out), false)) {
          state->stopped = true;
          return capture_e::ok;
        }
        state->last_frame = std::chrono::steady_clock::now();
      }

      // The capture signals when a callback stops it, or when it stops by itself: ScreenCaptureKit
      // ends a stream on errors, such as its display going away. Start over in that case.
      std::lock_guard lock {state->mutex};
      if (!state->stopped) {
        state->stopped = true;
        BOOST_LOG(warning) << "Capture of display "sv << display_id << " stopped by itself; restarting it"sv;
        return capture_e::reinit;
      }
      return capture_e::ok;
    }

    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        av_capture.pixelFormat = kCVPixelFormatType_32BGRA;

        return std::make_unique<avcodec_encode_device_t>();
      } else if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();

        device->init(static_cast<void *>(av_capture), pix_fmt, setResolution, setPixelFormat);

        return device;
      } else {
        BOOST_LOG(error) << "Unsupported Pixel Format."sv;
        return nullptr;
      }
    }

    int dummy_img(img_t *img) override {
      if (!platf::is_screen_capture_allowed()) {
        // If we don't have the screen capture permission, this function will hang
        // indefinitely without doing anything useful. Exit instead to avoid this.
        // A non-zero return value indicates failure to the calling function.
        return 1;
      }

      auto state = std::make_shared<capture_state_t>();
      FrameCallbackBlock on_frame = [^bool(CMSampleBufferRef sampleBuffer) {
        std::lock_guard lock {state->mutex};
        if (state->stopped) {
          return false;
        }

        auto new_sample_buffer = std::make_shared<av_sample_buf_t>(sampleBuffer);
        auto new_pixel_buffer = std::make_shared<av_pixel_buf_t>(new_sample_buffer->buf);

        auto av_img = (av_img_t *) img;

        auto old_data_retainer = std::make_shared<temp_retain_av_img_t>(
          av_img->sample_buffer,
          av_img->pixel_buffer,
          img->data
        );

        av_img->sample_buffer = new_sample_buffer;
        av_img->pixel_buffer = new_pixel_buffer;
        img->data = new_pixel_buffer->data();

        img->width = (int) CVPixelBufferGetWidth(new_pixel_buffer->buf);
        img->height = (int) CVPixelBufferGetHeight(new_pixel_buffer->buf);
        img->row_pitch = (int) CVPixelBufferGetBytesPerRow(new_pixel_buffer->buf);
        img->pixel_pitch = img->row_pitch / img->width;

        old_data_retainer = nullptr;

        state->delivered = true;
        // returning false here stops capture backend
        return false;
      } copy];
      auto signal = [av_capture capture:on_frame];
      [on_frame release];  // the capture keeps its own reference
      if (signal == nil) {
        return 1;
      }

      // A display that's asleep sends nothing: fail, so the client gets an error instead of a
      // connection that hangs forever. The capture can also end by itself without an image.
      dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
      std::lock_guard lock {state->mutex};
      state->stopped = true;
      if (!state->delivered) {
        BOOST_LOG(error) << "Display "sv << display_id << " sent no image within 3 seconds"sv;
        return 1;
      }

      return 0;
    }

    /**
     * A bridge from the pure C++ code of the hwdevice_t class to the pure Objective C code.
     *
     * display --> an opaque pointer to an object of this class
     * width --> the intended capture width
     * height --> the intended capture height
     */
    static void setResolution(void *display, int width, int height) {
      [static_cast<AVVideo *>(display) setFrameWidth:width frameHeight:height];
    }

    static void setPixelFormat(void *display, OSType pixelFormat) {
      static_cast<AVVideo *>(display).pixelFormat = pixelFormat;
    }
  };

  /**
   * @brief Black frames for Remote Input, which needs a video stream but shows no display.
   * @details One black image in the format and size the encoder asks for, paced by
   *          capture_synthetic_black() like the other platforms' synthetic sources.
   */
  struct black_display_t: public display_t {
    explicit black_display_t(const video::config_t &config):
        frame_rate {std::max(1, config.framerate)} {
      width = logical_width = env_width = env_logical_width = std::max(1, config.width);
      height = logical_height = env_height = env_logical_height = std::max(1, config.height);
    }

    ~black_display_t() override {
      if (black) {
        CFRelease(black);
      }
    }

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *) override {
      return capture_synthetic_black(push_captured_image_cb, pull_free_image_cb, frame_rate);
    }

    std::shared_ptr<img_t> alloc_img() override {
      return std::make_shared<av_img_t>();
    }

    int dummy_img(img_t *img) override {
      std::lock_guard lock {mutex};
      if (!black) {
        black = make_black_sample();
        if (!black) {
          return 1;
        }
      }

      auto av_img = static_cast<av_img_t *>(img);
      av_img->sample_buffer = std::make_shared<av_sample_buf_t>(black);
      av_img->pixel_buffer = std::make_shared<av_pixel_buf_t>(av_img->sample_buffer->buf);
      img->data = av_img->pixel_buffer->data();
      img->width = (int) CVPixelBufferGetWidth(av_img->pixel_buffer->buf);
      img->height = (int) CVPixelBufferGetHeight(av_img->pixel_buffer->buf);
      img->row_pitch = (int) CVPixelBufferGetBytesPerRow(av_img->pixel_buffer->buf);
      img->pixel_pitch = img->row_pitch / img->width;
      return 0;
    }

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
      if (pix_fmt == pix_fmt_e::yuv420p) {
        set_format(kCVPixelFormatType_32BGRA, width, height);
        return std::make_unique<avcodec_encode_device_t>();
      }
      if (pix_fmt == pix_fmt_e::nv12 || pix_fmt == pix_fmt_e::p010) {
        auto device = std::make_unique<nv12_zero_device>();
        device->init(
          this,
          pix_fmt,
          [](void *display, int frame_width, int frame_height) {
            auto self = static_cast<black_display_t *>(display);
            self->set_format(self->pixel_format, frame_width, frame_height);
          },
          [](void *display, int format) {
            auto self = static_cast<black_display_t *>(display);
            self->set_format(static_cast<OSType>(format), self->width, self->height);
          }
        );
        return device;
      }
      BOOST_LOG(error) << "Unsupported Pixel Format."sv;
      return nullptr;
    }

  private:
    void set_format(const OSType format, const int frame_width, const int frame_height) {
      std::lock_guard lock {mutex};
      if (format == pixel_format && frame_width == width && frame_height == height) {
        return;
      }
      pixel_format = format;
      width = frame_width;
      height = frame_height;
      if (black) {
        CFRelease(black);
        black = nullptr;
      }
    }

    // An IOSurface-backed buffer, which VideoToolbox encodes without copying.
    CMSampleBufferRef make_black_sample() const {
      NSDictionary *attributes = [NSDictionary dictionaryWithObject:[NSDictionary dictionary] forKey:(NSString *) kCVPixelBufferIOSurfacePropertiesKey];
      CVPixelBufferRef buffer = nullptr;
      if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixel_format, (__bridge CFDictionaryRef) attributes, &buffer) != kCVReturnSuccess) {
        BOOST_LOG(error) << "Couldn't create a black image for Remote Input"sv;
        return nullptr;
      }

      CVPixelBufferLockBaseAddress(buffer, 0);
      if (CVPixelBufferIsPlanar(buffer)) {
        // Video-range black: luma 16 and chroma 128, or 64 and 512 in the high bits of 10-bit samples.
        const bool ten_bit = pixel_format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
        for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(buffer); ++plane) {
          auto *base = static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
          const size_t size = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane) * CVPixelBufferGetHeightOfPlane(buffer, plane);
          if (ten_bit) {
            std::fill_n(reinterpret_cast<std::uint16_t *>(base), size / 2, static_cast<std::uint16_t>((plane == 0 ? 64 : 512) << 6));
          } else {
            std::memset(base, plane == 0 ? 16 : 128, size);
          }
        }
      } else {
        std::memset(CVPixelBufferGetBaseAddress(buffer), 0, CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer));
      }
      CVPixelBufferUnlockBaseAddress(buffer, 0);

      CMVideoFormatDescriptionRef format_description = nullptr;
      CMSampleBufferRef sample = nullptr;
      const CMSampleTimingInfo timing {kCMTimeInvalid, kCMTimeZero, kCMTimeInvalid};
      if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer, &format_description) == noErr) {
        CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, buffer, format_description, &timing, &sample);
        CFRelease(format_description);
      }
      CVPixelBufferRelease(buffer);
      return sample;
    }

    std::mutex mutex;
    OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CMSampleBufferRef black = nullptr;
    int frame_rate;
  };

  std::shared_ptr<display_t> black_display(const video::config_t &config) {
    return std::make_shared<black_display_t>(config);
  }

  std::shared_ptr<display_t> display(
    platf::mem_type_e hwdevice_type,
    const std::string &display_name,
    const video::config_t &config,
    const std::optional<adapter_id_t> &required_adapter
  ) {
    (void) required_adapter;
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::videotoolbox) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    // Like the Windows capture, power the displays on if they're asleep: a client connecting to a
    // sleeping Mac only half-wakes it, and a sleeping display sends no frames.
    wake_displays();

    auto display = std::make_shared<av_display_t>();

    // Default to main display
    display->display_id = CGMainDisplayID();

    // Print all displays available with it's name and id
    auto display_array = [AVVideo displayNames];
    BOOST_LOG(info) << "Detecting displays"sv;
    for (NSDictionary *item in display_array) {
      NSNumber *display_id = item[@"id"];
      // We need show display's product name and corresponding display number given by user
      NSString *name = item[@"displayName"];
      // We are using CGGetActiveDisplayList that only returns active displays so hardcoded connected value in log to true
      BOOST_LOG(info) << "Detected display: "sv << name.UTF8String << " (id: "sv << [NSString stringWithFormat:@"%@", display_id].UTF8String << ") connected: true"sv;
      if (!display_name.empty() && std::atoi(display_name.c_str()) == [display_id unsignedIntValue]) {
        display->display_id = [display_id unsignedIntValue];
      }
    }
    // A Remote Monitor stream captures its own display. For any other stream, the stream's virtual
    // display, while there is one, takes precedence over output_name.
    const bool remote_monitor = macos_virtual_display::is_remote_display(display->display_id);
    if (const auto virtual_id = macos_virtual_display::active_display_id(); virtual_id && !remote_monitor) {
      display->display_id = *virtual_id;
    }
    BOOST_LOG(info) << "Configuring selected display ("sv << display->display_id << ") to stream"sv;

    // HDR needs ScreenCaptureKit's HDR capture, from macOS 15, and an HDR display: Vibepollo's HDR
    // virtual display, or a physical one with EDR headroom, such as a MacBook Pro's.
    if (config.dynamicRange > 0 && !config.prefer_sdr_10bit && !config.force_sdr) {
      if (@available(macOS 15.0, *)) {
        display->headroom = edr_headroom(display->display_id);
        if (macos_virtual_display::is_hdr_display(display->display_id)) {
          // AppKit can lag behind a display that was just created.
          display->headroom = std::max(display->headroom, virtual_display_headroom);
        }
        display->hdr = display->headroom > 1.0;
      }
      if (display->hdr) {
        BOOST_LOG(info) << "Capturing HDR, up to "sv << display->peak_nits() << " nits"sv;
      } else {
        BOOST_LOG(info) << "Display "sv << display->display_id << " can't be captured in HDR, so the stream is SDR"sv;
      }
    }

    // AVCaptureScreenInput delivers no frames from virtual displays, nor HDR; ScreenCaptureKit does.
    if (remote_monitor || macos_virtual_display::active_display_id() == display->display_id || display->hdr) {
      SCVideo *capture = [[SCVideo alloc] initWithDisplay:display->display_id frameRate:config.framerate];
      capture.hdr = display->hdr;
      // The sync encode path only encodes delivered frames, so enforce the minimum frame rate here
      // (the same default as video.cpp: a fifth of the stream's rate, at least 10 fps).
      const double minimum_fps = config::video.minimum_fps_target > 0 ? config::video.minimum_fps_target : std::max(config.framerate / 5.0, 10.0);
      capture.keepaliveInterval = CMTimeMakeWithSeconds(1.0 / minimum_fps, 1000000);
      display->av_capture = capture;
    } else {
      display->av_capture = [[AVVideo alloc] initWithDisplay:display->display_id frameRate:config.framerate];
    }

    if (!display->av_capture) {
      BOOST_LOG(error) << "Video setup failed."sv;
      return nullptr;
    }

    display->width = display->av_capture.frameWidth;
    display->height = display->av_capture.frameHeight;
    // We also need set env_width and env_height for absolute mouse coordinates
    display->env_width = display->width;
    display->env_height = display->height;
    // Where the display sits on the desktop, in points. Input sends each stream's pointer to the
    // display at its offset, so a Remote Monitor client's clicks land on its own display.
    const CGRect bounds = CGDisplayBounds(display->display_id);
    display->offset_x = static_cast<int>(bounds.origin.x);
    display->offset_y = static_cast<int>(bounds.origin.y);
    if (remote_monitor) {
      macos_virtual_display::note_remote_capture_origin(display->display_id, display->offset_x, display->offset_y);
    }

    return display;
  }

  std::vector<std::string> display_names(mem_type_e hwdevice_type) {
    __block std::vector<std::string> display_names;

    auto display_array = [AVVideo displayNames];

    display_names.reserve([display_array count]);
    [display_array enumerateObjectsUsingBlock:^(NSDictionary *_Nonnull obj, NSUInteger idx, BOOL *_Nonnull stop) {
      NSString *name = obj[@"name"];
      display_names.emplace_back(name.UTF8String);
    }];

    return display_names;
  }

  /**
   * @brief Returns if GPUs/drivers have changed since the last call to this function.
   * @return `true` if a change has occurred or if it is unknown whether a change occurred.
   */
  bool needs_encoder_reenumeration() {
    // We don't track GPU state, so we will always reenumerate. Fortunately, it is fast on macOS.
    return true;
  }
}  // namespace platf
