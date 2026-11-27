#include "../tests_common.h"
#include "src/tablet_posture_policy.h"

namespace {
  using namespace tablet_posture;

  constexpr auto desktop = client_mode_e::desktop;
  constexpr auto tablet = client_mode_e::tablet;
  constexpr auto handheld = client_mode_e::handheld;
}  // namespace

TEST(TabletPosturePolicy, StartsAsLaptopWithoutClients) {
  registry_t registry;
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_EQ(registry.size(), 0u);
}

TEST(TabletPosturePolicy, TouchClientMakesTheHostASlate) {
  registry_t registry;
  EXPECT_TRUE(registry.start("phone", tablet));
  EXPECT_EQ(registry.state(), state_e::slate);
}

TEST(TabletPosturePolicy, LaptopClientKeepsDesktop) {
  registry_t registry;
  EXPECT_FALSE(registry.start("convertible", desktop));
  EXPECT_EQ(registry.state(), state_e::laptop);
}

TEST(TabletPosturePolicy, KeyboardFlipFollowsRuntimeUpdates) {
  registry_t registry;
  registry.start("convertible", desktop);
  EXPECT_TRUE(registry.update("convertible", tablet));  // keyboard folded back
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_FALSE(registry.update("convertible", tablet));  // same class again: no change
  EXPECT_TRUE(registry.update("convertible", desktop));  // keyboard back
  EXPECT_EQ(registry.state(), state_e::laptop);
}

TEST(TabletPosturePolicy, UpdateOfUnknownClientIsIgnored) {
  registry_t registry;
  EXPECT_FALSE(registry.update("ghost", tablet));
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_FALSE(registry.contains("ghost"));
}

TEST(TabletPosturePolicy, SlateWhileAnyClientIsTouchPrimary) {
  registry_t registry;
  registry.start("phone", tablet);
  EXPECT_FALSE(registry.start("convertible", desktop));
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_FALSE(registry.stop("convertible"));
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_TRUE(registry.stop("phone"));
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_EQ(registry.size(), 0u);
}

TEST(TabletPosturePolicy, ClientWithSeveralSessionsStaysUntilTheLastEnds) {
  registry_t registry;
  registry.start("phone", tablet);
  registry.start("phone", tablet);
  EXPECT_FALSE(registry.stop("phone"));
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_TRUE(registry.stop("phone"));
  EXPECT_EQ(registry.state(), state_e::laptop);
}

TEST(TabletPosturePolicy, StoppingAnUnknownClientIsIgnored) {
  registry_t registry;
  EXPECT_FALSE(registry.stop("ghost"));
  EXPECT_EQ(registry.state(), state_e::laptop);
}

TEST(TabletPosturePolicy, NewSessionTakesTheLatestPosture) {
  registry_t registry;
  registry.start("convertible", tablet);
  EXPECT_TRUE(registry.start("convertible", desktop));  // resumed with the keyboard attached
  EXPECT_EQ(registry.state(), state_e::laptop);
}

TEST(TabletPosturePolicy, OverrideBeatsTheClients) {
  registry_t registry;
  registry.start("convertible", desktop);
  registry.set_override(tablet);  // simulated keyboard detach
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_EQ(registry.client_state(), state_e::laptop);
  registry.set_override(desktop);
  registry.update("convertible", tablet);
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_EQ(registry.client_state(), state_e::slate);
  registry.set_override(std::nullopt);  // back to client control
  EXPECT_EQ(registry.state(), state_e::slate);
}

TEST(TabletPosturePolicy, OverrideWorksWithoutAnyClient) {
  registry_t registry;
  registry.set_override(tablet);
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_EQ(registry.override_mode(), std::optional<client_mode_e> {tablet});
}

TEST(TabletPosturePolicy, ClientsReportEveryActiveSession) {
  registry_t registry;
  registry.start("phone", tablet);
  registry.start("phone", tablet);
  registry.start("convertible", desktop);
  const auto clients = registry.clients();
  ASSERT_EQ(clients.size(), 2u);
  // Ordered by client id.
  EXPECT_EQ(clients[0].id, "convertible");
  EXPECT_EQ(clients[0].mode, desktop);
  EXPECT_EQ(clients[1].id, "phone");
  EXPECT_EQ(clients[1].sessions, 2);
  EXPECT_EQ(clients[1].mode, tablet);
}

TEST(TabletPosturePolicy, HandheldClientMakesASlateAndWantsGaming) {
  registry_t registry;
  EXPECT_FALSE(registry.gaming());
  EXPECT_TRUE(registry.start("handheld", handheld));
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_EQ(registry.mode(), handheld);
  EXPECT_TRUE(registry.gaming());
}

TEST(TabletPosturePolicy, TabletClientDoesNotWantGaming) {
  registry_t registry;
  registry.start("phone", tablet);
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_FALSE(registry.gaming());
}

TEST(TabletPosturePolicy, StrongestClientModeWins) {
  registry_t registry;
  registry.start("phone", tablet);
  registry.start("handheld", handheld);
  EXPECT_EQ(registry.mode(), handheld);
  EXPECT_TRUE(registry.gaming());
}

TEST(TabletPosturePolicy, HandheldLeavingKeepsSlateWithoutGaming) {
  registry_t registry;
  registry.start("phone", tablet);
  registry.start("handheld", handheld);
  EXPECT_TRUE(registry.stop("handheld"));
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_FALSE(registry.gaming());
  EXPECT_EQ(registry.mode(), tablet);
}

TEST(TabletPosturePolicy, OverrideHandheldWorksWithoutAnyClient) {
  registry_t registry;
  registry.set_override(handheld);
  EXPECT_EQ(registry.state(), state_e::slate);
  EXPECT_TRUE(registry.gaming());
  EXPECT_EQ(registry.client_mode(), desktop);
  registry.set_override(std::nullopt);
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_FALSE(registry.gaming());
}

TEST(TabletPosturePolicy, OverrideDesktopBeatsAStreamingHandheld) {
  registry_t registry;
  registry.start("handheld", handheld);
  registry.set_override(desktop);
  EXPECT_EQ(registry.state(), state_e::laptop);
  EXPECT_FALSE(registry.gaming());
  EXPECT_EQ(registry.client_mode(), handheld);
  registry.set_override(std::nullopt);
  EXPECT_TRUE(registry.gaming());
}
