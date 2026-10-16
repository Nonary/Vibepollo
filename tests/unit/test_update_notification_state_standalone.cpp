// Standalone compile-time proof: Release must retain all assert checks.
#include <cassert>
#if defined(NDEBUG)
#error "Updater standalone regression assertions must remain active in Release"
#endif
static_assert([] {
  int checks = 0;
  assert(++checks == 1);
  return checks == 1;
}(), "assert must evaluate its argument in this translation unit");

/**
 * @file tests/unit/test_update_notification_state_standalone.cpp
 * @brief Exercise the production notification parser and recent-key migration.
 */
#include "src/paired_state_policy.h"
#include "src/state_storage_migration.h"
#include "src/update.h"
#include "src/update_notification_state.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>

using boost::property_tree::ptree;
using nlohmann::json;
using update::notification::history_t;

static ptree tree(const json &value) {
  std::istringstream contents(value.dump());
  ptree result;
  boost::property_tree::read_json(contents, result);
  return result;
}

static void history_is(const history_t &actual, const std::string &last, std::vector<std::string> versions) {
  assert(actual.last_notified_version == last);
  assert(actual.notified_versions == versions);
}

static json paired_snapshot() {
  return json::parse(R"({"root":{
    "uniqueid":"11111111-1111-1111-1111-111111111111",
    "remote_display_layout":"independent",
    "named_devices":[{
      "uuid":"22222222-2222-2222-2222-222222222222",
      "cert":"existing-paired-client-certificate",
      "name":"Apollo owner",
      "perm":31,
      "enabled":true,
      "last_seen":1234,
      "allow_client_commands":true,
      "do":[{"cmd":"existing client command","elevated":false}],
      "undo":[],
      "config_overrides":{"virtual_display_mode":"per-client"},
      "unknown_client_metadata":{"retained":true}
    }],
    "unknown_root_metadata":{"retained":"Apollo"}
  }})");
}

static void optional_json_history() {
  history_is(update::notification::read_root(json()), "", {});
  for (const auto &bad : {json(), json(""), json(42), json::object({{"unexpected", "shape"}})}) {
    auto root = json::object({{"last_notified_version", "v2.0.0"}, {"notified_versions", bad}});
    const auto original = root;
    history_is(update::notification::read_root(root), "v2.0.0", {"v2.0.0"});
    assert(root == original);
  }
  auto root = json::object({
    {"last_notified_version", "v2.0.0"},
    {"notified_versions", json::array({"daily-2026-10-10", "", 9, false, json::object(), json::array(), "daily-2026-10-10", "v2.0.0"})}
  });
  history_is(update::notification::read_root(root), "v2.0.0", {"daily-2026-10-10", "v2.0.0"});
  root["last_notified_version"] = 7;
  history_is(update::notification::read_root(root), "", {"daily-2026-10-10", "v2.0.0"});
}

static void pairing_isolation() {
  for (const auto &bad : {json(), json(""), json(false), json::object({{"bad", "history"}}), json::array({"daily-2026-10-10", 9, json::object()})}) {
    auto snapshot = paired_snapshot();
    snapshot["root"]["last_notified_version"] = "v2.0.0";
    snapshot["root"]["notified_versions"] = bad;
    const auto original = snapshot;
    assert(nvhttp::state_policy::normalize_snapshot(snapshot));
    assert(snapshot == original);
    assert(nvhttp::state_policy::valid_primary_json(snapshot.dump()));
    const auto history = update::notification::read_root(snapshot["root"]);
    assert(history.last_notified_version == "v2.0.0");
    assert(snapshot == original);
  }
  auto invalid = paired_snapshot();
  invalid["root"]["last_notified_version"] = 7;
  assert(!nvhttp::state_policy::normalize_snapshot(invalid));
  invalid = paired_snapshot();
  invalid["root"]["named_devices"][0]["perm"] = -1;
  invalid["root"]["notified_versions"] = json::array();
  assert(!nvhttp::state_policy::normalize_snapshot(invalid));
}

static void json_and_ptree_roundtrips() {
  history_t expected {"v2.0.0", {"daily-2026-10-10", "v2.0.0"}};
  auto shared = paired_snapshot();
  const auto original_device = shared["root"]["named_devices"];
  update::notification::write_root(shared["root"], expected);
  history_is(update::notification::read_root(shared["root"]), expected.last_notified_version, expected.notified_versions);
  assert(shared["root"]["named_devices"] == original_device);
  assert(nvhttp::state_policy::normalize_snapshot(shared));
  auto separate = tree(json::object({{"root", {{"api_tokens", {{"existing", "token"}}}, {"unrelated", "retained"}}}}));
  const auto original_tokens = separate.get_child("root.api_tokens");
  update::notification::write_root(separate.get_child("root"), expected);
  std::ostringstream serialized;
  boost::property_tree::write_json(serialized, separate);
  std::istringstream contents(serialized.str());
  ptree restored;
  boost::property_tree::read_json(contents, restored);
  history_is(update::notification::read_file(restored), expected.last_notified_version, expected.notified_versions);
  assert(restored.get_child("root.api_tokens") == original_tokens);
  assert(restored.get<std::string>("root.unrelated") == "retained");
  update::notification::write_root(restored.get_child("root"), {});
  serialized.str("");
  boost::property_tree::write_json(serialized, restored);
  const auto encoded_empty = json::parse(serialized.str());
  assert(encoded_empty["root"]["notified_versions"] == "");
  history_is(update::notification::read_root(encoded_empty["root"]), "", {});
  auto malformed = tree(json::object({{"root", {{"last_notified_version", "v2.0.0"}, {"notified_versions", {{"object_key", "ignored"}}}}}}));
  history_is(update::notification::read_file(malformed), "v2.0.0", {"v2.0.0"});
  malformed = tree(json::object({{"root", {{"last_notified_version", "v2.0.0"}, {"notified_versions", json::array({"daily-2026-10-10", json::object({{"nested", "ignored"}}), "daily-2026-10-10", ""})}}}}));
  history_is(update::notification::read_file(malformed), "v2.0.0", {"daily-2026-10-10", "v2.0.0"});
}

static void mutex_state_and_reset() {
  update::state_t state;
  state.last_notified_url = "https://github.com/Nonary/Vibepollo/releases/tag/v2.0.0";
  state.last_notified_is_prerelease = true;
  update::notification::apply(state, {"v2.0.0", {"daily-2026-10-10", "v2.0.0"}});
  history_is(update::notification::snapshot(state), "v2.0.0", {"daily-2026-10-10", "v2.0.0"});
  assert(state.notification_mutex.try_lock());
  state.notification_mutex.unlock();
  update::notification::apply(state, {});
  history_is(update::notification::snapshot(state), "", {});
  assert(state.last_notified_url == "https://github.com/Nonary/Vibepollo/releases/tag/v2.0.0");
  assert(state.last_notified_is_prerelease);
}

static std::pair<ptree, ptree> migration_fixture() {
  auto source = paired_snapshot();
  source["root"]["api_tokens"] = json::object({{"old_api", "source-token"}});
  source["root"]["session_tokens"] = json::object({{"source_session", "source-session"}});
  source["root"]["last_notified_version"] = "v1.9.0";
  source["root"]["notified_versions"] = json::array({"daily-2026-10-09", "daily-2026-10-10", "daily-2026-10-09"});
  auto destination = json::object({{"root", {
    {"api_tokens", {{"existing_api", "destination-token"}}},
    {"last_notified_version", "v2.0.0"},
    {"notified_versions", json::array({"daily-2026-10-10", "v2.0.0"})},
    {"destination_only", "retained"}
  }}});
  return {tree(source), tree(destination)};
}

static void successful_migration() {
  const auto [source, destination] = migration_fixture();
  auto saved_source = source;
  auto saved_destination = destination;
  std::vector<std::string> calls;
  assert(statefile::migration::migrate_recent_keys(source, destination, true,
    [&](const ptree &next) { calls.push_back("destination"); saved_destination = next; return true; },
    [&](const ptree &next) { calls.push_back("source"); saved_source = next; return true; }));
  assert((calls == std::vector<std::string> {"destination", "source"}));
  history_is(update::notification::read_file(saved_destination), "v2.0.0", {"daily-2026-10-10", "v2.0.0", "daily-2026-10-09", "v1.9.0"});
  assert(saved_destination.get_child("root.api_tokens") == destination.get_child("root.api_tokens"));
  assert(saved_destination.get_child("root.session_tokens") == source.get_child("root.session_tokens"));
  assert(saved_destination.get<std::string>("root.destination_only") == "retained");
  assert(saved_source.get_child("root.named_devices") == source.get_child("root.named_devices"));
  assert(saved_source.get<std::string>("root.uniqueid") == source.get<std::string>("root.uniqueid"));
  assert(saved_source.get_child("root.unknown_root_metadata") == source.get_child("root.unknown_root_metadata"));
  assert(!saved_source.get_child_optional("root.api_tokens"));
  assert(!saved_source.get_child_optional("root.session_tokens"));
  assert(!saved_source.get_child_optional("root.notified_versions"));
  assert(!saved_source.get_child_optional("root.last_notified_version"));
  calls.clear();
  assert(statefile::migration::migrate_recent_keys(saved_source, saved_destination, true,
    [&](const ptree &) { calls.push_back("destination"); return true; },
    [&](const ptree &) { calls.push_back("source"); return true; }));
  assert(calls.empty());
}

static void destination_failure_preserves_source() {
  const auto [source, destination] = migration_fixture();
  auto saved_source = source;
  auto saved_destination = destination;
  int destination_writes = 0;
  int source_writes = 0;
  assert(!statefile::migration::migrate_recent_keys(source, destination, true,
    [&](const ptree &) { ++destination_writes; return false; },
    [&](const ptree &next) { ++source_writes; saved_source = next; return true; }));
  assert(destination_writes == 1);
  assert(source_writes == 0);
  assert(saved_source == source && saved_destination == destination);
  bool caught = false;
  try {
    (void)statefile::migration::migrate_recent_keys(source, destination, true,
      [&](const ptree &) -> bool { throw std::runtime_error("simulated atomic destination failure"); },
      [&](const ptree &next) { ++source_writes; saved_source = next; return true; });
  } catch (const std::runtime_error &) { caught = true; }
  assert(caught && source_writes == 0 && saved_source == source);
}

static void source_failure_can_retry() {
  const auto [source, destination] = migration_fixture();
  auto saved_source = source;
  auto saved_destination = destination;
  assert(!statefile::migration::migrate_recent_keys(saved_source, saved_destination, true,
    [&](const ptree &next) { saved_destination = next; return true; },
    [&](const ptree &) { return false; }));
  assert(saved_source == source);
  history_is(update::notification::read_file(saved_destination), "v2.0.0", {"daily-2026-10-10", "v2.0.0", "daily-2026-10-09", "v1.9.0"});
  std::vector<std::string> calls;
  assert(statefile::migration::migrate_recent_keys(saved_source, saved_destination, true,
    [&](const ptree &next) { calls.push_back("destination"); saved_destination = next; return true; },
    [&](const ptree &next) { calls.push_back("source"); saved_source = next; return true; }));
  assert((calls == std::vector<std::string> {"destination", "source"}));
  history_is(update::notification::read_file(saved_destination), "v2.0.0", {"daily-2026-10-10", "v2.0.0", "daily-2026-10-09", "v1.9.0"});
  assert(!saved_source.get_child_optional("root.notified_versions"));
}

static void bootstrap_and_destination_precedence() {
  const auto [source, destination] = migration_fixture();
  int writes = 0;
  auto count = [&](const ptree &) { ++writes; return true; };
  assert(statefile::migration::migrate_recent_keys(source, destination, false, count, count));
  assert(statefile::migration::migrate_recent_keys(ptree {}, destination, true, count, count));
  assert(writes == 0);
  auto old_only = tree(json::object({{"root", {{"last_notified_version", "v1.9.0"}}}}));
  ptree saved_source;
  ptree saved_destination;
  assert(statefile::migration::migrate_recent_keys(old_only, ptree {}, true,
    [&](const ptree &next) { saved_destination = next; return true; },
    [&](const ptree &next) { saved_source = next; return true; }));
  history_is(update::notification::read_file(saved_destination), "v1.9.0", {"v1.9.0"});
  auto empty_last_destination = tree(json::object({{"root", {{"last_notified_version", ""}}}}));
  assert(statefile::migration::migrate_recent_keys(old_only, empty_last_destination, true,
    [&](const ptree &next) { saved_destination = next; return true; },
    [&](const ptree &next) { saved_source = next; return true; }));
  history_is(update::notification::read_file(saved_destination), "", {"v1.9.0"});
}

int main() {
  optional_json_history();
  pairing_isolation();
  json_and_ptree_roundtrips();
  mutex_state_and_reset();
  successful_migration();
  destination_failure_preserves_source();
  source_failure_can_retry();
  bootstrap_and_destination_precedence();
  std::cout << "PASS: 8 notification and state migration regression groups\n";
}
