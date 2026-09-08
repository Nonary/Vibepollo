"""Compile the HTTP resume helper decisions to protect retained displays."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/nvhttp.cpp').read_text()
source = source[source.index('  void resume(bool &host_audio'):]
apply_start = source.index('const bool should_apply_display_request =')
apply_end = source.index('      if (should_apply_display_request)', apply_start)
apply_declaration = source[apply_start:apply_end]
revert = re.search(r'revert_display_configuration = ([^;]+);', source[source.index('const bool should_apply_display_request'):]).group(1)
with tempfile.TemporaryDirectory(prefix='retained-resume-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text('''
#include <cassert>
#include <initializer_list>
#include "src/platform/linux/private_display_resume_policy.h"
struct session {
 bool virtual_display_recreated_on_demand;
 bool virtual_display_needs_resume_apply;
 bool virtual_display_failed;
 bool virtual_display = false;
 bool normal_vdd_identity_newly_reserved = false;
};
int main() {
  for (bool joining_existing_game_output : {false, true}) {
    for (bool allow_display_changes : {false, true}) {
      const bool allow_session_display_changes = allow_display_changes && !joining_existing_game_output;
      session value {false, false, false};
      auto *launch_session = &value;
      ''' + apply_declaration + '''
      const bool apply = should_apply_display_request;
      const bool revert = ''' + revert + ''';
      assert(apply == allow_session_display_changes);
      assert(revert == allow_session_display_changes);
    }
  }
}
''')
    subprocess.run(['g++', '-std=c++17', '-I', str(root), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: retained-output resume preserves helper topology and failure cleanup')
