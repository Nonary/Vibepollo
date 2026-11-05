#!/usr/bin/env python3
"""Generate a fixture from real app-load control flow and migration functions.

Per-app field conversion, profile policy, and file I/O are test boundaries; the
real process_catalog publication code is linked by the CMake target.
"""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.root.resolve()
source = (root / 'src/process.cpp').read_text()
migrate_start = source.index('  void migrate_apps(')
migrate_end = source.index('  std::optional<proc::proc_t> parse(', migrate_start)
load_start = source.index('    bool apps_parsed_ok = false;', migrate_end)
load_end = source.index('    // Virtual Display entry', load_start)
persist_start = source.index('    // Persist only the fully parsed staged migration.', load_end)
builtins = source[load_end:persist_start]
# Exercise the unmodified Windows-only production block on non-Windows hosts.
# Defining _WIN32 globally would select unrelated Windows system headers.
if builtins.count('#ifdef _WIN32') != 1:
    raise ValueError('Windows built-in guard changed; update the portable fixture')
builtins = builtins.replace('#ifdef _WIN32', '#if defined(_WIN32) || defined(SUNSHINE_TEST_WINDOWS_BUILTIN)', 1)
status_header = (root / 'src/platform/windows/virtual_display.h').read_text()
status_match = re.search(r'  enum class DRIVER_STATUS\s*\{.*?\n  \};', status_header, re.S)
if not status_match:
    raise ValueError('Production virtual-display driver status declaration not found')
status = status_match.group(0)
factory_header = (root / 'src/builtin_virtual_display.h').read_text()
factory_start = factory_header.index('  template<class Expand>')
factory_end = factory_header.index('\n}  // namespace proc', factory_start)
factory = factory_header[factory_start:factory_end]
persist_end = source.index('    // Only reconcile the persisted alias map', persist_start)
persist = source[persist_start:persist_end]
loop = source[load_start:load_end]
app_start = loop.index('        for (auto &app_node : tree["apps"]) {')
app_end = loop.index('        fail_count = 0;', app_start)
loop = loop[:app_start] + '''        // App field conversion is a deterministic fixture boundary. Real JSON
        // loading, migration retries, failure guard and publication are used.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t app {};
          app.uuid = app_node.at("uuid").get<std::string>();
          app.name = app_node.at("name").get<std::string>();
          app.id = std::to_string(++i);
          apps.emplace_back(std::move(app));
        }

''' + loop[app_end:]
template = Path(__file__).with_suffix('.cpp.in').read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(template.replace('@DRIVER_STATUS@', status).replace('@BUILTIN_FACTORY@', factory).replace('@MIGRATION@', source[migrate_start:migrate_end]).replace('@LOAD_LOOP@', loop).replace('@BUILTIN@', builtins).replace('@PERSIST@', persist))
