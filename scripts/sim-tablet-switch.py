#!/usr/bin/env python3
"""Simulate the keyboard of a 2-in-1 being detached/attached on this Linux machine.

Creates a virtual input device with a SW_TABLET_MODE switch through /dev/uinput. KWin treats it like the
real tablet-mode switch of a convertible: org.kde.KWin.TabletModeManager.tabletModeAvailable becomes true
and tabletMode follows the switch. The Moonlight posture client reads that and reports the new posture to the
Vibepollo host (/posture), which then flips Windows between slate and laptop.

  sim-tablet-switch.py tablet     # keyboard detached / flipped back  -> tabletMode = true
  sim-tablet-switch.py laptop     # keyboard attached / flipped out   -> tabletMode = false
  sim-tablet-switch.py cycle [s]  # tablet, wait s seconds (default 8), laptop, wait, repeat until Ctrl+C

The device lives while this script runs (it keeps the last state; Ctrl+C removes it). Needs write access to
/dev/uinput (run with sudo). No packages beyond the Python standard library.
"""
import fcntl, os, struct, sys, time

EV_SYN, EV_SW = 0x00, 0x05
SW_TABLET_MODE = 0x01
UI_SET_EVBIT = 0x40045564
UI_SET_SWBIT = 0x4004556D
UI_DEV_SETUP = 0x405C5503
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502


def make_device():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SW)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)
    fcntl.ioctl(fd, UI_SET_SWBIT, SW_TABLET_MODE)
    name = b"Simulated tablet mode switch"
    # struct uinput_setup: input_id (bustype, vendor, product, version), name[80], ff_effects_max
    setup = struct.pack("<HHHH80sI", 0x06, 0x1234, 0x5678, 1, name, 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.0)
    return fd


def emit(fd, kind, code, value):
    now = time.time()
    sec, usec = int(now), int((now % 1) * 1e6)
    os.write(fd, struct.pack("llHHi", sec, usec, kind, code, value))


def set_tablet(fd, on):
    emit(fd, EV_SW, SW_TABLET_MODE, 1 if on else 0)
    emit(fd, EV_SYN, 0, 0)
    print("tablet mode ON (keyboard detached)" if on else "tablet mode OFF (keyboard attached)", flush=True)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode not in ("tablet", "laptop", "cycle"):
        print(__doc__)
        return 2
    fd = make_device()
    try:
        if mode == "cycle":
            period = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
            while True:
                set_tablet(fd, True)
                time.sleep(period)
                set_tablet(fd, False)
                time.sleep(period)
        if mode == "laptop":
            # A new switch starts at 0, and the kernel drops an event that does not change the state, so
            # pulse it on first; otherwise a stale "tablet" from an earlier device would stay in KWin.
            emit(fd, EV_SW, SW_TABLET_MODE, 1)
            emit(fd, EV_SYN, 0, 0)
            time.sleep(0.3)
        set_tablet(fd, mode == "tablet")
        print("holding the virtual switch; Ctrl+C to remove it", flush=True)
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
