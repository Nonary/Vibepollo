#include <gtest/gtest.h>

#include "src/platform/windows/display_clone_group_policy.h"

namespace {
  using display_helper::clone_group::active_display_t;
  using display_helper::clone_group::detach_origin;
  using display_helper::clone_group::topology_t;

  constexpr auto asus = "{17941ebb-e80f-544d-8915-c102c6992419}";
  constexpr auto samsung = "{d7c1541c-6e5b-5a62-8e11-fa43d5dd98a2}";
  constexpr auto virtual_display = "{7eec7179-a091-57d8-8ffb-32fb1945d5e2}";
  constexpr std::uint64_t gpu = 68107;
  constexpr std::uint64_t virtual_adapter = 5531467;

  active_display_t display(const std::string &id, const int x, const int y, const std::optional<std::uint64_t> adapter = gpu, const unsigned width = 1920, const unsigned height = 1080) {
    return {id, {x, y}, {width, height}, adapter};
  }

  std::vector<active_display_t> duplicate_pair_with_virtual_display() {
    return {display(asus, 0, 0), display(samsung, 0, 0), display(virtual_display, 0, 0, virtual_adapter)};
  }
}  // namespace

// Vibepollo#702: the virtual display joined a Duplicate pair of physical monitors.
TEST(DisplayCloneGroupPolicy, MovesVirtualDisplayOutOfDuplicatePair) {
  const topology_t topology {{asus, samsung, virtual_display}};

  const auto origin = detach_origin(topology, virtual_display, duplicate_pair_with_virtual_display());

  ASSERT_TRUE(origin);
  EXPECT_EQ(origin->m_x, 1920);
  EXPECT_EQ(origin->m_y, 0);
}

TEST(DisplayCloneGroupPolicy, MatchesDeviceIdsWithoutCase) {
  const topology_t topology {{asus, samsung, "{7EEC7179-A091-57D8-8FFB-32FB1945D5E2}"}};

  EXPECT_TRUE(detach_origin(topology, virtual_display, duplicate_pair_with_virtual_display()));
}

TEST(DisplayCloneGroupPolicy, PlacesDisplayAfterRightmostDisplayAndAlignsItsTop) {
  const topology_t topology {{asus, samsung, virtual_display}, {"side"}};
  const std::vector displays {
    display(asus, 0, 0, gpu, 2560, 1440),
    display(samsung, 0, 0, gpu, 2560, 1440),
    display(virtual_display, 0, 0, virtual_adapter),
    display("side", 2560, -300, gpu, 1080, 1920),
  };

  const auto origin = detach_origin(topology, virtual_display, displays);

  ASSERT_TRUE(origin);
  EXPECT_EQ(origin->m_x, 3640);
  EXPECT_EQ(origin->m_y, -300);
}

TEST(DisplayCloneGroupPolicy, LeavesValidDuplicatePairAlone) {
  const topology_t topology {{asus, virtual_display}};
  const std::vector displays {display(asus, 0, 0), display(virtual_display, 0, 0, virtual_adapter)};

  EXPECT_FALSE(detach_origin(topology, virtual_display, displays));
}

TEST(DisplayCloneGroupPolicy, LeavesExtendedLayoutAlone) {
  const topology_t topology {{asus, samsung}, {virtual_display}};
  const std::vector displays {display(asus, 0, 0), display(samsung, 0, 0), display(virtual_display, 1920, 0, virtual_adapter)};

  EXPECT_FALSE(detach_origin(topology, virtual_display, displays));
}

TEST(DisplayCloneGroupPolicy, NeverMovesDisplayThatSharesItsAdapterWithTheGroup) {
  // Three physical monitors on one GPU: the user's own layout, not a stray arrival.
  const topology_t topology {{asus, samsung, "third"}};
  const std::vector displays {display(asus, 0, 0), display(samsung, 0, 0), display("third", 0, 0)};

  EXPECT_FALSE(detach_origin(topology, "third", displays));
}

TEST(DisplayCloneGroupPolicy, DoesNotMoveWhenAnyAdapterIsUnknown) {
  const topology_t topology {{asus, samsung, virtual_display}};

  auto unknown_device = duplicate_pair_with_virtual_display();
  unknown_device[2].adapter.reset();
  EXPECT_FALSE(detach_origin(topology, virtual_display, unknown_device));

  auto unknown_member = duplicate_pair_with_virtual_display();
  unknown_member[0].adapter.reset();
  EXPECT_FALSE(detach_origin(topology, virtual_display, unknown_member));

  const std::vector missing_member {display(samsung, 0, 0), display(virtual_display, 0, 0, virtual_adapter)};
  EXPECT_FALSE(detach_origin(topology, virtual_display, missing_member));
}

TEST(DisplayCloneGroupPolicy, DoesNotMoveWhenRemainingGroupStaysTooLarge) {
  const topology_t topology {{asus, samsung, "third", virtual_display}};
  const std::vector displays {
    display(asus, 0, 0),
    display(samsung, 0, 0),
    display("third", 0, 0),
    display(virtual_display, 0, 0, virtual_adapter),
  };

  EXPECT_FALSE(detach_origin(topology, virtual_display, displays));
}

TEST(DisplayCloneGroupPolicy, DoesNotMoveWhenAnotherGroupIsTooLarge) {
  const topology_t topology {{asus, samsung, "third"}, {virtual_display}};
  const std::vector displays {
    display(asus, 0, 0),
    display(samsung, 0, 0),
    display("third", 0, 0),
    display(virtual_display, 1920, 0, virtual_adapter),
  };

  EXPECT_FALSE(detach_origin(topology, virtual_display, displays));
}

TEST(DisplayCloneGroupPolicy, IgnoresUnspecifiedDevice) {
  const topology_t topology {{asus, samsung, virtual_display}};

  EXPECT_FALSE(detach_origin(topology, "", duplicate_pair_with_virtual_display()));
}

TEST(DisplayCloneGroupPolicy, NeedsAnotherActiveDisplayToPlaceAgainst) {
  const topology_t topology {{asus, samsung, virtual_display}};
  const std::vector displays {display(virtual_display, 0, 0, virtual_adapter)};

  EXPECT_FALSE(detach_origin(topology, virtual_display, displays));
}
