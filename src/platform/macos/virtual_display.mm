/**
 * @file src/platform/macos/virtual_display.mm
 * @brief Per-client virtual display for macOS.
 *
 * macOS has no public API for virtual displays, so this uses the private CGVirtualDisplay classes
 * (as BetterDisplay, DeskPad, and Chromium's tests do), and the private CGSConfigureDisplayEnabled
 * to turn physical displays off for the exclusive layout. Both are looked up at runtime, so if a
 * macOS release removes them, streams fall back to the physical display instead of failing.
 *
 * Display configuration and display-mode queries only work in a process with an AppKit session,
 * which main() provides by running the AppKit event loop on the main thread.
 *
 * macOS does not turn a display back on when the process that turned it off dies, even with
 * app-only configuration scope (verified on macOS 27). The exclusive layout therefore keeps two
 * safeguards: a helper process that turns the displays back on if Vibepollo exits without doing
 * so, and a marker file of turned-off displays that the next launch restores.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// platform includes
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "virtual_display.h"

extern char **environ;

// Private CoreGraphics interfaces (macOS 11+), instantiated through NSClassFromString().
@interface CGVirtualDisplayDescriptor: NSObject
@property (retain, nonatomic) dispatch_queue_t queue;
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode: NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings: NSObject
@property (retain, nonatomic) NSArray *modes;
@property (nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay: NSObject
@property (readonly, nonatomic) unsigned int displayID;
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

using namespace std::literals;

namespace platf::macos_virtual_display {
  namespace {
    using configure_display_enabled_fn = CGError (*)(CGDisplayConfigRef, CGDirectDisplayID, bool);

    // A stable identity lets macOS remember the virtual display's arrangement between sessions.
    constexpr unsigned int vendor_id = 0x5650;  // "VP"
    constexpr unsigned int product_id = 0x0001;
    constexpr unsigned int serial_number = 0x0001;

    configure_display_enabled_fn configure_display_enabled() {
      static const auto fn = reinterpret_cast<configure_display_enabled_fn>(dlsym(RTLD_DEFAULT, "CGSConfigureDisplayEnabled"));
      return fn;
    }

    // For the restore helper, which has no event loop of its own.
    void pump_events(const std::chrono::milliseconds duration) {
      NSDate *until = [NSDate dateWithTimeIntervalSinceNow:duration.count() / 1000.0];
      while (until.timeIntervalSinceNow > 0) {
        @autoreleasepool {
          NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
          if (event) {
            [NSApp sendEvent:event];
          }
        }
      }
    }

    bool wait_until(const std::function<bool()> &condition, const std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(50ms);
      }
      return true;
    }

    std::vector<CGDirectDisplayID> display_list(CGError (*list)(uint32_t, CGDirectDisplayID *, uint32_t *)) {
      std::vector<CGDirectDisplayID> ids(32);
      uint32_t count = 0;
      if (list(static_cast<uint32_t>(ids.size()), ids.data(), &count) != kCGErrorSuccess) {
        return {};
      }
      ids.resize(count);
      return ids;
    }

    bool is_active(const CGDirectDisplayID id) {
      const auto active = display_list(CGGetActiveDisplayList);
      return std::find(active.begin(), active.end(), id) != active.end();
    }

    std::filesystem::path marker_path() {
      return platf::appdata() / "macos_displays_turned_off";
    }

    void write_marker(const std::vector<CGDirectDisplayID> &ids) {
      std::ofstream file {marker_path()};
      for (const auto id : ids) {
        file << id << '\n';
      }
    }

    std::vector<CGDirectDisplayID> read_marker() {
      std::vector<CGDirectDisplayID> ids;
      std::ifstream file {marker_path()};
      for (CGDirectDisplayID id; file >> id;) {
        ids.push_back(id);
      }
      return ids;
    }

    void remove_marker() {
      std::error_code ec;
      std::filesystem::remove(marker_path(), ec);
    }

    // Session scope, so the change doesn't depend on this process staying alive.
    bool set_displays_enabled(const std::vector<CGDirectDisplayID> &ids, const bool enabled) {
      const auto configure = configure_display_enabled();
      CGDisplayConfigRef config;
      if (!configure || CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        return false;
      }
      for (const auto id : ids) {
        configure(config, id, enabled);
      }
      return CGCompleteDisplayConfiguration(config, kCGConfigureForSession) == kCGErrorSuccess;
    }

    // Enabling a display that's already on fails the whole configuration, so skip those.
    bool turn_on(const std::vector<CGDirectDisplayID> &ids) {
      std::vector<CGDirectDisplayID> off;
      std::copy_if(ids.begin(), ids.end(), std::back_inserter(off), [](const CGDirectDisplayID id) {
        return !is_active(id);
      });
      return off.empty() || set_displays_enabled(off, true);
    }

    /**
     * @brief Map Vibepollo's display scale setting onto what macOS offers: Retina (2x) or standard (1x).
     * @details -1 (resolution-based) picks Retina above 1920x1200, where a standard-density desktop
     *          would have tiny text (phones at native resolution, 1440p, 4K); 0 keeps macOS's
     *          default (standard); otherwise 150% and up is Retina.
     */
    bool use_hidpi(const int width, const int height) {
      const int scale = config::video.dd.virtual_display_scale_percent;
      if (scale < 0) {
        return width > 1920 || height > 1200;
      }
      return scale >= 150;
    }

    struct virtual_display_t {
      CGVirtualDisplay *display = nil;
      CGDirectDisplayID id = kCGNullDirectDisplay;
      std::vector<CGDirectDisplayID> turned_off;  ///< Physical displays to turn back on.
      pid_t watchdog_pid = -1;
      int watchdog_fd = -1;  ///< Write end of the helper's stdin; EOF without "done" makes it restore.

      virtual_display_t() = default;
      virtual_display_t(const virtual_display_t &) = delete;
      virtual_display_t &operator=(const virtual_display_t &) = delete;
      ~virtual_display_t();
    };

    std::mutex state_mutex;
    std::unique_ptr<virtual_display_t> current;
    int current_users = 0;
    std::atomic<CGDirectDisplayID> current_id {kCGNullDirectDisplay};

    void start_watchdog(virtual_display_t &display, const std::vector<CGDirectDisplayID> &ids) {
      int fds[2];
      if (pipe(fds) != 0) {
        BOOST_LOG(warning) << "Virtual display: couldn't start the restore helper: "sv << std::strerror(errno);
        return;
      }
      fcntl(fds[0], F_SETFD, FD_CLOEXEC);
      fcntl(fds[1], F_SETFD, FD_CLOEXEC);
      // If the helper died first, writing "done" must fail rather than raise SIGPIPE here.
      fcntl(fds[1], F_SETNOSIGPIPE, 1);

      char executable[PATH_MAX];
      uint32_t size = sizeof(executable);
      if (_NSGetExecutablePath(executable, &size) != 0) {
        close(fds[0]);
        close(fds[1]);
        return;
      }

      std::vector<std::string> args {executable, std::string {restore_watchdog_arg}};
      for (const auto id : ids) {
        args.push_back(std::to_string(id));
      }
      std::vector<char *> argv;
      for (auto &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      // Inherit only the pipe (as stdin): an inherited listening socket would keep Vibepollo's
      // ports busy after a crash.
      posix_spawnattr_t attributes;
      posix_spawnattr_init(&attributes);
      posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);

      pid_t pid = -1;
      const int result = posix_spawn(&pid, executable, &actions, &attributes, argv.data(), environ);
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attributes);
      close(fds[0]);

      if (result != 0) {
        close(fds[1]);
        BOOST_LOG(warning) << "Virtual display: couldn't start the restore helper: "sv << std::strerror(result);
        return;
      }
      display.watchdog_pid = pid;
      display.watchdog_fd = fds[1];
    }

    void stop_watchdog(virtual_display_t &display, const bool displays_restored) {
      if (display.watchdog_fd < 0) {
        return;
      }
      if (displays_restored) {
        constexpr char done = 'd';
        (void) write(display.watchdog_fd, &done, 1);
      }
      // Without "done", the helper turns the displays back on itself.
      close(display.watchdog_fd);
      display.watchdog_fd = -1;
      wait_until([&display]() {
        return waitpid(display.watchdog_pid, nullptr, WNOHANG) != 0;
      },
                 3s);
      display.watchdog_pid = -1;
    }

    bool turn_off_other_displays(virtual_display_t &display) {
      if (!configure_display_enabled()) {
        return false;
      }

      std::vector<CGDirectDisplayID> others;
      for (const auto id : display_list(CGGetOnlineDisplayList)) {
        if (id != display.id) {
          others.push_back(id);
        }
      }
      if (others.empty()) {
        return true;
      }

      // Safeguards first: macOS won't turn these back on if Vibepollo dies.
      write_marker(others);
      start_watchdog(display, others);

      if (!set_displays_enabled(others, false)) {
        stop_watchdog(display, true);
        remove_marker();
        return false;
      }
      display.turned_off = std::move(others);
      wait_until([&display]() {
        return CGDisplayIsMain(display.id);
      },
                 5s);
      return true;
    }

    void make_main(const CGDirectDisplayID id) {
      // The display at (0, 0) is the main one; keep the others to its right in their current order.
      const auto others = display_list(CGGetActiveDisplayList);
      double min_x = 0;
      for (const auto other : others) {
        if (other != id) {
          min_x = std::min(min_x, CGDisplayBounds(other).origin.x);
        }
      }
      const double shift = CGDisplayBounds(id).size.width - min_x;

      CGDisplayConfigRef config;
      if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
        return;
      }
      CGConfigureDisplayOrigin(config, id, 0, 0);
      for (const auto other : others) {
        if (other != id) {
          const CGRect bounds = CGDisplayBounds(other);
          CGConfigureDisplayOrigin(config, other, static_cast<int32_t>(bounds.origin.x + shift), static_cast<int32_t>(bounds.origin.y));
        }
      }
      CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
    }

    void apply_layout(virtual_display_t &display) {
      using layout_e = config::video_t::virtual_display_layout_e;
      const auto layout = config::video.virtual_display_layout;

      if (layout == layout_e::exclusive) {
        if (turn_off_other_displays(display)) {
          return;
        }
        BOOST_LOG(warning) << "Virtual display: couldn't turn off the other displays; making it the main display instead"sv;
      }
      // macOS keeps displays adjacent, so the isolated layouts behave like their plain versions.
      if (layout == layout_e::extended || layout == layout_e::extended_isolated) {
        return;
      }
      make_main(display.id);
    }

    // With HiDPI, macOS defaults to the standard-density variant of the mode, so pick the exact pixel size.
    void select_mode(const CGDirectDisplayID id, const size_t pixel_width, const size_t pixel_height) {
      NSDictionary *options = @{(__bridge NSString *) kCGDisplayShowDuplicateLowResolutionModes: @YES};
      CGDisplayModeRef chosen = nullptr;
      wait_until([&]() {
        const CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef) options);
        if (!modes) {
          return false;
        }
        for (CFIndex i = 0; i < CFArrayGetCount(modes) && !chosen; ++i) {
          const auto mode = (CGDisplayModeRef) CFArrayGetValueAtIndex(modes, i);
          if (CGDisplayModeGetPixelWidth(mode) == pixel_width && CGDisplayModeGetPixelHeight(mode) == pixel_height) {
            chosen = CGDisplayModeRetain(mode);
          }
        }
        CFRelease(modes);
        return chosen != nullptr;
      },
                 3s);

      if (!chosen) {
        BOOST_LOG(warning) << "Virtual display: no "sv << pixel_width << 'x' << pixel_height << " mode; using macOS's default"sv;
        return;
      }
      CGDisplayConfigRef config;
      if (CGBeginDisplayConfiguration(&config) == kCGErrorSuccess) {
        CGConfigureDisplayWithDisplayMode(config, id, chosen, nullptr);
        CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
      }
      CGDisplayModeRelease(chosen);
    }

    std::unique_ptr<virtual_display_t> create(const video::config_t &config) {
      const Class descriptor_class = NSClassFromString(@"CGVirtualDisplayDescriptor");
      const Class display_class = NSClassFromString(@"CGVirtualDisplay");
      const Class settings_class = NSClassFromString(@"CGVirtualDisplaySettings");
      const Class mode_class = NSClassFromString(@"CGVirtualDisplayMode");
      if (!descriptor_class || !display_class || !settings_class || !mode_class) {
        BOOST_LOG(warning) << "Virtual display: not supported by this macOS; streaming the physical display"sv;
        return nullptr;
      }

      const bool hidpi = use_hidpi(config.width, config.height);
      // HiDPI modes are sized in points; their backing store, which is what gets captured, is 2x.
      const unsigned int mode_width = hidpi ? config.width / 2 : config.width;
      const unsigned int mode_height = hidpi ? config.height / 2 : config.height;
      const unsigned int pixel_width = hidpi ? mode_width * 2 : mode_width;
      const unsigned int pixel_height = hidpi ? mode_height * 2 : mode_height;
      const double refresh = config.framerateX100 > 0 ? config.framerateX100 / 100.0 : config.framerate;

      CGVirtualDisplayDescriptor *descriptor = [[descriptor_class alloc] init];
      descriptor.queue = dispatch_queue_create("dev.vibepollo.virtual-display", DISPATCH_QUEUE_SERIAL);
      descriptor.name = @PROJECT_NAME;
      descriptor.maxPixelsWide = pixel_width;
      descriptor.maxPixelsHigh = pixel_height;
      // Only informs macOS's defaults: a Retina or a standard pixel density to match the mode.
      const double ppi = hidpi ? 220.0 : 110.0;
      descriptor.sizeInMillimeters = CGSizeMake(pixel_width / ppi * 25.4, pixel_height / ppi * 25.4);
      descriptor.vendorID = vendor_id;
      descriptor.productID = product_id;
      descriptor.serialNum = serial_number;
      descriptor.terminationHandler = ^(id, id) {
        BOOST_LOG(warning) << "Virtual display: macOS removed the virtual display"sv;
      };

      auto result = std::make_unique<virtual_display_t>();
      result->display = [[display_class alloc] initWithDescriptor:descriptor];
      if (!result->display) {
        BOOST_LOG(error) << "Virtual display: macOS refused to create it; streaming the physical display"sv;
        return nullptr;
      }

      CGVirtualDisplaySettings *settings = [[settings_class alloc] init];
      settings.hiDPI = hidpi ? 1 : 0;
      settings.modes = @[[[mode_class alloc] initWithWidth:mode_width height:mode_height refreshRate:refresh]];
      if (![result->display applySettings:settings]) {
        BOOST_LOG(error) << "Virtual display: macOS rejected "sv << mode_width << 'x' << mode_height << '@' << refresh << "; streaming the physical display"sv;
        return nullptr;
      }
      result->id = result->display.displayID;

      if (!wait_until([id = result->id]() {
            return is_active(id);
          },
                      5s)) {
        BOOST_LOG(error) << "Virtual display: it never came online; streaming the physical display"sv;
        return nullptr;
      }

      if (hidpi) {
        select_mode(result->id, pixel_width, pixel_height);
      }
      apply_layout(*result);

      BOOST_LOG(info) << "Virtual display: "sv << pixel_width << 'x' << pixel_height << '@' << refresh << "Hz"sv
                      << (hidpi ? " (Retina, looks like "s + std::to_string(mode_width) + 'x' + std::to_string(mode_height) + ')' : ""s)
                      << ", display id "sv << result->id
                      << (result->turned_off.empty() ? ""sv : ", physical displays turned off"sv);
      return result;
    }

    virtual_display_t::~virtual_display_t() {
      bool restored = true;
      if (!turned_off.empty()) {
        restored = turn_on(turned_off) && wait_until([this]() {
          return std::all_of(turned_off.begin(), turned_off.end(), is_active);
        },
                                                                        5s);
        if (restored) {
          remove_marker();
          BOOST_LOG(info) << "Virtual display: physical displays turned back on"sv;
        } else {
          BOOST_LOG(error) << "Virtual display: couldn't turn the physical displays back on; the restore helper will retry"sv;
        }
      }
      stop_watchdog(*this, restored);

      display = nil;
      if (id != kCGNullDirectDisplay) {
        wait_until([id = id]() {
          return !is_active(id);
        },
                   3s);
      }
    }

    void release() {
      std::lock_guard lock {state_mutex};
      if (--current_users > 0) {
        return;
      }
      current_id = kCGNullDirectDisplay;
      current.reset();
    }
  }  // namespace

  std::shared_ptr<void> acquire(const video::config_t &config) {
    if (config::video.virtual_display_mode == config::video_t::virtual_display_mode_e::disabled) {
      return nullptr;
    }

    std::lock_guard lock {state_mutex};
    if (!current) {
      current = create(config);
      if (!current) {
        return nullptr;
      }
      current_id = current->id;
    }
    ++current_users;

    static int token;
    return std::shared_ptr<void>(&token, [](void *) {
      release();
    });
  }

  std::optional<std::uint32_t> active_display_id() {
    const auto id = current_id.load();
    return id == kCGNullDirectDisplay ? std::nullopt : std::optional<std::uint32_t> {id};
  }

  void recover_disabled_displays() {
    dispatch_async(dispatch_get_main_queue(), ^{
      const auto ids = read_marker();
      if (ids.empty()) {
        return;
      }
      BOOST_LOG(warning) << "Virtual display: turning back on displays a previous run left off"sv;
      if (turn_on(ids)) {
        remove_marker();
      } else {
        BOOST_LOG(error) << "Virtual display: couldn't turn them back on; logging out and back in will"sv;
      }
    });
  }

  int run_restore_watchdog(int argc, char *argv[]) {
    // Survive "killall Vibepollo" and hangups: the main process's exit is what this waits for.
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGTERM, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);

    std::vector<CGDirectDisplayID> ids;
    for (int i = 2; i < argc; ++i) {
      ids.push_back(static_cast<CGDirectDisplayID>(std::strtoul(argv[i], nullptr, 10)));
    }

    char message = 0;
    ssize_t count;
    do {
      count = read(STDIN_FILENO, &message, 1);
    } while (count < 0 && errno == EINTR);
    if (count == 1 && message == 'd') {
      return 0;  // Vibepollo turned them back on itself
    }

    // Vibepollo exited without restoring them. Display configuration needs an AppKit session.
    ensure_appkit_session();
    pump_events(500ms);
    const bool restored = turn_on(ids);
    pump_events(1s);
    if (restored) {
      remove_marker();
    }
    return restored ? 0 : 1;
  }
}  // namespace platf::macos_virtual_display
