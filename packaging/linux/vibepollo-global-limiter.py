#!/usr/bin/python3
"""Session-owned Proton launch policy. Runs only as the selected desktop user."""
import errno
import json
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import sys
import tempfile
import time

MODULE = "_vibepollo_frame_limiter.py"
BLOCK = b'''\n# BEGIN Vibepollo stream limiter\ntry:\n    from _vibepollo_frame_limiter import apply as _vibepollo_apply_limiter\n    _vibepollo_apply_limiter(globals())\nexcept Exception:\n    pass  # A missing host must never prevent a game from starting.\n# END Vibepollo stream limiter\n'''
SHIM = "/usr/$LIB/mangohud/libMangoHud_shim.so"


def address():
    return "\0vibepollo.proton-limiter.v1." + str(os.getuid())


def environment(policy, inherited):
    provider, millihz, preset, graph, method = policy
    if provider not in ("proton", "mangohud-proton", "mangohud"):
        raise ValueError("provider")
    if type(millihz) is not int or not 1000 <= millihz <= 1000000:
        raise ValueError("limit")
    if preset not in ("custom", "1", "2", "3", "4") or type(graph) is not bool:
        raise ValueError("overlay")
    if method not in ("early", "late"):
        raise ValueError("method")
    limit = (str(millihz // 1000) + "." + str(millihz % 1000).zfill(3)).rstrip("0").rstrip(".")
    result = {}
    overlay = provider != "proton"
    if provider != "mangohud":
        rounded = str((millihz + 500) // 1000)
        # Older DXVK and GE's protonfixes prefer DXVK_FRAME_RATE to DXVK_CONFIG.
        result["DXVK_FRAME_RATE"] = rounded
        result["VKD3D_FRAME_RATE"] = limit
        config = inherited.get("DXVK_CONFIG", "")
        result["DXVK_CONFIG"] = (config + "; " if config else "") + "; ".join(
            key + " = " + rounded for key in ("dxvk.maxFrameRate", "dxgi.maxFrameRate", "d3d9.maxFrameRate")
        )
    preload = [p for p in inherited.get("LD_PRELOAD", "").split(":") if p and p != SHIM]
    if overlay:
        config = "read_cfg"
        if inherited.get("MANGOHUD_CONFIG"):
            config += "," + inherited["MANGOHUD_CONFIG"]
        if preset != "custom":
            config += ",preset=" + preset
        if provider == "mangohud-proton" or preset != "custom" or graph:
            config += ",no_display=0"
        if graph:
            config += ",frame_timing=1"
        config += ",fps_limit_method=" + method
        config += ",fps_limit=" + (limit if provider == "mangohud" else "0")
        result.update(MANGOHUD="1", MANGOHUD_CONFIG=config)
        preload.append(SHIM)
    else:
        result.update(MANGOHUD=None, MANGOHUD_CONFIG=None)
    result["MANGOHUD_FPS_LIMIT"] = limit if provider == "mangohud" and millihz % 1000 == 0 else None
    result["LD_PRELOAD"] = ":".join(preload) or None
    return result


def apply(namespace):
    """Called from Proton's supported user_settings.py entry point."""
    try:
        session = getattr(sys.modules.get("__main__"), "g_session", None)
        inherited = getattr(session, "env", None)
        launch_env = inherited if isinstance(inherited, dict) else os.environ
        if launch_env.get("VIBEPOLLO_LIMITER_MANAGED") == "1":
            return
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(0.2)
            client.connect(address())
            _, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            if uid != os.getuid():
                return
            data = bytearray()
            while len(data) <= 1024:
                chunk = client.recv(1025 - len(data))
                if not chunk:
                    break
                data.extend(chunk)
            if len(data) > 1024:
                return
        settings = namespace.setdefault("user_settings", {})
        if not isinstance(settings, dict):
            return
        merged = dict(settings)
        merged.update(inherited if isinstance(inherited, dict) else os.environ)
        overrides = environment(json.loads(data), merged)
        for key, value in overrides.items():
            if value is None:
                settings.pop(key, None)
            else:
                settings[key] = value
        # Proton copies os.environ before importing user_settings, and only
        # imports missing keys afterward. Update that copy to defeat stale
        # launcher limits without modifying any unrelated environment setting.
        if isinstance(inherited, dict):
            for key, value in overrides.items():
                if value is None:
                    inherited.pop(key, None)
                else:
                    inherited[key] = value
    except (OSError, ValueError, TypeError):
        return


def read_owned(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise ValueError("not a private user-owned regular file: " + str(path))
        data = source.read(1024 * 1024 + 1)
        if len(data) > 1024 * 1024:
            raise ValueError("settings too large")
        return data, info


def replace_owned(path, data, previous):
    fd, temporary = tempfile.mkstemp(prefix=".vibepollo-limiter-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            if previous and os.fstat(output.fileno()).st_gid != previous.st_gid:
                os.fchown(output.fileno(), -1, previous.st_gid)
            os.fchmod(output.fileno(), stat.S_IMODE(previous.st_mode) if previous else 0o600)
        if previous:
            current = path.lstat()
            if (current.st_ino, current.st_mtime_ns, current.st_size) != (previous.st_ino, previous.st_mtime_ns, previous.st_size):
                raise ValueError("settings changed during update")
            os.replace(temporary, path)
        else:
            # Never overwrite a concurrently created user_settings.py.
            os.link(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def install(tool, module):
    target = tool / MODULE
    try:
        old, info = read_owned(target)
        if old != module:
            # This name is reserved for our copied module, but do not replace
            # somebody else's file just because its name happens to match.
            if not old.startswith(b'#!/usr/bin/python3\n"""Session-owned Proton launch policy.'):
                raise ValueError("unrecognized limiter module")
            replace_owned(target, module, info)
    except FileNotFoundError:
        replace_owned(target, module, None)
    settings = tool / "user_settings.py"
    try:
        contents, info = read_owned(settings)
    except FileNotFoundError:
        contents, info = b"", None
    if BLOCK in contents:
        return
    if b"# BEGIN Vibepollo stream limiter" in contents:
        raise ValueError("modified limiter hook")
    # Preserve the user's code verbatim, including its encoding declaration.
    compile(contents + BLOCK, str(settings), "exec")
    replace_owned(settings, contents + BLOCK, info)


def tools_in(roots):
    result = set()
    for root in roots:
        root = Path(root)
        candidates = [root]
        for parent in (root / "compatibilitytools.d", root / "steamapps/common"):
            try:
                candidates.extend(parent.iterdir())
            except OSError:
                pass
        for candidate in candidates:
            if (candidate / "proton").is_file() and (candidate / "user_settings.sample.py").is_file():
                result.add(candidate.resolve())
    return result


def serve(policy, roots):
    environment(policy, {})  # Validate before opening the endpoint or editing files.
    payload = json.dumps(policy).encode()
    module = Path(__file__).read_bytes()
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        deadline = time.monotonic() + 5
        while True:
            try:
                server.bind(address())
                break
            except OSError as error:
                if error.errno != errno.EADDRINUSE or time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        server.listen(16)
        server.settimeout(0.25)
        scanned_at = 0
        ready = False
        while not stopping:
            if time.monotonic() - scanned_at >= 30:
                count = 0
                for tool in tools_in(roots):
                    try:
                        install(tool, module)
                        count += 1
                    except (OSError, ValueError, SyntaxError) as error:
                        print("Vibepollo limiter: " + str(error), file=sys.stderr)
                scanned_at = time.monotonic()
                if not ready:
                    if not count:
                        raise RuntimeError("No writable Proton installations support the global limiter hook")
                    print("READY " + str(count), flush=True)
                    ready = True
            try:
                client, _ = server.accept()
            except socket.timeout:
                continue
            with client:
                client.settimeout(0.2)
                _, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid == os.getuid():
                    try:
                        client.sendall(payload)
                    except OSError:
                        pass
    # Hooks remain installed but inert. No persisted FPS settings survive the
    # server, including after SIGKILL, a host crash or a desktop-session switch.


if __name__ == "__main__":
    serve([sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] == "1", sys.argv[5]], sys.argv[6:])
