/**
 * @file tests/unit/test_app_order_policy.cpp
 */

#include "../tests_common.h"

#include <src/app_order_policy.h>

using namespace proc::app_order;

namespace {
  std::vector<std::string> names_in_order(const std::vector<entry_t> &entries, const settings_t &settings) {
    std::vector<std::string> result;
    for (const auto index : order(entries, settings)) {
      result.push_back(entries[index].name);
    }
    return result;
  }

  // apps.json order as Steam auto-sync leaves it: Desktop first, games appended.
  const std::vector<entry_t> catalog {
    {group_e::steam, "hades II", 300, 610},
    {group_e::custom, "Steam Big Picture"},
    {group_e::steam, "Elden Ring", 100, 9200},
    {group_e::playnite, "Halo", 50, 20},
    {group_e::custom, "Desktop"},
    {group_e::steam, "Balatro", 200, 140},
    {group_e::lutris, "Celeste", 10, 5},
  };
}  // namespace

TEST(AppOrderPolicy, EmptyGroupsKeepFileOrder) {
  const auto result = names_in_order(catalog, parse("", "recent", "", ""));
  EXPECT_EQ(result, (std::vector<std::string> {"hades II", "Steam Big Picture", "Elden Ring", "Halo", "Desktop", "Balatro", "Celeste"}));
}

TEST(AppOrderPolicy, CustomKeepsFileOrderAndProvidersSortByName) {
  const auto result = names_in_order(catalog, parse("custom,steam", "", "", ""));
  EXPECT_EQ(result, (std::vector<std::string> {"Steam Big Picture", "Desktop", "Balatro", "Elden Ring", "hades II", "Halo", "Celeste"}));
}

TEST(AppOrderPolicy, ProviderSortModes) {
  EXPECT_EQ(names_in_order(catalog, parse("steam", "recent", "", "")).front(), "hades II");
  EXPECT_EQ(names_in_order(catalog, parse("steam", "playtime", "", "")).front(), "Elden Ring");
}

TEST(AppOrderPolicy, GroupOrderIsConfigurable) {
  const auto result = names_in_order(catalog, parse(" Lutris , playnite,custom,steam,bogus,custom", "", "", ""));
  EXPECT_EQ(result.front(), "Celeste");
  EXPECT_EQ(result[1], "Halo");
  EXPECT_EQ(result[2], "Steam Big Picture");
  EXPECT_EQ(result.back(), "hades II");
}

TEST(AppOrderPolicy, NewlySyncedGameSlotsIntoItsGroup) {
  auto entries = catalog;
  entries.push_back({group_e::steam, "Cyberpunk 2077", 400, 4100});
  const auto by_name = names_in_order(entries, parse("custom,steam", "name", "", ""));
  EXPECT_EQ(by_name[3], "Cyberpunk 2077");
  const auto by_recent = names_in_order(entries, parse("custom,steam", "recent", "", ""));
  EXPECT_EQ(by_recent[2], "Cyberpunk 2077");
}

TEST(AppOrderPolicy, UnplayedGamesSortLast) {
  auto entries = catalog;
  entries.push_back({group_e::steam, "Aardvark", 0, 0});
  const auto result = names_in_order(entries, parse("custom,steam", "recent", "", ""));
  EXPECT_EQ(result[5], "Aardvark");
}

TEST(AppOrderPolicy, GroupOfMatchesWebUiPrecedence) {
  EXPECT_EQ(group_of("", "", ""), group_e::custom);
  EXPECT_EQ(group_of("", "570", ""), group_e::steam);
  EXPECT_EQ(group_of("abc", "570", ""), group_e::playnite);
  EXPECT_EQ(group_of("", "", "12"), group_e::lutris);
}
