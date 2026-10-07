#!/usr/bin/env python3
"""Exercise confined legacy source discovery as the ordinary test user."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory() as temporary:
    base = Path(temporary)
    harness = base / 'test.c'
    harness.write_text('#define main importer_main\n#include ' +
                       json.dumps(str(ROOT / 'packaging/linux/vibepollo-profile-import.c')) +
                       '\n#undef main\n' + r'''
int main(int argc, char **argv) {
  int home = open(argv[1], O_RDONLY | O_DIRECTORY);
  int source = open_source_profile(home, "auto");
  if (source >= 0) { close(source); close(home); return 0; }
  int error = errno;
  close(home);
  return error == ENOENT ? 10 : error == EEXIST ? 11 : 12;
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Wno-unused-parameter', str(harness),
                    '-lcap', '-o', str(base / 'test')], check=True)
    home = base / 'home'
    home.mkdir()
    (home / '.config').mkdir()
    def check(expected):
        assert subprocess.run([str(base / 'test'), str(home)]).returncode == expected
    check(10)
    (home / '.config/sunshine').mkdir()
    check(0)
    (home / '.config/vibeshine').mkdir()
    check(11)
    (home / '.config/sunshine').rmdir()
    check(0)
    (home / '.config/sunshine').symlink_to('vibeshine', target_is_directory=True)
    check(12)
print('PASS: fresh, Sunshine, Vibeshine, ambiguous and symlink profile sources')
