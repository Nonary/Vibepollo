/**
 * @file src/platform/macos/touchscreen.cpp
 * @brief The host's side of the macOS 27 virtual touchscreen: starts the touch helper, checks it,
 *        and sends it each touchscreen's contacts.
 */
// local includes
#include "touchscreen.h"

// standard includes
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// platform includes
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

// local includes
#include "src/config.h"
#include "src/jthread.h"
#include "src/logging.h"
#include "src/platform/macos/touchscreen_policy.h"

namespace platf::macos_touchscreen {
  using namespace std::literals;

  namespace {
    using clock = std::chrono::steady_clock;

    /**
     * @brief How long a new device is left alone before its first report.
     * @details macOS drops reports that arrive before it has matched the device's driver and
     *          attached the digitizer: of two taps sent right after creating a device, only the
     *          second registered, and both did after 300 ms (measured on macOS 27.2). The
     *          attach itself took about 50 ms.
     */
    constexpr auto device_settle_time = 300ms;

    /// How long the helper may take to answer a request to make a device. Past this it is taken
    /// to be stuck: it is stopped, and touch uses the emulation.
    constexpr auto answer_timeout = 5s;

    /// How long the helper may take to remove its devices and exit when it is no longer needed.
    constexpr auto stop_timeout = 2s;

    /// Reports kept while a device is being made; a gesture longer than this loses its start.
    constexpr std::size_t max_queued_reports = 128;

    /// Messages waiting for the writer; past this, contact updates are dropped (the next one carries the full state).
    constexpr std::size_t max_outgoing = 256;

    template<typename T>
    struct cf_release_t {
      T ref {};

      explicit cf_release_t(T value):
          ref {value} {
      }

      ~cf_release_t() {
        if (ref) {
          CFRelease(ref);
        }
      }

      cf_release_t(const cf_release_t &) = delete;
      cf_release_t &operator=(const cf_release_t &) = delete;
    };

    int os_major() {
      static const int major = [] {
        char version[32] {};
        std::size_t size = sizeof(version);
        if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0) {
          return 0;
        }
        return parse_os_major(version);
      }();
      return major;
    }

    std::string cf_to_string(CFStringRef text) {
      char buffer[256] {};
      if (!text || CFGetTypeID(text) != CFStringGetTypeID() || !CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return {};
      }
      return buffer;
    }

    /// The team this host is signed by, or empty for an ad-hoc or unsigned build.
    std::string host_team() {
      SecCodeRef self = nullptr;
      if (SecCodeCopySelf(kSecCSDefaultFlags, &self) != errSecSuccess) {
        return {};
      }
      cf_release_t self_ref {self};
      SecStaticCodeRef static_code = nullptr;
      if (SecCodeCopyStaticCode(self, kSecCSDefaultFlags, &static_code) != errSecSuccess) {
        return {};
      }
      cf_release_t static_ref {static_code};
      CFDictionaryRef info = nullptr;
      if (SecCodeCopySigningInformation(static_code, kSecCSSigningInformation, &info) != errSecSuccess) {
        return {};
      }
      cf_release_t info_ref {info};
      const auto team = cf_to_string(static_cast<CFStringRef>(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier)));
      // A team identifier is ten uppercase letters and digits; anything else can't go in a requirement.
      if (team.size() != 10 || team.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos) {
        return {};
      }
      return team;
    }

    /// The helper's code requirement: its identifier, an Apple-issued certificate, and the host's team.
    std::string helper_requirement(const std::string &team) {
      return "identifier \"" TOUCH_HELPER_IDENTIFIER "\" and anchor apple generic and certificate leaf[subject.OU] = \"" + team + "\"";
    }

    /// The helper bundle inside this app, found from the host's own executable rather than any setting.
    std::string helper_bundle_path() {
      // The kernel's path for this process is absolute. _NSGetExecutablePath returns argv[0]'s
      // form, which is relative when launchd starts the login agent ("Contents/MacOS/Vibepollo").
      char executable[PROC_PIDPATHINFO_MAXSIZE] {};
      if (proc_pidpath(getpid(), executable, sizeof(executable)) <= 0) {
        return {};
      }
      char resolved[PATH_MAX] {};
      if (!realpath(executable, resolved)) {
        return {};
      }
      // .../Vibepollo.app/Contents/MacOS/Vibepollo -> .../Vibepollo.app/Contents/Helpers/<helper>.app
      std::string path {resolved};
      const auto macos_dir = path.rfind('/');
      if (macos_dir == std::string::npos) {
        return {};
      }
      const auto contents = path.rfind('/', macos_dir - 1);
      if (contents == std::string::npos || path.compare(contents, macos_dir - contents, "/MacOS") != 0) {
        return {};
      }
      return path.substr(0, contents) + "/Helpers/" TOUCH_HELPER_BUNDLE_NAME ".app";
    }

    /**
     * @brief Checks the helper bundle's signature on disk: valid, all architectures, the expected
     *        identifier, and the host's team.
     * @return An empty string when it passes, otherwise why not.
     */
    std::string check_helper_on_disk(const std::string &bundle, const std::string &team) {
      cf_release_t url {CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bundle.c_str()), static_cast<CFIndex>(bundle.size()), true)};
      SecStaticCodeRef code = nullptr;
      if (!url.ref || SecStaticCodeCreateWithPath(url.ref, kSecCSDefaultFlags, &code) != errSecSuccess) {
        return "it isn't there";
      }
      cf_release_t code_ref {code};
      if (team.empty()) {
        return "the host isn't signed by a team, so it can't check the helper";
      }
      cf_release_t text {CFStringCreateWithCString(kCFAllocatorDefault, helper_requirement(team).c_str(), kCFStringEncodingUTF8)};
      SecRequirementRef requirement = nullptr;
      if (SecRequirementCreateWithString(text.ref, kSecCSDefaultFlags, &requirement) != errSecSuccess) {
        return "its requirement couldn't be built";
      }
      cf_release_t requirement_ref {requirement};
      const auto status = SecStaticCodeCheckValidity(code, kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode, requirement);
      if (status != errSecSuccess) {
        return "its signature isn't valid or isn't the expected identifier and team (OSStatus "s + std::to_string(status) + ")";
      }

      // A helper signed without the HID entitlement (release builds ship it that way) would
      // start and then fail to make a device, so don't start it.
      CFDictionaryRef info = nullptr;
      if (SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) != errSecSuccess) {
        return "its signing information couldn't be read";
      }
      cf_release_t info_ref {info};
      const auto entitlements = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict));
      const auto hid = entitlements && CFGetTypeID(entitlements) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(entitlements, CFSTR("com.apple.developer.hid.virtual.device")) : nullptr;
      if (hid != kCFBooleanTrue) {
        return "it isn't signed with the HID virtual device entitlement (see docs/macos_touch.md)"s;
      }
      return {};
    }

    /// Checks the running helper process the same way, by its pid, which stays ours until it's reaped.
    bool check_running_helper(const pid_t pid, const std::string &team) {
      cf_release_t pid_number {CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid)};
      const void *keys[] {kSecGuestAttributePid};
      const void *values[] {pid_number.ref};
      cf_release_t attributes {CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
      SecCodeRef code = nullptr;
      if (SecCodeCopyGuestWithAttributes(nullptr, attributes.ref, kSecCSDefaultFlags, &code) != errSecSuccess) {
        return false;
      }
      cf_release_t code_ref {code};
      cf_release_t text {CFStringCreateWithCString(kCFAllocatorDefault, helper_requirement(team).c_str(), kCFStringEncodingUTF8)};
      SecRequirementRef requirement = nullptr;
      if (SecRequirementCreateWithString(text.ref, kSecCSDefaultFlags, &requirement) != errSecSuccess) {
        return false;
      }
      cf_release_t requirement_ref {requirement};
      return SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement) == errSecSuccess;
    }

    /// The UUID macOS uses for a display, as SkyLight expects in the digitizer's displayUUID.
    std::string display_uuid(const display_t display) {
      cf_release_t uuid {CGDisplayCreateUUIDFromDisplayID(display)};
      if (!uuid.ref) {
        return {};
      }
      cf_release_t text {CFUUIDCreateString(kCFAllocatorDefault, uuid.ref)};
      return cf_to_string(text.ref);
    }

    enum class device_state_e {
      creating,
      ready,
      failed,
    };

    struct device_t {
      std::uint32_t id {};
      std::string uuid;
      device_state_e state = device_state_e::creating;
      clock::time_point requested_at {};  ///< When the helper was asked to make it.
      clock::time_point failed_at {};  ///< When it failed; it's retried after retry_after.
      contact_table_t contacts;
      std::set<owner_t> owners;  ///< Clients streaming this display that have touched it.
      std::deque<std::vector<contact_t>> queued;  ///< Reports made while the device is created.
    };

    class manager_t {
    public:
      ~manager_t() {
        std::unique_lock lock {mutex_};
        stop_helper(lock);
      }

      bool begin_gesture(const owner_t owner, const display_t display) {
        std::unique_lock lock {mutex_};
        const auto now = clock::now();
        if (auto *device = find(display)) {
          if (device->state == device_state_e::creating && now - device->requested_at >= answer_timeout) {
            // The helper never answered. Stop it, so the reader cleans up, and emulate from here.
            BOOST_LOG(warning) << "Touchscreen: the touch helper didn't answer within "sv << answer_timeout.count() << " seconds; stopping it"sv;
            end_helper_now();
            device->state = device_state_e::failed;
            device->failed_at = now;
            device->queued.clear();
            return false;
          }
          if (device->state != device_state_e::failed) {
            device->owners.insert(owner);
            // While the helper is still being checked and started, it may turn out to be
            // unusable, so this gesture is emulated rather than held for it.
            return !launching_;
          }
          if (now - device->failed_at < retry_after) {
            return false;
          }
        }

        if (stopping_ && socket_ >= 0) {
          // An idle helper is on its way out; the next gesture starts a new one.
          return false;
        }
        gate_t gate {os_major(), config::input.native_pen_touch, helper_};
        if (helper_ == helper_state_e::ready || helper_ == helper_state_e::starting) {
          return create_device(lock, owner, display) && !launching_;
        }
        if (!should_try_helper(gate, now, last_attempt_)) {
          return false;
        }
        last_attempt_ = now;
        start_helper();
        // The device is asked for now and made once the helper is up; this gesture is emulated.
        create_device(lock, owner, display);
        return false;
      }

      bool touch(const owner_t owner, const display_t display, const std::uint32_t pointer, const double x, const double y) {
        std::unique_lock lock {mutex_};
        auto *device = find(display);
        if (!device || device->state == device_state_e::failed) {
          return false;
        }
        const auto bounds = CGDisplayBounds(display);
        const auto point = to_logical(x, y, {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height});
        if (!device->contacts.move(owner, pointer, point) && !device->contacts.down(owner, pointer, point)) {
          // Every contact id is in use; this finger is left out.
          return true;
        }
        report(*device);
        return true;
      }

      bool lift(const owner_t owner, const display_t display, const std::uint32_t pointer) {
        std::unique_lock lock {mutex_};
        auto *device = find(display);
        if (!device || device->state == device_state_e::failed) {
          return false;
        }
        if (device->contacts.up(owner, pointer)) {
          report(*device);
        }
        return true;
      }

      void lift_owner(const owner_t owner) {
        std::unique_lock lock {mutex_};
        for (auto &[display, device] : devices_) {
          if (device.contacts.lift_owner(owner) > 0) {
            report(device);
          }
        }
      }

      void release(const owner_t owner) {
        std::unique_lock lock {mutex_};
        for (auto it = devices_.begin(); it != devices_.end();) {
          auto &device = it->second;
          if (device.contacts.lift_owner(owner) > 0) {
            report(device);
          }
          device.owners.erase(owner);
          if (device.owners.empty()) {
            // The last stream on this display is gone; the helper cancels the device and waits
            // for the cancel before releasing it.
            if (device.state != device_state_e::failed) {
              send(wire::make(wire::type_e::destroy, device.id));
            }
            BOOST_LOG(info) << "Touchscreen: removed the touchscreen of display "sv << it->first;
            it = devices_.erase(it);
          } else {
            ++it;
          }
        }
        if (devices_.empty() && helper_ == helper_state_e::ready) {
          stop_helper(lock);
        }
      }

    private:
      device_t *find(const display_t display) {
        const auto found = devices_.find(display);
        return found == devices_.end() ? nullptr : &found->second;
      }

      /// The next free device id, or max_devices when all are in use. Ids are handed out in
      /// turn rather than lowest first, so a reply about a device that was just removed can't
      /// be taken for one about the device made right after it.
      std::uint32_t free_device_id() {
        for (std::uint32_t tried = 0; tried < wire::max_devices; ++tried) {
          const auto id = (next_device_id_ + tried) % wire::max_devices;
          bool used = false;
          for (const auto &[display, device] : devices_) {
            used = used || device.id == id;
          }
          if (!used) {
            next_device_id_ = (id + 1) % wire::max_devices;
            return id;
          }
        }
        return wire::max_devices;
      }

      bool create_device(std::unique_lock<std::mutex> &, const owner_t owner, const display_t display) {
        // A failed device is made again; the clients that touched it still own the new one.
        std::set<owner_t> owners {owner};
        if (auto *existing = find(display); existing && existing->state == device_state_e::failed) {
          owners.insert(existing->owners.begin(), existing->owners.end());
          devices_.erase(display);
        }
        const auto uuid = display_uuid(display);
        const auto id = free_device_id();
        if (!wire::valid_uuid(uuid) || id >= wire::max_devices) {
          return false;
        }
        auto &device = devices_[display];
        device.id = id;
        device.uuid = uuid;
        device.state = device_state_e::creating;
        device.requested_at = clock::now();
        device.owners = std::move(owners);

        auto message = wire::make(wire::type_e::create, id);
        std::copy(uuid.begin(), uuid.end(), message.uuid);
        send(message);
        BOOST_LOG(info) << "Touchscreen: making a touchscreen for display "sv << display << " ("sv << uuid << ')';
        return true;
      }

      /// Sends a device's contacts, or queues them while it's being made.
      void report(device_t &device) {
        auto contacts = device.contacts.take_report();
        if (device.state == device_state_e::creating) {
          // macOS has seen none of these yet, so when the queue fills, starting over from the
          // newest state loses only the start of a long first gesture and never leaves a finger
          // down that already lifted.
          if (device.queued.size() >= max_queued_reports) {
            device.queued.clear();
          }
          device.queued.push_back(std::move(contacts));
          return;
        }
        if (device.state == device_state_e::ready) {
          send(wire::make_contacts(device.id, contacts));
        }
      }

      void send(const wire::message_t &message) {
        // While the helper is being started there is no socket yet; what is queued goes out once
        // it is up.
        if (socket_ < 0 && !launching_) {
          return;
        }
        // A pinch's per-finger moves go out as one report: a newer message replaces an unsent
        // one only when it is the same fingers, all still touching (see wire::supersedes).
        // Replacing anything else loses a finger landing or lifting, and with it a tap.
        const bool is_contacts = message.type == static_cast<std::uint16_t>(wire::type_e::contacts);
        if (!outgoing_.empty() && wire::supersedes(message, outgoing_.back())) {
          outgoing_.back() = message;
          return;
        }
        if (outgoing_.size() >= max_outgoing && is_contacts) {
          if (!warned_backlog_) {
            BOOST_LOG(warning) << "Touchscreen: the touch helper is falling behind; skipping touch updates"sv;
            warned_backlog_ = true;
          }
          // Keep the newest state rather than the oldest: every report names every contact, so
          // a finger that lifted in a skipped update still reads as lifted in this one. The
          // newest state is never dropped, or a last lift could be lost and the finger left down.
          for (auto queued = outgoing_.rbegin(); queued != outgoing_.rend(); ++queued) {
            if (queued->type == message.type && queued->device == message.device) {
              *queued = message;
              return;
            }
          }
          // Nothing is queued for this device, so this adds one message per device at most.
        }
        outgoing_.push_back(message);
        outgoing_ready_.notify_one();
      }

      void set_helper(const helper_state_e state, const std::string &why = {}) {
        if (state == helper_) {
          return;
        }
        helper_ = state;
        switch (state) {
          case helper_state_e::ready:
            BOOST_LOG(info) << "Touchscreen: the touch helper is running; fingers go to a touchscreen on the streamed display"sv;
            break;
          case helper_state_e::unavailable:
            BOOST_LOG(info) << "Touchscreen: not using the touch helper: "sv << why << "; touch uses the emulation"sv;
            break;
          case helper_state_e::failed:
            BOOST_LOG(warning) << "Touchscreen: the touch helper stopped: "sv << why << "; touch uses the emulation"sv;
            break;
          default:
            break;
        }
      }

      /// The threads of an earlier helper, for the launcher to join.
      struct old_threads_t {
        util::jthread launcher;
        util::jthread reader;
        util::jthread writer;
      };

      /**
       * @brief Starts the helper in the background. Call with the lock held.
       * @details Checking the helper's signature on disk reads and hashes its bundle, and an
       *          earlier helper's threads may still be finishing, so none of that runs on the
       *          caller's thread, which is the input thread when a touch starts the helper.
       */
      void start_helper() {
        if (launching_) {
          return;
        }
        launching_ = true;
        outgoing_.clear();
        set_helper(helper_state_e::starting);
        auto old = std::make_shared<old_threads_t>();
        old->launcher = std::move(launcher_);
        old->reader = std::move(reader_);
        old->writer = std::move(writer_);
        launcher_ = util::jthread {[this, old] {
          launch(*old);
        }};
      }

      /// Marks every device that is waiting for the helper as failed, to be retried later.
      void fail_waiting_devices(const clock::time_point failed_at) {
        for (auto &[display, device] : devices_) {
          if (device.state == device_state_e::creating) {
            device.state = device_state_e::failed;
            device.failed_at = failed_at;
            device.queued.clear();
          }
        }
      }

      /// The launcher thread: checks the helper on disk and spawns it.
      void launch(old_threads_t &old) {
        // The threads of a helper that already exited have finished or are finishing.
        for (auto *thread : {&old.writer, &old.reader, &old.launcher}) {
          if (thread->joinable()) {
            thread->join();
          }
        }

        const auto bundle = helper_bundle_path();
        const auto team = host_team();
        auto state = helper_state_e::starting;
        std::string why;
        int fds[2] {-1, -1};
        pid_t pid = -1;
        if (const auto problem = check_helper_on_disk(bundle, team); !problem.empty()) {
          state = helper_state_e::unavailable;
          why = (bundle.empty() ? "(no app bundle)"s : bundle) + ": "s + problem;
        } else if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
          state = helper_state_e::failed;
          why = "socketpair failed: "s + std::strerror(errno);
        } else {
          // Neither end may leak into another process this host starts: a copy of the helper's
          // end held elsewhere would keep the connection open after the helper exits. The
          // spawn's own dup2 to descriptor 3 is what the helper gets.
          fcntl(fds[0], F_SETFD, FD_CLOEXEC);
          fcntl(fds[1], F_SETFD, FD_CLOEXEC);
          const int nosigpipe = 1;
          setsockopt(fds[0], SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));

          posix_spawn_file_actions_t actions;
          posix_spawn_file_actions_init(&actions);
          posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
          posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
          posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
          posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
          posix_spawnattr_t attributes;
          posix_spawnattr_init(&attributes);
          // Only the descriptors named above reach the helper.
          posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

          const auto executable = bundle + "/Contents/MacOS/" TOUCH_HELPER_EXECUTABLE_NAME;
          char *argv[] {const_cast<char *>(executable.c_str()), nullptr};
          char *envp[] {nullptr};
          const auto spawned = posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv, envp);
          posix_spawn_file_actions_destroy(&actions);
          posix_spawnattr_destroy(&attributes);
          close(fds[1]);
          if (spawned != 0) {
            close(fds[0]);
            state = helper_state_e::failed;
            why = "couldn't start it: "s + std::strerror(spawned);
          }
        }

        std::unique_lock lock {mutex_};
        launching_ = false;
        const auto now = clock::now();
        last_attempt_ = now;
        if (state != helper_state_e::starting) {
          set_helper(state, why);
          outgoing_.clear();
          fail_waiting_devices(now);
          return;
        }

        pid_ = pid;
        socket_ = fds[0];
        stopping_ = false;
        stop_requested_ = false;
        verified_ = false;
        closers_done_ = 0;
        warned_backlog_ = false;
        reader_ = util::jthread {[this, pid, team, fd = socket_] {
          read_loop(pid, team, fd);
        }};
        writer_ = util::jthread {[this, fd = socket_] {
          write_loop(fd);
        }};
      }

      /// Ends a helper that isn't answering, so its reader cleans up. Call with the lock held.
      void end_helper_now() {
        if (pid_ > 0) {
          kill(pid_, SIGKILL);
        }
        if (socket_ >= 0) {
          // Unblocks the reader and the writer even if something else holds the helper's end.
          shutdown(socket_, SHUT_RDWR);
        }
      }

      /// Closes the connection; the helper destroys its devices when it reads end of file, and exits.
      void stop_helper(std::unique_lock<std::mutex> &lock) {
        if (launching_ || launcher_.joinable()) {
          // A helper still being started is stopped once it is up.
          auto launcher = std::move(launcher_);
          lock.unlock();
          if (launcher.joinable()) {
            launcher.join();
          }
          lock.lock();
        }
        if (socket_ < 0) {
          return;
        }
        stopping_ = true;
        stop_requested_ = true;
        outgoing_ready_.notify_all();
        // The helper removes its devices and exits by itself. One that doesn't is ended, so
        // whoever is closing a stream never waits on it without limit.
        if (!stopped_.wait_for(lock, stop_timeout, [this] {
              return socket_ < 0;
            })) {
          BOOST_LOG(warning) << "Touchscreen: the touch helper didn't exit within "sv << stop_timeout.count() << " seconds; ending it"sv;
          end_helper_now();
        }
        auto writer = std::move(writer_);
        auto reader = std::move(reader_);
        lock.unlock();
        if (writer.joinable()) {
          writer.join();
        }
        if (reader.joinable()) {
          reader.join();
        }
        lock.lock();
      }

      void write_loop(const int fd) {
        std::unique_lock lock {mutex_};
        while (true) {
          // Nothing goes to the helper until the host has checked the running process.
          outgoing_ready_.wait(lock, [this] {
            return stopping_ || (verified_ && !outgoing_.empty());
          });
          if (!verified_ || (outgoing_.empty() && stopping_)) {
            break;
          }
          const auto message = outgoing_.front();
          outgoing_.pop_front();
          lock.unlock();
          const auto *data = reinterpret_cast<const char *>(&message);
          std::size_t sent = 0;
          bool ok = true;
          while (sent < sizeof(message)) {
            const auto n = write(fd, data + sent, sizeof(message) - sent);
            if (n < 0 && errno == EINTR) {
              continue;
            }
            if (n <= 0) {
              ok = false;
              break;
            }
            sent += static_cast<std::size_t>(n);
          }
          lock.lock();
          if (!ok) {
            break;
          }
        }
        // Lets the helper see end of file once everything queued has gone out. The reader is
        // still reading, so the descriptor is closed only after both threads are done with it.
        shutdown(fd, SHUT_WR);
        lock.unlock();
        finished_with(fd);
      }

      /// Called by the reader and the writer; the second one to finish closes the socket.
      void finished_with(const int fd) {
        std::unique_lock close_lock {close_mutex_};
        if (++closers_done_ == 2) {
          close(fd);
        }
      }

      void read_loop(const pid_t pid, const std::string team, const int fd) {
        bool verified = false;
        bool check_failed = false;
        bool clean_eof = false;
        wire::message_t message {};
        while (true) {
          auto *data = reinterpret_cast<char *>(&message);
          std::size_t received = 0;
          while (received < sizeof(message)) {
            const auto n = read(fd, data + received, sizeof(message) - received);
            if (n < 0 && errno == EINTR) {
              continue;
            }
            if (n <= 0) {
              clean_eof = n == 0 && received == 0;
              break;
            }
            received += static_cast<std::size_t>(n);
          }
          if (received < sizeof(message) || message.magic != wire::magic || message.version != wire::version) {
            break;
          }

          std::unique_lock lock {mutex_};
          switch (static_cast<wire::type_e>(message.type)) {
            case wire::type_e::hello:
              // Check the process itself too, in case the bundle changed after the check on disk.
              // Not under the lock: the input thread takes it for every touch.
              lock.unlock();
              verified = check_running_helper(pid, team);
              lock.lock();
              if (!verified) {
                clean_eof = false;
                check_failed = true;
                lock.unlock();
                goto done;
              }
              verified_ = true;
              set_helper(helper_state_e::ready);
              if (devices_.empty()) {
                // Every stream that wanted it ended while it started. The writer sends what is
                // queued and closes the connection, and the helper exits.
                stopping_ = true;
                stop_requested_ = true;
              }
              outgoing_ready_.notify_all();
              break;
            case wire::type_e::created:
              // Not under the lock, so touches keep queueing meanwhile.
              if (message.status == 0) {
                lock.unlock();
                std::this_thread::sleep_for(device_settle_time);
                lock.lock();
              }
              on_created(message);
              break;
            default:
              break;
          }
        }
      done:
        int status = 0;
        // A helper that closed its end is exiting on its own. One that sent something wrong, or
        // failed the check, is stopped. Reaping it only now keeps its pid ours until here.
        if (!clean_eof) {
          kill(pid, SIGKILL);
        }
        shutdown(fd, SHUT_RDWR);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

        std::unique_lock lock {mutex_};
        stopping_ = true;
        outgoing_ready_.notify_all();
        outgoing_.clear();
        socket_ = -1;
        pid_ = -1;
        stopped_.notify_all();
        const bool expected = stop_requested_ || devices_.empty();
        const auto now = clock::now();
        for (auto &[display, device] : devices_) {
          device.state = device_state_e::failed;
          // A device made while the host was closing an idle helper isn't a failure: let the
          // next gesture start the helper again at once.
          device.failed_at = stop_requested_ ? clock::time_point {} : now;
          device.queued.clear();
        }
        if (!verified) {
          set_helper(helper_state_e::failed, check_failed ? "the running helper didn't pass its signature check"s : WIFEXITED(status) ? "it exited with status "s + std::to_string(WEXITSTATUS(status)) + " before it was ready (see Console for touch helper messages)" : "it was ended by signal "s + std::to_string(WTERMSIG(status)) + " before it was ready"s);
        } else if (expected) {
          helper_ = helper_state_e::not_started;
        } else {
          set_helper(helper_state_e::failed, "it exited while touchscreens were in use"s);
        }
        last_attempt_ = now;
        lock.unlock();
        finished_with(fd);
      }

      void on_created(const wire::message_t &message) {
        for (auto &[display, device] : devices_) {
          if (device.id != message.device || device.state != device_state_e::creating) {
            continue;
          }
          if (message.status != 0) {
            device.state = device_state_e::failed;
            device.failed_at = clock::now();
            device.queued.clear();
            BOOST_LOG(warning) << "Touchscreen: the helper couldn't make a touchscreen for display "sv << display << " (IOReturn 0x"sv << std::hex << message.status << std::dec << "); is it signed with the HID virtual device entitlement? Touch uses the emulation"sv;
            return;
          }
          device.state = device_state_e::ready;
          for (const auto &contacts : device.queued) {
            send(wire::make_contacts(device.id, contacts));
          }
          device.queued.clear();
          return;
        }
      }

      std::mutex mutex_;
      std::condition_variable outgoing_ready_;
      std::condition_variable stopped_;  ///< The reader finished with a helper.
      std::deque<wire::message_t> outgoing_;
      std::map<display_t, device_t> devices_;
      helper_state_e helper_ = helper_state_e::not_started;
      clock::time_point last_attempt_ {};
      int socket_ = -1;
      pid_t pid_ = -1;
      bool launching_ = false;  ///< The launcher thread is checking and starting the helper.
      std::uint32_t next_device_id_ = 0;
      bool stopping_ = false;
      bool stop_requested_ = false;  ///< The host closed the connection on purpose.
      bool verified_ = false;  ///< The running helper passed the host's signature check.
      bool warned_backlog_ = false;
      std::mutex close_mutex_;
      int closers_done_ = 0;
      util::jthread launcher_;
      util::jthread reader_;
      util::jthread writer_;
    };

    manager_t &manager() {
      static manager_t instance;
      return instance;
    }
  }  // namespace

  bool supported() {
    return os_supports_touchscreen(os_major()) && config::input.native_pen_touch;
  }

  bool begin_gesture(const owner_t owner, const display_t display) {
    return supported() && manager().begin_gesture(owner, display);
  }

  void prepare(const owner_t owner, const display_t display) {
    begin_gesture(owner, display);
  }

  bool touch(const owner_t owner, const display_t display, const std::uint32_t pointer, const double global_x, const double global_y) {
    return manager().touch(owner, display, pointer, global_x, global_y);
  }

  bool lift(const owner_t owner, const display_t display, const std::uint32_t pointer) {
    return manager().lift(owner, display, pointer);
  }

  void lift_owner(const owner_t owner) {
    manager().lift_owner(owner);
  }

  void release(const owner_t owner) {
    manager().release(owner);
  }
}  // namespace platf::macos_touchscreen
