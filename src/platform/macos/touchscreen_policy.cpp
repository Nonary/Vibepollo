/**
 * @file src/platform/macos/touchscreen_policy.cpp
 * @brief Framework-free parts of the macOS 27 virtual touchscreen.
 */
// local includes
#include "touchscreen_policy.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>

// lib includes
#include <moonlight-common-c/src/Limelight.h>

namespace platf::macos_touchscreen {
  namespace {
    void append(std::vector<std::uint8_t> &out, std::initializer_list<std::uint8_t> bytes) {
      out.insert(out.end(), bytes);
    }

    std::vector<std::uint8_t> build_descriptor() {
      std::vector<std::uint8_t> d;
      append(d, {0x05, 0x0D});  // Usage Page (Digitizer)
      append(d, {0x09, 0x04});  // Usage (Touch Screen)
      append(d, {0xA1, 0x01});  // Collection (Application)
      append(d, {0x85, input_report_id});  // Report ID

      for (std::size_t i = 0; i < max_contacts; ++i) {
        append(d, {0x05, 0x0D});  // Usage Page (Digitizer)
        append(d, {0x09, 0x22});  // Usage (Finger)
        append(d, {0xA1, 0x02});  // Collection (Logical)
        append(d, {0x09, 0x42});  //   Usage (Tip Switch)
        append(d, {0x09, 0x32});  //   Usage (In Range)
        append(d, {0x09, 0x47});  //   Usage (Touch Valid)
        append(d, {0x15, 0x00});  //   Logical Minimum (0)
        append(d, {0x25, 0x01});  //   Logical Maximum (1)
        append(d, {0x75, 0x01});  //   Report Size (1)
        append(d, {0x95, 0x03});  //   Report Count (3)
        append(d, {0x81, 0x02});  //   Input (Data, Variable, Absolute)
        append(d, {0x95, 0x05});  //   Report Count (5)
        append(d, {0x81, 0x03});  //   Input (Constant): padding to a byte
        append(d, {0x09, 0x51});  //   Usage (Contact Identifier)
        append(d, {0x25, static_cast<std::uint8_t>(max_contacts - 1)});  // Logical Maximum
        append(d, {0x75, 0x08});  //   Report Size (8)
        append(d, {0x95, 0x01});  //   Report Count (1)
        append(d, {0x81, 0x02});  //   Input (Data, Variable, Absolute)
        append(d, {0x05, 0x01});  //   Usage Page (Generic Desktop)
        append(d, {0x26, logical_max & 0xFF, logical_max >> 8});  // Logical Maximum
        append(d, {0x75, 0x10});  //   Report Size (16)
        append(d, {0x95, 0x01});  //   Report Count (1)
        append(d, {0x09, 0x30});  //   Usage (X)
        append(d, {0x81, 0x02});  //   Input (Data, Variable, Absolute)
        append(d, {0x09, 0x31});  //   Usage (Y)
        append(d, {0x81, 0x02});  //   Input (Data, Variable, Absolute)
        append(d, {0xC0});  // End Collection
      }

      append(d, {0x05, 0x0D});  // Usage Page (Digitizer)
      append(d, {0x09, 0x54});  // Usage (Contact Count)
      append(d, {0x25, static_cast<std::uint8_t>(max_contacts)});  // Logical Maximum
      append(d, {0x75, 0x08});  // Report Size (8)
      append(d, {0x95, 0x01});  // Report Count (1)
      append(d, {0x81, 0x02});  // Input (Data, Variable, Absolute)

      append(d, {0x85, feature_report_id});  // Report ID
      append(d, {0x09, 0x55});  // Usage (Contact Count Maximum)
      append(d, {0xB1, 0x02});  // Feature (Data, Variable, Absolute)
      append(d, {0xC0});  // End Collection
      return d;
    }

    std::uint16_t clamp_logical(const int value) {
      return static_cast<std::uint16_t>(std::clamp(value, 0, static_cast<int>(logical_max)));
    }
  }  // namespace

  const std::vector<std::uint8_t> &report_descriptor() {
    static const auto descriptor = build_descriptor();
    return descriptor;
  }

  std::array<std::uint8_t, input_report_size> pack_input_report(const std::vector<contact_t> &contacts) {
    std::array<std::uint8_t, input_report_size> report {};
    report[0] = input_report_id;

    // Each finger collection is one contact id, always in the same place, so a contact never
    // changes position when another lifts. A collection with nothing on it still carries its
    // own id with every flag clear: all zeros would read as contact 0, lifted.
    for (std::size_t slot = 0; slot < max_contacts; ++slot) {
      report[1 + slot * contact_report_size + 1] = static_cast<std::uint8_t>(slot);
    }

    std::uint8_t count = 0;
    bool written[max_contacts] {};
    for (const auto &contact : contacts) {
      if (contact.id >= max_contacts || written[contact.id]) {
        continue;
      }
      written[contact.id] = true;
      auto *out = report.data() + 1 + contact.id * contact_report_size;
      // A finger is touching and in range, or lifted and out of range: "in range" without the
      // tip means hovering. A lifted finger keeps Touch Valid and its last position, so the
      // lift is a finger leaving there, not a contact being rejected.
      out[0] = flag_valid | (contact.tip ? flag_tip | flag_in_range : 0);
      out[2] = static_cast<std::uint8_t>(contact.x & 0xFF);
      out[3] = static_cast<std::uint8_t>(contact.x >> 8);
      out[4] = static_cast<std::uint8_t>(contact.y & 0xFF);
      out[5] = static_cast<std::uint8_t>(contact.y >> 8);
      ++count;
    }
    report[input_report_size - 1] = count;
    return report;
  }

  std::array<std::uint8_t, feature_report_size> pack_feature_report() {
    return {feature_report_id, static_cast<std::uint8_t>(max_contacts)};
  }

  logical_point_t to_logical(const double global_x, const double global_y, const bounds_t &display) {
    if (!(display.width > 0) || !(display.height > 0)) {
      return {0, 0};
    }
    const auto scale = [](const double offset, const double size) {
      const auto fraction = std::clamp(offset / size, 0.0, 1.0);
      return clamp_logical(static_cast<int>(std::lround(fraction * logical_max)));
    };
    return {scale(global_x - display.x, display.width), scale(global_y - display.y, display.height)};
  }

  contact_table_t::slot_t *contact_table_t::find(const owner_t owner, const std::uint32_t pointer) {
    for (auto &slot : slots_) {
      if (slot.state == slot_state_e::touching && slot.owner == owner && slot.pointer == pointer) {
        return &slot;
      }
    }
    return nullptr;
  }

  std::optional<std::uint8_t> contact_table_t::down(const owner_t owner, const std::uint32_t pointer, const logical_point_t point) {
    if (auto *slot = find(owner, pointer)) {
      slot->point = point;
      return static_cast<std::uint8_t>(slot - slots_.data());
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].state == slot_state_e::free) {
        slots_[i] = {slot_state_e::touching, owner, pointer, point};
        return static_cast<std::uint8_t>(i);
      }
    }
    return std::nullopt;
  }

  bool contact_table_t::move(const owner_t owner, const std::uint32_t pointer, const logical_point_t point) {
    auto *slot = find(owner, pointer);
    if (!slot) {
      return false;
    }
    slot->point = point;
    return true;
  }

  bool contact_table_t::up(const owner_t owner, const std::uint32_t pointer) {
    auto *slot = find(owner, pointer);
    if (!slot) {
      return false;
    }
    slot->state = slot_state_e::lifting;
    return true;
  }

  std::size_t contact_table_t::lift_owner(const owner_t owner) {
    std::size_t lifted = 0;
    for (auto &slot : slots_) {
      if (slot.state == slot_state_e::touching && slot.owner == owner) {
        slot.state = slot_state_e::lifting;
        ++lifted;
      }
    }
    return lifted;
  }

  std::size_t contact_table_t::lift_all() {
    std::size_t lifted = 0;
    for (auto &slot : slots_) {
      if (slot.state == slot_state_e::touching) {
        slot.state = slot_state_e::lifting;
        ++lifted;
      }
    }
    return lifted;
  }

  std::vector<contact_t> contact_table_t::take_report() {
    std::vector<contact_t> contacts;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto &slot = slots_[i];
      if (slot.state == slot_state_e::free) {
        continue;
      }
      contacts.push_back({static_cast<std::uint8_t>(i), slot.state == slot_state_e::touching, slot.point.x, slot.point.y});
      if (slot.state == slot_state_e::lifting) {
        slot = {};
      }
    }
    return contacts;
  }

  std::size_t contact_table_t::touching() const {
    return std::count_if(slots_.begin(), slots_.end(), [](const slot_t &slot) {
      return slot.state == slot_state_e::touching;
    });
  }

  std::size_t contact_table_t::touching(const owner_t owner) const {
    return std::count_if(slots_.begin(), slots_.end(), [owner](const slot_t &slot) {
      return slot.state == slot_state_e::touching && slot.owner == owner;
    });
  }

  bool contact_table_t::lifting() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const slot_t &slot) {
      return slot.state == slot_state_e::lifting;
    });
  }

  bool os_supports_touchscreen(const int os_major) {
    return os_major >= 27;
  }

  int parse_os_major(const std::string_view product_version) {
    int major = 0;
    const auto *end = product_version.data() + product_version.size();
    const auto [ptr, ec] = std::from_chars(product_version.data(), end, major);
    if (ec != std::errc {} || ptr == product_version.data() || (ptr != end && *ptr != '.')) {
      return 0;
    }
    return major;
  }

  bool use_touchscreen(const gate_t &gate) {
    return os_supports_touchscreen(gate.os_major) && gate.native_pen_touch && gate.helper == helper_state_e::ready;
  }

  bool event_may_create(const std::uint8_t event_type) {
    return event_type == LI_TOUCH_EVENT_DOWN;
  }

  bool should_try_helper(const gate_t &gate, const std::chrono::steady_clock::time_point now, const std::chrono::steady_clock::time_point last_attempt) {
    if (!os_supports_touchscreen(gate.os_major) || !gate.native_pen_touch) {
      return false;
    }
    switch (gate.helper) {
      case helper_state_e::not_started:
        return true;
      case helper_state_e::unavailable:
      case helper_state_e::failed:
        return now - last_attempt >= retry_after;
      case helper_state_e::starting:
      case helper_state_e::ready:
        return false;
    }
    return false;
  }

  namespace wire {
    message_t make(const type_e type, const std::uint32_t device) {
      message_t message {};
      message.magic = magic;
      message.version = version;
      message.type = static_cast<std::uint16_t>(type);
      message.device = device;
      return message;
    }

    message_t make_contacts(const std::uint32_t device, const std::vector<contact_t> &contacts) {
      auto message = make(type_e::contacts, device);
      message.count = static_cast<std::uint32_t>(std::min(contacts.size(), max_contacts));
      for (std::size_t i = 0; i < message.count; ++i) {
        message.contacts[i] = {contacts[i].id, static_cast<std::uint8_t>(contacts[i].tip ? 1 : 0), contacts[i].x, contacts[i].y};
      }
      return message;
    }

    bool valid_uuid(const std::string_view uuid) {
      if (uuid.size() != uuid_length) {
        return false;
      }
      for (std::size_t i = 0; i < uuid.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? uuid[i] != '-' : !std::isxdigit(static_cast<unsigned char>(uuid[i]))) {
          return false;
        }
      }
      return true;
    }

    bool valid_command(const message_t &message) {
      if (message.magic != magic || message.version != version || message.device >= max_devices) {
        return false;
      }
      switch (static_cast<type_e>(message.type)) {
        case type_e::create:
          return valid_uuid(std::string_view {message.uuid, uuid_length});
        case type_e::destroy:
          return true;
        case type_e::contacts: {
          if (message.count > max_contacts) {
            return false;
          }
          bool seen[max_contacts] {};
          for (std::uint32_t i = 0; i < message.count; ++i) {
            const auto &contact = message.contacts[i];
            if (contact.id >= max_contacts || seen[contact.id] || contact.tip > 1 || contact.x > logical_max || contact.y > logical_max) {
              return false;
            }
            seen[contact.id] = true;
          }
          return true;
        }
        default:
          return false;
      }
    }

    bool supersedes(const message_t &newer, const message_t &queued) {
      const auto contacts = static_cast<std::uint16_t>(type_e::contacts);
      if (newer.type != contacts || queued.type != contacts || newer.device != queued.device || newer.count != queued.count || newer.count > max_contacts) {
        return false;
      }
      for (std::uint32_t i = 0; i < newer.count; ++i) {
        if (newer.contacts[i].id != queued.contacts[i].id || !newer.contacts[i].tip || !queued.contacts[i].tip) {
          return false;
        }
      }
      return true;
    }

    std::vector<contact_t> contacts_of(const message_t &message) {
      std::vector<contact_t> contacts;
      const auto count = std::min<std::size_t>(message.count, max_contacts);
      for (std::size_t i = 0; i < count; ++i) {
        const auto &contact = message.contacts[i];
        contacts.push_back({contact.id, contact.tip != 0, contact.x, contact.y});
      }
      return contacts;
    }
  }  // namespace wire
}  // namespace platf::macos_touchscreen
