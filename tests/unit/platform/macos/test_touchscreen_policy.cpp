/**
 * @file tests/unit/platform/macos/test_touchscreen_policy.cpp
 * @brief Tests for the macOS 27 touchscreen's report format, contact ids, coordinates, gate, and
 *        host-helper messages.
 */
#include "../../../tests_common.h"

#include <moonlight-common-c/src/Limelight.h>
#include <src/platform/macos/touchscreen_policy.h>

#include <cstring>
#include <map>
#include <random>
#include <set>

using namespace platf::macos_touchscreen;
using namespace std::literals;

namespace {
  struct report_bits_t {
    std::map<int, int> input;  ///< Bits per input report id.
    std::map<int, int> feature;  ///< Bits per feature report id.
    int max_contact_identifier = -1;
    int finger_collections = 0;
    int valid_usages = 0;
  };

  // A small HID descriptor walker: enough to add up report sizes and check the contact fields.
  report_bits_t walk(const std::vector<std::uint8_t> &d) {
    report_bits_t bits;
    int report_id = 0, report_size = 0, report_count = 0, usage_page = 0, logical_max = 0;
    std::vector<int> usages;
    for (std::size_t i = 0; i < d.size();) {
      const auto prefix = d[i];
      const int size = (prefix & 0x03) == 3 ? 4 : (prefix & 0x03);
      std::uint32_t value = 0;
      for (int b = 0; b < size; ++b) {
        value |= static_cast<std::uint32_t>(d[i + 1 + b]) << (8 * b);
      }
      const auto tag = prefix & 0xFC;
      switch (tag) {
        case 0x04: usage_page = static_cast<int>(value); break;
        case 0x24: logical_max = static_cast<int>(value); break;
        case 0x74: report_size = static_cast<int>(value); break;
        case 0x94: report_count = static_cast<int>(value); break;
        case 0x84: report_id = static_cast<int>(value); break;
        case 0x08: usages.push_back(usage_page << 16 | static_cast<int>(value)); break;
        case 0xA0:
          if (!usages.empty() && usages.back() == (0x0D << 16 | 0x22)) {
            ++bits.finger_collections;
          }
          usages.clear();
          break;
        case 0x80:
        case 0xB0: {
          auto &map = tag == 0x80 ? bits.input : bits.feature;
          map[report_id] += report_size * report_count;
          for (const auto usage : usages) {
            if (usage == (0x0D << 16 | 0x51)) {
              bits.max_contact_identifier = logical_max;
            }
            if (usage == (0x0D << 16 | 0x47)) {
              ++bits.valid_usages;
            }
          }
          usages.clear();
          break;
        }
        default: break;
      }
      i += 1 + size;
    }
    return bits;
  }

  int a = 0, b = 0;
  const contact_table_t::owner_t client_a = &a;
  const contact_table_t::owner_t client_b = &b;
}  // namespace

TEST(MacosTouchscreenDescriptor, ReportSizesMatchThePackedReports) {
  const auto bits = walk(report_descriptor());
  EXPECT_EQ(bits.input.at(input_report_id), static_cast<int>((input_report_size - 1) * 8));
  EXPECT_EQ(bits.feature.at(feature_report_id), static_cast<int>((feature_report_size - 1) * 8));
}

TEST(MacosTouchscreenDescriptor, EveryContactIsAValidFingerWithAnIdBelowBackBoardsLimit) {
  const auto bits = walk(report_descriptor());
  EXPECT_EQ(bits.finger_collections, static_cast<int>(max_contacts));
  EXPECT_EQ(bits.valid_usages, static_cast<int>(max_contacts));
  EXPECT_LT(bits.max_contact_identifier, 30);
  EXPECT_LT(max_contacts, 30u);
}

TEST(MacosTouchscreenDescriptor, IsATouchScreenApplicationCollection) {
  const auto &d = report_descriptor();
  ASSERT_GE(d.size(), 6u);
  // Usage Page (Digitizer), Usage (Touch Screen), Collection (Application): SkyLight looks for 0x0D/0x04.
  EXPECT_EQ(std::vector<std::uint8_t>(d.begin(), d.begin() + 6), (std::vector<std::uint8_t> {0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01}));
}

namespace {
  // The bytes of one finger collection in a report.
  const std::uint8_t *slot_of(const std::array<std::uint8_t, input_report_size> &report, const std::size_t id) {
    return report.data() + 1 + id * contact_report_size;
  }
}  // namespace

TEST(MacosTouchscreenReport, PutsEachContactInTheCollectionOfItsId) {
  const auto report = pack_input_report({{3, true, 0x1234, 0x7FFF}, {0, false, 1, 2}});
  ASSERT_EQ(report.size(), input_report_size);
  EXPECT_EQ(report[0], input_report_id);

  const auto *touching = slot_of(report, 3);
  EXPECT_EQ(touching[0], flag_tip | flag_in_range | flag_valid);
  EXPECT_EQ(touching[1], 3);
  EXPECT_EQ(touching[2], 0x34);
  EXPECT_EQ(touching[3], 0x12);
  EXPECT_EQ(touching[4], 0xFF);
  EXPECT_EQ(touching[5], 0x7F);

  // A lifted finger is out of range (in range without the tip would be hovering), still a
  // valid finger, at its last position.
  const auto *lifted = slot_of(report, 0);
  EXPECT_EQ(lifted[0], flag_valid);
  EXPECT_EQ(lifted[1], 0);
  EXPECT_EQ(lifted[2], 1);
  EXPECT_EQ(lifted[4], 2);
  EXPECT_EQ(report[input_report_size - 1], 2);
}

TEST(MacosTouchscreenReport, AContactKeepsItsPlaceWhenAnotherLifts) {
  const auto both = pack_input_report({{0, true, 100, 100}, {1, true, 200, 200}});
  const auto second_only = pack_input_report({{1, true, 210, 210}});
  // Contact 1 stays in collection 1, and collection 0 reads as contact 0, not touching.
  EXPECT_EQ(slot_of(both, 1)[1], 1);
  EXPECT_EQ(slot_of(second_only, 1)[0], flag_tip | flag_in_range | flag_valid);
  EXPECT_EQ(slot_of(second_only, 1)[1], 1);
  EXPECT_EQ(slot_of(second_only, 0)[0], 0);
  EXPECT_EQ(slot_of(second_only, 0)[1], 0);
}

TEST(MacosTouchscreenReport, EmptyCollectionsCarryTheirOwnIdAndNoFlags) {
  const auto report = pack_input_report({});
  EXPECT_EQ(report[0], input_report_id);
  for (std::size_t id = 0; id < max_contacts; ++id) {
    const auto *slot = slot_of(report, id);
    EXPECT_EQ(slot[0], 0) << id;
    EXPECT_EQ(slot[1], id) << id;
    for (std::size_t i = 2; i < contact_report_size; ++i) {
      EXPECT_EQ(slot[i], 0) << id;
    }
  }
  EXPECT_EQ(report[input_report_size - 1], 0);
}

TEST(MacosTouchscreenReport, LeavesOutIdsPastTheMaximumAndRepeats) {
  std::vector<contact_t> contacts;
  for (std::uint8_t i = 0; i < max_contacts + 3; ++i) {
    contacts.push_back({i, true, i, i});
  }
  contacts.push_back({2, true, 999, 999});
  const auto report = pack_input_report(contacts);
  EXPECT_EQ(report[input_report_size - 1], max_contacts);
  EXPECT_EQ(slot_of(report, 2)[2], 2);  // The first one for an id wins.
}

TEST(MacosTouchscreenReport, FeatureReportGivesTheContactMaximum) {
  const auto feature = pack_feature_report();
  EXPECT_EQ(feature[0], feature_report_id);
  EXPECT_EQ(feature[1], max_contacts);
}

TEST(MacosTouchscreenContacts, GivesTheLowestFreeIdAndReusesLiftedOnes) {
  contact_table_t table;
  EXPECT_EQ(table.down(client_a, 1000, {1, 1}), 0);
  EXPECT_EQ(table.down(client_a, 77, {2, 2}), 1);
  EXPECT_EQ(table.down(client_a, 5, {3, 3}), 2);
  EXPECT_TRUE(table.up(client_a, 77));

  // The lifted contact is reported once with its tip up, then its id is free.
  const auto report = table.take_report();
  ASSERT_EQ(report.size(), 3u);
  EXPECT_EQ(report[1], (contact_t {1, false, 2, 2}));
  EXPECT_EQ(table.take_report().size(), 2u);

  EXPECT_EQ(table.down(client_a, 999999, {4, 4}), 1);
}

TEST(MacosTouchscreenContacts, IdsStayBelowTheLimitOverManyTouches) {
  contact_table_t table;
  std::mt19937 random {7};
  std::set<std::uint32_t> down;
  for (int i = 0; i < 5000; ++i) {
    const auto pointer = static_cast<std::uint32_t>(random());
    if (down.size() < max_contacts && random() % 2) {
      const auto id = table.down(client_a, pointer, {0, 0});
      ASSERT_TRUE(id.has_value());
      EXPECT_LT(*id, max_contacts);
      down.insert(pointer);
    } else if (!down.empty()) {
      auto it = down.begin();
      std::advance(it, random() % down.size());
      EXPECT_TRUE(table.up(client_a, *it));
      down.erase(it);
    }
    for (const auto &contact : table.take_report()) {
      EXPECT_LT(contact.id, 30);
      EXPECT_LT(contact.id, max_contacts);
    }
  }
}

TEST(MacosTouchscreenContacts, RefusesAFingerPastTheMaximum) {
  contact_table_t table;
  for (std::uint32_t i = 0; i < max_contacts; ++i) {
    EXPECT_TRUE(table.down(client_a, i, {0, 0}).has_value());
  }
  EXPECT_FALSE(table.down(client_a, 100, {0, 0}).has_value());
  EXPECT_FALSE(table.move(client_a, 100, {0, 0}));
}

TEST(MacosTouchscreenContacts, DownOnAFingerAlreadyDownMovesIt) {
  contact_table_t table;
  EXPECT_EQ(table.down(client_a, 1, {1, 1}), 0);
  EXPECT_EQ(table.down(client_a, 1, {9, 9}), 0);
  EXPECT_EQ(table.touching(), 1u);
  EXPECT_EQ(table.take_report(), (std::vector<contact_t> {{0, true, 9, 9}}));
}

TEST(MacosTouchscreenContacts, ClientsShareTheDeviceWithoutMixingFingers) {
  contact_table_t table;
  EXPECT_EQ(table.down(client_a, 1, {1, 1}), 0);
  EXPECT_EQ(table.down(client_b, 1, {2, 2}), 1);
  EXPECT_TRUE(table.move(client_b, 1, {3, 3}));
  EXPECT_FALSE(table.up(client_a, 2));

  EXPECT_EQ(table.lift_owner(client_a), 1u);
  EXPECT_EQ(table.touching(client_a), 0u);
  EXPECT_EQ(table.touching(client_b), 1u);
  EXPECT_EQ(table.take_report(), (std::vector<contact_t> {{0, false, 1, 1}, {1, true, 3, 3}}));
}

TEST(MacosTouchscreenContacts, LiftAllLiftsEveryFingerOnce) {
  contact_table_t table;
  table.down(client_a, 1, {1, 1});
  table.down(client_b, 2, {2, 2});
  EXPECT_EQ(table.lift_all(), 2u);
  EXPECT_TRUE(table.lifting());
  EXPECT_EQ(table.take_report().size(), 2u);
  EXPECT_FALSE(table.lifting());
  EXPECT_TRUE(table.take_report().empty());
  EXPECT_EQ(table.lift_all(), 0u);
}

TEST(MacosTouchscreenCoordinates, MapsTheDisplaysCornersAndCenter) {
  const bounds_t display {0, 0, 1512, 982};
  EXPECT_EQ(to_logical(0, 0, display), (logical_point_t {0, 0}));
  EXPECT_EQ(to_logical(1512, 982, display), (logical_point_t {logical_max, logical_max}));
  EXPECT_EQ(to_logical(756, 491, display), (logical_point_t {16384, 16384}));
}

TEST(MacosTouchscreenCoordinates, MapsRelativeToADisplaysOrigin) {
  // A second display to the left of and above the main one, as a Remote Monitor display may be.
  const bounds_t display {-1920, -200, 1920, 1080};
  EXPECT_EQ(to_logical(-1920, -200, display), (logical_point_t {0, 0}));
  EXPECT_EQ(to_logical(-960, 340, display), (logical_point_t {16384, 16384}));
}

TEST(MacosTouchscreenCoordinates, ClampsPointsOffTheDisplay) {
  const bounds_t display {100, 100, 200, 200};
  EXPECT_EQ(to_logical(50, 1000, display), (logical_point_t {0, logical_max}));
}

TEST(MacosTouchscreenCoordinates, AnEmptyDisplayMapsToTheOrigin) {
  EXPECT_EQ(to_logical(5, 5, {0, 0, 0, 0}), (logical_point_t {0, 0}));
}

TEST(MacosTouchscreenGate, ParsesTheMajorVersion) {
  EXPECT_EQ(parse_os_major("27.2"), 27);
  EXPECT_EQ(parse_os_major("27"), 27);
  EXPECT_EQ(parse_os_major("15.7.1"), 15);
  EXPECT_EQ(parse_os_major(""), 0);
  EXPECT_EQ(parse_os_major("x27"), 0);
  EXPECT_EQ(parse_os_major("27x"), 0);
}

TEST(MacosTouchscreenGate, NeedsMacOS27TheSettingAndAReadyHelper) {
  EXPECT_TRUE(use_touchscreen({27, true, helper_state_e::ready}));
  EXPECT_TRUE(use_touchscreen({28, true, helper_state_e::ready}));
  EXPECT_FALSE(use_touchscreen({26, true, helper_state_e::ready}));
  EXPECT_FALSE(use_touchscreen({15, true, helper_state_e::ready}));
  EXPECT_FALSE(use_touchscreen({27, false, helper_state_e::ready}));
  for (const auto state : {helper_state_e::not_started, helper_state_e::starting, helper_state_e::unavailable, helper_state_e::failed}) {
    EXPECT_FALSE(use_touchscreen({27, true, state}));
  }
}

TEST(MacosTouchscreenGate, OnlyAFingerLandingMayStartTheHelper) {
  EXPECT_TRUE(event_may_create(LI_TOUCH_EVENT_DOWN));
  for (const auto type : {LI_TOUCH_EVENT_CANCEL_ALL, LI_TOUCH_EVENT_CANCEL, LI_TOUCH_EVENT_UP, LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOUCH_EVENT_BUTTON_ONLY}) {
    EXPECT_FALSE(event_may_create(type)) << type;
  }
}

TEST(MacosTouchscreenGate, RetriesAFailedHelperOnlyAfterAWhile) {
  const auto start = std::chrono::steady_clock::time_point {} + 1h;
  EXPECT_TRUE(should_try_helper({27, true, helper_state_e::not_started}, start, start));
  for (const auto state : {helper_state_e::unavailable, helper_state_e::failed}) {
    EXPECT_FALSE(should_try_helper({27, true, state}, start + 1s, start));
    EXPECT_TRUE(should_try_helper({27, true, state}, start + retry_after, start));
  }
  EXPECT_FALSE(should_try_helper({27, true, helper_state_e::ready}, start + 1h, start));
  EXPECT_FALSE(should_try_helper({27, true, helper_state_e::starting}, start + 1h, start));
  EXPECT_FALSE(should_try_helper({26, true, helper_state_e::not_started}, start, start));
  EXPECT_FALSE(should_try_helper({27, false, helper_state_e::not_started}, start, start));
}

namespace {
  wire::message_t valid_create() {
    auto message = wire::make(wire::type_e::create, 0);
    const auto uuid = "37D8832A-2D66-02CA-B9F7-8F30A301B230"sv;
    std::memcpy(message.uuid, uuid.data(), uuid.size());
    return message;
  }
}  // namespace

TEST(MacosTouchscreenWire, AcceptsWellFormedCommands) {
  EXPECT_TRUE(wire::valid_command(valid_create()));
  EXPECT_TRUE(wire::valid_command(wire::make(wire::type_e::destroy, wire::max_devices - 1)));
  const auto contacts = wire::make_contacts(3, {{0, true, 0, logical_max}, {9, false, 100, 200}});
  EXPECT_TRUE(wire::valid_command(contacts));
  EXPECT_EQ(wire::contacts_of(contacts), (std::vector<contact_t> {{0, true, 0, logical_max}, {9, false, 100, 200}}));
}

TEST(MacosTouchscreenWire, RejectsMalformedCommands) {
  auto message = valid_create();
  message.magic ^= 1;
  EXPECT_FALSE(wire::valid_command(message));

  message = valid_create();
  message.version = wire::version + 1;
  EXPECT_FALSE(wire::valid_command(message));

  message = valid_create();
  message.device = wire::max_devices;
  EXPECT_FALSE(wire::valid_command(message));

  message = valid_create();
  message.uuid[8] = 'x';
  EXPECT_FALSE(wire::valid_command(message));

  // Replies from the helper aren't commands.
  EXPECT_FALSE(wire::valid_command(wire::make(wire::type_e::hello, 0)));
  EXPECT_FALSE(wire::valid_command(wire::make(wire::type_e::created, 0)));

  auto contacts = wire::make_contacts(0, {{0, true, 1, 1}});
  contacts.count = max_contacts + 1;
  EXPECT_FALSE(wire::valid_command(contacts));

  contacts = wire::make_contacts(0, {{0, true, 1, 1}, {0, true, 2, 2}});
  EXPECT_FALSE(wire::valid_command(contacts)) << "duplicate contact id";

  contacts = wire::make_contacts(0, {{static_cast<std::uint8_t>(max_contacts), true, 1, 1}});
  EXPECT_FALSE(wire::valid_command(contacts)) << "contact id out of range";

  contacts = wire::make_contacts(0, {{0, true, 1, 1}});
  contacts.contacts[0].x = logical_max + 1;
  EXPECT_FALSE(wire::valid_command(contacts)) << "x out of range";

  contacts = wire::make_contacts(0, {{0, true, 1, 1}});
  contacts.contacts[0].tip = 2;
  EXPECT_FALSE(wire::valid_command(contacts));
}

TEST(MacosTouchscreenWire, ChecksUuidShape) {
  EXPECT_TRUE(wire::valid_uuid("37d8832a-2d66-02ca-b9f7-8f30a301b230"));
  EXPECT_FALSE(wire::valid_uuid("37D8832A2D6602CAB9F78F30A301B230"));
  EXPECT_FALSE(wire::valid_uuid("37D8832A-2D66-02CA-B9F7-8F30A301B23"));
  EXPECT_FALSE(wire::valid_uuid("37D8832A-2D66-02CA-B9F7-8F30A301B23G"));
}

TEST(MacosTouchscreenWire, ANewerMoveReplacesAnUnsentMoveOfTheSameFingers) {
  const auto queued = wire::make_contacts(0, {{0, true, 10, 10}, {1, true, 50, 50}});
  const auto newer = wire::make_contacts(0, {{0, true, 12, 11}, {1, true, 48, 52}});
  EXPECT_TRUE(wire::supersedes(newer, queued));
}

TEST(MacosTouchscreenWire, AFingerLandingOrLiftingIsNeverReplaced) {
  const auto down = wire::make_contacts(0, {{0, true, 10, 10}});
  const auto up = wire::make_contacts(0, {{0, false, 10, 10}});
  const auto second_finger = wire::make_contacts(0, {{0, true, 10, 10}, {1, true, 50, 50}});
  const auto nothing = wire::make_contacts(0, {});

  // A tap: the lift must not replace the unsent landing, or macOS never sees the finger.
  EXPECT_FALSE(wire::supersedes(up, down));
  EXPECT_FALSE(wire::supersedes(nothing, up));
  EXPECT_FALSE(wire::supersedes(down, up));
  EXPECT_FALSE(wire::supersedes(second_finger, down));
  EXPECT_FALSE(wire::supersedes(down, second_finger));

  // The same slot reused by a new finger after a lift shows up as a different set in between.
  const auto other_finger = wire::make_contacts(0, {{1, true, 10, 10}});
  EXPECT_FALSE(wire::supersedes(other_finger, down));
}

TEST(MacosTouchscreenWire, OnlyContactsForTheSameDeviceAreReplaced) {
  const auto queued = wire::make_contacts(0, {{0, true, 10, 10}});
  EXPECT_FALSE(wire::supersedes(wire::make_contacts(1, {{0, true, 12, 11}}), queued));
  EXPECT_FALSE(wire::supersedes(wire::make(wire::type_e::destroy, 0), queued));
  EXPECT_FALSE(wire::supersedes(queued, wire::make(wire::type_e::create, 0)));
}
