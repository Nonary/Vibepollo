/**
 * @file tests/unit/test_process.cpp
 * @brief Deterministic application catalog, artwork, and lifecycle policy tests.
 */
#include "../tests_common.h"

#include <src/app_display_policy.h>
#include <src/app_catalog_policy.h>
#include <src/deferred_action.h>
#include <src/remote_session.h>

#include <algorithm>
#include <filesystem>
#include <map>

namespace {
  using proc::catalog::alias_state_t;
  using proc::catalog::app_identity_t;
  using proc::display_policy::app_override_e;

  TEST(ProcessDisplayPolicy, GlobalVirtualModeIsInheritedWithoutAppOverride) {
    EXPECT_TRUE(proc::display_policy::resolve_virtual_display_request(true, app_override_e::inherit));
  }

  TEST(ProcessDisplayPolicy, ExplicitPhysicalAppOverrideWinsOverGlobalVirtualMode) {
    EXPECT_FALSE(proc::display_policy::resolve_virtual_display_request(true, app_override_e::physical));
  }

  TEST(ProcessDisplayPolicy, ExplicitVirtualAppOverrideWinsOverGlobalPhysicalMode) {
    EXPECT_TRUE(proc::display_policy::resolve_virtual_display_request(false, app_override_e::virtual_display));
  }

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

  TEST(ProcessArtwork, MachinePathsStayInsideImmutableAssetsOrSharedCovers) {
    const auto fixture_root = std::filesystem::temp_directory_path() / "vibepollo-artwork-policy";
    const std::string assets = (fixture_root / "assets").string();
    const std::string covers = (fixture_root / "covers").string();

    EXPECT_TRUE(proc::catalog::machine_image_path_is_confined("desktop.png", assets, covers));
    EXPECT_TRUE(proc::catalog::machine_image_path_is_confined("./assets/steam.png", assets, covers));
    EXPECT_TRUE(proc::catalog::machine_image_path_is_confined(
      (std::filesystem::path(assets) / "remote-session/input.png").string(), assets, covers));
    EXPECT_TRUE(proc::catalog::machine_image_path_is_confined(
      (std::filesystem::path(covers) / "game.png").string(), assets, covers));
    EXPECT_FALSE(proc::catalog::machine_image_path_is_confined(
      covers, assets, covers));
    EXPECT_FALSE(proc::catalog::machine_image_path_is_confined(
      (std::filesystem::path(covers) / "../../secrets.png").string(), assets, covers));
    EXPECT_FALSE(proc::catalog::machine_image_path_is_confined(
      "../../home/chasep/.config/vibeshine/secret.png", assets, covers));
    EXPECT_FALSE(proc::catalog::machine_image_path_is_confined(
      "/home/chasep/custom.png", assets, covers));
  }

  TEST(ProcessArtwork, ConfiguredPreviewUsesTheClientPngPathAndBytes) {
    const std::string assets = "virtual-assets";
    const auto cover = proc::catalog::asset_path(assets, "cover.png");
    const auto box = proc::catalog::asset_path(assets, "box.png");
    const auto steam = proc::catalog::asset_path(assets, "steam.png");
    fake_images_t images {{{cover, png(1)}, {"custom.PNG", png(2)}, {box, png(3)}, {steam, png(4)}}};
    const auto read = [&](const std::string &path) {
      return images.read(path);
    };
    const auto content = [&](const std::string &path) -> std::optional<std::string> {
      const auto bytes = images.read(path);
      if (!bytes) {
        return std::nullopt;
      }
      return std::string(bytes->begin(), bytes->end());
    };

    for (const auto &path : {"cover.png", "custom.PNG", "box.png", "./assets/steam.png"}) {
      const auto client_path = proc::catalog::validate_image_path(path, assets, "fallback", read);
      EXPECT_EQ(proc::catalog::read_configured_image(path, assets, read, content), content(client_path));
    }
  }

  TEST(ProcessArtwork, ConfiguredPreviewRejectsBrowserOnlyArtworkWithoutSubstitution) {
    const std::string assets = "virtual-assets";
    const auto box = proc::catalog::asset_path(assets, "box.png");
    fake_images_t images {{{box, png()}, {"cover.jpg", {0xff, 0xd8, 0xff, 0xe0}}, {"renamed.png", {0xff, 0xd8, 0xff, 0xe0}}, {"truncated.png", {0x89, 0x50, 0x4e, 0x47}}, {"covers/game.png", png()}, {"config/relative.png", png()}}};
    const auto read = [&](const std::string &path) {
      return images.read(path);
    };
    int complete_reads = 0;
    const auto content = [&](const std::string &) -> std::optional<std::string> {
      ++complete_reads;
      return "unexpected read";
    };

    for (const auto &path : {"", "missing.png", "cover.jpg", "renamed.png", "truncated.png", "relative.png"}) {
      EXPECT_EQ(proc::catalog::validate_image_path(path, assets, box, read), box);
      EXPECT_FALSE(proc::catalog::read_configured_image(path, assets, read, content));
    }
    EXPECT_EQ(complete_reads, 0);
  }

  TEST(ProcessArtwork, ConfiguredPreviewRechecksCompleteBytesAndTheSizeLimit) {
    const auto header = png();
    const auto read_header = [&](const std::string &) {
      return std::optional {header};
    };
    std::optional<std::string> bytes;
    const auto content = [&](const std::string &) {
      return bytes;
    };
    const auto preview = [&] {
      return proc::catalog::read_configured_image("cover.png", "virtual-assets", read_header, content);
    };

    EXPECT_FALSE(preview());  // Disappeared after header validation.
    bytes = std::string(8, 'x');
    EXPECT_FALSE(preview());  // Replaced after header validation.
    bytes = std::string(header.begin(), header.begin() + 4);
    EXPECT_FALSE(preview());
    bytes = std::string(proc::catalog::maximum_image_bytes, 'x');
    std::copy(header.begin(), header.end(), bytes->begin());
    EXPECT_EQ(preview(), bytes);
    bytes->push_back('x');
    EXPECT_FALSE(preview());
  }

  TEST(ProcessArtwork, ConfiguredPreviewKeepsRuntimeReaderRestrictions) {
    const auto root = std::filesystem::temp_directory_path() / "vibeshine-preview-policy";
    const std::string assets = (root / "assets").string();
    const std::string covers = (root / "covers").string();
    const auto header = png();
    const auto confined = [&](const std::string &path) {
      return proc::catalog::machine_image_path_is_confined(path, assets, covers);
    };
    const auto read_header = [&](const std::string &path) -> std::optional<proc::catalog::byte_buffer_t> {
      return confined(path) ? std::optional {header} : std::nullopt;
    };
    bool allow_complete_read = true;
    const auto content = [&](const std::string &path) -> std::optional<std::string> {
      if (!confined(path) || !allow_complete_read) {
        return std::nullopt;
      }
      return std::string(header.begin(), header.end());
    };

    EXPECT_TRUE(proc::catalog::read_configured_image("remote-session/remote-input.png", assets, read_header, content));
    EXPECT_TRUE(proc::catalog::read_configured_image(covers + "/game.png", assets, read_header, content));
    EXPECT_FALSE(proc::catalog::read_configured_image("../../outside.png", assets, read_header, content));
    EXPECT_FALSE(proc::catalog::read_configured_image((root / "custom.png").string(), assets, read_header, content));
    allow_complete_read = false;
    EXPECT_FALSE(proc::catalog::read_configured_image("cover.png", assets, read_header, content));
  }

  TEST(ProcessArtwork, PreviewVersionUsesValidatedParsedPathAndCurrentStamp) {
    const std::string assets = "virtual-assets";
    const auto path = proc::catalog::asset_path(assets, "cover.png");
    fake_images_t images {{{path, png()}}};
    const auto read = [&](const std::string &p) { return images.read(p); };
    std::string stamp = "100-9";
    int stamps = 0;
    const auto stat = [&](const std::string &p) -> std::optional<std::string> {
      EXPECT_EQ(p, path);
      ++stamps;
      return stamp;
    };
    const auto first = proc::catalog::configured_image_version("cover.png", assets, "art-a", read, stat);
    EXPECT_EQ(stamps, 1);
    stamp = "200-9";
    EXPECT_NE(first, proc::catalog::configured_image_version("cover.png", assets, "art-a", read, stat));
    EXPECT_NE(first, proc::catalog::configured_image_version("cover.png", assets, "art-b", read, stat));
    const auto raw = proc::catalog::configured_image_version("$(COVER)", assets, "art-a", read, stat);
    EXPECT_NE(first, raw);
    EXPECT_EQ(stamps, 3); // Unresolved saved JSON does not drive a filesystem stamp.
  }

  TEST(ProcessArtwork, PreviewVersionDoesNotStatUnvalidatedPaths) {
    const auto read = [](const std::string &) -> std::optional<proc::catalog::byte_buffer_t> { return std::nullopt; };
    const auto stat = [](const std::string &) -> std::optional<std::string> {
      ADD_FAILURE() << "Rejected paths must not reach the metadata reader";
      return "unexpected";
    };
    const auto version = proc::catalog::configured_image_version("outside.png", "assets", "art-a", read, stat);
    EXPECT_TRUE(version.ends_with("-unavailable"));
  }

  TEST(ProcessCatalog, FirstSeenUuidVersionsArtworkAndRetainsLegacyLaunchId) {
    app_identity_t app {"Game", "11111111-1111-1111-1111-111111111111", "ignored-cover", "sha256:first"};
    std::set<std::string> ids;
    std::set<std::string> active;
    std::map<std::string, alias_state_t> state;
    bool changed = false;

    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_EQ(app.id, std::get<0>(proc::catalog::calculate_versioned_ids(app.uuid, app.art_version, 0)));
    EXPECT_EQ(app.art_version, "sha256:first");
    EXPECT_EQ(app.aliases, std::vector<std::string> {std::get<0>(proc::catalog::calculate_ids(app.name, app.uuid, {}, 0))});
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

  TEST(ProcessCatalog, ExistingUuidOnlyCoverMigratesOnce) {
    app_identity_t app {"Steam game", "steam-uuid", {}, "sha256:cover"};
    const auto legacy = std::get<0>(proc::catalog::calculate_ids(app.name, app.uuid, {}, 0));
    std::map<std::string, alias_state_t> state {{app.uuid, {legacy, app.art_version, {}}}};
    std::set<std::string> ids, active;
    bool changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_NE(app.id, legacy);
    EXPECT_EQ(proc::catalog::resolve_app({app}, legacy), 0u);
    EXPECT_TRUE(changed);
    const auto migrated = app.id;
    ids.clear();
    changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_EQ(app.id, migrated);
    EXPECT_FALSE(changed);
  }

  TEST(ProcessCatalog, ReaddedGameDoesNotReusePlaceholderCacheId) {
    std::map<std::string, alias_state_t> state;
    std::set<std::string> ids, active;
    bool changed = false;
    app_identity_t app {"Steam game", "steam-uuid", {}, "sha256:steam-placeholder"};
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    const auto placeholder_id = app.id;
    std::vector<app_identity_t> empty;
    proc::catalog::prune_and_filter_aliases(empty, state, {}, changed);
    ASSERT_TRUE(state.empty());
    ids.clear();
    app.art_version = "sha256:real-cover";
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_NE(app.id, placeholder_id);
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
  TEST(ProcessCatalog, FreshDefaultArtCollisionCannotUseSyntheticMonitorId) {
    app_identity_t app {"Work Monitor", "00211013-3213-4121-8110-000000000000", {}, "default"};
    EXPECT_EQ(std::get<0>(proc::catalog::calculate_versioned_ids(app.uuid, app.art_version, 0)), std::to_string(remote_session::monitor_id));
    std::set<std::string> ids, active;
    std::map<std::string, alias_state_t> state;
    bool changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_NE(app.id, std::to_string(remote_session::monitor_id));
    EXPECT_EQ(remote_session::identify(std::stoi(app.id), app.uuid), remote_session::control_e::none);
    const auto assigned = app.id;
    ids.clear();
    changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_EQ(app.id, assigned);
    EXPECT_FALSE(changed);
  }

  TEST(ProcessCatalog, EverySyntheticIdIsRemappedAndRemovedFromAliases) {
    const std::vector<int> reserved {
      remote_session::resume_id,
      remote_session::disconnect_monitor_id,
      remote_session::disconnect_input_id,
      remote_session::terminate_id,
      remote_session::monitor_id,
      remote_session::input_id,
      remote_session::running_game_id,
      remote_session::secondary_resume_id,
      remote_session::secondary_terminate_id,
      remote_session::secondary_monitor_id,
      remote_session::secondary_input_id
    };
    for (const auto id : reserved) {
      app_identity_t app {"Ordinary game", "game-" + std::to_string(id), {}, "default"};
      std::vector<std::string> aliases;
      for (const auto alias : reserved) {
        aliases.push_back(std::to_string(alias));
      }
      aliases.push_back("123");
      std::map<std::string, alias_state_t> state {{app.uuid, {std::to_string(id), app.art_version, {aliases.begin(), aliases.end()}}}};
      std::set<std::string> ids, active;
      bool changed = false;
      proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
      std::vector<app_identity_t> apps {app};
      proc::catalog::prune_and_filter_aliases(apps, state, active, changed);
      EXPECT_EQ(remote_session::identify(std::stoi(app.id), app.uuid), remote_session::control_e::none);
      EXPECT_EQ(apps[0].aliases, std::vector<std::string> {"123"});
      EXPECT_EQ(proc::catalog::resolve_app(apps, "123"), 0u);
      EXPECT_TRUE(changed);
    }
  }

  TEST(ProcessCatalog, ExhaustedPreferredIdsRetryWithoutDuplicatingAnAppId) {
    app_identity_t app {"Game", "collision-game", {}, "default"};
    const auto legacy = proc::catalog::calculate_ids(app.name, app.uuid, {}, 0);
    const auto versioned = proc::catalog::calculate_versioned_ids(app.uuid, app.art_version, 0);
    std::set<std::string> ids {"101", std::get<0>(legacy), std::get<1>(legacy), std::get<0>(versioned), std::get<1>(versioned)};
    const auto occupied = ids;
    std::set<std::string> active;
    std::map<std::string, alias_state_t> state {{app.uuid, {"101", app.art_version, {}}}};
    bool changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_FALSE(occupied.contains(app.id));
    EXPECT_EQ(ids.size(), occupied.size() + 1);
    EXPECT_EQ(remote_session::identify(std::stoi(app.id), app.uuid), remote_session::control_e::none);
  }

  TEST(ProcessCatalog, DuplicatePersistedIdsPreserveFirstAppAndResolveSecondUniquely) {
    std::vector<app_identity_t> apps {{"A", "uuid-a", {}, "default"}, {"B", "uuid-b", {}, "default"}};
    std::map<std::string, alias_state_t> state {{"uuid-a", {"101", "default", {"201"}}}, {"uuid-b", {"101", "default", {"201"}}}};
    std::set<std::string> ids, active;
    bool changed = false;
    proc::catalog::assign_compatible_id(apps[0], 0, ids, state, active, changed);
    proc::catalog::assign_compatible_id(apps[1], 1, ids, state, active, changed);
    proc::catalog::prune_and_filter_aliases(apps, state, active, changed);
    EXPECT_EQ(apps[0].id, "101");
    EXPECT_NE(apps[1].id, "101");
    EXPECT_EQ(proc::catalog::resolve_app(apps, "101"), 0u);
    EXPECT_EQ(proc::catalog::resolve_app(apps, apps[1].id), 1u);
    EXPECT_FALSE(proc::catalog::resolve_app(apps, "201"));
  }

  TEST(ProcessCatalog, MalformedPersistedIdsCannotReachNumericCatalogueConversion) {
    for (const std::string bad : {"", "bad", "0", "-1", "2147483648", "+2147483505", "2147483505suffix", "02147483505"}) {
      app_identity_t app {"Game", "malformed-game", {}, "default"};
      std::map<std::string, alias_state_t> state {{app.uuid, {bad, "default", {bad, "123"}}}};
      std::set<std::string> ids, active;
      bool changed = false;
      proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
      std::vector<app_identity_t> apps {app};
      proc::catalog::prune_and_filter_aliases(apps, state, active, changed);
      EXPECT_GT(std::stoi(app.id), 0);
      EXPECT_EQ(remote_session::identify(std::stoi(app.id), app.uuid), remote_session::control_e::none);
      EXPECT_NE(std::find(apps[0].aliases.begin(), apps[0].aliases.end(), "123"), apps[0].aliases.end());
      EXPECT_EQ(std::find(apps[0].aliases.begin(), apps[0].aliases.end(), bad), apps[0].aliases.end());
      for (const auto &alias : apps[0].aliases) {
        EXPECT_EQ(remote_session::identify(std::stoi(alias), app.uuid), remote_session::control_e::none);
      }
    }
  }

  TEST(ProcessCatalog, LegacyNamesReserveControlsAndExhaustedFallbacksRetry) {
    app_identity_t app {"00211013-3213-4121-8110-000000000000\ndefault", {}, {}, {}};
    const auto legacy = proc::catalog::calculate_ids(app.name, {}, {}, 0);
    EXPECT_EQ(std::get<0>(legacy), std::to_string(remote_session::monitor_id));
    std::set<std::string> ids {std::get<1>(legacy)}, active;
    const auto occupied = ids;
    std::map<std::string, alias_state_t> state;
    bool changed = false;
    proc::catalog::assign_compatible_id(app, 0, ids, state, active, changed);
    EXPECT_FALSE(occupied.contains(app.id));
    EXPECT_EQ(remote_session::identify(std::stoi(app.id), {}), remote_session::control_e::none);
    EXPECT_TRUE(state.empty());
  }
}  // namespace

TEST(ProcessCatalog, FreshLegacyControlWitnessesSurviveAllocationProjectionAndSelection) {
  const std::vector<std::pair<int, std::string>> witnesses {
    {2147483601, "02113232-3321-4330-8110-000000000000"},
    {2147483602, "31332122-3011-4022-8100-000000000000"},
    {2147483603, "20323020-0201-4131-8110-000000000000"},
    {2147483604, "31213120-0033-4220-8010-000000000000"},
    {2147483605, "20202022-3223-4333-8000-000000000000"},
    {2147483606, "13023332-3113-4021-8010-000000000000"},
  };
  for (const auto &[old_id, uuid] : witnesses) {
    proc::catalog::app_identity_t app {.name = "Custom profile", .uuid = uuid, .art_version = "default"};
    ASSERT_EQ(std::get<0>(proc::catalog::calculate_versioned_ids(uuid, "default", 0)), std::to_string(old_id));
    std::set<std::string> occupied, active;
    std::map<std::string, proc::catalog::alias_state_t> state;
    bool changed = false;
    proc::catalog::assign_compatible_id(app, 0, occupied, state, active, changed);
    // Persisted cached controls must also be scrubbed from safe current apps.
    app.aliases.push_back(std::to_string(old_id));
    app.aliases.push_back("123");
    std::vector<proc::catalog::app_identity_t> apps {app};
    proc::catalog::prune_and_filter_aliases(apps, state, active, changed);
    app = apps[0];
    EXPECT_NE(app.id, std::to_string(old_id));
    EXPECT_EQ(std::find(app.aliases.begin(), app.aliases.end(), std::to_string(old_id)), app.aliases.end());
    EXPECT_NE(std::find(app.aliases.begin(), app.aliases.end(), "123"), app.aliases.end());
    remote_session::app_t row {std::stoi(app.id), uuid, app.name, false, true, app.aliases};
    const auto projected = remote_session::project({}, {}, {}, {row}, false);
    ASSERT_EQ(projected.catalogue.front().uuid, uuid);
    EXPECT_EQ(remote_session::identify(row.id, {}, 0, {row}), remote_session::control_e::none);
    const auto selected = proc::catalog::resolve_app(apps, std::to_string(projected.catalogue.front().id), {});
    ASSERT_TRUE(selected);
    EXPECT_EQ(apps[*selected].uuid, uuid);
    EXPECT_EQ(proc::catalog::resolve_app(apps, "123", {}), selected);
    EXPECT_EQ(remote_session::identify(old_id), static_cast<remote_session::control_e>(old_id - 2147483600));
  }
}
