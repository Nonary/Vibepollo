"""Exercise the production unprivileged socket credential probe, no desktop socket."""
import os
import pathlib
import shlex
import socket
import subprocess
import sys
import tempfile
import threading

root = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(path / 'wayland-1'))
    server.listen(4)
    server.settimeout(0.1)
    stopped = threading.Event()

    def accept():
        while not stopped.is_set():
            try:
                connection, _ = server.accept()
                connection.close()
            except socket.timeout:
                pass

    worker = threading.Thread(target=accept)
    worker.start()
    cgroup = next(line[3:] for line in pathlib.Path('/proc/self/cgroup').read_text().splitlines() if line.startswith('0::'))
    start = pathlib.Path('/proc/self/stat').read_text().rsplit(') ', 1)[1].split()[19]
    script = f'''
set -euo pipefail
source {shlex.quote(str(root / 'packaging/linux/vibepollo-session-controller'))}
candidate_runtime={shlex.quote(directory)}
candidate_display=wayland-1
candidate_uid={os.getuid()}
candidate_gid={os.getgid()}
# Identity setup is the inert boundary; run the exact production env/python
# probe under this existing unprivileged test identity.
session_command_bounded() {{ shift 3; "$@"; }}
[[ $(hyprland_socket_peer_identity {shlex.quote(cgroup)}) == {os.getpid()}-{start} ]]
if hyprland_socket_peer_identity {shlex.quote(cgroup + '-different-unit')}; then exit 2; else [[ $? == 3 ]]; fi
candidate_display=wayland-absent
if hyprland_socket_peer_identity {shlex.quote(cgroup)}; then exit 3; else [[ $? == 1 ]]; fi
'''
    try:
        subprocess.run(['bash', '-c', script], check=True)
    finally:
        stopped.set()
        worker.join()
        server.close()
print('PASS: production Wayland peer check binds peer PID/start time to the selected cgroup and rejects another unit or missing socket')
