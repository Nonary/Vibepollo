"""Compare PyroWave GPU encode/decode with a separate native SDK process."""
import ctypes
import os
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/linux_truehdr"))
from truehdr import TrueHDR


def main():
    runtime, executable, *capture_args = sys.argv[1:]
    try:
        cuda = ctypes.CDLL("libcuda.so.1")
    except OSError:
        print("SKIP: NVIDIA driver unavailable")
        return 77
    status = cuda.cuInit(0)
    if status == 100:  # CUDA_ERROR_NO_DEVICE
        print("SKIP: no CUDA device")
        return 77
    if status:
        raise RuntimeError(f"CUDA initialization failed: {status}")
    with tempfile.TemporaryDirectory(prefix="pyrowave-truehdr-") as directory:
        reference = Path(directory) / "reference.txt"
        values = []
        for variant in range(4):
            rgba = bytearray()
            for y in range(128):
                top = y >= 64 if variant == 1 else y < 64
                rgb = [16, 160, 64] if top else [192, 96, 32]
                if variant == 2:
                    rgb = [255 - v for v in rgb]
                if variant == 3:
                    rgb = [round(c + v * 127 / 255) for c, v in zip([64, 0, 32], rgb)]
                rgba.extend(bytes([*rgb, 255]) * 128)
            for peak in (1000, 1400):
                gain = peak / 1000
                with TrueHDR(Path(runtime), 128, 128, peak=min(peak, 1000),
                             middle_gray=round(50 / gain)) as effect:
                    output = effect.convert(bytes(rgba))
                pixel = struct.unpack_from("<I", output, (96 * 128 + 64) * 4)[0]
                for shift in (0, 10, 20):
                    code = (pixel >> shift) & 1023
                    if peak > 1000:
                        p = (code / 1023) ** (32 / 2523)
                        linear = (max(p - 3424 / 4096, 0) / (2413 / 128 - 2392 / 128 * p)) ** (16384 / 2610)
                        x = (linear * gain) ** (2610 / 16384)
                        code = math.floor(1023 * ((3424 / 4096 + 2413 / 128 * x) / (1 + 2392 / 128 * x)) ** (2523 / 32) + 0.5)
                    values.append(255 * code / 1023)
        reference.write_text(" ".join(map(str, values)))
        env = dict(os.environ, PYROWAVE_TEST_TRUEHDR_RUNTIME=runtime,
                   PYROWAVE_TEST_TRUEHDR_REFERENCE=str(reference))
        status = subprocess.call([executable, *capture_args], env=env)
        if status:
            return status
        # Exercise identical pixels/settings with the compatibility CPU fences.
        env["PYROWAVE_TEST_TRUEHDR_CPU_SYNC"] = "1"
        return subprocess.call([executable, *capture_args], env=env)


if __name__ == "__main__":
    sys.exit(main())
