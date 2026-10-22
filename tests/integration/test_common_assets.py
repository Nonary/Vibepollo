"""Execute common.cmake to verify build-tree and installed asset layouts."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise AssertionError(f"Command returned {result.returncode}: {command}\n{result.stdout}")


def write_files(root, files):
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


def assert_payload(root, expected):
    actual = {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}
    missing = sorted(expected.keys() - actual.keys())
    extra = sorted(actual.keys() - expected.keys())
    changed = sorted(name for name in actual.keys() & expected.keys() if actual[name] != expected[name])
    if missing or extra or changed:
        raise AssertionError(f"Unexpected payload in {root}: missing={missing}, extra={extra}, changed={changed}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    repository = args.repository.resolve()

    # Use the real artwork, including the Remote Monitor and Remote Input paths
    # resolved by nvhttp::appasset, rather than merely asserting CMake source text.
    source_artwork = repository / "src_assets/common/assets/remote-session"
    static_assets = {
        f"remote-session/{name}": (source_artwork / name).read_bytes()
        for name in ("remote-monitor.png", "remote-input.png", "disconnect-remote-monitor.png")
    }
    static_assets.update({
        "desktop.png": b"root asset",
        "shared.png": b"root basename",
        "remote-session/nested/shared.png": b"first nested basename",
        "other/shared.png": b"second nested basename",
        "directory with spaces/deeper/icon with spaces.png": b"deeply nested asset",
        "web-extra/keep.txt": b"not a browser source directory",
    })
    browser_sources = {
        "web/index.html": b"unbuilt v2 source",
        "web/src/main.js": b"v2 source code",
        "web-legacy/index.html": b"unbuilt legacy source",
        "web-legacy/src/main.js": b"legacy source code",
    }
    built_web = {
        "web/index.html": b"built legacy page",
        "web/assets/legacy.js": b"built legacy script",
        "web/v2/index.html": b"built v2 page",
        "web/v2/assets/v2.js": b"built v2 script",
    }

    # Spaces in the source, build and destination paths exercise CMake quoting.
    with tempfile.TemporaryDirectory(prefix="common assets ") as temporary:
        fixture = Path(temporary)
        source = fixture / "source tree"
        build = fixture / "build tree"
        install = fixture / "install tree"
        write_files(source / "src_assets/common/assets", static_assets | browser_sources)
        (source / "LICENSE").write_text("Fixture license\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(CommonAssetsFixture VERSION 1.0.0 LANGUAGES NONE)
set(PROJECT_VERSION_NUMERIC "${{PROJECT_VERSION}}")
set(SUNSHINE_SOURCE_ASSETS_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}/src_assets")
set(SUNSHINE_ASSETS_DIR "share/vibeshine/assets")
# Isolate common packaging without loading platform SDKs or target rules.
set(WIN32 FALSE)
set(UNIX FALSE)
set(APPLE FALSE)
include("{repository.as_posix()}/cmake/packaging/common.cmake")
""", encoding="utf-8")

        configure = [args.cmake, "-S", str(source), "-B", str(build)]
        run(configure)
        assert_payload(build / "assets", static_assets)
        print("PASS: runtime icon paths, nested directories, duplicate basenames and browser-source exclusion")

        # The web target owns these paths. Reconfiguring common packaging must
        # neither overwrite its output with source files nor remove it.
        write_files(build / "assets", built_web)
        run(configure)
        expected = static_assets | built_web
        assert_payload(build / "assets", expected)
        print("PASS: reconfigure preserves generated browser output")

        # Exercise the real install(DIRECTORY) rules and compare every file and
        # byte, including generated web output, to the unchanged install layout.
        run([args.cmake, "--install", str(build), "--prefix", str(install)])
        assert_payload(install / "share/vibeshine/assets", expected)
        print("PASS: installed payload retains directory layout and excludes browser sources")


if __name__ == "__main__":
    main()
