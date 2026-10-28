"""Compile complete production translation units; never extract/generate C++."""
from pathlib import Path
import os
import subprocess
import tempfile


def run(repository, compiler, sanitize, group):
    repository = Path(repository).resolve()
    with tempfile.TemporaryDirectory(prefix="audio-component-") as directory:
        binary = Path(directory) / "audio-tests"
        flags = ["-std=c++23", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror", "-DSUNSHINE_ENABLE_WEBRTC"]
        json_include = os.environ.get("AUDIO_SNAPSHOT_JSON_INCLUDE_DIR")
        if json_include:
            flags += ["-I", json_include]
        if sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([compiler, *flags, "-I", str(repository),
                        str(repository / "tests/unit/test_audio_context.cpp"),
                        str(repository / "src/audio_context.cpp"), "-o", str(binary)],
                       check=True, timeout=120)
        subprocess.run([str(binary), group], check=True, timeout=30)
        if group == "context.":
            subprocess.run([str(binary), "transition."], check=True, timeout=30)
            for mode in ("--owner-unwind-video", "--owner-unwind-audio"):
                subprocess.run([str(binary), mode], check=True, timeout=30)
