"""Exercise the production web target and install rules without Node or host SDKs."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


def run(command, *, env=None, success=True, contains=None):
    result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != success:
        raise AssertionError(f"Command returned {result.returncode}: {command}\n{result.stdout}")
    if contains and contains not in result.stdout:
        raise AssertionError(f"Expected {contains!r}:\n{result.stdout}")
    return result.stdout


def change_dependency(path, output_dir):
    newest_output = max((output_dir / entry).stat().st_mtime_ns
                        for entry in ("index.html", "v2/index.html", ".build-stamp"))
    path.write_text(path.read_text() + "// changed\n")
    # A fixed delay can stay within the same two-second filesystem timestamp
    # bucket. Observe a strictly newer dependency instead, without setting a
    # future mtime that could make the subsequent no-op builds rerun npm.
    deadline = time.monotonic() + 10
    while path.stat().st_mtime_ns <= newest_output:
        if time.monotonic() >= deadline:
            raise AssertionError(f"Dependency mtime did not advance beyond output mtime {newest_output}: {path}")
        time.sleep(0.05)
        path.touch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", required=True, choices=("Unix Makefiles", "Ninja"))
    parser.add_argument("--make-program", required=True)
    args = parser.parse_args()
    repository = args.repository.resolve()

    # Spaces also exercise quoting of the output directories passed through -E env.
    with tempfile.TemporaryDirectory(prefix="web build ") as directory:
        fixture = Path(directory)
        source = fixture / "source"
        source.mkdir()
        build = fixture / "build"
        prefix = fixture / "install"
        log = fixture / "npm.jsonl"
        fail = fixture / "fail-v2"
        env = dict(os.environ, WEB_TEST_LOG=str(log), WEB_TEST_FAIL=str(fail))
        assets = source / "assets/common/assets"
        for application in ("web-legacy", "web"):
            app = assets / application
            (app / "src").mkdir(parents=True)
            (app / "package.json").write_text("{}\n")
            (app / "package-lock.json").write_text("{}\n")
            (app / "src/page.js").write_text("// source fixture\n")
            # Generated files must remain excluded even if present at configure time.
            for generated in ("node_modules", "dist", ".vite", ".cache", "coverage"):
                (app / generated).mkdir()
                (app / generated / "generated.js").write_text("// generated fixture\n")

        npm_script = fixture / "fake_npm.py"
        npm_script.write_text('''import json
import os
from pathlib import Path
import shutil
import sys

app = Path.cwd().name
with open(os.environ["WEB_TEST_LOG"], "a") as log:
    log.write(json.dumps([app, sys.argv[1:]]) + "\\n")
if sys.argv[1] == "ci":
    (Path.cwd() / "node_modules").mkdir(exist_ok=True)
elif sys.argv[1:] == ["run", "build"]:
    # Model Vite's emptyOutDir, including legacy's removal of the nested v2 tree.
    key = "SUNSHINE_LEGACY_WEB_OUTPUT_DIR" if app == "web-legacy" else "SUNSHINE_WEB_OUTPUT_DIR"
    output = Path(os.environ[key])
    shutil.rmtree(output, ignore_errors=True)
    if app == "web" and Path(os.environ["WEB_TEST_FAIL"]).exists():
        sys.exit("injected v2 build failure")
    output.mkdir(parents=True)
    (output / "index.html").write_text(app + " entrypoint\\n")
else:
    sys.exit("unexpected npm invocation")
''')
        npm = fixture / "npm"
        npm.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} {shlex.quote(str(npm_script))} \"$@\"\n")
        npm.chmod(0o755)

        verify = source / "verify.cmake"
        verify.write_text('''foreach(entry IN ITEMS index.html v2/index.html .build-stamp)
    if(NOT EXISTS "${WEB_ROOT}/${entry}")
        message(FATAL_ERROR "Native consumer ran before web output: ${entry}")
    endif()
endforeach()
''')
        # Only platform-specific packaging is stubbed. The real shared installer
        # checks, exclusions and asset copying are included unchanged below.
        modules = source / "modules/packaging"
        modules.mkdir(parents=True)
        for platform in ("unix", "linux", "macos"):
            (modules / f"{platform}.cmake").write_text("# No platform SDK needed by this fixture.\n")
        shutil.copyfile(repository / "LICENSE", source / "LICENSE")
        (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(WebBuildFixture VERSION 1.0.0 LANGUAGES NONE)
set(SUNSHINE_SOURCE_ASSETS_DIR "${{CMAKE_SOURCE_DIR}}/assets")
set(SUNSHINE_ASSETS_DIR assets)
set(PROJECT_VERSION_NUMERIC 1.0.0)
set(CMAKE_MODULE_PATH "${{CMAKE_SOURCE_DIR}}/modules")
include("{repository.as_posix()}/cmake/targets/web.cmake")
# Match sunshine's target-level dependency and verify it orders native consumers.
add_custom_target(native_consumer ALL
    COMMAND "${{CMAKE_COMMAND}}" "-DWEB_ROOT=${{CMAKE_BINARY_DIR}}/assets/web"
        -P "${{CMAKE_SOURCE_DIR}}/verify.cmake"
    VERBATIM)
add_dependencies(native_consumer web_ui)
include("{repository.as_posix()}/cmake/packaging/common.cmake")
''')
        run([args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
             f"-DCMAKE_MAKE_PROGRAM={args.make_program}", f"-DSUNSHINE_NPM_EXECUTABLE={npm}",
             f"-DCMAKE_INSTALL_PREFIX={prefix}"], env=env)

        def calls():
            return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

        def build_web(*, success=True):
            run([args.cmake, "--build", str(build), "--parallel", "8"], env=env, success=success,
                contains=None if success else "injected v2 build failure")

        def install(*, success=True, contains=None):
            run([args.cmake, "--install", str(build), "--prefix", str(prefix), "--component", "assets"],
                env=env, success=success, contains=contains)

        expected_cycle = [
            ["web-legacy", ["ci", "--ignore-scripts", "--no-audit", "--no-fund", "--prefer-offline"]],
            ["web-legacy", ["run", "build"]],
            ["web", ["ci", "--ignore-scripts", "--no-audit", "--no-fund", "--prefer-offline"]],
            ["web", ["run", "build"]],
        ]
        cycles = 0

        def expect_cycle():
            nonlocal cycles
            cycles += 1
            assert calls() == expected_cycle * cycles, calls()

        def expect_noop():
            before = calls()
            build_web()
            run([args.cmake, "--build", str(build), "--target", "web_ui", "--parallel", "8"], env=env)
            assert calls() == before, "An unchanged build reran npm"

        install(success=False, contains="legacy Web UI is missing")
        build_web()
        expect_cycle()
        expect_noop()
        root = build / "assets/web"
        install()
        for entry in ("index.html", "v2/index.html"):
            assert (prefix / "assets/web" / entry).read_text() == (root / entry).read_text()

        for entry in ("index.html", "v2/index.html", ".build-stamp"):
            (root / entry).unlink()
            if entry != ".build-stamp":
                install(success=False, contains="Web UI is missing")
            build_web()
            expect_cycle()
            expect_noop()

        for application in ("web-legacy", "web"):
            change_dependency(assets / application / "src/page.js", root)
            build_web()
            expect_cycle()
            expect_noop()
        for application in ("web-legacy", "web"):
            for generated in ("node_modules", "dist", ".vite", ".cache", "coverage"):
                (assets / application / generated / "generated.js").write_text("// updated generated file\n")
        expect_noop()

        # A failed second build must not publish success or reuse old v2 output.
        change_dependency(assets / "web/package-lock.json", root)
        fail.touch()
        build_web(success=False)
        expect_cycle()
        assert not (root / ".build-stamp").exists()
        install(success=False, contains="v2 Web UI is missing")
        fail.unlink()
        build_web()
        expect_cycle()
        expect_noop()

        shutil.rmtree(root)
        build_web()
        expect_cycle()
        expect_noop()
        run([args.cmake, "--build", str(build), "--target", "clean"], env=env)
        assert not any((root / entry).exists() for entry in ("index.html", "v2/index.html", ".build-stamp"))
        # The generated install target must build its ALL/native prerequisites.
        run([args.cmake, "--build", str(build), "--target", "install", "--parallel", "8"], env=env)
        expect_cycle()
        expect_noop()
        install()
        assert not (prefix / "assets/web/package.json").exists()
        assert not (prefix / "assets/web-legacy").exists()
        print(f"PASS: {args.generator}: parallel build, incremental no-ops, output/source recovery, "
              "ignored generated files, failure/retry, clean and production install rules")


if __name__ == "__main__":
    main()
