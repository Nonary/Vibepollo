#!/usr/bin/env python3
"""Compile and exercise the actual repaint gate from a patched Gamescope tree.

Usage: python3 test-gamescope-repaint.py /path/to/patched/gamescope
Only a C++20 compiler is required; no compositor or live session is started.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

source = (pathlib.Path(sys.argv[1]) / 'src/steamcompmgr.cpp').read_text()
start = source.index('\t// Capture includes Steam\'s overlay')
end = source.index('\n\tconst uint32_t uCompositeDebugBackup', start)
gate = source[start:end].replace('\t\treturn;', '\t\treturn false;')
assert source.index('s_LastPipewireState = pipewireState;') > source.index('if ( oPipewireSequence )', end)

harness = r'''
#include <array>
#include <optional>
#include <cstdint>
#include <cassert>
struct Window { uint64_t commit; uint32_t opacity = 255; };
struct Focus { Window *focusWindow, *overrideWindow, *overlayWindow; };
struct Texture {
    uint32_t w = 1920, h = 1080;
    uint32_t width() { return w; }
    uint32_t height() { return h; }
};
struct Buffer { Texture *texture; struct { uint32_t format = 0; } video_info; };
uint64_t window_last_done_commit_id(Window *w) { return w ? w->commit : 0; }
bool capture(Focus *pFocus, Buffer *s_pPipewireBuffer, uint64_t ulFocusAppId,
             bool bHDRCapture, bool renderSucceeded = true) {
''' + gate + r'''
    (void)bHDRCapture;
    if (!renderSucceeded) return false;
    s_LastPipewireState = pipewireState;
    return true;
}
int main() {
    Window game{1}, overlay{2}, replacement{1};
    Focus focus{&game, nullptr, &overlay};
    Texture texture;
    Buffer buffer{&texture, {}};
    auto frame = [&](uint64_t app = 0, bool hdr = false, bool ok = true) {
        return capture(&focus, &buffer, app, hdr, ok);
    };
    assert(frame());
    assert(!frame()); // Unchanged scenes still avoid extra GPU work.
    ++overlay.commit;
    assert(frame()); // Steam menu updates with an idle game.
    overlay.opacity = 0;
    assert(frame()); // Hiding the menu must clear its previous pixels.
    overlay.opacity = 255;
    assert(frame());
    focus.overlayWindow = nullptr;
    assert(frame());
    focus.overlayWindow = &overlay;
    assert(frame());
    focus.focusWindow = &replacement;
    assert(frame()); // Focus change with the same commit value.
    texture.w = 1280;
    assert(frame());
    buffer.video_info.format = 1;
    assert(frame(0, true)); // Format changes also invalidate capture.
    assert(frame(42)); // App-specific streams exclude Steam overlays.
    ++overlay.commit;
    assert(!frame(42));
    ++replacement.commit;
    assert(!frame(42, false, false)); // Failed GPU submission.
    assert(frame(42)); // Retry the same unsubmitted content.
    assert(!frame(42));
}
'''
with tempfile.TemporaryDirectory(prefix='vibeshine-repaint-') as directory:
    root = pathlib.Path(directory)
    (root / 'test.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Gamescope capture repaint regression checks passed')
