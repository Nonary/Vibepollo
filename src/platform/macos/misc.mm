/**
 * @file src/platform/macos/misc.mm
 * @brief Miscellaneous definitions for macOS platform.
 */

// Required for IPV6_PKTINFO with Darwin headers
#ifndef __APPLE_USE_RFC_3542  // NOLINT(bugprone-reserved-identifier)
  #define __APPLE_USE_RFC_3542 1
#endif

// standard includes
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// platform includes
#include <AppKit/AppKit.h>
#include <arpa/inet.h>
#include <crt_externs.h>
#include <dlfcn.h>
#include <Foundation/Foundation.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach-o/dyld.h>
#include <net/if_dl.h>
#include <pwd.h>
#include <ScreenCaptureKit/ScreenCaptureKit.h>
#include <ServiceManagement/ServiceManagement.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/qos.h>
#include <sys/wait.h>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/program_options/parsers.hpp>

// local includes
#include "misc.h"
#include "src/platform/common_services.h"
#include "src/boost_process_shim.h"
#include "src/entry_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;
namespace fs = std::filesystem;
namespace bp = boost_process_shim;
namespace v2 = boost::process::v2;

namespace platf {

// Even though the following two functions are available starting in macOS 10.15, they weren't
// actually in the Mac SDK until Xcode 12.2, the first to include the SDK for macOS 11
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 110000  // __MAC_11_0
  // If they're not in the SDK then we can use our own function definitions.
  // Need to use weak import so that this will link in macOS 10.14 and earlier
  extern "C" bool CGPreflightScreenCaptureAccess(void) __attribute__((weak_import));
  extern "C" bool CGRequestScreenCaptureAccess(void) __attribute__((weak_import));
#endif

  namespace {
    auto screen_capture_allowed = std::atomic<bool> {false};
  }  // namespace

  // Return whether screen capture is allowed for this process.
  bool is_screen_capture_allowed() {
    return screen_capture_allowed;
  }

  void ensure_appkit_session() {
    static std::once_flag once;
    std::call_once(once, []() {
      [NSApplication sharedApplication];
      [NSApp finishLaunching];
    });
  }

  void run_main_event_loop(const std::function<bool()> &should_exit) {
    ensure_appkit_session();
    while (!should_exit()) {
      @autoreleasepool {
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:0.5];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        if (event != nil) {
          [NSApp sendEvent:event];
        }
      }
    }
  }

  namespace {
    SMAppService *login_agent() API_AVAILABLE(macos(13.0)) {
      // Contents/Library/LaunchAgents/<this>, whose Label is also PROJECT_FQDN.
      return [SMAppService agentServiceWithPlistName:@PROJECT_FQDN ".plist"];
    }

    // launchd stops the login agent when it's unregistered, so carry on in a copy of our own.
    void launch_replacement() {
      NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
      configuration.createsNewApplicationInstance = YES;
      configuration.activates = NO;
      [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL
                                          configuration:configuration
                                      completionHandler:^(NSRunningApplication *, NSError *launch_error) {
                                        if (launch_error != nil) {
                                          BOOST_LOG(error) << "Couldn't start Vibepollo again without Open at Login: "sv << launch_error.localizedDescription.UTF8String;
                                        }
                                      }];
    }

    int run(const std::vector<std::string> &args) {
      std::vector<char *> argv;
      for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
      }
      argv.push_back(nullptr);

      pid_t pid;
      if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), *_NSGetEnviron()) != 0) {
        return -1;
      }
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
  }  // namespace

  bool is_login_agent() {
    // launchd names the job it started in XPC_SERVICE_NAME. Restarting from the menu bar
    // re-executes in place, so it survives that too.
    const char *service = getenv("XPC_SERVICE_NAME");
    return service != nullptr && std::string_view {service} == PROJECT_FQDN;
  }

  bool opens_at_login() {
    if (@available(macOS 13.0, *)) {
      return login_agent().status == SMAppServiceStatusEnabled;
    }
    return false;
  }

  bool set_opens_at_login(bool enabled) {
    if (@available(macOS 13.0, *)) {
      SMAppService *agent = login_agent();
      NSError *error = nil;
      if (enabled) {
        // launchd starts the agent right away, and it takes over from this copy.
        if (![agent registerAndReturnError:&error]) {
          BOOST_LOG(warning) << "Couldn't turn on Open at Login: "sv << error.localizedDescription.UTF8String;
        }

        // If the user turned Vibepollo off in System Settings, only they can turn it back on.
        if (agent.status == SMAppServiceStatusRequiresApproval) {
          BOOST_LOG(info) << "Opening Login Items so the user can allow Vibepollo in the background"sv;
          [SMAppService openSystemSettingsLoginItems];
        }
        return opens_at_login();
      }

      bool replace = is_login_agent();
      if (![agent unregisterAndReturnError:&error]) {
        BOOST_LOG(warning) << "Couldn't turn off Open at Login: "sv << error.localizedDescription.UTF8String;
        return false;
      }
      if (replace) {
        launch_replacement();
      }
      return true;
    }

    BOOST_LOG(warning) << "Open at Login requires macOS 13 or later"sv;
    return false;
  }

  namespace {
    fs::path instance_lock_path() {
      return appdata() / "vibepollo.lock";
    }

    // Whether another process holds the instance lock, as the login agent does once it runs.
    bool instance_lock_held_elsewhere() {
      int fd = open(instance_lock_path().c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return false;
      }
      const bool held = flock(fd, LOCK_EX | LOCK_NB) != 0;
      close(fd);
      return held;
    }

    // Starts the agent unless it's already running, and waits for it to take over.
    bool start_login_agent(const std::string &service) {
      if (run({"/bin/launchctl", "kickstart", service}) != 0) {
        return false;
      }
      // kickstart succeeds even when launchd then fails to spawn the agent.
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!instance_lock_held_elsewhere()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(100ms);
      }
      return true;
    }
  }  // namespace

  bool defer_to_login_agent() {
    if (is_login_agent() || !opens_at_login()) {
      return false;
    }

    auto service = "gui/"s + std::to_string(getuid()) + "/" PROJECT_FQDN;
    if (!start_login_agent(service)) {
      // launchd only starts the agent for the build that registered it, so an update can leave it
      // unable to. Turning Open at Login off and on again registers it for this build.
      BOOST_LOG(warning) << "Couldn't start "sv << service << ", so running without it"sv;
      BOOST_LOG(warning) << "If Vibepollo was just updated, turn Open at Login off and on again"sv;
      return false;
    }
    BOOST_LOG(info) << "Open at Login is on, so leaving it to "sv << service << " to run Vibepollo"sv;
    return true;
  }

  bool acquire_instance_lock(std::chrono::milliseconds timeout) {
    std::error_code ec;
    fs::create_directories(appdata(), ec);
    auto path = instance_lock_path();

    // Held until the process exits. O_CLOEXEC keeps launched apps from holding it after that, and
    // restarting from the menu bar closes it before re-executing.
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
      BOOST_LOG(warning) << "Couldn't open "sv << path << ", so not checking for other copies of Vibepollo: "sv << strerror(errno);
      return true;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      // The login agent takes over from other copies, like the one that just turned Open at Login on.
      char pid[16] = {};
      if (is_login_agent() && pread(fd, pid, sizeof(pid) - 1, 0) > 0 && std::atoi(pid) > 0) {
        BOOST_LOG(info) << "Asking the copy of Vibepollo with PID "sv << pid << " to quit"sv;
        kill(std::atoi(pid), SIGTERM);
      }

      BOOST_LOG(info) << "Waiting for another copy of Vibepollo to quit"sv;
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
          close(fd);
          return false;
        }
        std::this_thread::sleep_for(100ms);
      }
    }

    auto pid = std::to_string(getpid());
    if (ftruncate(fd, 0) != 0 || pwrite(fd, pid.data(), pid.size(), 0) < 0) {
      BOOST_LOG(warning) << "Couldn't record this process in "sv << path;
    }
    return true;
  }

  namespace {
    // A sleeping display drops out of the active list, and during a dark wake every display does.
    bool displays_awake() {
      uint32_t count = 0;
      return CGGetActiveDisplayList(0, nullptr, &count) == kCGErrorSuccess && count > 0 && !CGDisplayIsAsleep(CGMainDisplayID());
    }
  }  // namespace

  bool wake_displays(const std::chrono::milliseconds timeout) {
    if (displays_awake()) {
      return true;
    }

    // Declaring user activity finishes a dark wake and turns the displays on. The assertion
    // expires on its own after the display sleep delay.
    IOPMAssertionID activity = kIOPMNullAssertionID;
    if (IOPMAssertionDeclareUserActivity(CFSTR("Vibepollo is waking the displays for a stream"), kIOPMUserActiveLocal, &activity) != kIOReturnSuccess) {
      BOOST_LOG(warning) << "Couldn't wake the displays for capture"sv;
      return false;
    }

    // A Mac without a screen of its own (a headless Mac mini, a closed MacBook) has none to wait for.
    uint32_t online = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &online) != kCGErrorSuccess || online == 0) {
      return false;
    }

    const auto start = std::chrono::steady_clock::now();
    while (!displays_awake()) {
      if (std::chrono::steady_clock::now() - start >= timeout) {
        BOOST_LOG(warning) << "Displays are still asleep after "sv << timeout.count() << " ms; capture may fail"sv;
        return false;
      }
      std::this_thread::sleep_for(50ms);
    }
    BOOST_LOG(info) << "Woke the displays for capture in "sv << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << " ms"sv;
    return true;
  }

  namespace {
    struct sleep_watch_t {
      io_connect_t root_port = MACH_PORT_NULL;
      IONotificationPortRef port = nullptr;
      io_object_t notifier = IO_OBJECT_NULL;
      std::function<void()> handler;
    } sleep_watch;

    void system_power_changed(void *, io_service_t, natural_t message, void *argument) {
      switch (message) {
        case kIOMessageCanSystemSleep:
          // Idle sleep: streaming holds its own assertions against it, so just answer.
          IOAllowPowerChange(sleep_watch.root_port, reinterpret_cast<intptr_t>(argument));
          break;
        case kIOMessageSystemWillSleep:
          sleep_watch.handler();
          IOAllowPowerChange(sleep_watch.root_port, reinterpret_cast<intptr_t>(argument));
          break;
        default:
          break;
      }
    }
  }  // namespace

  void on_system_will_sleep(std::function<void()> handler) {
    static std::once_flag once;
    std::call_once(once, [&handler]() {
      sleep_watch.handler = std::move(handler);
      sleep_watch.root_port = IORegisterForSystemPower(nullptr, &sleep_watch.port, system_power_changed, &sleep_watch.notifier);
      if (sleep_watch.root_port == MACH_PORT_NULL) {
        BOOST_LOG(warning) << "Couldn't watch for sleep; streams will just stop when the Mac sleeps"sv;
        return;
      }
      IONotificationPortSetDispatchQueue(sleep_watch.port, dispatch_queue_create("dev.vibepollo.sleep", DISPATCH_QUEUE_SERIAL));
    });
  }

  namespace {
    struct lid_watch_t {
      IONotificationPortRef port = nullptr;
      io_object_t notifier = IO_OBJECT_NULL;
      bool closed = false;
      std::function<void()> handler;
    } lid_watch;

    void root_domain_message(void *, io_service_t, natural_t message, void *argument) {
      if (message != kIOPMMessageClamshellStateChange) {
        return;
      }
      // Also sent while the lid stays closed, when power or displays change: act on closing only.
      const bool closed = (reinterpret_cast<uintptr_t>(argument) & kClamshellStateBit) != 0;
      const bool just_closed = closed && !lid_watch.closed;
      lid_watch.closed = closed;
      if (just_closed) {
        lid_watch.handler();
      }
    }
  }  // namespace

  void on_lid_closed(std::function<void()> handler) {
    static std::once_flag once;
    std::call_once(once, [&handler]() {
      const io_service_t root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
      if (root == IO_OBJECT_NULL) {
        return;
      }
      const CFTypeRef state = IORegistryEntryCreateCFProperty(root, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0);
      lid_watch.closed = state == kCFBooleanTrue;
      if (state != nullptr) {
        CFRelease(state);
      }
      lid_watch.handler = std::move(handler);
      lid_watch.port = IONotificationPortCreate(kIOMainPortDefault);
      IONotificationPortSetDispatchQueue(lid_watch.port, dispatch_queue_create("dev.vibepollo.lid", DISPATCH_QUEUE_SERIAL));
      if (IOServiceAddInterestNotification(lid_watch.port, root, kIOGeneralInterest, root_domain_message, nullptr, &lid_watch.notifier) != KERN_SUCCESS) {
        BOOST_LOG(warning) << "Couldn't watch the lid; closing it won't end streams"sv;
      }
      IOObjectRelease(root);
    });
  }

  std::unique_ptr<deinit_t> init() {
    // Remote mouse and keyboard input is posted as synthetic events, which macOS only delivers
    // for apps allowed under Privacy & Security > Accessibility. Streaming works without it.
    if (!CGPreflightPostEventAccess()) {
      BOOST_LOG(warning) << "No accessibility permission; remote mouse and keyboard input will be ignored"sv;
      BOOST_LOG(warning) << "Please allow it in 'System Settings' -> 'Privacy & Security'"sv;
      CGRequestPostEventAccess();
    }

    // This will generate a warning about CGPreflightScreenCaptureAccess and
    // CGRequestScreenCaptureAccess being unavailable before macOS 10.15, but
    // we have a guard to prevent it from being called on those earlier systems.
    // Unfortunately the supported way to silence this warning, using @available,
    // produces linker errors for __isPlatformVersionAtLeast, so we have to use
    // a different method.
    // We also ignore "tautological-pointer-compare" because when compiling with
    // Xcode 12.2 and later, these functions are not weakly linked and will never
    // be null, and therefore generate this warning. Since we are weakly linking
    // when compiling with earlier Xcode versions, the check for null is
    // necessary, and so we ignore the warning.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
    if ([[NSProcessInfo processInfo] isOperatingSystemAtLeastVersion:((NSOperatingSystemVersion) {10, 15, 0})] &&
        // Double check that these weakly-linked symbols have been loaded:
        CGPreflightScreenCaptureAccess != nullptr && CGRequestScreenCaptureAccess != nullptr &&
        !CGPreflightScreenCaptureAccess()) {
      BOOST_LOG(error) << "No screen capture permission!"sv;
      BOOST_LOG(error) << "Please allow it in 'System Settings' -> 'Privacy & Security', then restart Vibepollo"sv;
      // Asking ScreenCaptureKit shows the system prompt and lists the app under Privacy & Security.
      // CGRequestScreenCaptureAccess() no longer does either on current macOS.
      [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *, NSError *) {
      }];
      return nullptr;
    }
#pragma clang diagnostic pop
    // Record that we determined that we have the screen capture permission.
    screen_capture_allowed = true;
    return std::make_unique<deinit_t>();
  }

  fs::path appdata() {
    const char *homedir;
    if ((homedir = getenv("HOME")) == nullptr) {
      homedir = getpwuid(geteuid())->pw_dir;
    }

    return services::home_config_root(std::optional<fs::path> {fs::path {homedir}}, fs::path {homedir});
  }

  using ifaddr_t = util::safe_ptr<ifaddrs, freeifaddrs>;

  ifaddr_t get_ifaddrs() {
    ifaddrs *p {nullptr};

    getifaddrs(&p);

    return ifaddr_t {p};
  }

  std::string from_sockaddr(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    }

    return std::string {data};
  }

  std::pair<std::uint16_t, std::string> from_sockaddr_ex(const sockaddr *const ip_addr) {
    char data[INET6_ADDRSTRLEN] = {};

    auto family = ip_addr->sa_family;
    std::uint16_t port = 0;
    if (family == AF_INET6) {
      inet_ntop(AF_INET6, &((sockaddr_in6 *) ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
      port = ((sockaddr_in6 *) ip_addr)->sin6_port;
    } else if (family == AF_INET) {
      inet_ntop(AF_INET, &((sockaddr_in *) ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
      port = ((sockaddr_in *) ip_addr)->sin_port;
    }

    return {port, std::string {data}};
  }

  std::string get_mac_address(const std::string_view &address) {
    auto ifaddrs = get_ifaddrs();

    for (auto pos = ifaddrs.get(); pos != nullptr; pos = pos->ifa_next) {
      if (pos->ifa_addr && address == from_sockaddr(pos->ifa_addr)) {
        BOOST_LOG(verbose) << "Looking for MAC of "sv << pos->ifa_name;

        struct ifaddrs *ifap, *ifaptr;
        unsigned char *ptr;
        std::string mac_address;

        if (getifaddrs(&ifap) == 0) {
          for (ifaptr = ifap; ifaptr != nullptr; ifaptr = (ifaptr)->ifa_next) {
            if (!strcmp((ifaptr)->ifa_name, pos->ifa_name) && (((ifaptr)->ifa_addr)->sa_family == AF_LINK)) {
              ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) (ifaptr)->ifa_addr);
              char buff[100];

              snprintf(buff, sizeof(buff), "%02x:%02x:%02x:%02x:%02x:%02x", *ptr, *(ptr + 1), *(ptr + 2), *(ptr + 3), *(ptr + 4), *(ptr + 5));
              mac_address = buff;
              break;
            }
          }

          freeifaddrs(ifap);

          if (ifaptr != nullptr) {
            BOOST_LOG(verbose) << "Found MAC of "sv << pos->ifa_name << ": "sv << mac_address;
            return mac_address;
          }
        }
      }
    }

    BOOST_LOG(warning) << "Unable to find MAC address for "sv << address;
    return "00:00:00:00:00:00"s;
  }

  // TODO: return actual IP
  std::string get_local_ip_for_gateway() {
    return "";
  }

  bp::child run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir, const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    (void) elevated;
    (void) interactive;
    ec.clear();

    std::vector<std::string> parts;
    try {
      parts = boost::program_options::split_unix(cmd);
    } catch (...) {
    }

    if (parts.empty()) {
      ec = std::make_error_code(std::errc::invalid_argument);
      return bp::child();
    }

    auto exe_path = v2::filesystem::path(parts.front());
    // Only PATH-search when there's no directory component (e.g., "foo" not "./foo" or "../foo")
    if (!exe_path.is_absolute() && exe_path.parent_path().empty()) {
      exe_path = v2::environment::find_executable(exe_path);
    }

    if (exe_path.empty()) {
      ec = std::make_error_code(std::errc::no_such_file_or_directory);
      return bp::child();
    }

    std::vector<std::string> args;
    if (parts.size() > 1) {
      args.assign(parts.begin() + 1, parts.end());
    }

    v2::process_stdio stdio {};
    stdio.in = nullptr;
    if (file) {
      stdio.out = file;
      stdio.err = file;
    } else {
      stdio.out = nullptr;
      stdio.err = nullptr;
    }

    auto env_init = env.to_process_environment();
    boost::asio::system_executor exec;

    try {
      if (group) {
        if (!working_dir.empty()) {
          auto start = v2::process_start_dir(v2::filesystem::path(working_dir.string()));
          auto proc = v2::process(exec, exe_path, args, start, stdio, env_init, bp::detail::posix_group_initer {group});
          return bp::child(std::move(proc));
        }
        auto proc = v2::process(exec, exe_path, args, stdio, env_init, bp::detail::posix_group_initer {group});
        return bp::child(std::move(proc));
      }

      if (!working_dir.empty()) {
        auto start = v2::process_start_dir(v2::filesystem::path(working_dir.string()));
        auto proc = v2::process(exec, exe_path, args, start, stdio, env_init);
        return bp::child(std::move(proc));
      }
      auto proc = v2::process(exec, exe_path, args, stdio, env_init);
      return bp::child(std::move(proc));
    } catch (const std::system_error &e) {
      ec = e.code();
      return bp::child();
    } catch (...) {
      ec = std::make_error_code(std::errc::invalid_argument);
      return bp::child();
    }
  }

  /**
   * @brief Open a url in the default web browser.
   * @param url The url to open.
   */
  void open_url(const std::string &url) {
    boost::filesystem::path working_dir;
    std::string cmd = R"(open ")" + url + R"(")";

    bp::environment _env = bp::this_process::env();
    std::error_code ec;
    auto child = run_command(false, false, cmd, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't open url ["sv << url << "]: System: "sv << ec.message();
    } else {
      BOOST_LOG(info) << "Opened url ["sv << url << "]"sv;
      child.detach();
    }
  }

  void adjust_thread_priority(thread_priority_e priority) {
    qos_class_t mac_priority;

    switch (priority) {
      case thread_priority_e::low:
        mac_priority = QOS_CLASS_UTILITY;
        break;
      case thread_priority_e::normal:
        mac_priority = QOS_CLASS_DEFAULT;
        break;
      case thread_priority_e::high:
        mac_priority = QOS_CLASS_USER_INITIATED;
        break;
      case thread_priority_e::critical:
        mac_priority = QOS_CLASS_USER_INTERACTIVE;
        break;
      default:
        BOOST_LOG(error) << "Unknown thread priority: "sv << (int) priority;
        return;
    }

    // https://github.com/apple/darwin-libpthread/blob/main/include/sys/qos.h
    pthread_set_qos_class_self_np(mac_priority, 0);
  }

  void set_thread_name(const std::string &name) {
    pthread_setname_np(name.c_str());
  }

  void enable_mouse_keys() {
    // Unimplemented
  }

  void streaming_will_start() {
    // Nothing to do
  }

  void streaming_will_stop() {
    // Nothing to do
  }

  void restart_on_exit() {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) < 0) {
      BOOST_LOG(fatal) << "NSGetExecutablePath() failed: "sv << errno;
      return;
    }

    // ASIO doesn't use O_CLOEXEC, so we have to close all fds ourselves
    int openmax = (int) sysconf(_SC_OPEN_MAX);
    for (int fd = STDERR_FILENO + 1; fd < openmax; fd++) {
      close(fd);
    }

    // Re-exec ourselves with the same arguments
    if (execv(executable, lifetime::get_argv()) < 0) {
      BOOST_LOG(fatal) << "execv() failed: "sv << errno;
      return;
    }
  }

  void restart() {
    // Gracefully clean up and restart ourselves instead of exiting
    atexit(restart_on_exit);
    lifetime::exit_sunshine(0, true);
  }

  int set_env(const std::string &name, const std::string &value) {
    return services::process_environment().set(name, value);
  }

  int unset_env(const std::string &name) {
    return services::process_environment().unset(name);
  }

  bool request_process_group_exit(std::uintptr_t native_handle) {
    if (killpg((pid_t) native_handle, SIGTERM) == 0 || errno == ESRCH) {
      BOOST_LOG(debug) << "Successfully sent SIGTERM to process group: "sv << native_handle;
      return true;
    } else {
      BOOST_LOG(warning) << "Unable to send SIGTERM to process group ["sv << native_handle << "]: "sv << errno;
      return false;
    }
  }

  bool process_group_running(std::uintptr_t native_handle) {
    return waitpid(-((pid_t) native_handle), nullptr, WNOHANG) >= 0;
  }

  struct sockaddr_in to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    struct sockaddr_in saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  struct sockaddr_in6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    struct sockaddr_in6 saddr_v6 = {};

    saddr_v6.sin6_family = AF_INET6;
    saddr_v6.sin6_port = htons(port);
    saddr_v6.sin6_scope_id = address.scope_id();

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v6.sin6_addr, addr_bytes.data(), sizeof(saddr_v6.sin6_addr));

    return saddr_v6;
  }

  bool send_batch(batched_send_info_t &send_info) {
    // Fall back to unbatched send calls
    return false;
  }

  bool send(send_info_t &send_info) {
    auto sockfd = (int) send_info.native_socket;
    struct msghdr msg = {};

    // Convert the target address into a sockaddr
    struct sockaddr_in taddr_v4 = {};
    struct sockaddr_in6 taddr_v6 = {};
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v6;
      msg.msg_namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.msg_name = (struct sockaddr *) &taddr_v4;
      msg.msg_namelen = sizeof(taddr_v4);
    }

    union {
      char buf[std::max(CMSG_SPACE(sizeof(struct in_pktinfo)), CMSG_SPACE(sizeof(struct in6_pktinfo)))];
      struct cmsghdr alignment;
    } cmbuf {};

    socklen_t cmbuflen = 0;

    msg.msg_control = cmbuf.buf;
    msg.msg_controllen = sizeof(cmbuf.buf);

    auto pktinfo_cm = CMSG_FIRSTHDR(&msg);
    if (send_info.source_address.is_v6()) {
      struct in6_pktinfo pktInfo {};

      struct sockaddr_in6 saddr_v6 = to_sockaddr(send_info.source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IPV6;
      pktinfo_cm->cmsg_type = IPV6_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    } else {
      struct in_pktinfo pktInfo {};

      struct sockaddr_in saddr_v4 = to_sockaddr(send_info.source_address.to_v4(), 0);
      pktInfo.ipi_spec_dst = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += CMSG_SPACE(sizeof(pktInfo));

      pktinfo_cm->cmsg_level = IPPROTO_IP;
      pktinfo_cm->cmsg_type = IP_PKTINFO;
      pktinfo_cm->cmsg_len = CMSG_LEN(sizeof(pktInfo));
      memcpy(CMSG_DATA(pktinfo_cm), &pktInfo, sizeof(pktInfo));
    }

    struct iovec iovs[2] = {};
    int iovlen = 0;
    if (send_info.header) {
      iovs[iovlen].iov_base = (void *) send_info.header;
      iovs[iovlen].iov_len = send_info.header_size;
      iovlen++;
    }
    iovs[iovlen].iov_base = (void *) send_info.payload;
    iovs[iovlen].iov_len = send_info.payload_size;
    iovlen++;

    msg.msg_iov = iovs;
    msg.msg_iovlen = iovlen;

    msg.msg_controllen = cmbuflen;

    auto bytes_sent = sendmsg(sockfd, &msg, 0);

    // If there's no send buffer space, wait for some to be available
    while (bytes_sent < 0 && errno == EAGAIN) {
      struct pollfd pfd;

      pfd.fd = sockfd;
      pfd.events = POLLOUT;

      if (poll(&pfd, 1, -1) != 1) {
        BOOST_LOG(warning) << "poll() failed: "sv << errno;
        break;
      }

      // Try to send again
      bytes_sent = sendmsg(sockfd, &msg, 0);
    }

    if (bytes_sent < 0) {
      BOOST_LOG(warning) << "sendmsg() failed: "sv << errno;
      return false;
    }

    return true;
  }

  // We can't track QoS state separately for each destination on this OS,
  // so we keep a ref count to only disable QoS options when all clients
  // are disconnected.
  static std::atomic<int> qos_ref_count = 0;

  class qos_t: public deinit_t {
  public:
    qos_t(int sockfd, std::vector<std::tuple<int, int, int>> options):
        sockfd(sockfd),
        options(options) {
      qos_ref_count++;
    }

    virtual ~qos_t() {
      if (--qos_ref_count == 0) {
        for (const auto &tuple : options) {
          auto reset_val = std::get<2>(tuple);
          if (setsockopt(sockfd, std::get<0>(tuple), std::get<1>(tuple), &reset_val, sizeof(reset_val)) < 0) {
            BOOST_LOG(warning) << "Failed to reset option: "sv << errno;
          }
        }
      }
    }

  private:
    int sockfd;
    std::vector<std::tuple<int, int, int>> options;
  };

  /**
   * @brief Enables QoS on the given socket for traffic to the specified destination.
   * @param native_socket The native socket handle.
   * @param address The destination address for traffic sent on this socket.
   * @param port The destination port for traffic sent on this socket.
   * @param data_type The type of traffic sent on this socket.
   * @param dscp_tagging Specifies whether to enable DSCP tagging on outgoing traffic.
   */
  std::unique_ptr<deinit_t> enable_socket_qos(uintptr_t native_socket, boost::asio::ip::address &address, uint16_t port, qos_data_type_e data_type, bool dscp_tagging) {
    int sockfd = (int) native_socket;
    std::vector<std::tuple<int, int, int>> reset_options;

    // We can use SO_NET_SERVICE_TYPE to set link-layer prioritization without DSCP tagging
    int service_type = 0;
    switch (data_type) {
      case qos_data_type_e::video:
        service_type = NET_SERVICE_TYPE_VI;
        break;
      case qos_data_type_e::audio:
        service_type = NET_SERVICE_TYPE_VO;
        break;
      default:
        BOOST_LOG(error) << "Unknown traffic type: "sv << (int) data_type;
        break;
    }

    if (service_type) {
      if (setsockopt(sockfd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &service_type, sizeof(service_type)) == 0) {
        // Reset SO_NET_SERVICE_TYPE to best-effort when QoS is disabled
        reset_options.emplace_back(std::make_tuple(SOL_SOCKET, SO_NET_SERVICE_TYPE, NET_SERVICE_TYPE_BE));
      } else {
        BOOST_LOG(error) << "Failed to set SO_NET_SERVICE_TYPE: "sv << errno;
      }
    }

    if (dscp_tagging) {
      int level;
      int option;
      if (address.is_v6()) {
        level = IPPROTO_IPV6;
        option = IPV6_TCLASS;
      } else {
        level = IPPROTO_IP;
        option = IP_TOS;
      }

      // The specific DSCP values here are chosen to be consistent with Windows,
      // except that we use CS6 instead of CS7 for audio traffic.
      int dscp = 0;
      switch (data_type) {
        case qos_data_type_e::video:
          dscp = 40;
          break;
        case qos_data_type_e::audio:
          dscp = 48;
          break;
        default:
          BOOST_LOG(error) << "Unknown traffic type: "sv << (int) data_type;
          break;
      }

      if (dscp) {
        // Shift to put the DSCP value in the correct position in the TOS field
        dscp <<= 2;

        if (setsockopt(sockfd, level, option, &dscp, sizeof(dscp)) == 0) {
          // Reset TOS to -1 when QoS is disabled
          reset_options.emplace_back(std::make_tuple(level, option, -1));
        } else {
          BOOST_LOG(error) << "Failed to set TOS/TCLASS: "sv << errno;
        }
      }
    }

    return std::make_unique<qos_t>(sockfd, reset_options);
  }

  std::string get_host_name() {
    services::function_host_name_provider_t provider {[]() -> std::optional<std::string> {
      try {
        return boost::asio::ip::host_name();
      } catch (boost::system::system_error &err) {
        BOOST_LOG(error) << "Failed to get hostname: "sv << err.what();
        return std::nullopt;
      }
    }};
    return services::host_name_or(provider);
  }

  class macos_high_precision_timer: public high_precision_timer {
  public:
    void sleep_for(const std::chrono::nanoseconds &duration) override {
      std::this_thread::sleep_for(duration);
    }

    operator bool() override {
      return true;
    }
  };

  std::unique_ptr<high_precision_timer> create_high_precision_timer() {
    return std::make_unique<macos_high_precision_timer>();
  }

  std::string
    get_clipboard() {
    // Placeholder
    return "";
  }

  bool
    set_clipboard(const std::string &content) {
    // Placeholder
    return false;
  }

  std::string resolve_render_device() {
    return {};
  }
}  // namespace platf

namespace dyn {
  void *handle(const std::vector<const char *> &libs) {
    void *handle;

    for (auto lib : libs) {
      handle = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
      if (handle) {
        return handle;
      }
    }

    std::stringstream ss;
    ss << "Couldn't find any of the following libraries: ["sv << libs.front();
    std::for_each(std::begin(libs) + 1, std::end(libs), [&](auto lib) {
      ss << ", "sv << lib;
    });

    ss << ']';

    BOOST_LOG(error) << ss.str();

    return nullptr;
  }

  int load(void *handle, const std::vector<std::tuple<apiproc *, const char *>> &funcs, bool strict) {
    int err = 0;
    for (auto &func : funcs) {
      TUPLE_2D_REF(fn, name, func);

      *fn = (void (*)()) dlsym(handle, name);

      if (!*fn && strict) {
        BOOST_LOG(error) << "Couldn't find function: "sv << name;

        err = -1;
      }
    }

    return err;
  }
}  // namespace dyn
