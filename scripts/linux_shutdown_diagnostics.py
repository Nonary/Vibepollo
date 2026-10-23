#!/usr/bin/python3
"""Host-local netconsole setup; arm PID 1 logging before shutdown starts.

Install as /usr/local/libexec/gpu-shutdown-diagnostics, with the companion
gpu-shutdown-diagnostics.service and NETCONSOLE_TARGET in its EnvironmentFile.
Netconsole remains in the kernel after userspace and writable disks go away.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess

TARGET = Path('/sys/kernel/config/netconsole/gpu-shutdown')
# Last receiver address that worked; used when no configured name resolves.
LAST_ADDRESS = Path('/var/lib/gpu-shutdown-diagnostics/receiver-address')


def command(*args):
    return subprocess.check_output(args, text=True, timeout=5).strip()


def resolve(name):
    # getaddrinfo has no timeout; an unanswered mDNS lookup can take ~8 s.
    try:
        output = subprocess.run(['getent', 'ahostsv4', name], capture_output=True,
                                text=True, timeout=4).stdout.split()
    except subprocess.TimeoutExpired:
        return None
    return output[0] if output else None


def receiver_address():
    # NETCONSOLE_TARGET may list several names: mDNS renames a host on a name
    # conflict (steamdeck.local became steamdeck10.local on 2026-10-17).
    names = os.environ['NETCONSOLE_TARGET'].split()
    for name in names:
        address = resolve(name)
        if address:
            return name, address
    try:
        return 'last working address', LAST_ADDRESS.read_text().strip()
    except OSError:
        raise RuntimeError('none of ' + ', '.join(names) + ' resolved') from None


def kernel_message(message):
    with open('/dev/kmsg', 'w') as output:
        output.write('<5>gpu-shutdown-diagnostics: ' + message + '\n')


def configure_netconsole():
    name, address = receiver_address()
    ipaddress.IPv4Address(address)
    route = json.loads(command('ip', '-j', '-4', 'route', 'get', address))[0]
    interface, source = route['dev'], route['prefsrc']
    neighbor_address = route.get('gateway', address)
    # Resolve the next hop while networking is still running. Netconsole does
    # not perform ARP and must keep an explicit destination MAC at shutdown.
    subprocess.run(['ping', '-c', '1', '-W', '1', neighbor_address],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
    neighbors = json.loads(command('ip', '-j', 'neigh', 'show', 'to', neighbor_address, 'dev', interface))
    mac = next((row['lladdr'] for row in neighbors if row.get('lladdr')), None)
    if not mac or not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', mac):
        raise RuntimeError('No usable next-hop MAC for ' + address)
    command('modprobe', 'netconsole')
    TARGET.mkdir(exist_ok=True)
    if (TARGET / 'enabled').read_text().strip() == '1':
        (TARGET / 'enabled').write_text('0\n')
    for key, value in {
        'dev_name': interface, 'local_ip': source, 'local_port': '6665',
        'remote_ip': address, 'remote_port': '6666', 'remote_mac': mac,
        'extended': '1',
    }.items():
        (TARGET / key).write_text(value + '\n')
    (TARGET / 'enabled').write_text('1\n')
    try:
        LAST_ADDRESS.write_text(address + '\n')
    except OSError:
        pass
    kernel_message(f'netconsole armed {source}/{interface} -> {address}:6666 ({name})')
    print(f'Netconsole armed: {source}/{interface} -> {address}:6666', flush=True)


def arm():
    command('sysctl', '-q', '-w', 'kernel.printk=8 4 1 7')
    command('systemd-analyze', 'log-target', 'kmsg')
    command('systemd-analyze', 'log-level', 'debug')
    kernel_message('shutdown capture armed; PID 1 debug output goes to kmsg/netconsole')
    # This runs before network shutdown. Refresh a DHCP/mDNS target if possible;
    # keep the last working target if name resolution is no longer available.
    try:
        configure_netconsole()
    except Exception as error:
        kernel_message(f'target refresh failed; retaining previous target: {error}')
    # Captures any already-blocked tasks without crashing or resetting the host.
    Path('/proc/sysrq-trigger').write_text('w\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['prepare', 'arm'])
    args = parser.parse_args()
    if args.mode == 'prepare':
        # Even if the receiver is currently offline, retain ExecStop so local
        # late-shutdown logging is armed and resolution is retried at shutdown.
        try:
            configure_netconsole()
        except Exception as error:
            print(f'WARNING: netconsole not configured: {error}', flush=True)
    else:
        arm()
