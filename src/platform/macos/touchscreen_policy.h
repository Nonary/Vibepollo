/**
 * @file src/platform/macos/touchscreen_policy.h
 * @brief Framework-free parts of the macOS 27 virtual touchscreen: the HID report format, contact
 *        ids, coordinate mapping, the host-helper messages, and when the touchscreen is used.
 * @details Both the host and the touch helper compile this file. It uses no Apple frameworks, so
 *          the unit tests can check it without a device.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace platf::macos_touchscreen {
  /**
   * @brief Contacts one device reports at once.
   * @details BackBoard drops any contact whose index is 30 or more ("%d >= MAX_PATH_COUNT" in
   *          BackBoardHIDTouchEventProcessor), so ids stay below this and are reused.
   */
  constexpr std::size_t max_contacts = 10;

  /// Highest X or Y value; 0..logical_max spans the whole display.
  constexpr std::uint16_t logical_max = 32767;

  /// Report ids in the descriptor.
  constexpr std::uint8_t input_report_id = 1;
  constexpr std::uint8_t feature_report_id = 2;

  /// Bytes in one contact of the input report: flags, contact id, X, and Y.
  constexpr std::size_t contact_report_size = 6;

  /// Bytes in the input report, including its report id and the contact count.
  constexpr std::size_t input_report_size = 1 + max_contacts * contact_report_size + 1;

  /// Bytes in the feature report: its report id and Contact Count Maximum.
  constexpr std::size_t feature_report_size = 2;

  /// Bits in a contact's flags byte.
  enum contact_flag_e : std::uint8_t {
    flag_tip = 1 << 0,  ///< Tip Switch: the finger touches the screen.
    flag_in_range = 1 << 1,  ///< In Range.
    flag_valid = 1 << 2,  ///< Touch Valid (Confidence, 0x0D/0x47): a finger, not a palm.
  };

  /**
   * @brief The HID report descriptor of a multitouch touchscreen.
   * @details One application collection (Digitizer page 0x0D, Touch Screen 0x04, which SkyLight
   *          accepts as a digitizer) with max_contacts finger collections, a contact count, and a
   *          Contact Count Maximum feature report.
   */
  const std::vector<std::uint8_t> &report_descriptor();

  /// One contact as the helper reports it.
  struct contact_t {
    std::uint8_t id;  ///< Contact Identifier, below max_contacts.
    bool tip;  ///< Touching; false in the one report that lifts it.
    std::uint16_t x;
    std::uint16_t y;

    bool operator==(const contact_t &) const = default;
  };

  /**
   * @brief Packs contacts into an input report.
   * @param contacts The contacts; each goes in the finger collection of its id, and ids at or
   *        past max_contacts are left out.
   * @return The report, input_report_size bytes. A collection with no contact carries its id
   *         with no flags, so a contact missing from a report reads as lifted.
   */
  std::array<std::uint8_t, input_report_size> pack_input_report(const std::vector<contact_t> &contacts);

  /// The feature report answering a GET_REPORT for Contact Count Maximum.
  std::array<std::uint8_t, feature_report_size> pack_feature_report();

  /// A display's bounds, in global points.
  struct bounds_t {
    double x;
    double y;
    double width;
    double height;
  };

  /// A point in the device's logical range.
  struct logical_point_t {
    std::uint16_t x;
    std::uint16_t y;

    bool operator==(const logical_point_t &) const = default;
  };

  /**
   * @brief Maps a global screen point to the logical range of the touchscreen on that display.
   * @details The device covers the whole display, so the display's top left is 0 and its bottom
   *          right is logical_max. Points outside the display are clamped to its edge.
   */
  logical_point_t to_logical(double global_x, double global_y, const bounds_t &display);

  /**
   * @brief The contacts of one touchscreen, shared by every client streaming its display.
   * @details Each client's pointer ids map to small contact ids, lowest free first, so ids stay
   *          below max_contacts however many touches a client has made. A lifted contact stays in
   *          one more report with its tip up, then its id is free again.
   */
  class contact_table_t {
  public:
    using owner_t = const void *;

    /**
     * @brief Puts a finger down, or moves it if it is already down.
     * @return Its contact id, or nothing when every id is in use.
     */
    std::optional<std::uint8_t> down(owner_t owner, std::uint32_t pointer, logical_point_t point);

    /// Moves a finger that is down. Returns false when the finger isn't down.
    bool move(owner_t owner, std::uint32_t pointer, logical_point_t point);

    /// Lifts a finger. Returns false when the finger isn't down.
    bool up(owner_t owner, std::uint32_t pointer);

    /// Lifts every finger of one client. Returns how many were down.
    std::size_t lift_owner(owner_t owner);

    /// Lifts every finger. Returns how many were down.
    std::size_t lift_all();

    /**
     * @brief The contacts for the next report; lifted contacts are in it once, then forgotten.
     */
    std::vector<contact_t> take_report();

    /// Fingers down, not counting ones lifted since the last report.
    std::size_t touching() const;

    /// Fingers down for one client.
    std::size_t touching(owner_t owner) const;

    /// Whether a report is needed to show a lift.
    bool lifting() const;

  private:
    enum class slot_state_e : std::uint8_t {
      free,
      touching,
      lifting,
    };

    struct slot_t {
      slot_state_e state = slot_state_e::free;
      owner_t owner = nullptr;
      std::uint32_t pointer = 0;
      logical_point_t point {};
    };

    slot_t *find(owner_t owner, std::uint32_t pointer);

    std::array<slot_t, max_contacts> slots_ {};
  };

  /// What the host knows about the touch helper.
  enum class helper_state_e {
    not_started,  ///< Never started, or stopped because no stream needed it.
    starting,  ///< Spawned, waiting for it to answer.
    ready,  ///< Running and answering.
    unavailable,  ///< Missing, or its signature isn't valid or isn't the expected identifier.
    failed,  ///< Ran but couldn't make a device, or exited.
  };

  /// Whether this macOS has the digitizer routing the touchscreen relies on.
  bool os_supports_touchscreen(int os_major);

  /**
   * @brief Parses the major version from a product version string such as "27.2".
   * @return The major version, or 0 when the string isn't a version.
   */
  int parse_os_major(std::string_view product_version);

  /// Inputs to the touchscreen gate.
  struct gate_t {
    int os_major = 0;
    bool native_pen_touch = false;
    helper_state_e helper = helper_state_e::not_started;
  };

  /// Whether fingers go to the HID touchscreen now; otherwise they use the emulation.
  bool use_touchscreen(const gate_t &gate);

  /**
   * @brief Whether a touch event may start the helper or create a device.
   * @details Only a finger landing does. LI_TOUCH_EVENT_CANCEL_ALL, which every session end sends,
   *          and the other events never do.
   * @param event_type The touch event's LI_TOUCH_EVENT_* type.
   */
  bool event_may_create(std::uint8_t event_type);

  /**
   * @brief Whether to try the helper again after it was unavailable or failed.
   * @details It is retried after retry_after, so a helper signed later or restarted after a crash
   *          is picked up, without retrying (and logging) on every touch.
   */
  constexpr std::chrono::seconds retry_after {30};

  bool should_try_helper(const gate_t &gate, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point last_attempt);

  /**
   * @brief Messages between the host and the helper over their socket pair.
   * @details Every message is message_size bytes. The helper checks each field, so a bad message
   *          can't make it report anything outside one device's contacts.
   */
  namespace wire {
    constexpr std::uint32_t magic = 0x56544348;  // "VTCH"
    constexpr std::uint16_t version = 1;

    /// Devices one helper keeps at once, one per streamed display.
    constexpr std::uint32_t max_devices = 8;

    /// Characters in a display UUID string such as "37D8832A-2D66-02CA-B9F7-8F30A301B230".
    constexpr std::size_t uuid_length = 36;

    enum class type_e : std::uint16_t {
      // Host to helper.
      create = 1,  ///< Make a touchscreen on the display named by uuid.
      contacts = 2,  ///< The full set of contacts on a device.
      destroy = 3,  ///< Lift every contact, cancel the device, and wait for the cancel.

      // Helper to host.
      hello = 100,  ///< The helper checked its parent and is ready for commands.
      created = 101,  ///< status is 0, or the IOReturn that creating failed with.
      destroyed = 102,
    };

    struct wire_contact_t {
      std::uint8_t id;
      std::uint8_t tip;
      std::uint16_t x;
      std::uint16_t y;
    };

    struct message_t {
      std::uint32_t magic;
      std::uint16_t version;
      std::uint16_t type;
      std::uint32_t device;
      std::int32_t status;
      std::uint32_t count;
      char uuid[uuid_length];
      wire_contact_t contacts[max_contacts];
    };

    constexpr std::size_t message_size = sizeof(message_t);

    /// A message with the header filled in.
    message_t make(type_e type, std::uint32_t device);

    /// A contacts message for a device.
    message_t make_contacts(std::uint32_t device, const std::vector<contact_t> &contacts);

    /// Whether a display UUID string is the expected shape: hex digits and dashes, 8-4-4-4-12.
    bool valid_uuid(std::string_view uuid);

    /**
     * @brief Checks a message from the host, as the helper does before acting on it.
     * @return Whether every field is in range for its type.
     */
    bool valid_command(const message_t &message);

    /**
     * @brief Whether a newer contacts message may replace one that hasn't been sent yet.
     * @details Only when both are for the same device and hold the same fingers, all touching:
     *          then the newer one is the same fingers a moment later, and skipping the older
     *          one loses nothing. A finger landing or lifting is never skipped, since macOS has
     *          to see each of those.
     */
    bool supersedes(const message_t &newer, const message_t &queued);

    /// The contacts in a valid contacts message.
    std::vector<contact_t> contacts_of(const message_t &message);
  }  // namespace wire
}  // namespace platf::macos_touchscreen
