"""Private syscall faults shared by identity and bootstrap regression fixtures."""


def prelude(package_path):
    return r'''
import errno
import os
from pathlib import Path
import sys
case, proc, old_device = sys.argv[2:]
old_device = int(old_device)
real_read = Path.read_text
real_fstat = os.fstat
real_open = os.open
real_link = os.readlink
real_stat = Path.stat
pins = {}
def open_executable(path, flags, *args, **kwargs):
    if str(path).endswith('/exe'):
        if case == 'open-denied':
            raise PermissionError(errno.EACCES, 'fixture ptrace denial', str(path))
        fd = real_open(path, flags, *args, **kwargs)
        pins[fd] = Path(path).parent.name
        if case == 'pid-reuse':
            # Recycle the directory entry after pinning the old host executable.
            entry = Path(proc) / '100'
            entry.unlink()
            entry.mkdir()
            (entry / 'exe').symlink_to('/usr/bin/true')
            print('PINNED_BEFORE_PID_REUSE', file=sys.stderr)
        return fd
    return real_open(path, flags, *args, **kwargs)
def fstat(fd):
    if fd in pins and pins[fd] == '100' and case not in ('normal', 'pid-reuse'):
        if case == 'io-error':
            raise OSError(errno.EIO, 'fixture I/O failure')
        raise PermissionError(errno.EACCES, 'fixture FUSE getattr denial')
    return real_fstat(fd)
def stat(path, *args, **kwargs):
    # Also inject the original path-stat failure for the baseline reproduction.
    if str(path).endswith('/100/exe') and case not in ('normal', 'pid-reuse'):
        raise PermissionError(errno.EACCES, 'fixture FUSE getattr denial', str(path))
    return real_stat(path, *args, **kwargs)
def read_text(path, *args, **kwargs):
    text = real_read(path, *args, **kwargs)
    if str(path).startswith('/proc/self/fdinfo/'):
        if case == 'missing-mount-id':
            return 'flags:\t012000000\n'
        if case == 'duplicate-mount-id':
            return text + next(line for line in text.splitlines() if line.startswith('mnt_id:')) + '\n'
    if str(path) == '/proc/self/mountinfo':
        if case == 'invisible-mount':
            return ''
        device = old_device if case == 'same-device' else os.makedev(0, 999999)
        entries = []
        for line in text.splitlines():
            before, _, after = line.partition(' - ')
            fields, filesystem = before.split(), after.split()
            fields[2] = f'{os.major(device)}:{os.minor(device)}'
            filesystem[0] = 'ext4' if case == 'non-fuse' else 'fuse.Orca.AppImage'
            entries.append(' '.join(fields) + ' - ' + ' '.join(filesystem))
        text = '\n'.join(entries)
        if case == 'duplicate-mount':
            text += '\n' + text
    return text
def readlink(path, *args, **kwargs):
    if str(path).startswith('/proc/self/fd/'):
        if case == 'link-denied':
            raise PermissionError(errno.EACCES, 'fixture link denial')
        if case == 'exact-path':
            return PACKAGE_PATH
        if case == 'deleted-path':
            return PACKAGE_PATH + ' (deleted)'
    return real_link(path, *args, **kwargs)
os.open = open_executable
os.fstat = fstat
os.readlink = readlink
Path.stat = stat
Path.read_text = read_text
'''.replace('PACKAGE_PATH', repr(package_path))
