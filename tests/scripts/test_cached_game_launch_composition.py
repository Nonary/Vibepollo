#!/usr/bin/env python3
"""Exercise the production NVHTTP tile registration and launch selection.

The catalogue, process lookup, and request arguments are deterministic test
adapters; this does not start a server, RTSP transport, or real application.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


PREAMBLE = r'''
#include "src/remote_session.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace std::string_literals;
using args_t = std::multimap<std::string, std::string>;
std::string get_arg(const args_t &args, const char *key, const char *fallback = nullptr) {
  auto it = args.find(key);
  return it == args.end() ? std::string {fallback} : it->second;
}
namespace util { int from_view(const std::string &value) { return std::stoi(value); } }
namespace proc {
  struct resolved_t : remote_session::app_t { bool remote_monitor_profile; };
  struct process_t {
    std::vector<remote_session::app_t> apps;
    std::vector<remote_session::app_t> get_apps() const { return apps; }
    std::optional<resolved_t> resolve_app(const std::string &id, const std::string &uuid) const {
      const auto it = std::find_if(apps.begin(), apps.end(), [&](const auto &app) {
        return !uuid.empty() ? app.uuid == uuid : std::to_string(app.id) == id;
      });
      return it == apps.end() ? std::nullopt : std::optional {resolved_t {*it, it->monitor_profile}};
    }
  } proc;
}
auto remote_configured_catalogue(const std::vector<remote_session::app_t> &apps) { return apps; }
remote_session::cached_game_tiles_t cached_running_game_tiles;
struct identity_t { std::string uuid; };
struct tree_t {
  int status {};
  void put(const char *key, int value) { if (std::string {key} == "root.<xmlattr>.status_code") status = value; }
  void put(const char *, const char *) {}
};
struct outcome_t {
  int status {};
  remote_session::control_e control {};
  std::optional<proc::resolved_t> selected;
  args_t args;
};
'''


TESTS = r'''
int main() {
  const remote_session::app_t game {42, "game-uuid", "Game", false};
  const remote_session::app_t other {77, "other-uuid", "Other", false};
  proc::proc.apps = {game, other};
  const remote_session::game_t active {.running = true, .owner_uuid = "owner", .app = game};
  const remote_session::caller_t caller {.uuid = "viewer", .paired = true};
  const auto projection = remote_session::project(caller, active, {}, proc::proc.apps, false);
  const auto tile = projection.catalogue.front();
  advertise("viewer", tile, active);

  for (const auto &uuid : {std::string {}, tile.uuid}) {
    auto result = route("viewer", tile.id, uuid, 0);
    assert(result.status == 0 && result.control == remote_session::control_e::none);
    assert(result.selected && result.selected->uuid == game.uuid);
    assert(get_arg(result.args, "appid") == "42");
    assert(get_arg(result.args, "appuuid") == game.uuid);
    assert(get_arg(result.args, "mode") == "1920x1080x60");

    // The same request must still resume when the original game is running.
    result = route("viewer", tile.id, uuid, 42);
    assert(result.control == remote_session::control_e::running_game);
    // If a different game took over, normal replacement admission sees the
    // original requested game, not a request to resume the new one.
    result = route("viewer", tile.id, uuid, 77);
    assert(result.control == remote_session::control_e::none);
    assert(result.selected && result.selected->uuid == game.uuid);
  }

  auto result = route("viewer", 0, tile.uuid, 0);
  assert(result.selected && result.selected->uuid == game.uuid);
  proc::proc.apps[0].id = 84;
  result = route("viewer", tile.id, "", 0);
  assert(result.selected && result.selected->id == 84);
  assert(get_arg(result.args, "appid") == "84");
  result = route("viewer", tile.id, "", 84);
  assert(result.control == remote_session::control_e::running_game);

  proc::proc.apps.erase(proc::proc.apps.begin());
  result = route("viewer", tile.id, "", 0);
  assert(result.status == 404);
  result = route("viewer", remote_session::secondary_monitor_id, "", 0);
  assert(result.control == remote_session::control_e::monitor);
  result = route("viewer", remote_session::secondary_input_id, "", 0);
  assert(result.control == remote_session::control_e::input);
}
'''


def fixture(source: str) -> str:
    start = source.index('          cached_running_game_tiles.remember(', source.index('void applist('))
    end = source.index(';', start) + 1
    registration = source[start:end]

    start = source.index('    auto appid_str =', source.index('void launch('))
    end = source.index('    if (cached_running_game) synthetic_control =', start)
    end = source.index(';', end) + 1
    launch_prefix = source[start:end]

    return PREAMBLE + r'''
void advertise(const std::string &client, const remote_session::app_t &entry, const remote_session::game_t &game) {
  const identity_t identity {client};
''' + registration + r'''
}
outcome_t route(const std::string &client, int id, const std::string &uuid, int current_appid) {
  args_t args {{"appid", std::to_string(id)}, {"mode", "1920x1080x60"}};
  if (!uuid.empty()) args.emplace("appuuid", uuid);
  tree_t tree;
  const identity_t request_client_identity {client};
''' + launch_prefix.replace('      return;', '      return {tree.status, remote_session::control_e::none, std::nullopt, args};') + r'''
  return {tree.status, synthetic_control, selected_app, args};
}
''' + TESTS


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--json-include', default='')
    args = parser.parse_args()
    source = (args.source / 'src/nvhttp.cpp').read_text()
    with tempfile.TemporaryDirectory() as directory:
        test_source = Path(directory) / 'cached-game.cpp'
        executable = Path(directory) / 'cached-game'
        test_source.write_text(fixture(source))
        subprocess.run([
            args.compiler, '-std=c++23', '-O0', '-pthread', '-I' + str(args.source),
            *['-I' + include for include in args.json_include.split(';') if include],
            str(args.source / 'src/remote_session.cpp'), str(test_source), '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
