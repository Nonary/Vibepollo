"""Validate the actual CI portable ZIP without starting the host or installing anything."""
import argparse
import hashlib
import shutil
import stat
import subprocess
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--product", choices=("Vibeshine", "Vibepollo"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repository, build = args.repository.resolve(), args.build.resolve()
    name = f"{args.product}-Windows-AMD64-{args.version}-portable"
    archives = list((build / "cpack_artifacts").glob("*-portable.zip"))
    if len(archives) != 1 or archives[0].name != name + ".zip":
        raise ValueError(f"Expected exactly the fresh {name}.zip; found {archives}")
    archive = archives[0]
    templates = repository / "packaging/windows/portable"
    with tempfile.TemporaryDirectory(prefix="portable-zip-validation-") as temporary:
        destination = Path(temporary)
        with zipfile.ZipFile(archive) as zipped:
            seen = set()
            for entry in zipped.infolist():
                path = PurePosixPath(entry.filename)
                if (not path.parts or path.parts[0] != name or
                        entry.filename != path.as_posix() + ("/" if entry.is_dir() else "") or
                        any(part in (".", "..") or ":" in part or "\\" in part for part in path.parts) or
                        stat.S_ISLNK(entry.external_attr >> 16)):
                    raise ValueError(f"Unexpected portable ZIP path: {entry.filename}")
                key = path.as_posix().casefold()
                if key in seen:
                    raise ValueError(f"Duplicate portable ZIP path: {entry.filename}")
                seen.add(key)
            zipped.extractall(destination)
        stage = destination / name
        if not stage.is_dir() or (stage / "uninstall.exe").exists():
            raise ValueError("Portable ZIP must contain one runtime root without the MSI uninstaller")
        config = stage / "config"
        if sorted(path.name for path in config.iterdir()) != ["sunshine.conf"]:
            raise ValueError("Portable ZIP contains unexpected user configuration")
        for relative, template in (("config/sunshine.conf", "sunshine.conf"),
                                   ("start-portable.cmd", "start-portable.cmd"),
                                   ("README-portable.txt", "README-portable.txt")):
            if (stage / relative).read_bytes().replace(b"\r\n", b"\n") != (templates / template).read_bytes().replace(b"\r\n", b"\n"):
                raise ValueError(f"Portable ZIP differs from the pristine {template}")
        # The generated hook owns the configured native runtime/helper/plugin list,
        # asset checks and PE/import closure. Recheck the EXTRACTED ZIP with it,
        # rather than maintaining a second dependency or payload validator here.
        # Remove only the verified pristine profile so the pre-build hook can
        # enforce its no-existing-credentials rule and regenerate that profile.
        def contents():
            return {path.relative_to(stage).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in stage.rglob("*") if path.is_file()}
        original_files = contents()
        shutil.rmtree(config)
        subprocess.run(["cmake", f"-DCPACK_TEMPORARY_INSTALL_DIRECTORY={stage}",
                        "-P", str(build / "portable_pre_build.cmake")], check=True)
        if contents() != original_files:
            raise ValueError("Extracted portable ZIP required repair during runtime revalidation")
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(archive, args.output / archive.name)
    print(f"Validated actual AMD64 portable ZIP: {archive.name} (packaging only; host not executed)")


if __name__ == "__main__":
    main()
