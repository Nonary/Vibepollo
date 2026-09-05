"""Check the migrated Terminate call against Vibepollo's actual API and lock entry.

This focused Linux test compiles the production declaration, lock-acquisition
prefix, and HTTP call expressions. Cleanup is stubbed after that boundary; it
checks lock transfer, not Windows display/audio teardown. Pass a repository path
and optional nvhttp source override to reproduce the pre-fix deadlock.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
http = pathlib.Path(sys.argv[2]).read_text() if len(sys.argv) > 2 else (root / "src/nvhttp.cpp").read_text()
header = (root / "src/process.h").read_text()
process = (root / "src/process.cpp").read_text()
declaration = re.search(r"void terminate\([\s\S]*?\);", header).group()
entry = process[process.index("void proc_t::terminate("):]
entry = entry[:entry.index("// Mark termination before process teardown")]


def call_after(marker):
    return re.search(r"proc::proc\.terminate\([^;]*;", http[http.index(marker):]).group()


calls = [
    call_after("if (decision.terminate)"),
    call_after("// Preserve Vibepollo's legacy Terminate control"),
    call_after("if (appid > 0) proc::proc.terminate"),
]
with tempfile.TemporaryDirectory(prefix="terminate-lock-test-") as directory:
    path = pathlib.Path(directory)
    source = r'''
#include <cassert>
#include <mutex>
namespace nvhttp {
  std::mutex gate;
  std::mutex &stream_lifecycle_mutex() { return gate; }
}
namespace proc {
  int cleanup_calls = 0;
  struct proc_t {
''' + declaration + r'''
  };
''' + entry + r'''
    assert(!immediate && needs_refresh && !skip_display_revert);
    ++cleanup_calls;
  }
  proc_t proc;
}
int main() {
''' + "\n".join(r'''
  {
    std::unique_lock lock(nvhttp::stream_lifecycle_mutex());
''' + call + r'''
    assert(lock.owns_lock());
  }
''' for call in calls) + r'''
  proc::proc.terminate();
  assert(proc::cleanup_calls == 4);
}
'''
    (path / "test.cpp").write_text(source)
    executable = path / "test"
    subprocess.run(["g++", "-std=c++17", "-pthread", str(path / "test.cpp"), "-o", str(executable)], check=True)
    try:
        subprocess.run([str(executable)], timeout=3, check=True)
    except subprocess.TimeoutExpired:
        sys.exit("FAIL: Terminate reacquired the lifecycle mutex already held by /launch")
print("PASS: synthetic, legacy, and rejected-launch termination transfer the held lock; ordinary termination acquires it")
