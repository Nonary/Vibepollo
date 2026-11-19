/**
 * @file tools/macos_touch_helper/main.cpp
 * @brief The macOS 27 touch helper: owns the virtual HID touchscreens for the Vibepollo host.
 * @details The host spawns this helper with one end of a socket pair as file descriptor 3. The
 *          helper serves only that socket, and only when the process on the other end is its
 *          parent and is signed as the Vibepollo host by the same team as the helper. It has no
 *          other way in: no command line options, no socket on disk, and no Mach services. It exits
 *          when its parent does.
 *
 *          Creating an IOHIDUserDevice needs the com.apple.developer.hid.virtual.device
 *          entitlement, so only this helper is signed with it; the host is not.
 */
// standard includes
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// platform includes
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// local includes
#include "src/platform/macos/touchscreen_policy.h"

namespace ts = platf::macos_touchscreen;
namespace wire = ts::wire;

namespace {
  constexpr int socket_fd = 3;

  os_log_t log_handle() {
    static const os_log_t handle = os_log_create(TOUCH_HELPER_IDENTIFIER, "touch");
    return handle;
  }

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

  CFStringRef cf_string(const char *text) {
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
  }

  CFNumberRef cf_number(const int value) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
  }

  /// The team the helper itself is signed by, or empty when it has none.
  std::string own_team() {
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
    const auto team = static_cast<CFStringRef>(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier));
    char buffer[64] {};
    if (!team || CFGetTypeID(team) != CFStringGetTypeID() || !CFStringGetCString(team, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
      return {};
    }
    return buffer;
  }

  /// Whether a team identifier is the expected shape (ten uppercase letters and digits), so it can go in a requirement.
  bool plain_team(const std::string &team) {
    if (team.size() != 10) {
      return false;
    }
    for (const char c : team) {
      if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Checks that the process on the other end of the socket is the parent and is the host.
   * @details The host made the socket pair, so the peer's credentials are the host's. Checking the
   *          peer's audit token, rather than a pid, ties the check to that exact process.
   */
  bool peer_is_host() {
    pid_t peer_pid = 0;
    socklen_t length = sizeof(peer_pid);
    if (getsockopt(socket_fd, SOL_LOCAL, LOCAL_PEERPID, &peer_pid, &length) != 0 || peer_pid != getppid()) {
      os_log_error(log_handle(), "touch helper: the socket's peer is not this helper's parent");
      return false;
    }

    audit_token_t token {};
    length = sizeof(token);
    if (getsockopt(socket_fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &length) != 0) {
      os_log_error(log_handle(), "touch helper: couldn't read the parent's audit token (errno %d)", errno);
      return false;
    }

    cf_release_t token_data {CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(&token), sizeof(token))};
    const void *keys[] {kSecGuestAttributeAudit};
    const void *values[] {token_data.ref};
    cf_release_t attributes {CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    SecCodeRef code = nullptr;
    if (SecCodeCopyGuestWithAttributes(nullptr, attributes.ref, kSecCSDefaultFlags, &code) != errSecSuccess) {
      os_log_error(log_handle(), "touch helper: couldn't look up the parent's code");
      return false;
    }
    cf_release_t code_ref {code};

    // The host must be signed by the same team as this helper. A helper with no team (ad-hoc)
    // couldn't make a device anyway, since the entitlement needs a provisioning profile.
    const auto team = own_team();
    if (!plain_team(team)) {
      os_log_error(log_handle(), "touch helper: this helper isn't signed by a team");
      return false;
    }
    const std::string text = "identifier \"" TOUCH_HELPER_HOST_IDENTIFIER "\" and anchor apple generic and certificate leaf[subject.OU] = \"" + team + "\"";
    cf_release_t requirement_text {cf_string(text.c_str())};
    SecRequirementRef requirement = nullptr;
    if (SecRequirementCreateWithString(requirement_text.ref, kSecCSDefaultFlags, &requirement) != errSecSuccess) {
      return false;
    }
    cf_release_t requirement_ref {requirement};
    const auto status = SecCodeCheckValidity(code, kSecCSDefaultFlags, requirement);
    if (status != errSecSuccess) {
      os_log_error(log_handle(), "touch helper: the parent isn't the Vibepollo host signed by this team (%d)", static_cast<int>(status));
      return false;
    }
    return true;
  }

  bool write_message(const wire::message_t &message) {
    const auto *data = reinterpret_cast<const char *>(&message);
    std::size_t sent = 0;
    while (sent < sizeof(message)) {
      const auto n = write(socket_fd, data + sent, sizeof(message) - sent);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  /// One virtual touchscreen.
  struct device_t {
    IOHIDUserDeviceRef ref {};
    dispatch_queue_t queue {};
  };

  std::map<std::uint32_t, device_t> devices;

  IOHIDUserDeviceRef create_device(const std::string &display_uuid, IOReturn &result) {
    const auto &descriptor = ts::report_descriptor();
    cf_release_t descriptor_data {CFDataCreate(kCFAllocatorDefault, descriptor.data(), static_cast<CFIndex>(descriptor.size()))};
    cf_release_t properties {CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    cf_release_t transport {cf_string("Virtual")};
    cf_release_t product {cf_string("Vibepollo Touchscreen")};
    cf_release_t manufacturer {cf_string("Vibepollo")};
    cf_release_t uuid {cf_string(display_uuid.c_str())};
    cf_release_t page {cf_number(0x0D)};
    cf_release_t usage {cf_number(0x04)};
    cf_release_t zero {cf_number(0)};

    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDReportDescriptorKey), descriptor_data.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDTransportKey), transport.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDProductKey), product.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDManufacturerKey), manufacturer.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDVendorIDKey), zero.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDProductIDKey), zero.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDPrimaryUsagePageKey), page.ref);
    CFDictionarySetValue(properties.ref, CFSTR(kIOHIDPrimaryUsageKey), usage.ref);
    // SkyLight's WSHIDDisplayEventListener reads this from a Touch Screen or Digitizer service
    // and parses it with CFUUIDCreateFromString to pick the display the digitizer belongs to.
    CFDictionarySetValue(properties.ref, CFSTR("displayUUID"), uuid.ref);

    auto *device = IOHIDUserDeviceCreateWithProperties(kCFAllocatorDefault, properties.ref, 0);
    result = device ? kIOReturnSuccess : kIOReturnNotPermitted;
    return device;
  }

  void destroy_device(const std::uint32_t id) {
    const auto found = devices.find(id);
    if (found == devices.end()) {
      return;
    }
    auto device = found->second;
    devices.erase(found);

    // Lift whatever is still down before the device goes away.
    const auto lift = ts::pack_input_report({});
    IOHIDUserDeviceHandleReportWithTimeStamp(device.ref, mach_absolute_time(), lift.data(), lift.size());

    // The header says to release the device only after the cancel handler has run.
    const auto done = dispatch_semaphore_create(0);
    IOHIDUserDeviceSetCancelHandler(device.ref, ^{
      dispatch_semaphore_signal(done);
    });
    IOHIDUserDeviceCancel(device.ref);
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CFRelease(device.ref);
    dispatch_release(device.queue);
    dispatch_release(done);
  }

  void handle_create(const wire::message_t &message) {
    destroy_device(message.device);
    const std::string uuid {message.uuid, wire::uuid_length};

    auto reply = wire::make(wire::type_e::created, message.device);
    IOReturn result = kIOReturnSuccess;
    auto *ref = create_device(uuid, result);
    if (!ref) {
      os_log_error(log_handle(), "touch helper: IOHIDUserDeviceCreateWithProperties failed for display %{public}s; is the helper signed with the HID virtual device entitlement?", uuid.c_str());
      reply.status = result;
      write_message(reply);
      return;
    }

    device_t device {ref, dispatch_queue_create("touch helper device", DISPATCH_QUEUE_SERIAL)};
    IOHIDUserDeviceSetDispatchQueue(ref, device.queue);
    IOHIDUserDeviceRegisterGetReportBlock(ref, ^IOReturn(IOHIDReportType type, uint32_t report_id, uint8_t *report, CFIndex *length) {
      if (type != kIOHIDReportTypeFeature || report_id != ts::feature_report_id || !length || *length < static_cast<CFIndex>(ts::feature_report_size)) {
        return kIOReturnUnsupported;
      }
      const auto feature = ts::pack_feature_report();
      std::memcpy(report, feature.data(), feature.size());
      *length = static_cast<CFIndex>(feature.size());
      return kIOReturnSuccess;
    });
    IOHIDUserDeviceActivate(ref);
    devices[message.device] = device;
    os_log(log_handle(), "touch helper: touchscreen %u created for display %{public}s", message.device, uuid.c_str());
    write_message(reply);
  }

  void handle_contacts(const wire::message_t &message) {
    const auto found = devices.find(message.device);
    if (found == devices.end()) {
      return;
    }
    const auto report = ts::pack_input_report(wire::contacts_of(message));
    const auto status = IOHIDUserDeviceHandleReportWithTimeStamp(found->second.ref, mach_absolute_time(), report.data(), report.size());
    if (status != kIOReturnSuccess) {
      os_log_error(log_handle(), "touch helper: report on touchscreen %u failed (0x%x)", message.device, status);
    }
  }

  void destroy_all() {
    while (!devices.empty()) {
      destroy_device(devices.begin()->first);
    }
  }

  /// Reads one whole message. Returns false at end of file or on error.
  bool read_message(wire::message_t &message) {
    auto *data = reinterpret_cast<char *>(&message);
    std::size_t received = 0;
    while (received < sizeof(message)) {
      const auto n = read(socket_fd, data + received, sizeof(message) - received);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      received += static_cast<std::size_t>(n);
    }
    return true;
  }

  /// Exits when the parent does, even if something else still holds the other end of the socket.
  void watch_parent() {
    const auto parent = getppid();
    const auto kq = kqueue();
    if (kq < 0) {
      return;
    }
    struct kevent change {};
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    if (kevent(kq, &change, 1, nullptr, 0, nullptr) != 0) {
      // The parent is already gone.
      _exit(0);
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      struct kevent event {};
      while (kevent(kq, nullptr, 0, &event, 1, nullptr) < 0 && errno == EINTR) {}
      // Cancelling devices from here would race the main loop; the kernel removes a user device
      // when its owner exits, so exiting is enough.
      _exit(0);
    });
  }
}  // namespace

int main(const int argc, const char *[]) {
  // There are no modes or options: any argument means this wasn't started by the host.
  if (argc != 1) {
    std::fprintf(stderr, "This helper is started by Vibepollo and takes no arguments.\n");
    return 64;
  }

  struct stat info {};
  if (fstat(socket_fd, &info) != 0 || !S_ISSOCK(info.st_mode)) {
    std::fprintf(stderr, "This helper is started by Vibepollo and can't be run on its own.\n");
    return 64;
  }

  // Without this, a write after the host exits would kill the helper before it cleans up.
  signal(SIGPIPE, SIG_IGN);

  if (!peer_is_host()) {
    return 77;
  }
  watch_parent();

  if (!write_message(wire::make(wire::type_e::hello, 0))) {
    return 0;
  }

  wire::message_t message {};
  while (read_message(message)) {
    if (!wire::valid_command(message)) {
      os_log_error(log_handle(), "touch helper: dropped a malformed message; exiting");
      break;
    }
    switch (static_cast<wire::type_e>(message.type)) {
      case wire::type_e::create:
        handle_create(message);
        break;
      case wire::type_e::contacts:
        handle_contacts(message);
        break;
      case wire::type_e::destroy:
        destroy_device(message.device);
        write_message(wire::make(wire::type_e::destroyed, message.device));
        break;
      default:
        break;
    }
  }

  destroy_all();
  return 0;
}
