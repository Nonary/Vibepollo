#!/usr/bin/env python3
"""Compile the real host adapter and virtual SC2026 encoder together.

Requires the libvirtualgamepad submodule; Linux needs no Windows SDK or device.
The boundary shims define Windows types/IOCTL constants only. No encoder is faked.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'third-party/libvirtualgamepad'
TEST = r'''
#include <cassert>
#include <iostream>
#include "src/platform/windows/vhf_gamepad_policy.h"
#include "steam_controller.h"
using namespace platf::vhf_gamepad;
int main() {
    constexpr std::uint32_t flags[] = {lvg::left_pad_click, lvg::right_pad_click,
                                       lvg::left_trigger_click, lvg::right_trigger_click};
    constexpr std::uint32_t physical[] = {lvg::sc26_usb::btn_left_pad_click, lvg::sc26_usb::btn_right_pad_click,
                                          lvg::sc26_usb::btn_left_trigger_click, lvg::sc26_usb::btn_right_trigger_click};
    constexpr auto clicks = physical[0] | physical[1] | physical[2] | physical[3];
    for (unsigned combination = 0; combination < 16; ++combination) {
        normalized_state_t state {};
        state.left_trigger = 255; state.right_trigger = 0; state.left_x = 20000;
        std::uint32_t expected = 0;
        for (unsigned bit = 0; bit < 4; ++bit) if (combination & (1u << bit)) {
            state.button_flags |= flags[bit]; expected |= physical[bit];
        }
        if (combination & 3) state.button_flags |= lvg::touchpad;
        lvg::driver::sc26_state device {}; device.reset();
        device.device.pad_touched[0] = device.device.pad_touched[1] = true;
        const auto request = make_input_state(3, state, lvg::profile::steam_controller, 0x1000);
        assert(request.controller_id == 3 && request.left_trigger == 255);
        const auto report = lvg::driver::encode_sc26_input(request, &device);
        assert((report.buttons & clicks) == expected);
        assert(!(report.buttons & lvg::sc26_usb::btn_left_stick_touch));
        state.button_flags = 0;
        const auto released = lvg::driver::encode_sc26_input(
            make_input_state(3, state, lvg::profile::steam_controller, 0x1000), &device);
        assert(!(released.buttons & clicks));
    }
    normalized_state_t untrusted {};
    untrusted.button_flags = lvg::sc26_buttons_explicit;
    assert(make_input_state(0, untrusted).buttons == 0);
    assert(make_input_state(0, untrusted, lvg::profile::steam_controller, 0).buttons == 0);
    assert(make_input_state(0, untrusted, lvg::profile::dualsense, 0x1000).buttons == 0);
    lvg::driver::sc26_state legacy {}; legacy.reset();
    untrusted.button_flags = lvg::touchpad; untrusted.left_trigger = 255;
    auto old = lvg::driver::encode_sc26_input(make_input_state(0, untrusted), &legacy);
    assert((old.buttons & clicks) == (physical[1] | physical[2]));
    std::cout << "PASS all 16 SC2026 physical click combinations and releases through host and driver\n"
              << "PASS capability/profile gating, injected metadata rejection and legacy fallbacks\n";
}
'''
with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    (work/'windows.h').write_text('#pragma once\n#include <cstdint>\nusing DWORD=std::uint32_t;\nstruct GUID {std::uint32_t Data1; std::uint16_t Data2,Data3; std::uint8_t Data4[8];};\n')
    (work/'winioctl.h').write_text('#pragma once\n#define FILE_DEVICE_UNKNOWN 0x22\n#define METHOD_BUFFERED 0\n#define FILE_READ_DATA 1\n#define FILE_WRITE_DATA 2\n#define CTL_CODE(d,f,m,a) (((d)<<16)|((a)<<14)|((f)<<2)|(m))\n')
    # Limelight.h is the real pinned header; only its C declarations are used.
    (work/'test.cpp').write_text(TEST)
    subprocess.run(['g++','-std=c++20','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-ffunction-sections','-fdata-sections',f'-I{work}',f'-I{ROOT}',f'-I{ROOT/"third-party"}',
                    f'-I{DRIVER/"include"}',f'-I{DRIVER/"driver/src"}',str(work/'test.cpp'),
                    str(ROOT/'src/platform/windows/vhf_gamepad_policy.cpp'),str(DRIVER/'driver/src/steam_controller.cpp'),
                    '-Wl,--gc-sections','-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
