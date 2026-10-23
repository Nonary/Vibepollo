"""Exercise the real CPack ZIP hooks using tiny PE fixtures, without a Windows SDK."""
import argparse
import os
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path


def run(command, *, success=True, contains=None, env=None):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    if (result.returncode == 0) != success:
        raise AssertionError(f"Command returned {result.returncode}: {command}\n{result.stdout}")
    if contains and contains not in result.stdout:
        raise AssertionError(f"Expected {contains!r}:\n{result.stdout}")
    return result.stdout


def cmake_path(path):
    return str(path).replace("\\", "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cpack", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--link", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--architecture", choices=("AMD64", "ARM64", "x86"), default="AMD64")
    args = parser.parse_args()
    repository = args.repository.resolve()
    target, machine = {
        "AMD64": ("x86_64-pc-windows-msvc", "x64"),
        "ARM64": ("aarch64-pc-windows-msvc", "arm64"),
        "x86": ("i686-pc-windows-msvc", "x86"),
    }[args.architecture]
    with tempfile.TemporaryDirectory(prefix="portable package ") as temp:
        fixture = Path(temp)
        payload = fixture / "payload"
        payload.mkdir()

        # No SDK/system imports. Model /MD TrueHDR and a transitive VC CRT import.
        redist = fixture / "compiler redist"
        redist.mkdir()
        imports = []
        for name in ("vcruntime140_1", "vcruntime140", "zlib1", "libwebrtc", "vibeshine_truehdr", "nvngx_truehdr"):
            source = fixture / f"{name}.c"
            dependency = "vcruntime140_1" if name == "vcruntime140" else "vcruntime140" if name in ("libwebrtc", "vibeshine_truehdr") else None
            extra_import = f"__declspec(dllimport) int {dependency}_fixture(void);\n" if dependency else ""
            value = f"{dependency}_fixture()" if dependency else "0"
            source.write_text(extra_import + f"__declspec(dllexport) int {name}_fixture(void) {{ return {value}; }}\n")
            obj = fixture / f"{name}.obj"
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            library = fixture / f"{name}.lib"
            run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{payload / (name + '.dll')}", f"/implib:{library}", str(obj),
                 *([str(fixture / (dependency + ".lib"))] if dependency else [])])
            if name in ("zlib1", "libwebrtc"):
                imports.append(library)
        for name in ("vcruntime140", "vcruntime140_1"):
            shutil.move(payload / (name + ".dll"), redist / (name + ".dll"))
        source = fixture / "host.c"
        source.write_text("""__declspec(dllimport) int zlib1_fixture(void);
__declspec(dllimport) int libwebrtc_fixture(void);
volatile int zero_filled_fixture[256];
int main(void) { return zlib1_fixture() + libwebrtc_fixture() + zero_filled_fixture[0]; }
""")
        obj = fixture / "host.obj"
        run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
        run([args.link, "/entry:main", "/subsystem:console", "/nodefaultlib", f"/machine:{machine}",
             f"/out:{payload / 'sunshine.exe'}", str(obj), *map(str, imports)])
        shutil.copyfile(payload / "sunshine.exe", payload / "uninstall.exe")
        tools = payload / "tools"
        tools.mkdir()
        for tool in ("dxgi-info", "audio-info", "playnite-launcher", "sunshine_wgc_capture", "sunshine_display_helper", "vibepollo_dualsense_haptics"):
            shutil.copyfile(payload / "sunshine.exe", tools / f"{tool}.exe")
        shutil.copyfile(payload / "zlib1.dll", tools / "vibepollo_dualsense_audio.dll")
        assets = {
            "assets/apps.json": '{"apps": [{"name": "Desktop"}]}',
            "assets/desktop.png": "fixture image",
            "assets/steam.png": "fixture image",
            "assets/web/index.html": '<script src="assets/legacy.js"></script>',
            "assets/web/assets/legacy.js": "console.log('legacy');",
            "assets/web/v2/index.html": '<script src="assets/v2.js"></script>',
            "assets/web/v2/assets/v2.js": "console.log('v2');",
            "assets/shaders/directx/cursor_vs.hlsl": "fixture shader",
            "plugins/playnite/SunshinePlaynite/extension.yaml": "Id: fixture",
            "plugins/playnite/SunshinePlaynite/VibepolloPlaynite.dll": "fixture managed plugin",
            "plugins/playnite/SunshinePlaynite/icon.png": "fixture icon",
        }
        for relative, content in assets.items():
            path = payload / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

        # Include the production configuration module, not a duplicate test hook.
        (fixture / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.20)
project(PortableFixture LANGUAGES NONE)
set(FIXTURE_PROCESSOR {args.architecture} CACHE STRING "Target architecture")
set(CMAKE_SYSTEM_PROCESSOR "${{FIXTURE_PROCESSOR}}")
set(FIXTURE_VERSION 2.0.1-beta.1 CACHE STRING "Package version")
set(PROJECT_VERSION_FULL "${{FIXTURE_VERSION}}")
set(BUILD_VERSION 2.0.1-beta.1 CACHE STRING "Explicit version input")
set(SUNSHINE_ASSETS_DIR assets CACHE STRING "Runtime assets")
set(CMAKE_OBJDUMP "{cmake_path(args.objdump)}")
set(SUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS "{cmake_path(redist)}" CACHE STRING "Fixture release CRT directory")
set(SUNSHINE_PLAYNITE_PLUGIN_DLL "{cmake_path(payload)}/plugins/playnite/SunshinePlaynite/VibepolloPlaynite.dll")
set(CPACK_GENERATOR WIX)
set(CPACK_PACKAGE_NAME Vibepollo)
set(CPACK_PACKAGE_VERSION 2.0.1)
set(CPACK_PACKAGE_FILE_NAME Vibepollo)
set(CPACK_PACKAGE_DIRECTORY "${{CMAKE_BINARY_DIR}}/packages")
set(CPACK_PROJECT_CONFIG_FILE "{cmake_path(repository)}/packaging/windows/wix/generate_sortable_product_guid.cmake")
set(ZLIB "{cmake_path(payload)}/zlib1.dll")
set(WEBRTC_RUNTIME_DLL "{cmake_path(payload)}/libwebrtc.dll")
set(SUNSHINE_TRUEHDR_RUNTIME_FILES "{cmake_path(payload)}/vibeshine_truehdr.dll;{cmake_path(payload)}/nvngx_truehdr.dll")
foreach(tool IN ITEMS playnite-launcher sunshine_wgc_capture sunshine_display_helper vibepollo_dualsense_haptics)
    add_custom_target(${{tool}})
endforeach()
install(DIRECTORY "{cmake_path(payload)}/" DESTINATION .)
include("{cmake_path(repository)}/cmake/packaging/windows_portable.cmake")
include(CPack)
""")
        build = fixture / "build"
        configure = [args.cmake, "-S", str(fixture), "-B", str(build)]
        run(configure)
        package = [args.cpack, "-G", "ZIP", "--config", str(build / "CPackConfig.cmake")]
        archive = build / f"packages/Vibepollo-Windows-{args.architecture}-2.0.1-beta.1-portable.zip"

        def check_archive():
            with zipfile.ZipFile(archive) as zipped:
                prefix = f"Vibepollo-Windows-{args.architecture}-2.0.1-beta.1-portable/"
                contents = {name.removeprefix(prefix) for name in zipped.namelist()}
                for required in ("sunshine.exe", "zlib1.dll", "libwebrtc.dll", "vcruntime140.dll", "vcruntime140_1.dll",
                                 "vibeshine_truehdr.dll", "nvngx_truehdr.dll",
                                 "tools/sunshine_display_helper.exe", "tools/sunshine_wgc_capture.exe",
                                 "tools/playnite-launcher.exe", "tools/vibepollo_dualsense_haptics.exe",
                                 "tools/vibepollo_dualsense_audio.dll", *assets,
                                 "start-portable.cmd", "README-portable.txt", "config/sunshine.conf"):
                    assert required in contents, required
                assert "uninstall.exe" not in contents, "MSI uninstaller must not ship in a portable ZIP"
                config = zipped.read(prefix + "config/sunshine.conf").decode()
                settings = dict(line.split(" = ", 1) for line in config.splitlines() if " = " in line)
                assert settings == {"virtual_display_mode": "disabled", "dd_configuration_option": "disabled",
                                    "install_steam_audio_drivers": "false"}
                assert not any("credentials" in name or "private-key" in name for name in contents)

        run(package)
        check_archive()
        print("PASS: /MD TrueHDR and transitive release CRTs are staged from compiler redist into the actual CPack archive")
        # The NVIDIA driver supplies nvcuda.dll on GPU hosts. It is absent on
        # CPU-only builders and must not be bundled. A different missing DLL
        # must still fail the same real CPack dependency-closure hook.
        truehdr = payload / "nvngx_truehdr.dll"
        truehdr_original = truehdr.read_bytes()
        for dependency, allowed in (("nvcuda", True), ("NVCUDA", True),
                                    ("unrelated_missing_driver", False)):
            source = fixture / "driver-import.c"
            source.write_text(f"__declspec(dllexport) int {dependency}_fixture(void) {{ return 0; }}\n")
            obj = fixture / "driver-import.obj"
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            library = fixture / f"{dependency}.lib"
            run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{fixture / (dependency + '.dll')}", f"/implib:{library}", str(obj)])
            source.write_text(f"__declspec(dllimport) int {dependency}_fixture(void);\n"
                              f"__declspec(dllexport) int nvngx_truehdr_fixture(void) {{ return {dependency}_fixture(); }}\n")
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{truehdr}", str(obj), str(library)])
            try:
                if allowed:
                    run(package)
                    check_archive()
                    with zipfile.ZipFile(archive) as zipped:
                        assert not any(name.casefold().endswith("/nvcuda.dll") for name in zipped.namelist())
                    if dependency == "nvcuda":
                        nested = tools / "nvngx_truehdr.dll"
                        shutil.copyfile(truehdr, nested)
                        try:
                            run(package, success=False,
                                contains="Unexpected eager nvcuda.dll import in portable image")
                        finally:
                            nested.unlink()
                else:
                    run(package, success=False, contains="Unresolved portable DLL imports: unrelated_missing_driver.dll")
            finally:
                truehdr.write_bytes(truehdr_original)
        print("PASS: only the unbundled NVIDIA driver import is allowed; other missing DLLs reject packaging")
        # The driver import is optional only while confined to the dlopened NGX
        # runtime. An eager host import would break startup on non-NVIDIA PCs.
        host = payload / "sunshine.exe"
        host_original = host.read_bytes()
        try:
            source = fixture / "host-driver-import.c"
            source.write_text("__declspec(dllimport) int nvcuda_fixture(void);\n"
                              "int main(void) { return nvcuda_fixture(); }\n")
            obj = fixture / "host-driver-import.obj"
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            run([args.link, "/entry:main", "/subsystem:console", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{host}", str(obj), str(fixture / "nvcuda.lib")])
            run(package, success=False, contains="Unexpected eager nvcuda.dll import in portable image")
            resolved_driver = redist / "nvcuda.dll"
            shutil.copyfile(fixture / "nvcuda.dll", resolved_driver)
            try:
                run(package, success=False, contains="Unexpected eager nvcuda.dll import in portable image")
            finally:
                resolved_driver.unlink()
        finally:
            host.write_bytes(host_original)
        print("PASS: an eager sunshine.exe nvcuda.dll import rejects portable packaging")
        # Replace only one image at a time, keeping its import filename. The
        # real CPack pre-build hook must reject each incompatible PE boundary.
        wrong_target = "i686-pc-windows-msvc" if machine != "x86" else "x86_64-pc-windows-msvc"
        wrong_machine = "x86" if machine != "x86" else "x64"
        wrong_source = fixture / "wrong-machine.c"
        wrong_source.write_text("__declspec(dllexport) int vcruntime140_1_fixture(void) { return 0; }\n")
        wrong_obj = fixture / "wrong-machine.obj"
        wrong_dll = fixture / "wrong-machine.dll"
        run([args.clang, f"--target={wrong_target}", "-c", str(wrong_source), "-o", str(wrong_obj)])
        run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{wrong_machine}",
             f"/out:{wrong_dll}", str(wrong_obj)])
        wrong_host_source = fixture / "wrong-host.c"
        wrong_host_source.write_text("int main(void) { return 0; }\n")
        run([args.clang, f"--target={wrong_target}", "-c", str(wrong_host_source), "-o", str(wrong_obj)])
        wrong_exe = fixture / "wrong-host.exe"
        run([args.link, "/entry:main", "/subsystem:console", "/nodefaultlib", f"/machine:{wrong_machine}",
             f"/out:{wrong_exe}", str(wrong_obj)])
        for image, replacement in (
            (payload / "sunshine.exe", wrong_exe),
            (payload / "tools/sunshine_wgc_capture.exe", wrong_exe),
            (payload / "zlib1.dll", wrong_dll),
            (payload / "tools/vibepollo_dualsense_audio.dll", wrong_dll),
            (redist / "vcruntime140_1.dll", wrong_dll),
        ):
            backup = image.read_bytes()
            image.write_bytes(replacement.read_bytes())
            try:
                run(package, success=False, contains="Portable PE architecture mismatch")
            finally:
                image.write_bytes(backup)
        print("PASS: wrong-machine host, helper, dlopened DLL, audio DLL and transitive redist CRT reject production CPack")
        image = payload / "sunshine.exe"
        original = image.read_bytes()
        pe = int.from_bytes(original[0x3c:0x40], "little")

        def section_headers(data):
            header = int.from_bytes(data[0x3c:0x40], "little")
            count = int.from_bytes(data[header + 6:header + 8], "little")
            optional_size = int.from_bytes(data[header + 20:header + 22], "little")
            return [header + 24 + optional_size + 40 * index for index in range(count)]

        # The linker emits a real uninitialized section with no raw payload.
        # Such sections and overlays (for example, signing data) must not be
        # mistaken for truncated sections or require EOF at the last section.
        assert any(int.from_bytes(original[section + 8:section + 12], "little") > 0 and
                   int.from_bytes(original[section + 16:section + 20], "little") == 0
                   for section in section_headers(original)), "Fixture lacks a zero-filled section"
        overlay = original + b"portable fixture trailing data"
        image.write_bytes(overlay)
        try:
            run(package)
            with zipfile.ZipFile(archive) as zipped:
                prefix = f"Vibepollo-Windows-{args.architecture}-2.0.1-beta.1-portable/"
                assert zipped.read(prefix + "sunshine.exe") == overlay
        finally:
            image.write_bytes(original)
        print("PASS: linked zero-filled section and trailing data survive actual CPack unchanged")

        # Exercise the real CPack hook for selected roots, resolved staged DLLs
        # and a transitive redistributable. Import discovery alone can accept
        # these code sections even when their raw-data range is outside EOF.
        for image in (payload / "sunshine.exe", payload / "zlib1.dll", redist / "vcruntime140_1.dll"):
            backup = image.read_bytes()
            sections = section_headers(backup)
            backed = [section for section in sections
                      if int.from_bytes(backup[section + 16:section + 20], "little") > 0]
            first = backed[0]
            raw_pointer = int.from_bytes(backup[first + 20:first + 24], "little")
            invalid_ranges = (
                (first, 20, len(backup) + 4096),  # retained review counterexample
                (first, 20, len(backup)),         # nonempty range starts at EOF
                (first, 16, len(backup) - raw_pointer + 1),  # crosses EOF by one
                (first, 20, 0),                   # nonempty payload overlaps headers
                (first, 20, 0xffffffff),          # sum must not wrap at 32 bits
                (first, 16, 0xffffffff),
                (backed[-1], 20, len(backup) + 4096),  # validate every section
            )
            for section, field, value in invalid_ranges:
                invalid = bytearray(backup)
                invalid[section + field:section + field + 4] = value.to_bytes(4, "little")
                image.write_bytes(invalid)
                try:
                    # A dependency scanner can itself reject a damaged import
                    # before returning it for validation (notably LLVM/ARM64).
                    # Roots are always checked by our gate before that scan.
                    message = "section raw data bounds" if image.parent == payload else None
                    run(package, success=False, contains=message)
                finally:
                    image.write_bytes(backup)
            # The whole table must exist before any section fields are read.
            image.write_bytes(backup[:sections[-1] + 39])
            try:
                run(package, success=False)
            finally:
                image.write_bytes(backup)
        image = payload / "sunshine.exe"
        print("PASS: selected host, staged DLL and transitive CRT reject section raw-data bounds and truncated tables in actual CPack")

        bad_dos = bytearray(original)
        bad_dos[0:2] = b"NO"
        bad_offset = bytearray(original)
        bad_offset[0x3c:0x40] = (len(original) + 1).to_bytes(4, "little")
        bad_signature = bytearray(original)
        bad_signature[pe:pe + 4] = b"NOPE"
        unknown_machine = bytearray(original)
        unknown_machine[pe + 4:pe + 6] = b"\x00\x00"
        bad_optional = bytearray(original)
        bad_optional[pe + 24:pe + 26] = b"\x00\x00"
        for invalid in (b"not a PE", bad_dos, bad_offset, bad_signature, unknown_machine, bad_optional, original[:pe + 26]):
            image.write_bytes(invalid)
            try:
                run(package, success=False)
            finally:
                image.write_bytes(original)
        # Invalid/unknown transitive imports must fail as well, even if objdump
        # rejects them before our explicit architecture check sees the result.
        crt = redist / "vcruntime140_1.dll"
        backup = crt.read_bytes()
        crt_pe = int.from_bytes(backup[0x3c:0x40], "little")
        unknown_crt = bytearray(backup)
        unknown_crt[crt_pe + 4:crt_pe + 6] = b"\x00\x00"
        for invalid in (b"invalid CRT", unknown_crt):
            crt.write_bytes(invalid)
            try:
                run(package, success=False)
            finally:
                crt.write_bytes(backup)
        run([*configure, "-DFIXTURE_PROCESSOR=unknown"])
        run(package, success=False, contains="Unsupported portable target architecture")
        run([*configure, f"-DFIXTURE_PROCESSOR={args.architecture}"])
        run(package)
        check_archive()
        print(f"PASS: {args.architecture} native PE closure succeeds; tested invalid headers and unknown machines/target reject packaging; MSI uninstaller excluded")
        stage = next((build / "packages/_CPack_Packages").glob("*/ZIP/Vibepollo*portable"))
        (stage / "config/private-key").write_text("must not ship")
        run(package)
        check_archive()
        print("PASS: repeated packaging discards stale CPack profile/credentials")

        def missing(relative, message):
            path = payload / relative
            backup = path.read_bytes()
            path.unlink()
            try:
                run(package, success=False, contains=message)
            finally:
                path.write_bytes(backup)

        missing("zlib1.dll", "Required portable runtime file missing: zlib1.dll")
        missing("tools/sunshine_wgc_capture.exe", "Required portable runtime file missing: tools/sunshine_wgc_capture.exe")
        missing("tools/vibepollo_dualsense_audio.dll", "Required portable runtime file missing: tools/vibepollo_dualsense_audio.dll")
        missing("plugins/playnite/SunshinePlaynite/VibepolloPlaynite.dll", "Required portable runtime file missing")
        missing("assets/web/v2/assets/v2.js", "Portable Web UI bundle missing")
        crt = redist / "vcruntime140_1.dll"
        crt_backup = crt.read_bytes()
        crt.unlink()
        try:
            run(package, success=False, contains="Unresolved portable DLL imports: vcruntime140_1.dll")
        finally:
            crt.write_bytes(crt_backup)
        print("PASS: missing dlopened DLL, helper, UI bundle and transitive PE import reject packaging")
        # Imported debug CRTs must be rejected even when already staged. Include
        # the companion DLL form that puts the 'd' before a filename suffix.
        runtime = payload / "nvngx_truehdr.dll"
        runtime_backup = runtime.read_bytes()
        for debug_name in ("vcruntime140d", "msvcp140d_atomic_wait", "ucrtbased"):
            source = fixture / "debug-crt.c"
            source.write_text(f"__declspec(dllexport) int {debug_name}_fixture(void) {{ return 0; }}\n")
            obj = fixture / "debug-crt.obj"
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            debug_import = fixture / "debug-crt.lib"
            run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{redist / (debug_name + '.dll')}", f"/implib:{debug_import}", str(obj)])
            source.write_text(f"__declspec(dllimport) int {debug_name}_fixture(void);\n"
                              f"__declspec(dllexport) int nvngx_truehdr_fixture(void) {{ return {debug_name}_fixture(); }}\n")
            run([args.clang, f"--target={target}", "-c", str(source), "-o", str(obj)])
            run([args.link, "/dll", "/noentry", "/nodefaultlib", f"/machine:{machine}",
                 f"/out:{runtime}", str(obj), str(debug_import)])
            staged_debug = payload / (debug_name + ".dll")
            try:
                run(package, success=False, contains="non-redistributable debug CRT")
                shutil.copyfile(redist / staged_debug.name, staged_debug)
                run(package, success=False, contains="non-redistributable debug CRT")
                if debug_name == "ucrtbased" and os.name != "nt":
                    # Resolve the debug UCRT from modeled System32 without a
                    # staged copy; the normal Windows-DLL allowance must not
                    # treat a developer-only debug runtime as an OS dependency.
                    staged_debug.unlink()
                    debug_system_root = fixture / "debug UCRT Windows"
                    debug_system_dir = debug_system_root / "System32"
                    debug_system_dir.mkdir(parents=True)
                    debug_system_dll = debug_system_dir / staged_debug.name
                    shutil.move(redist / staged_debug.name, debug_system_dll)
                    try:
                        runtime_search = f"{cmake_path(redist)};{cmake_path(debug_system_dir)}"
                        run([*configure, f"-DSUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS={runtime_search}"])
                        run(package, success=False, contains="non-redistributable debug CRT",
                            env={**os.environ, "SystemRoot": str(debug_system_root)})
                    finally:
                        shutil.move(debug_system_dll, redist / staged_debug.name)
                        run([*configure, f"-DSUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS={cmake_path(redist)}"])
                    print("PASS: a debug UCRT import resolved from modeled System32 rejects packaging")
            finally:
                staged_debug.unlink(missing_ok=True)
                runtime.write_bytes(runtime_backup)
        print("PASS: imported debug VC and UCRT DLLs reject packaging from external redist and pre-staged payloads")
        if os.name != "nt":
            extra = payload / "vcruntime140.dll"
            backup = (redist / "vcruntime140.dll").read_bytes()
            external = fixture / "outside.dll"
            external.write_bytes(backup)
            extra.symlink_to(external)
            try:
                run(package, success=False, contains="Portable DLL dependency is not bundled")
            finally:
                extra.unlink()
            system_root = fixture / "Windows"
            system_dll = system_root / "System32/vcruntime140.dll"
            system_dll.parent.mkdir(parents=True)
            system_dll.write_bytes(backup)
            extra.symlink_to(system_dll)
            try:
                run(package, success=False, contains="Portable DLL dependency is not bundled",
                    env={**os.environ, "SystemRoot": str(system_root)})
            finally:
                extra.unlink()
            print("PASS: DLL imports outside the ZIP stage, including a developer's System32 CRT, reject packaging")
            shutil.copyfile(redist / "vcruntime140_1.dll", system_dll.parent / "vcruntime140_1.dll")
            run([*configure, f"-DSUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS={system_dll.parent}"])
            run(package, success=False, contains="Portable DLL dependency is not bundled",
                env={**os.environ, "SystemRoot": str(system_root)})
            run([*configure, f"-DSUNSHINE_PORTABLE_REDISTRIBUTABLE_DIRS={redist}"])
            print("PASS: a configured System32 search directory cannot supply redistributed CRTs")
        path = payload / "assets/web/v2/index.html"
        backup = path.read_bytes()
        path.write_bytes(b"")
        run(package, success=False, contains="Required portable runtime file is empty")
        path.write_bytes(backup)
        private = payload / "config"
        private.mkdir()
        (private / "private-key").write_text("must not ship")
        run(package, success=False, contains="must not include an existing config directory")
        shutil.rmtree(private)
        print("PASS: empty runtime file and user configuration reject packaging")

        run([*configure, "-DSUNSHINE_ASSETS_DIR=/absolute/assets"])
        run(package, success=False, contains="require SUNSHINE_ASSETS_DIR=assets")
        run([*configure, "-DSUNSHINE_ASSETS_DIR=assets", "-DFIXTURE_VERSION=0.0.0"])
        run(package, success=False, contains="require an explicit BUILD_VERSION")
        run([*configure, "-DFIXTURE_VERSION=2.0.1-beta.1", "-DBUILD_VERSION="])
        run(package, success=False, contains="require an explicit BUILD_VERSION")
        print("PASS: a nonzero inferred version does not satisfy the explicit BUILD_VERSION requirement")
        probe = fixture / "wix_probe.cmake"
        probe.write_text(f"""include("{cmake_path(build)}/CPackConfig.cmake")
if(NOT CPACK_GENERATOR STREQUAL "WIX")
    message(FATAL_ERROR "Default generator was changed by portable packaging")
endif()
include("{cmake_path(build)}/portable_cpack_project_config.cmake")
if(NOT CPACK_WIX_PRODUCT_GUID OR NOT CPACK_PACKAGE_FILE_NAME STREQUAL "Vibepollo")
    message(FATAL_ERROR "WiX identity/configuration was changed by portable packaging")
endif()
""")
        run([args.cmake, "-P", str(probe)], contains="CPACK_WIX_PRODUCT_GUID")
        run([*configure, "-DBUILD_VERSION=2.0.1-beta.1"])
        run(package)
        check_archive()
        print("PASS: portable assets must be relative; WiX keeps existing GUID generation and installer name")


if __name__ == "__main__":
    main()
