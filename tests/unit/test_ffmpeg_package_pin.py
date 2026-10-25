#!/usr/bin/env python3
"""Exercise the real CMake dependency loader with local, isolated archives."""
import hashlib
import io
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import tarfile
import tempfile
import unittest


LOADER = Path(__file__).resolve().parents[2] / "cmake/dependencies/ffmpeg.cmake"


class FFmpegPackagePin(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ffmpeg-package-pin-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.release = self.root / "release"
        self.source.mkdir()
        self.build.mkdir()
        self.release.mkdir()
        shutil.copyfile(LOADER, self.source / "ffmpeg.cmake")
        self.archive = self.release / "Linux-x86_64-ffmpeg.tar.gz"
        with tarfile.open(self.archive, "w:gz") as archive:
            content = b"pinned-new-encoder-package"
            entry = tarfile.TarInfo("ffmpeg/lib/libavcodec.a")
            entry.size = len(content)
            archive.addfile(entry, io.BytesIO(content))
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.archive_bytes = self.archive.read_bytes()
        (self.source / "ffmpeg-release.cmake").write_text(
            'set(FFMPEG_RELEASE_TAG "test-pin")\n'
            f'set(FFMPEG_RELEASE_URL "{self.release.as_uri()}")\n'
            f'set(FFMPEG_SHA256_Linux_x86_64 "{self.digest}")\n'
        )

    def run_loader(self, extra="", expected=0):
        script = self.root / "driver.cmake"
        script.write_text(
            f'set(CMAKE_BINARY_DIR "{self.build.as_posix()}")\n'
            'set(CMAKE_SYSTEM_NAME "Linux")\n'
            'set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n'
            f'{extra}\ninclude("{self.source.as_posix()}/ffmpeg.cmake")\n'
            f'file(WRITE "{self.root.as_posix()}/selected.txt" "${{FFMPEG_PREPARED_BINARIES}}")\n'
        )
        result = subprocess.run(["cmake", "-P", str(script)], text=True, capture_output=True, timeout=30)
        if expected == 0:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_ignores_legacy_extracted_cache(self):
        old = self.build / "_deps/ffmpeg/lib"
        old.mkdir(parents=True)
        (old / "libavcodec.a").write_bytes(b"stock-without-audio-encoders")
        self.run_loader()
        selected = Path((self.root / "selected.txt").read_text())
        self.assertIn(self.digest, str(selected))
        self.assertEqual((selected / "lib/libavcodec.a").read_bytes(), b"pinned-new-encoder-package")
        self.assertEqual((old / "libavcodec.a").read_bytes(), b"stock-without-audio-encoders")

    def test_checks_cached_archive_even_when_extracted_library_exists(self):
        self.run_loader()
        selected = Path((self.root / "selected.txt").read_text())
        (selected.parent / self.archive.name).write_bytes(b"corrupt cached archive")
        result = self.run_loader(expected=1)
        self.assertIn("checksum mismatch", result.stderr)

    def test_rejects_wrong_download_hash(self):
        self.archive.write_bytes(b"wrong release asset")
        result = self.run_loader(expected=1)
        self.assertIn("HASH mismatch", result.stderr)
        self.archive.write_bytes(self.archive_bytes)
        self.run_loader()

    def test_unpinned_platform_requires_explicit_override(self):
        result = self.run_loader('set(CMAKE_SYSTEM_PROCESSOR "unsupported")', expected=1)
        self.assertIn("No pinned FFmpeg archive", result.stderr)

    def test_prepared_binaries_override_needs_no_release_manifest(self):
        (self.source / "ffmpeg-release.cmake").unlink()
        for prepared in (self.root / "custom-package", PureWindowsPath(r"C:\ffmpeg-path-format-test\custom-package")):
            with self.subTest(prepared=str(prepared)):
                self.run_loader(f'set(FFMPEG_PREPARED_BINARIES "{prepared.as_posix()}")')
                self.assertEqual((self.root / "selected.txt").read_text(), prepared.as_posix())
        self.assertFalse((self.build / "_deps").exists())


if __name__ == "__main__":
    unittest.main()
