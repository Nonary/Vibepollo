"""Compile the HTTP resume helper decisions to protect retained displays."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/nvhttp.cpp').read_text()
source = source[source.index('  void resume(bool &host_audio'):]
apply = re.search(r'const bool should_apply_display_request =\s*(.*?);', source, re.S).group(1)
revert = re.search(r'revert_display_configuration = ([^;]+);', source[source.index('const bool should_apply_display_request'):]).group(1)
with tempfile.TemporaryDirectory(prefix='retained-resume-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text('''
#include <cassert>
#include <initializer_list>
struct session { bool virtual_display_recreated_on_demand; bool virtual_display_needs_resume_apply; bool virtual_display_failed; };
int main() {
  for (bool joining_existing_game_output : {false, true}) {
    for (bool allow_display_changes : {false, true}) {
      const bool allow_session_display_changes = allow_display_changes && !joining_existing_game_output;
      session value {false, false, false};
      auto *launch_session = &value;
      const bool apply = ''' + apply + ''';
      const bool revert = ''' + revert + ''';
      assert(apply == allow_session_display_changes);
      assert(revert == allow_session_display_changes);
    }
  }
}
''')
    subprocess.run(['g++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: retained-output resume preserves helper topology and failure cleanup')
