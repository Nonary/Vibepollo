#!/usr/bin/python3
"""Local GPU incident recorder. Deliberately never opens a DRM/NVIDIA device.

Installed on the development host as /usr/local/libexec/gpu-incident-recorder
and run by gpu-incident-recorder.service (`watch`). Install or update with:

    sudo install -o root -g root -m 0755 scripts/linux_gpu_incident_recorder.py \\
        /usr/local/libexec/gpu-incident-recorder
    sudo systemctl restart gpu-incident-recorder.service

A GPU hang and an application's own GPU fault are recorded separately. The
earlier recorder kept one NVIDIA slot per boot, so a game's Xid 13 consumed it
and the later whole-GPU hang in that boot produced no bundle. It also missed
the RC watchdog line, which is the first sign of most hangs.
"""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import threading

ROOT = Path('/var/log/gpu-incidents')
XID = re.compile(r'NVRM: Xid \([^)]*\): (\d+)')
# Firmware or engine failures after which the GPU (and every client waiting on
# the RM lock) is presumed stuck. Anything else with an Xid is an application's
# own fault that RM recovers from by killing that channel.
HANG_XIDS = {8, 38, 48, 62, 79, 109, 119, 120, 140, 154, 175}
HANG_MESSAGE = re.compile(r'RC watchdog: GPU is probably locked|GSP task watchdog timeout|'
                          r'GSP task exception|GSP-RM unresponsive|GSP-RM heartbeat timed out')
FAULT = re.compile(r'NVRM: Xid|' + HANG_MESSAGE.pattern + r'|INFO: task .*blocked for more than|'
                   r'BUG: soft lockup|watchdog:.*hard LOCKUP|rcu:.*stall|kernel BUG at|Oops:')
# Long enough for the RC watchdog, GSP timeouts and hung-task reports that
# follow a hang to reach the journal.
FOLLOWUP_SECONDS = 150
UNITS = ['vibeshine.service', 'vibepollo.service',
         'vibeshine-session-controller.service', 'vibepollo-session-controller.service',
         'vibeshine-vkms.service', 'plymouth-reboot.service']


def durable(path, text):
    with path.open('w') as output:
        output.write(text)
        output.flush()
        os.fsync(output.fileno())


def read(path):
    try:
        return Path(path).read_text()[:262144]
    except OSError as error:
        return str(error)


def command(path, args):
    # Each command has a time and output limit, and never queries the GPU.
    def limit():
        resource.setrlimit(resource.RLIMIT_FSIZE, (16 * 1024 * 1024,) * 2)
    with path.open('w') as output:
        try:
            result = subprocess.run(args, stdout=output, stderr=subprocess.STDOUT,
                                    timeout=12, preexec_fn=limit)
            if output.tell() < 16 * 1024 * 1024 - 1024:
                output.write(f'\n[exit={result.returncode}]\n')
        except (OSError, subprocess.TimeoutExpired) as error:
            try:
                output.write(f'\n[capture error: {error}]\n')
            except OSError:
                pass
        output.flush()
        os.fsync(output.fileno())


def classify(message):
    if 'INFO: task' in message:
        return 'blocked-task'
    xid = XID.search(message)
    if xid:
        return 'gpu-hang' if int(xid.group(1)) in HANG_XIDS else 'gpu-app-fault'
    if HANG_MESSAGE.search(message):
        return 'gpu-hang'
    return 'kernel-fault'


def gpu_clients():
    """List processes holding NVIDIA or DRM nodes, from /proc fd links only.

    The process that was exiting just before a hang is the most useful lead,
    and nvidia-smi cannot be used once the GPU is stuck.
    """
    clock = os.sysconf('SC_CLK_TCK')
    try:
        uptime = float(read('/proc/uptime').split()[0])
    except (ValueError, IndexError):
        uptime = 0.0
    rows = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        devices = set()
        try:
            for fd in (proc / 'fd').iterdir():
                try:
                    target = os.readlink(fd)
                except OSError:
                    continue
                if target.startswith(('/dev/nvidia', '/dev/dri/')):
                    devices.add(target)
        except OSError:
            continue
        if not devices:
            continue
        stat = read(proc / 'stat')
        try:
            age = f'{uptime - int(stat.rsplit(")", 1)[1].split()[19]) / clock:.1f}s'
        except (IndexError, ValueError):
            age = '?'
        command_line = read(proc / 'cmdline').replace('\0', ' ').strip()[:400]
        rows.append(f'{proc.name}\tage={age}\t{read(proc / "comm").strip()}\t'
                    f'{",".join(sorted(devices))}\t{command_line}')
    return 'pid\tage\tcomm\tdevices\tcmdline\n' + '\n'.join(sorted(rows, key=lambda row: int(row.split('\t')[0]))) + '\n'


def snapshot(reason, trigger=''):
    ROOT.mkdir(mode=0o700, exist_ok=True)
    with (ROOT / '.capture.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        # Bounded retention: 20 captures, each file capped at 16 MiB.
        previous = sorted(p for p in ROOT.glob('incident-*') if p.is_dir() and not p.is_symlink())
        for old in previous[:-19]:
            shutil.rmtree(old)
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        dest = ROOT / f'incident-{stamp}-{reason}'
        dest.mkdir(mode=0o700)
        durable(dest / 'trigger.txt', trigger + '\n')
        meta = {'reason': reason, 'utc': stamp, 'uname': list(os.uname()),
                'boot_id': read('/proc/sys/kernel/random/boot_id').strip(),
                'uptime': read('/proc/uptime'), 'cmdline': read('/proc/cmdline')}
        for module in ['nvidia', 'nvidia_drm', 'nvidia_modeset', 'vibeshine_drm']:
            meta[module] = {field: read(f'/sys/module/{module}/{field}').strip()
                            for field in ['version', 'srcversion', 'taint']}
        durable(dest / 'metadata.json', json.dumps(meta, indent=2) + '\n')
        # Persist the directory entries as well as the initial evidence.
        for directory in [dest, ROOT]:
            fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            os.fsync(fd)
            os.close(fd)
        durable(dest / 'gpu-clients.txt', gpu_clients())
        command(dest / 'threads.txt', ['ps', '-eLo', 'pid,tid,ppid,stat,wchan:40,comm'])
        stacks = []
        for row in read(dest / 'threads.txt').splitlines()[1:]:
            columns = row.split()
            if len(columns) >= 6 and columns[0].isdigit() and columns[1].isdigit():
                pid, tid, _, state = columns[:4]
                if 'D' in state or any(x in row for x in ['vibeshine', 'vibepollo', 'kwin', 'plymouth']):
                    stacks.append(row + '\n' + read(f'/proc/{pid}/task/{tid}/stack'))
                    if len(stacks) >= 256:
                        break
        durable(dest / 'thread-stacks.txt', '\n\n'.join(stacks))
        command(dest / 'kernel.log', ['journalctl', '-b', '-k', '-n', '20000', '--no-pager', '-o', 'short-precise'])
        command(dest / 'journal.log', ['journalctl', '-b', '--since=-15min', '-n', '20000', '--no-pager', '-o', 'short-precise'])
        command(dest / 'services.txt', ['systemctl', 'show', *UNITS, '-p', 'Id', '-p', 'ActiveState', '-p', 'SubState', '-p', 'MainPID', '-p', 'ExecStart', '-p', 'Result'])
        for module in ['nvidia', 'vibeshine_drm']:
            command(dest / f'installed-{module}.txt', ['modinfo', module])
        durable(dest / 'COMPLETE', datetime.datetime.now(datetime.timezone.utc).isoformat() + '\n')
        print(f'GPU incident snapshot saved: {dest}', flush=True)


def watch():
    # Open the follower first, so faults during the initial snapshot stay queued.
    follower = subprocess.Popen(['journalctl', '-k', '-b', '-f', '-n', '0', '-o', 'json', '--no-pager'],
                                stdout=subprocess.PIPE, text=True)
    snapshot('recorder-start')
    boot = read('/proc/sys/kernel/random/boot_id').strip()
    state_path = ROOT / '.captured.json'
    try:
        state = json.loads(state_path.read_text())
    except (OSError, json.JSONDecodeError):
        state = {}
    captured = set(state.get('captured', [])) if state.get('boot') == boot else set()
    try:
        for line in follower.stdout:
            try:
                message = json.loads(line).get('MESSAGE', '')
            except json.JSONDecodeError:
                continue
            if not isinstance(message, str):
                continue
            if message.startswith('sysrq: Show Blocked State'):
                snapshot('sysrq-dump', line)
            elif FAULT.search(message):
                kind = classify(message)
                # Preserve the first evidence of each class per boot. A fault
                # storm must not rotate away its own first evidence, and an
                # application fault must not hide a later GPU hang.
                if kind in captured:
                    continue
                snapshot(kind, line)
                captured.add(kind)
                durable(state_path, json.dumps({'boot': boot, 'captured': sorted(captured)}) + '\n')
                if kind == 'gpu-hang':
                    followup = threading.Timer(FOLLOWUP_SECONDS, snapshot, ('gpu-hang-followup', line))
                    followup.daemon = True
                    followup.start()
        raise RuntimeError('kernel journal follower exited')
    finally:
        follower.terminate()
        try:
            follower.wait(timeout=3)
        except subprocess.TimeoutExpired:
            follower.kill()


if __name__ == '__main__':
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['watch', 'snapshot'])
    arguments = parser.parse_args()
    if arguments.mode == 'watch':
        watch()
    else:
        snapshot('manual', 'Explicit manual snapshot; no failure implied.')
