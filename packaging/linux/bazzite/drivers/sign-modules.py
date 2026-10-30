"""Root helper: receive module bytes on stdin; keep signing material root-only."""
import base64
import hashlib
import json
import os
import re
import stat
import subprocess
import tempfile
from pathlib import Path

NAMES = ("vibeshine_drm.ko", "vibeshine_ds5.ko")
KEYDIR = Path("/var/lib/vibepollo-module-signing")


def run(*args, **kwargs):
    return subprocess.check_output(args, **kwargs)


def trusted(path, private=False):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0:
        raise RuntimeError(f"Unsafe root file: {path}")
    if info.st_mode & (0o077 if private else 0o022):
        raise RuntimeError(f"Unsafe permissions: {path}")


def main():
    if os.geteuid() != 0:
        raise RuntimeError("Root is required")
    os.umask(0o077)
    request = json.loads(__import__('sys').stdin.buffer.read(48 * 1024 * 1024))
    if set(request) != {"kernel", "modules", "require_existing_key"}:
        raise RuntimeError("Unexpected signing request")
    kernel = request["kernel"]
    if not isinstance(kernel, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[A-Za-z0-9._+-]+\.x86_64", kernel):
        raise RuntimeError("Invalid target kernel")
    require_existing = request["require_existing_key"]
    if type(require_existing) is not bool:
        raise RuntimeError("Invalid key-reuse policy")
    request = request["modules"]
    if set(request) != set(NAMES):
        raise RuntimeError("Unexpected module names")
    payload = {name: base64.b64decode(request[name], validate=True) for name in NAMES}
    if any(not data.startswith(b'\x7fELF') or len(data) > 16 * 1024 * 1024
           for data in payload.values()):
        raise RuntimeError("Invalid module input")
    # sign-file is kernel-independent. Execute only the trusted host tool,
    # never an executable taken from a user-controlled target container.
    host_kernel = os.uname().release
    signfile = Path(f"/usr/src/kernels/{host_kernel}/scripts/sign-file").resolve(strict=True)
    trusted(signfile)
    KEYDIR.mkdir(mode=0o700, exist_ok=True)
    info = KEYDIR.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
        raise RuntimeError("Signing directory must be root-owned, mode 0700")
    key, cert = KEYDIR / "mok.key", KEYDIR / "mok.der"
    if key.is_symlink() or cert.is_symlink():
        raise RuntimeError("Refusing symlink signing files")
    if key.exists() != cert.exists():
        raise RuntimeError("Incomplete signing key pair; stopped without replacing it")
    if require_existing and not key.exists():
        raise RuntimeError("Update preparation requires the existing enrolled signing key")
    if not key.exists():
        # Generate in a private directory; no private key enters user storage.
        with tempfile.TemporaryDirectory(dir=KEYDIR) as temporary:
            temporary = Path(temporary)
            run('/usr/bin/openssl', 'req', '-new', '-x509', '-newkey', 'rsa:3072',
                '-nodes', '-days', '3650', '-subj', '/CN=Vibepollo local module signing/',
                '-addext', 'extendedKeyUsage=codeSigning', '-outform', 'DER',
                '-keyout', str(temporary / 'key'), '-out', str(temporary / 'cert'),
                stderr=subprocess.PIPE)
            os.rename(temporary / 'key', key)
            os.rename(temporary / 'cert', cert)
    trusted(key, private=True)
    trusted(cert, private=True)
    public_key = run('/usr/bin/openssl', 'pkey', '-in', str(key), '-pubout')
    certificate_key = run('/usr/bin/openssl', 'x509', '-inform', 'DER',
                          '-in', str(cert), '-pubkey', '-noout')
    if public_key != certificate_key:
        raise RuntimeError("Signing key does not match certificate")
    signed = {}
    with tempfile.TemporaryDirectory(dir=KEYDIR) as temporary:
        for name, data in payload.items():
            path = Path(temporary) / name
            path.write_bytes(data)
            vermagic = run('/usr/sbin/modinfo', '-F', 'vermagic', str(path)).decode().strip()
            if vermagic.split()[0] != kernel:
                raise RuntimeError(f"Wrong kernel: {name}")
            run(str(signfile), 'sha256', str(key), str(cert), str(path))
            signer = run('/usr/sbin/modinfo', '-F', 'signer', str(path)).decode().strip()
            if signer != 'Vibepollo local module signing':
                raise RuntimeError(f"Signature missing: {name}")
            signed[name] = base64.b64encode(path.read_bytes()).decode()
    certificate = cert.read_bytes()
    print(json.dumps({'modules': signed, 'certificate': base64.b64encode(certificate).decode(),
                      'certificate_sha256': hashlib.sha256(certificate).hexdigest()}))


if __name__ == '__main__':
    main()
