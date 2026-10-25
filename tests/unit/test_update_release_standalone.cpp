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

// Standalone regression harness: g++ -std=c++20 -UNDEBUG -I. tests/unit/test_update_release_standalone.cpp
#include "src/update_release.h"
#include <cassert>
#include <iostream>
#ifdef NDEBUG
#error Release selection regression checks require assertions enabled.
#endif
using nlohmann::json;
std::string platform_package() {
#ifdef _WIN32
  return "VibepolloSetup.exe";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "Vibepollo-macOS-arm64.dmg";
#else
  return "Vibepollo-macOS-x86_64.dmg";
#endif
#elif defined(__aarch64__)
  return "vibepollo-2.0.0-1-aarch64.pkg.tar.zst";
#else
  return "vibepollo-2.0.0-1-x86_64.pkg.tar.zst";
#endif
}
json release(std::string tag, bool pre = false) { return {{"tag_name",tag},{"draft",false},{"prerelease",pre},{"assets",json::array({{{"name",platform_package()},{"state","uploaded"},{"size",100}}})}}; }
json daily(std::string day, std::string base = "2.0.0") {
  auto result=release("daily-2026-10-"+day,true);
  for(auto name : {"nightly-metadata.json","SHA256SUMS"}) result["assets"].push_back({{"name",name},{"state","uploaded"},{"size",100}});
  result["nightly_metadata"]={{"schema_version",1},{"channel","nightly"},{"tag",result["tag_name"]},{"version",base+"-daily.d202610"+day+"+aaaaaaaaaaaa"},{"source_commit",std::string(40,'a')},{"nightly_sequence",std::stoi("202610"+day)}};
  return result;
}
json nightly(std::string day, std::string sha = std::string(40, 'b')) {
  auto tag = "2.0.1-nightly.d202610" + day + "." + sha.substr(0, 12);
  auto result = release(tag, true);
  for (auto name : {"nightly-metadata.json", "SHA256SUMS"}) result["assets"].push_back({{"name",name},{"state","uploaded"},{"size",100}});
  result["nightly_metadata"] = {{"schema_version",1},{"channel","nightly"},{"tag",tag},{"version",tag},
                                 {"source_commit",sha},{"nightly_sequence",std::stoi("202610"+day)}};
  return result;
}
json revised_nightly(std::string day, int revision, std::string sha) {
  auto result = nightly(day, sha);
  auto tag = "2.0.1-nightly.d202610" + day + ".r." + std::to_string(revision) + "." + sha.substr(0, 12);
  result["tag_name"] = tag;
  result["nightly_metadata"]["tag"] = tag;
  result["nightly_metadata"]["version"] = tag;
  result["nightly_metadata"]["nightly_sequence"] = std::stoll("202610" + day) * 1000 + revision;
  return result;
}
int main() {
  auto select = [](json releases, const std::string &installed, bool pre) {
    for (auto &item : releases) if (item.is_object() && item.contains("nightly_metadata"))
      item["body"] = "<!-- nightly-identity: " + item["nightly_metadata"].dump() + " -->\n";
    return release_selection::select(releases, installed, pre);
  };
  auto current_nightly = nightly("22");
  auto next_nightly = nightly("23", std::string(40, '0'));
  auto same_day_nightly = nightly("22", std::string(40, 'c'));
  auto with_body = [](json item) {
    item["body"] = "<!-- nightly-identity: " + item["nightly_metadata"].dump() + " -->\n";
    return item;
  };
  assert(!release_selection::select(json::array({with_body(current_nightly)}), "2.0.1-daily.d20261022.2+aaaaaaaaaaaa", true).is_null());
  assert(release_selection::select(json::array({with_body(current_nightly)}), current_nightly["tag_name"], true).is_null());
  assert(release_selection::select(json::array({with_body(same_day_nightly)}), current_nightly["tag_name"], true).is_null());
  assert(!release_selection::select(json::array({with_body(next_nightly)}), current_nightly["tag_name"], true).is_null());
  assert(release_selection::select(json::array({with_body(next_nightly)}), "2.0.0", false).is_null());
  assert(release_selection::select(json::array({with_body(next_nightly)}), "2.0.1", true).is_null());
  auto revision1 = revised_nightly("22", 1, std::string(40, '0'));
  auto revision2 = revised_nightly("22", 2, std::string(40, 'a'));
  auto revision10 = revised_nightly("22", 10, std::string(40, '1'));
  auto revisions = json::array({revision10, current_nightly, revision2, revision1});
  assert(select(revisions, current_nightly["tag_name"], true)["tag_name"] == revision10["tag_name"]);
  assert(select(revisions, revision1["tag_name"], true)["tag_name"] == revision10["tag_name"]);
  assert(select(revisions, revision10["tag_name"], true).is_null());
  assert(select(json::array({current_nightly}), revision1["tag_name"], true).is_null());
  assert(select(json::array({next_nightly}), revision10["tag_name"], true)["tag_name"] == next_nightly["tag_name"]);
  assert(select(json::array({revision10}), next_nightly["tag_name"], true).is_null());
  assert(select(revisions, "2.0.1", true).is_null());
  assert(select(revisions, "2.0.0", false).is_null());
  assert(select(json::array({revision1}), "2.0.1-daily.d20261021.999+aaaaaaaaaaaa", true)["tag_name"] == revision1["tag_name"]);
  assert(select(json::array({revision1}), "2.0.1-daily.d20261023.1+aaaaaaaaaaaa", true).is_null());
  assert(select(json::array({daily("23", "2.0.1")}), revision1["tag_name"], true)["tag_name"] == "daily-2026-10-23");
  for (const auto &field : {"source_commit", "nightly_sequence", "version"}) {
    auto invalid = revision1;
    invalid["nightly_metadata"][field] = "tampered";
    assert(select(json::array({invalid}), "2.0.0", true).is_null());
  }
  auto malformed = revision1;
  malformed["tag_name"] = "2.0.1-nightly.d20261022.r.0.aaaaaaaaaaaa";
  assert(select(json::array({malformed}), "2.0.0", true).is_null());
  auto missing_body = with_body(revision1);
  missing_body.erase("body");
  assert(release_selection::select(json::array({missing_body}), "2.0.0", true).is_null());
  auto missing_sums = revision1;
  missing_sums["assets"].erase(2);
  assert(select(json::array({missing_sums}), "2.0.0", true).is_null());
  auto tampered_nightly = current_nightly;
  tampered_nightly["nightly_metadata"]["source_commit"] = std::string(40, 'c');
  assert(release_selection::select(json::array({with_body(tampered_nightly)}), "2.0.0", true).is_null());
  auto api = daily("12", "2.0.1");
  api["body"] = "<!-- nightly-identity: " + api["nightly_metadata"].dump() + " -->\n";
  api.erase("nightly_metadata");
  api["assets"].erase(1); // No metadata asset required by new clients.
  assert(!release_selection::select(json::array({api}), "2.0.0", true).is_null());
  assert(release_selection::select(json::array({api}), "2.0.0", false).is_null());
  auto duplicate = api;
  duplicate["body"] = api["body"].get<std::string>() + api["body"].get<std::string>();
  assert(release_selection::select(json::array({duplicate}), "2.0.0", true).is_null());
  api.erase("body");
  assert(release_selection::select(json::array({api}), "2.0.0", true).is_null());
  auto releases=json::array({release("1.9.0-stable.2"),release("2.0.0-beta.10",true),release("2.0.0-beta.2",true)});
  assert(release_selection::complete(releases[0],false));
  assert(release_selection::compare_notification_versions("1.9.0-stable.1","1.9.0-stable.2")<0);
  assert(select(releases,"1.9.0-stable.1",false)["tag_name"]=="1.9.0-stable.2");
  assert(select(json::array({release("1.9.0"),release("1.9.0-stable.1")}),"1.9.0-stable.2",false).is_null());
  assert(select(json::array({release("1.9.0-stable.2"),release("1.9.0-stable.10")}),"1.9.0",false)["tag_name"]=="1.9.0-stable.10");
  assert(select(releases,"2.0.0-beta.2",false).is_null());
  assert(select(releases,"2.0.0-beta.2",true)["tag_name"]=="2.0.0-beta.10");
  auto days=json::array({daily("10"),daily("11"),daily("12","1.0.0")});
  assert(select(days,"2.0.0-daily.d20261010+aaaaaaaaaaaa",true)["tag_name"]=="daily-2026-10-11");
  assert(select(days,"2.0.0-daily.d20261011+aaaaaaaaaaaa",true).is_null());
  assert(select(days,"2.0.0",true).is_null());
  assert(select(days,"2.0.0-beta.1",true).is_null());
  assert(select(json::array({release("2.0.0-rc.1",true)}),"2.0.0-daily.d20261011+aaaaaaaaaaaa",true).is_null());
  days.push_back(release("2.0.0"));
  assert(select(days,"2.0.0-daily.d20261010+aaaaaaaaaaaa",true)["tag_name"]=="2.0.0");
  // Actual published identities reproduce the old same-core downgrade.
  assert(release_selection::compare_notification_versions("2.0.0-daily.d20261011+aaa39188cc78", "2.0.0") < 0);
  assert(release_selection::compare_notification_versions("2.0.0-daily.d20261011+0d62c962e081", "2.0.0") < 0);
  auto revision=daily("11", "2.0.1");
  revision["tag_name"]="daily-2026-10-11.2";
  revision["nightly_metadata"]["tag"]=revision["tag_name"];
  revision["nightly_metadata"]["version"]="2.0.1-daily.d20261011.2+aaaaaaaaaaaa";
  revision["nightly_metadata"]["nightly_sequence"]=20261011002LL;
  auto qualified=json::array({revision});
  assert(select(qualified,"2.0.0",true)["tag_name"]==revision["tag_name"]);
  assert(select(qualified,"2.0.0-daily.d20261011+aaa39188cc78",true)["tag_name"]==revision["tag_name"]);
  assert(select(qualified,"2.0.1-daily.d20261011+bbbbbbbbbbbb",true)["tag_name"]==revision["tag_name"]);
  assert(select(qualified,"2.0.1-daily.d20261011.2+aaaaaaaaaaaa",true).is_null());
  assert(select(qualified,"2.0.1-daily.d20261012+bbbbbbbbbbbb",true).is_null());
  assert(select(qualified,"2.0.0",false).is_null());
  assert(select(qualified,"2.0.1",true).is_null());
  qualified.push_back(release("2.0.1"));
  assert(select(qualified,"2.0.1-daily.d20261011.2+aaaaaaaaaaaa",true)["tag_name"]=="2.0.1");
  auto invalid_revision=revision;
  invalid_revision["nightly_metadata"]["nightly_sequence"]=20261011;
  assert(select(json::array({invalid_revision}),"2.0.0",true).is_null());
  auto published=release("2.0.0");
#ifdef _WIN32
  published["assets"][0]["name"]="VibepolloSetup-v2.0.0.exe";
#endif
  assert(select(json::array({published}),"1.18.4-stable.4",false)["tag_name"]=="2.0.0");
  auto wrong_platform=release("9.0.0");
#ifdef _WIN32
  wrong_platform["assets"][0]["name"]="vibepollo-fixture-x86_64.pkg.tar.zst";
#else
  wrong_platform["assets"][0]["name"]="VibepolloSetup.exe";
#endif
  assert(select(json::array({wrong_platform}),"1.0.0",true).is_null());
#ifndef _WIN32
  auto wrong_arch=release("9.0.0");
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  wrong_arch["assets"][0]["name"]="Vibepollo-macOS-x86_64.dmg";
#else
  wrong_arch["assets"][0]["name"]="Vibepollo-macOS-arm64.dmg";
#endif
#elif defined(__x86_64__)
  wrong_arch["assets"][0]["name"]="vibepollo-fixture-aarch64.pkg.tar.zst";
#else
  wrong_arch["assets"][0]["name"]="vibepollo-fixture-x86_64.pkg.tar.zst";
#endif
  assert(select(json::array({wrong_arch}),"1.0.0",true).is_null());
#endif
  auto bad=daily("11"); bad["nightly_metadata"]["source_commit"]="bad";
  auto draft=release("9.0.0"); draft["draft"]=true;
  auto incomplete=release("9.0.0"); incomplete["assets"][0]["state"]="new";
  auto empty_package=release("9.0.0"); empty_package["assets"][0]["size"]=0;
  auto missing_checksums=daily("11"); missing_checksums["assets"].erase(2);
  assert(select(json::array({bad,draft,incomplete,empty_package,missing_checksums,json("bad"),json::object()}),"1.0.0",true).is_null());
  assert(select(releases,"unknown",true).is_null());
  assert(select(releases,"99999999999999999999999.0.0",true).is_null());
  auto mac=release("2.0.0"); mac["assets"][0]["name"]="Vibepollo-macOS-arm64.dmg";
  assert(release_selection::macos_package(mac,true));
  assert(!release_selection::macos_package(mac,false));
  mac["assets"][0]["name"]="Vibepollo-macOS-x86_64.dmg";
  assert(release_selection::macos_package(mac,false));
  assert(!release_selection::macos_package(mac,true));
  mac["assets"][0]["size"]=0;
  assert(!release_selection::macos_package(mac,false));
  mac["assets"][0]["size"]=100; mac["assets"][0]["state"]="new";
  assert(!release_selection::macos_package(mac,false));
  mac["assets"][0]["state"]="uploaded"; mac["assets"][0]["name"]="Vibeshine-macOS-x86_64.dmg";
  assert(!release_selection::macos_package(mac,false));
  mac["assets"]=json::array();
  assert(!release_selection::macos_package(mac,false));
  auto compatible = daily("11");
  compatible["tag_name"] = "2.0.1-daily.d20261011.3";
  compatible["nightly_metadata"]["tag"] = compatible["tag_name"];
  compatible["nightly_metadata"]["version"] = "2.0.1-daily.d20261011.3+aaaaaaaaaaaa";
  compatible["nightly_metadata"]["nightly_sequence"] = 20261011003LL;
  assert(version_compare::compare_semver("2.0.0", compatible["tag_name"].get<std::string>()) < 0);
  assert(select(json::array({compatible}), "2.0.0", true)["tag_name"] == compatible["tag_name"]);
  assert(select(json::array({compatible}), "2.0.0", false).is_null());
  assert(select(json::array({compatible}), "2.0.1", true).is_null());
  auto unverified = compatible;
  unverified.erase("nightly_metadata");
  assert(select(json::array({unverified}), "2.0.0", true).is_null());
  auto wrong_tag = compatible;
  wrong_tag["nightly_metadata"]["tag"] = "daily-2026-10-11.3";
  assert(select(json::array({wrong_tag}), "2.0.0", true).is_null());
  auto wrong_core = compatible;
  wrong_core["nightly_metadata"]["version"] = "2.0.2-daily.d20261011.3+aaaaaaaaaaaa";
  assert(select(json::array({wrong_core}), "2.0.0", true).is_null());
  std::cout << "release selection regression checks passed\n";
}
