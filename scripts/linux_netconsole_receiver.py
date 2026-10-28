#!/usr/bin/python3
"""Durable UDP netconsole receiver. Run on a second, awake machine.

The companion user unit should run this through systemd-inhibit to prevent
automatic sleep while collecting an incident. Messages are fsynced per packet;
logs rotate daily and at 16 MiB, retaining 32 files.
"""
import argparse
import datetime
import os
from pathlib import Path
import socket


def inhibit_desktop_sleep():
    # Hold the session-bus connection for the receiver's lifetime. An ssh
    # session may not obtain a system sleep inhibitor, while the desktop
    # exposes its own API for unattended recording applications.
    import dbus
    bus = dbus.SessionBus()
    proxy = bus.get_object('org.freedesktop.PowerManagement',
                           '/org/freedesktop/PowerManagement/Inhibit')
    cookie = dbus.Interface(proxy, 'org.freedesktop.PowerManagement.Inhibit').Inhibit(
        'Vibepollo shutdown diagnostics', 'Preserve host shutdown logs')
    return bus, cookie


def receive(source, root, bind='0.0.0.0', port=6666):
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))
    output = None
    filename = None
    try:
        while True:
            packet, peer = sock.recvfrom(65535)
            if peer[0] != source:
                continue
            now = datetime.datetime.now(datetime.timezone.utc)
            day = now.strftime('%Y%m%d')
            if output is None or day not in filename.name or output.tell() >= 16 * 1024 * 1024:
                if output:
                    output.close()
                filename = root / ('netconsole-' + now.strftime('%Y%m%dT%H%M%S.%fZ') + '.log')
                output = filename.open('ab')
                # Make the new log's directory entry durable too.
                directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
                for old in sorted(root.glob('netconsole-*.log'))[:-32]:
                    old.unlink()
            output.write(now.isoformat().encode() + b' ' + packet.rstrip(b'\n') + b'\n')
            output.flush()
            os.fsync(output.fileno())
    finally:
        if output:
            output.close()
        sock.close()


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True)
    parser.add_argument('--directory', type=Path, default=Path.home() / '.local/state/vibepollo-netconsole')
    parser.add_argument('--bind', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=6666)
    parser.add_argument('--inhibit-desktop-sleep', action='store_true')
    args = parser.parse_args()
    inhibitor = inhibit_desktop_sleep() if args.inhibit_desktop_sleep else None
    receive(args.source, args.directory, args.bind, args.port)
