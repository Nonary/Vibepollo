"""Run production fingerprint functions with a signed 128-bit file-clock port.

The filesystem boundary is deterministic; this is not a native libc++ or macOS
runtime test. Function bodies are extracted unchanged except for routing the
filesystem namespace to the port. No third-party libraries are needed.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from test_build_api_compatibility import function


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()
    root = args.root
    provider = (root / 'src/provider_scan_protocol.cpp').read_text()
    lutris = (root / 'src/lutris_integration.cpp').read_text()
    autosync = (root / 'src/steam_auto_sync_policy.cpp').read_text()
    artwork = (root / 'src/steam_artwork.cpp').read_text()
    code = r'''
#include "src/steam_artwork.h"
#include "src/file_clock.h"
#include <fstream>
#include <set>
#include <cassert>
#include <chrono>
#include <limits>
#include <system_error>
namespace testfs {
using path = std::filesystem::path;
using std::filesystem::remove;
#ifdef __SIZEOF_INT128__
using rep = __int128;
#else
#error This compiler does not support the signed 128-bit file-clock fixture
#endif
rep ticks = 0;
bool fail = false;
bool is_regular_file(const path &, std::error_code &ec) {
  ec.clear(); return !fail;
}
path weakly_canonical(const path &p, std::error_code &ec) { ec.clear(); return p; }
path absolute(const path &p, std::error_code &ec) { ec.clear(); return p; }
std::uintmax_t file_size(const path &, std::error_code &ec) {
  ec = fail ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{};
  return 42;
}
struct clock_time {
  auto time_since_epoch() const { return std::chrono::duration<rep, std::nano>(ticks); }
};
clock_time last_write_time(const path &, std::error_code &ec) {
  ec = fail ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{};
  return {};
}
}
namespace fs = testfs;
'''
    code += function(provider, 'std::string artwork_revision(')
    code += function(lutris, 'void hash_bytes(')
    code += function(lutris, 'void hash_path(').replace('std::filesystem', 'testfs')
    code += 'constexpr std::string_view separator {"\\x1f", 1};\n'
    code += function(autosync, 'void mix(')
    code += function(autosync, 'template<typename T>')
    code += function(autosync, 'void mix_path(').replace('std::filesystem', 'testfs')
    code += function(artwork, 'std::optional<platf::steam::artwork::source_fingerprint_t> fingerprint(')
    code += function(artwork, 'bool read_meta(')
    code += r'''
std::string unique_suffix() { return "fixture"; }
bool replace_atomically(const fs::path &temporary, const fs::path &target) {
  std::error_code ec;
  std::filesystem::remove(target, ec);
  std::filesystem::rename(temporary, target, ec);
  return !ec;
}
'''
    code += function(artwork, 'bool write_meta(')
    code += r'''
int main(int argc, char **argv) {
  assert(argc == 2);
  const fs::path metadata(argv[1]);
  const fs::path path("cover.png");
  for (std::int64_t tick : {std::numeric_limits<std::int64_t>::min(),
                           std::int64_t(-1700000000000000000LL), std::int64_t(-1),
                           std::int64_t(0), std::int64_t(1),
                           std::int64_t(1700000000000000000LL),
                           std::numeric_limits<std::int64_t>::max()}) {
    testfs::ticks = tick;
    const auto text = std::to_string(tick);
    std::uint64_t expected = 1469598103934665603ULL;
    for (unsigned char c : "cover.png:42:" + text) expected = (expected ^ c) * 1099511628211ULL;
    assert(artwork_revision({path}) == std::to_string(expected));
    assert(artwork_revision({}) == "");
    expected = 123;
    hash_bytes(expected, "cover.png"); hash_bytes(expected, "42"); hash_bytes(expected, text);
    std::uint64_t actual = 123; hash_path(actual, path); assert(actual == expected);
    expected = 123;
    mix(expected, "cover.png"); mix(expected, separator);
    mix(expected, "42"); mix(expected, separator);
    mix(expected, text); mix(expected, separator);
    actual = 123; mix_path(actual, path); assert(actual == expected);
    const auto result = fingerprint(path);
    assert(result && result->mtime == text && result->size == 42 && result->path == "cover.png");
  }

  // Check exact wide decimal text as well as the original modulo-alias pair.
  const testfs::rep wide = testfs::rep(1) << 64;
  const testfs::rep maximum = std::numeric_limits<testfs::rep>::max();
  const testfs::rep minimum = std::numeric_limits<testfs::rep>::min();
  const std::pair<testfs::rep, std::string> cases[] = {
    {0, "0"}, {wide, "18446744073709551616"}, {-wide, "-18446744073709551616"},
    {wide + 1, "18446744073709551617"}, {-wide - 1, "-18446744073709551617"},
    {testfs::rep(std::numeric_limits<std::int64_t>::max()) + 1, "9223372036854775808"},
    {testfs::rep(std::numeric_limits<std::int64_t>::min()) - 1, "-9223372036854775809"},
    {maximum, "170141183460469231731687303715884105727"},
    {minimum, "-170141183460469231731687303715884105728"}
  };
  std::set<std::string> revisions, mtimes;
  std::set<std::uint64_t> lutris_hashes, autosync_hashes;
  for (const auto &[tick, text] : cases) {
    testfs::ticks = tick;
    assert(file_clock::count_string(tick) == text);
    std::uint64_t expected = 1469598103934665603ULL;
    for (unsigned char c : "cover.png:42:" + text) expected = (expected ^ c) * 1099511628211ULL;
    const auto revision = artwork_revision({path});
    assert(revision == std::to_string(expected) && revisions.insert(revision).second);
    expected = 123;
    hash_bytes(expected, "cover.png"); hash_bytes(expected, "42"); hash_bytes(expected, text);
    std::uint64_t actual = 123; hash_path(actual, path);
    assert(actual == expected && lutris_hashes.insert(actual).second);
    expected = 123;
    mix(expected, "cover.png"); mix(expected, separator);
    mix(expected, "42"); mix(expected, separator); mix(expected, text); mix(expected, separator);
    actual = 123; mix_path(actual, path);
    assert(actual == expected && autosync_hashes.insert(actual).second);
    const auto result = fingerprint(path);
    assert(result && result->mtime == text && mtimes.insert(result->mtime).second);
    assert(write_meta(metadata, *result));
    platf::steam::artwork::source_fingerprint_t cached;
    assert(read_meta(metadata, cached) && cached == *result);
  }
  // Existing version-1 numeric sidecars still match ordinary native counts.
  {
    std::ofstream output(metadata);
    output << "version=1\npath=cover.png\nsize=42\nmtime=-1700000000000000000\n";
  }
  testfs::ticks = -1700000000000000000LL;
  platf::steam::artwork::source_fingerprint_t legacy;
  assert(read_meta(metadata, legacy) && legacy == *fingerprint(path));
  testfs::fail = true;
  assert(artwork_revision({path}).empty());
  assert(!fingerprint(path));
  std::uint64_t expected = 123, actual = 123;
  hash_bytes(expected, "cover.png"); hash_bytes(expected, "missing"); hash_bytes(expected, "unstatable");
  hash_path(actual, path); assert(actual == expected);
  expected = actual = 123;
  mix(expected, "cover.png"); mix(expected, separator); mix(expected, "missing"); mix(expected, "unstatable");
  mix_path(actual, path); assert(actual == expected);
  actual = 123; hash_path(actual, {}); expected = 123; hash_bytes(expected, ""); assert(actual == expected);
  actual = 123; mix_path(actual, {}); expected = 123; mix(expected, ""); mix(expected, separator); assert(actual == expected);
}
'''
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / 'file_clock.cpp'
        binary = Path(temp) / 'file_clock'
        source.write_text(code)
        subprocess.run([args.compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(Path(temp) / "artwork.meta")], check=True)
    print('Four production fingerprint functions passed with signed 128-bit clock counts')


if __name__ == '__main__':
    main()
