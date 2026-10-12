"""Exercise the production Task Scheduler probe with deterministic COM results."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = sys.argv[1] if len(sys.argv) > 1 else 'g++'
with tempfile.TemporaryDirectory(prefix='display-restore-task-') as temporary:
    directory = Path(temporary)
    (directory / 'comdef.h').write_text('''#pragma once
struct _bstr_t { explicit _bstr_t(const wchar_t *) {} };
''')
    (directory / 'taskschd.h').write_text('''#pragma once
using HRESULT = int;
using VARIANT_BOOL = short;
constexpr short VARIANT_FALSE = 0;
constexpr short VARIANT_TRUE = -1;
constexpr int ERROR_FILE_NOT_FOUND = 2;
constexpr int ERROR_PATH_NOT_FOUND = 3;
constexpr int HRESULT_FROM_WIN32(int error) { return -error; }
constexpr bool FAILED(int result) { return result < 0; }
struct IRegisteredTask {
  int read_result = 0;
  VARIANT_BOOL enabled = VARIANT_TRUE;
  int releases = 0;
  HRESULT get_Enabled(VARIANT_BOOL *result) { *result = enabled; return read_result; }
  void Release() { ++releases; }
};
struct ITaskFolder {
  HRESULT lookup_result = 0;
  IRegisteredTask *task = nullptr;
  HRESULT GetTask(_bstr_t, IRegisteredTask **result) { *result = task; return lookup_result; }
};
''')
    source = directory / 'test.cpp'
    source.write_text('''#include <cassert>
#include <initializer_list>
#include "src/platform/windows/display_restore_task.h"
int main() {
  using namespace display_helper;
  IRegisteredTask task;
  ITaskFolder folder {0, &task};
  auto probe = [&] { return read_restore_task_state(&folder, L"VibepolloDisplayRestore"); };
  assert(probe() == restore_task_state_e::enabled);
  assert(can_manage_restore_task(probe()));
  // A user-disabled task remains opted out across both registration and cleanup.
  task.enabled = VARIANT_FALSE;
  int registrations = 0, deletions = 0;
  for (int session = 0; session < 2; ++session) {
    if (can_manage_restore_task(probe())) ++registrations;
    if (can_manage_restore_task(probe())) ++deletions;
  }
  assert(registrations == 0 && deletions == 0);
  assert(task.enabled == VARIANT_FALSE);
  task.read_result = -5;
  assert(probe() == restore_task_state_e::unavailable);
  assert(!can_manage_restore_task(probe()));
  assert(task.releases == 8);
  folder.lookup_result = -5;
  folder.task = nullptr;
  assert(probe() == restore_task_state_e::unavailable);
  assert(!can_manage_restore_task(probe()));
  for (const int missing : {ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND}) {
    folder.lookup_result = HRESULT_FROM_WIN32(missing);
    assert(probe() == restore_task_state_e::missing);
    assert(can_manage_restore_task(probe()));
  }
  folder.lookup_result = 0;
  assert(probe() == restore_task_state_e::unavailable);
}
''')
    binary = directory / 'test'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(directory), '-I', str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
