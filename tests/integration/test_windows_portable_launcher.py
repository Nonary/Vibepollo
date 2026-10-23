"""Execute the production launcher with an SDK-free PE child, on Windows or Wine."""
import argparse
import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path


CHILD = r'''
__declspec(dllimport) unsigned long __stdcall GetCurrentDirectoryA(unsigned long, char *);
__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char *, char *, unsigned long);
__declspec(dllimport) void *__stdcall CreateFileA(const char *, unsigned long, unsigned long, void *, unsigned long, unsigned long, void *);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);
static unsigned long length(const char *text) { unsigned long size = 0; while (text[size]) ++size; return size; }
static void line(void *file, const char *text) { unsigned long count; WriteFile(file, text, length(text), &count, 0); WriteFile(file, "\n", 1, &count, 0); }
void entry(void) {
    char cwd[32768], path[32768], code[16];
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    GetEnvironmentVariableA("PATH", path, sizeof(path));
    GetEnvironmentVariableA("PORTABLE_TEST_EXIT", code, sizeof(code));
    void *file = CreateFileA("launcher-observed.txt", 0x40000000, 0, 0, 2, 0x80, 0);
    if (file == (void *)-1) ExitProcess(98);
    line(file, cwd); line(file, GetCommandLineA()); line(file, path); CloseHandle(file);
    ExitProcess((unsigned int)(code[0] - '0'));
}
'''


def run(command, **kwargs):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=60, **kwargs)
    if result.returncode:
        raise AssertionError(f"{command}: {result.returncode}\n{result.stdout}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--link", required=True)
    parser.add_argument("--kernel32", required=True, help="Windows SDK or Wine kernel32 import library")
    parser.add_argument("--wine", help="Explicit Wine executable for a Linux-only behavioral test")
    args = parser.parse_args()
    if os.name != "nt" and not args.wine:
        parser.error("Use Windows or explicitly provide --wine with an isolated WINEPREFIX")
    with tempfile.TemporaryDirectory(prefix="portable launcher ") as temporary:
        fixture = Path(temporary)
        package = fixture / "package !literal! & spaces"
        package.mkdir()
        source = fixture / "child.c"
        source.write_text(CHILD)
        obj = fixture / "child.obj"
        run([args.clang, "--target=x86_64-pc-windows-msvc", "-c", str(source), "-o", str(obj)])
        run([args.link, "/entry:entry", "/subsystem:console", "/nodefaultlib", "/machine:x64",
             f"/out:{fixture / 'child.exe'}", str(obj), args.kernel32])
        shutil.copyfile(fixture / "child.exe", package / "sunshine.exe")
        portable = args.repository.resolve() / "packaging/windows/portable"
        shutil.copyfile(portable / "start-portable.cmd", package / "start-portable.cmd")
        config = package / "config"
        config.mkdir()
        shutil.copyfile(portable / "sunshine.conf", config / "sunshine.conf")
        (config / "private-key").write_text("existing private fixture")
        originals = {p.name: p.read_bytes() for p in config.iterdir()}
        observed = package / "launcher-observed.txt"
        env = {**os.environ, "PORTABLE_TEST_EXIT": "0"}
        prefix = [args.wine] if args.wine else []
        def windows_path(path):
            return "Z:" + str(path).replace("/", "\\") if args.wine else str(path)
        (fixture / "external-launch.cmd").write_text('@echo off\n"' + windows_path(package / "start-portable.cmd").replace("%", "%%") + '"\n')
        def launch(*, external=False, code=0):
            env["PORTABLE_TEST_EXIT"] = str(code)
            # An external invocation proves that the launcher changes directory.
            # An inherited /v:on invocation checks literal ! characters in its path.
            command = "external-launch.cmd" if external else "start-portable.cmd"
            result = subprocess.run([*prefix, "cmd.exe", "/d", "/v:off" if external else "/v:on", "/c", command],
                                    cwd=fixture if external else package, env=env, input="\n", text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            if result.returncode != code:
                raise AssertionError(f"Launcher external={external} expected {code}, got {result.returncode}:\n{result.stdout}")
            return result
        for external, code, inherited in ((True, 0, None), (False, 0, None), (False, 7, None),
                                           (True, 7, "0"), (False, 7, "0"), (False, 0, "7")):
            if inherited is None:
                env.pop("ERRORLEVEL", None)
            else:
                env["ERRORLEVEL"] = inherited
            launch(external=external, code=code)
            cwd, command, path = observed.read_text().splitlines()
            expected = windows_path(package)
            assert cwd.lower() == expected.lower(), (cwd, expected)
            assert command.endswith('"config\\sunshine.conf"'), command
            assert path.lower().startswith((expected + "\\;" + expected + "\\tools;").lower()), path
            assert {p.name: p.read_bytes() for p in config.iterdir()} == originals
            observed.unlink()
        print("PASS: actual launcher changes cwd, passes config, finds bundled DLL paths, preserves private state, handles inherited delayed expansion and propagates host exit status despite inherited ERRORLEVEL=0/7")
        for missing in (package / "sunshine.exe", config / "sunshine.conf"):
            saved = missing.read_bytes()
            missing.unlink()
            launch(code=1)
            assert not observed.exists(), "Incomplete package launched the child"
            missing.write_bytes(saved)
        # CMD allows an explicit RANDOM environment variable to override its
        # dynamic value. A directory at that exact leaf makes redirection fail
        # without relying on host ACLs or running the test as a limited user.
        env["RANDOM"] = "write-probe"
        collision = config / ".portable-write-write-probe-write-probe.tmp"
        collision.mkdir()
        denied = launch(code=1)
        assert not observed.exists(), "Failed write probe launched the child: " + denied.stdout + repr(list(config.iterdir()))
        collision.rmdir()
        assert {p.name: p.read_bytes() for p in config.iterdir()} == originals
        if args.wine:
            mode = stat.S_IMODE(config.stat().st_mode)
            config.chmod(0o500)
            try:
                denied = launch(code=1)
                assert not observed.exists(), "Unwritable config launched the child: " + denied.stdout
            finally:
                config.chmod(mode)
            print("PASS: real unwritable config rejects startup before child launch (Wine/POSIX boundary)")
        print("PASS: missing executable/config and colliding write probe reject startup before child launch")


if __name__ == "__main__":
    main()
