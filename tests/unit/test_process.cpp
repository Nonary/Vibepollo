/**
 * @file tests/unit/test_process.cpp
 * @brief Deterministic application catalog, artwork, and lifecycle policy tests.
 */
#include "../tests_common.h"

#include <src/app_catalog_policy.h>
#include <src/deferred_action.h>

#include <algorithm>
#include <map>

namespace {
  using proc::catalog::alias_state_t;
  using proc::catalog::app_identity_t;

  proc::catalog::byte_buffer_t png(std::uint8_t payload = 0) {
    return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, payload};
  }

  struct fake_images_t {
    std::map<std::string, proc::catalog::byte_buffer_t> files;

    std::optional<proc::catalog::byte_buffer_t> read(const std::string &path) const {
      const auto found = files.find(path);
      return found == files.end() ? std::nullopt : std::optional {found->second};
    }
  };

  TEST(ProcessLifecycle, DeferredActionPersistsUntilConsumedOrCleared) {
    lifecycle::deferred_action_t action;
    action.defer();
    EXPECT_TRUE(action.consume());
    EXPECT_FALSE(action.consume());

    action.defer();
    action.clear();
    EXPECT_FALSE(action.consume());
  }

  TEST(ProcessArtwork, PngSignatureRequiresAllEightBytes) {
    EXPECT_TRUE(proc::catalog::has_png_signature(png()));
    EXPECT_FALSE(proc::catalog::has_png_signature({}));
    EXPECT_FALSE(proc::catalog::has_png_signature(proc::catalog::byte_buffer_t {0x89, 0x50, 0x4e, 0x47}));
    EXPECT_FALSE(proc::catalog::has_png_signature(proc::catalog::byte_buffer_t(8, 0)));
  }

  TEST(ProcessArtwork, ValidationUsesInjectedAssetsAndNeverTouchesDisk) {
    const std::string assets = "virtual-assets";
    const std::string fallback = "virtual-assets/box.png";
    const auto asset_cover = proc::catalog::asset_path(assets, "cover.png");
    fake_images_t images {{{asset_cover, png(1)}, {"custom.PNG", png(2)}, {"bad.png", {1, 2, 3}}}};
    const auto read = [&](const std::string &path) { return images.read(path); };

    EXPECT_EQ(proc::catalog::validate_image_path("", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("cover.jpg", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("missing.png", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("bad.png", assets, fallback, read), fallback);
    EXPECT_EQ(proc::catalog::validate_image_path("cover.png", assets, fallback, read), asset_cover);
    EXPECT_EQ(proc::catalog::validate_image_path("custom.PNG", assets, fallback, read), "custom.PNG");
    EXPECT_EQ(
      proc::catalog::validate_image_path("./assets/steam.png", assets, fallback, read),
      proc::catalog::asset_path(assets, "steam.png"));
  }

  TEST(ProcessCatalog, FirstSeenUuidSeedsStableUuidOnlyId) {
    app_identity_t app {"Game", "11111111-1111-1111-1111-111111111111", "ignored-cover", "sha256:first"};
    std::set<std::string> ids;
    std::set<std::string> active;
    std::map<std::string, alias_state_t> state;
    bool changed = false;

    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_EQ(app.id, std::get<0>(proc::catalog::calculate_ids(app.name, app.uuid, {}, 0)));
    EXPECT_EQ(app.art_version, "sha256:first");
    EXPECT_TRUE(app.aliases.empty());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, CoverChangeRotatesCurrentIdAndRetainsOldAlias) {
    const std::string uuid = "22222222-2222-2222-2222-222222222222";
    std::map<std::string, alias_state_t> state;
    std::set<std::string> ids;
    std::set<std::string> active;
    bool changed = false;
    app_identity_t first {"Game", uuid, {}, "sha256:first"};
    proc::catalog::assign_compatible_id(first, 0, ids, state, active, changed);

    ids.clear();
    active.clear();
    changed = false;
    app_identity_t second {"Game", uuid, {}, "sha256:second"};
    proc::catalog::assign_compatible_id(second, 0, ids, state, active, changed);
    EXPECT_NE(second.id, first.id);
    EXPECT_NE(std::find(second.aliases.begin(), second.aliases.end(), first.id), second.aliases.end());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, DeletedAppsAndConflictingAliasesArePruned) {
    std::map<std::string, alias_state_t> state {
      {"active-a", {"101", "a", {"202", "999"}}},
      {"active-b", {"202", "b", {"999"}}},
      {"deleted", {"303", "c", {"404"}}}
    };
    std::vector<app_identity_t> apps {
      {"A", "active-a", {}, "a", "101", {"202", "999"}},
      {"B", "active-b", {}, "b", "202", {"999"}}
    };
    bool changed = false;
    proc::catalog::prune_and_filter_aliases(apps, state, {"active-a", "active-b"}, changed);

    EXPECT_FALSE(state.contains("deleted"));
    EXPECT_TRUE(apps[0].aliases.empty());
    EXPECT_TRUE(apps[1].aliases.empty());
    EXPECT_TRUE(changed);
  }

  TEST(ProcessCatalog, ResolverPrefersUuidAndRejectsAmbiguousAliases) {
    const std::vector<app_identity_t> apps {
      {"A", "uuid-a", {}, "a", "101", {"999"}},
      {"B", "uuid-b", {}, "b", "202", {"999"}}
    };
    EXPECT_EQ(proc::catalog::resolve_app(apps, "101"), 0u);
    EXPECT_EQ(proc::catalog::resolve_app(apps, "bad", "uuid-b"), 1u);
    EXPECT_FALSE(proc::catalog::resolve_app(apps, "999").has_value());
    EXPECT_FALSE(proc::catalog::resolve_app(apps, "0").has_value());
  }

  TEST(ProcessCatalog, LegacyAppsIncludeImageIdentityAndIndexFallback) {
    const auto first = proc::catalog::calculate_ids("Legacy", "", "cover-a", 0);
    const auto second = proc::catalog::calculate_ids("Legacy", "", "cover-b", 0);
    const auto indexed = proc::catalog::calculate_ids("Legacy", "", "cover-a", 1);
    EXPECT_NE(std::get<0>(first), std::get<0>(second));
    EXPECT_NE(std::get<1>(first), std::get<1>(indexed));
  }
}  // namespace
