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
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
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
#include "src/remote_display_topology.h"
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

    // A display that no longer exists, such as another app's virtual display removed while it was
    // turned off, reports this vendor. A display that's only turned off keeps its real one.
    bool is_gone(const CGDirectDisplayID id) {
      return CGDisplayVendorNumber(id) == 0xFFFFFFFF;
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

    // One configuration per display: a single display that fails, like one that's since been
    // removed, would otherwise fail them all and leave every screen off. Enabling a display that's
    // already on fails too, so skip those. Every display is attempted; only a removed one may fail.
    bool turn_on(const std::vector<CGDirectDisplayID> &ids) {
      bool restored = true;
      for (const auto id : ids) {
        if (!is_active(id) && !set_displays_enabled({id}, true) && !is_gone(id)) {
          restored = false;
        }
      }
      return restored;
    }

    bool is_active_or_gone(const CGDirectDisplayID id) {
      return is_active(id) || is_gone(id);
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

    bool same_mode(const CGDisplayModeRef a, const CGDisplayModeRef b) {
      return CGDisplayModeGetPixelWidth(a) == CGDisplayModeGetPixelWidth(b) && CGDisplayModeGetPixelHeight(a) == CGDisplayModeGetPixelHeight(b) &&
             CGDisplayModeGetWidth(a) == CGDisplayModeGetWidth(b) && CGDisplayModeGetHeight(a) == CGDisplayModeGetHeight(b);
    }

    /**
     * @brief The other displays' modes, from before a new display joins them.
     * @details macOS keeps display settings per combination of displays, so a new display gives the
     *          others their default modes and drops the user's scaling, such as More Space. So does
     *          turning a display back on, or removing one. restore() puts back any that changed.
     */
    class saved_modes_t {
    public:
      saved_modes_t() {
        for (const auto id : display_list(CGGetActiveDisplayList)) {
          if (const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
            modes.emplace_back(id, mode);
          }
        }
      }

      saved_modes_t(const saved_modes_t &) = delete;
      saved_modes_t &operator=(const saved_modes_t &) = delete;

      ~saved_modes_t() {
        for (const auto &[_, mode] : modes) {
          CGDisplayModeRelease(mode);
        }
      }

      /// @param wait How long macOS may take to make the change, which it does just after the displays change.
      void restore(const std::chrono::milliseconds wait) const {
        wait_until([this]() {
          return !changed().empty();
        },
                   wait);
        const auto displays = changed();
        CGDisplayConfigRef config;
        if (displays.empty() || CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
          return;
        }
        for (const auto &[id, mode] : displays) {
          BOOST_LOG(info) << "Virtual display: putting back the mode macOS changed on display "sv << id;
          CGConfigureDisplayWithDisplayMode(config, id, mode, nullptr);
        }
        CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly);
      }

    private:
      std::vector<std::pair<CGDirectDisplayID, CGDisplayModeRef>> changed() const {
        std::vector<std::pair<CGDirectDisplayID, CGDisplayModeRef>> result;
        for (const auto &[id, mode] : modes) {
          const CGDisplayModeRef current = CGDisplayCopyDisplayMode(id);
          if (current && is_active(id) && !same_mode(current, mode)) {
            result.emplace_back(id, mode);
          }
          if (current) {
            CGDisplayModeRelease(current);
          }
        }
        return result;
      }

      std::vector<std::pair<CGDirectDisplayID, CGDisplayModeRef>> modes;
    };

    struct virtual_display_t {
      CGVirtualDisplay *display = nil;
      CGDirectDisplayID id = kCGNullDirectDisplay;
      std::vector<CGDirectDisplayID> turned_off;  ///< Physical displays to turn back on.
      std::unique_ptr<saved_modes_t> turned_off_modes;  ///< Their modes from before.
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
        // Remote Monitors are other clients' displays, not the host's.
        if (id != display.id && !is_remote_display(id)) {
          others.push_back(id);
        }
      }
      if (others.empty()) {
        return true;
      }

      // Safeguards first: macOS won't turn these back on if Vibepollo dies.
      display.turned_off_modes = std::make_unique<saved_modes_t>();
      write_marker(others);
      start_watchdog(display, others);

      if (!set_displays_enabled(others, false)) {
        stop_watchdog(display, true);
        remove_marker();
        return false;
      }
      display.turned_off = std::move(others);
      // Let the desktop settle before capture starts: a capture set up while displays are still
      // going away can get no frames at all.
      wait_until([&display]() {
        return CGDisplayIsMain(display.id) && std::none_of(display.turned_off.begin(), display.turned_off.end(), is_active);
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

    /**
     * @brief Create a virtual display, bring it online, and pick its mode, without arranging it.
     * @param name Shown in System Settings > Displays.
     * @param serial Distinguishes the display from Vibepollo's others, so macOS remembers each one's settings.
     */
    std::unique_ptr<virtual_display_t> make_display(NSString *name, const unsigned int serial, const int width, const int height, const double refresh) {
      const Class descriptor_class = NSClassFromString(@"CGVirtualDisplayDescriptor");
      const Class display_class = NSClassFromString(@"CGVirtualDisplay");
      const Class settings_class = NSClassFromString(@"CGVirtualDisplaySettings");
      const Class mode_class = NSClassFromString(@"CGVirtualDisplayMode");
      if (!descriptor_class || !display_class || !settings_class || !mode_class) {
        BOOST_LOG(warning) << "Virtual display: not supported by this macOS"sv;
        return nullptr;
      }

      // During a dark wake no display comes online, virtual ones included.
      platf::wake_displays();
      const saved_modes_t other_modes;

      const bool hidpi = use_hidpi(width, height);
      // HiDPI modes are sized in points; their backing store, which is what gets captured, is 2x.
      const unsigned int mode_width = hidpi ? width / 2 : width;
      const unsigned int mode_height = hidpi ? height / 2 : height;
      const unsigned int pixel_width = hidpi ? mode_width * 2 : mode_width;
      const unsigned int pixel_height = hidpi ? mode_height * 2 : mode_height;

      CGVirtualDisplayDescriptor *descriptor = [[descriptor_class alloc] init];
      descriptor.queue = dispatch_queue_create("dev.vibepollo.virtual-display", DISPATCH_QUEUE_SERIAL);
      descriptor.name = name;
      descriptor.maxPixelsWide = pixel_width;
      descriptor.maxPixelsHigh = pixel_height;
      // Only informs macOS's defaults: a Retina or a standard pixel density to match the mode.
      const double ppi = hidpi ? 220.0 : 110.0;
      descriptor.sizeInMillimeters = CGSizeMake(pixel_width / ppi * 25.4, pixel_height / ppi * 25.4);
      descriptor.vendorID = vendor_id;
      descriptor.productID = product_id;
      descriptor.serialNum = serial;
      descriptor.terminationHandler = ^(id, id) {
        BOOST_LOG(warning) << "Virtual display: macOS removed the virtual display"sv;
      };

      auto result = std::make_unique<virtual_display_t>();
      result->display = [[display_class alloc] initWithDescriptor:descriptor];
      if (!result->display) {
        BOOST_LOG(error) << "Virtual display: macOS refused to create it"sv;
        return nullptr;
      }

      CGVirtualDisplaySettings *settings = [[settings_class alloc] init];
      settings.hiDPI = hidpi ? 1 : 0;
      settings.modes = @[[[mode_class alloc] initWithWidth:mode_width height:mode_height refreshRate:refresh]];
      if (![result->display applySettings:settings]) {
        BOOST_LOG(error) << "Virtual display: macOS rejected "sv << mode_width << 'x' << mode_height << '@' << refresh << "Hz"sv;
        return nullptr;
      }
      result->id = result->display.displayID;

      if (!wait_until([id = result->id]() {
            return is_active(id);
          },
                      5s)) {
        BOOST_LOG(error) << "Virtual display: it never came online"sv;
        return nullptr;
      }

      if (hidpi) {
        select_mode(result->id, pixel_width, pixel_height);
      }
      other_modes.restore(1s);
      BOOST_LOG(info) << "Virtual display: "sv << name.UTF8String << ' ' << pixel_width << 'x' << pixel_height << '@' << refresh << "Hz"sv
                      << (hidpi ? " (Retina, looks like "s + std::to_string(mode_width) + 'x' + std::to_string(mode_height) + ')' : ""s)
                      << ", display id "sv << result->id;
      return result;
    }

    std::unique_ptr<virtual_display_t> create(const video::config_t &config) {
      const double refresh = config.framerateX100 > 0 ? config.framerateX100 / 100.0 : config.framerate;
      auto result = make_display(@PROJECT_NAME, serial_number, config.width, config.height, refresh);
      if (!result) {
        BOOST_LOG(warning) << "Virtual display: streaming the physical display instead"sv;
        return nullptr;
      }
      apply_layout(*result);
      if (!result->turned_off.empty()) {
        BOOST_LOG(info) << "Virtual display: physical displays turned off"sv;
      }
      return result;
    }

    virtual_display_t::~virtual_display_t() {
      const saved_modes_t other_modes;
      bool restored = true;
      if (!turned_off.empty()) {
        // A stream can end while the Mac is half-awake, where displays never come back online.
        platf::wake_displays();
        const auto all_back = [this]() {
          return std::all_of(turned_off.begin(), turned_off.end(), is_active_or_gone);
        };
        restored = turn_on(turned_off) && wait_until(all_back, 5s);
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
      if (turned_off_modes) {
        turned_off_modes->restore(500ms);
      }
      other_modes.restore(500ms);
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

  namespace {
    std::mutex launch_hold_mutex;
    std::shared_ptr<void> launch_hold;
    std::uint64_t launch_hold_generation = 0;
  }  // namespace

  void hold_for_launch(const video::config_t &config) {
    auto held = acquire(config);
    if (!held) {
      return;
    }

    std::shared_ptr<void> previous;  // released outside the lock: releasing can take seconds
    std::uint64_t generation;
    {
      std::lock_guard lock {launch_hold_mutex};
      previous = std::exchange(launch_hold, std::move(held));
      generation = ++launch_hold_generation;
    }

    std::thread([generation]() {
      std::this_thread::sleep_for(30s);
      std::shared_ptr<void> expired;
      {
        std::lock_guard lock {launch_hold_mutex};
        if (launch_hold_generation == generation) {
          expired = std::move(launch_hold);
        }
      }
      if (expired) {
        BOOST_LOG(info) << "Virtual display: no stream started within 30 seconds of the launch; removing it"sv;
      }
    }).detach();
  }

  void end_launch_hold() {
    std::shared_ptr<void> held;
    {
      std::lock_guard lock {launch_hold_mutex};
      held = std::move(launch_hold);
      ++launch_hold_generation;
    }
  }

  std::optional<std::uint32_t> active_display_id() {
    const auto id = current_id.load();
    return id == kCGNullDirectDisplay ? std::nullopt : std::optional<std::uint32_t> {id};
  }

  namespace {
    struct remote_display_t {
      std::unique_ptr<virtual_display_t> display;
      remote_display_topology::mode_t mode;  ///< As requested.
      std::size_t pixel_width = 0;  ///< As created, which is what gets captured.
      std::size_t pixel_height = 0;
    };

    std::mutex remote_mutex;  ///< Held while displays are created, which takes seconds.
    std::map<std::string, remote_display_t> remote_displays;  ///< By client UUID.

    // The displays' IDs, for lookups from input and capture that mustn't wait on remote_mutex.
    std::mutex remote_ids_mutex;
    std::vector<CGDirectDisplayID> remote_ids;
    std::map<CGDirectDisplayID, std::pair<int, int>> remote_capture_origins;

    // Call with remote_mutex held.
    void publish_remote_ids() {
      std::vector<CGDirectDisplayID> ids;
      for (const auto &[_, remote] : remote_displays) {
        if (remote.display) {
          ids.push_back(remote.display->id);
        }
      }
      std::lock_guard lock {remote_ids_mutex};
      std::erase_if(remote_capture_origins, [&ids](const auto &entry) {
        return std::find(ids.begin(), ids.end(), entry.first) == ids.end();
      });
      remote_ids = std::move(ids);
    }

    // A serial per client (FNV-1a of its UUID), so macOS remembers each Remote Monitor's settings.
    unsigned int remote_serial(const std::string &client_uuid) {
      std::uint32_t hash = 2166136261u;
      for (const unsigned char c : client_uuid) {
        hash = (hash ^ c) * 16777619u;
      }
      return hash == serial_number ? hash + 1 : hash;
    }

    std::pair<std::size_t, std::size_t> current_pixels(const CGDirectDisplayID id) {
      const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
      if (!mode) {
        return {0, 0};
      }
      const std::pair<std::size_t, std::size_t> pixels {CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode)};
      CGDisplayModeRelease(mode);
      return pixels;
    }

    int current_refresh_hz(const CGDirectDisplayID id) {
      const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
      const double refresh = mode ? CGDisplayModeGetRefreshRate(mode) : 0;
      if (mode) {
        CGDisplayModeRelease(mode);
      }
      // Built-in and virtual displays can report 0.
      return refresh > 0 ? static_cast<int>(std::lround(refresh)) : 60;
    }

    std::string display_label(const CGDirectDisplayID id) {
      for (NSScreen *screen in NSScreen.screens) {
        NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
        if (number.unsignedIntValue == id) {
          return screen.localizedName.UTF8String;
        }
      }
      return "Display "s + std::to_string(id);
    }
  }  // namespace

  bool remote_create_or_reclaim(const std::string &client_uuid, const std::string &client_label, const remote_display_topology::mode_t &mode) {
    std::lock_guard lock {remote_mutex};
    auto &remote = remote_displays[client_uuid];
    if (remote.display && is_active(remote.display->id) && remote.mode.width == mode.width && remote.mode.height == mode.height && remote.mode.refresh_hz == mode.refresh_hz) {
      return true;
    }

    // A new mode gets a new display: two at once with the same serial would confuse macOS.
    remote.display.reset();
    publish_remote_ids();
    NSString *name = client_label.empty() ? @PROJECT_NAME " Remote Monitor" : @(client_label.c_str());
    remote.display = make_display(name, remote_serial(client_uuid), mode.width, mode.height, mode.refresh_hz);
    if (!remote.display) {
      remote_displays.erase(client_uuid);
      return false;
    }
    remote.mode = mode;
    std::tie(remote.pixel_width, remote.pixel_height) = current_pixels(remote.display->id);
    publish_remote_ids();
    return true;
  }

  void remote_resolve_mode(const std::string &, remote_display_topology::mode_t &mode) {
    // Vibepollo's macOS virtual displays are SDR.
    mode.hdr = false;
  }

  bool remote_apply_composed_topology(const std::vector<remote_display_topology::node_t> &composed) {
    std::lock_guard lock {remote_mutex};
    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) {
      return false;
    }
    for (const auto &node : composed) {
      CGDirectDisplayID id = kCGNullDirectDisplay;
      if (node.preexisting) {
        id = static_cast<CGDirectDisplayID>(std::strtoul(node.device_id.c_str(), nullptr, 10));
      } else if (const auto remote = remote_displays.find(node.id); remote != remote_displays.end() && remote->second.display) {
        id = remote->second.display->id;
      }
      if (node.active && id != kCGNullDirectDisplay && is_active(id)) {
        CGConfigureDisplayOrigin(config, id, node.x, node.y);
      }
    }
    // Like the game's virtual display, the arrangement lasts only while Vibepollo runs.
    return CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly) == kCGErrorSuccess;
  }

  std::optional<std::string> remote_exact_capture_output(const std::string &client_uuid, const remote_display_topology::mode_t &) {
    std::lock_guard lock {remote_mutex};
    const auto remote = remote_displays.find(client_uuid);
    if (remote == remote_displays.end() || !remote->second.display || !is_active(remote->second.display->id)) {
      return std::nullopt;
    }
    // Ready once the display is online in the mode it was created with; macOS offers no other.
    const auto id = remote->second.display->id;
    if (current_pixels(id) != std::pair {remote->second.pixel_width, remote->second.pixel_height}) {
      return std::nullopt;
    }
    // platf::display_names() names displays by their ID.
    return std::to_string(id);
  }

  bool remote_remove_owned_display(const std::string &client_uuid) {
    std::lock_guard lock {remote_mutex};
    remote_displays.erase(client_uuid);
    publish_remote_ids();
    return true;
  }

  std::vector<remote_display_topology::node_t> remote_baseline() {
    std::vector<remote_display_topology::node_t> baseline;
    for (const auto id : display_list(CGGetActiveDisplayList)) {
      if (is_remote_display(id)) {
        continue;
      }
      const CGRect bounds = CGDisplayBounds(id);
      const auto [pixel_width, pixel_height] = current_pixels(id);
      remote_display_topology::node_t node;
      node.id = node.device_id = std::to_string(id);
      node.label = display_label(id);
      node.preexisting = true;
      node.physical = id != current_id.load();
      node.active = true;
      node.primary = CGDisplayIsMain(id);
      // Desktop coordinates are in points, while modes are in pixels.
      node.x = static_cast<int>(bounds.origin.x);
      node.y = static_cast<int>(bounds.origin.y);
      node.configured_mode = {
        .width = static_cast<int>(pixel_width),
        .height = static_cast<int>(pixel_height),
        .refresh_hz = current_refresh_hz(id),
      };
      node.layout_width = static_cast<int>(bounds.size.width);
      node.layout_height = static_cast<int>(bounds.size.height);
      baseline.push_back(std::move(node));
    }
    return baseline;
  }

  bool is_remote_display(const std::uint32_t display_id) {
    std::lock_guard lock {remote_ids_mutex};
    return std::find(remote_ids.begin(), remote_ids.end(), display_id) != remote_ids.end();
  }

  void note_remote_capture_origin(const std::uint32_t display_id, const int x, const int y) {
    std::lock_guard lock {remote_ids_mutex};
    if (std::find(remote_ids.begin(), remote_ids.end(), display_id) != remote_ids.end()) {
      remote_capture_origins[display_id] = {x, y};
    }
  }

  std::optional<std::uint32_t> remote_display_captured_at(const int x, const int y) {
    std::lock_guard lock {remote_ids_mutex};
    for (const auto &[id, origin] : remote_capture_origins) {
      if (origin == std::pair {x, y}) {
        return id;
      }
    }
    return std::nullopt;
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
