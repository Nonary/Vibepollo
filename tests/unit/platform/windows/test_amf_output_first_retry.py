#!/usr/bin/env python3
"""Run production native AMF submission/delivery code against deterministic boundaries.

Usage: python3 test_amf_output_first_retry.py [repository] [--video-source PATH]
AMF/D3D, the clock, and capture arrivals are fakes; native encode/drain functions,
video submission/delivery/session methods, and the capture wait are extracted
from the selected production sources. No codec or network implementation is run.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("repository", nargs="?", type=Path, default=Path(__file__).resolve().parents[4])
parser.add_argument("--video-source", type=Path)
parser.add_argument("--compiler", default="g++")
args = parser.parse_args()
root = args.repository.resolve()
video = (args.video_source or root / "src/video.cpp").read_text()
amf = (root / "src/amf/amf_d3d11.cpp").read_text()
header = (root / "src/amf/amf_d3d11.h").read_text()


def extract(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def fake_clock(source):
    return source.replace("std::chrono::steady_clock", "model_clock")


pieces = {
    "encode_frame.inc": extract(amf, "  amf_encode_result\n  amf_d3d11::encode_frame(",
                                "\n  amf_encode_result\n  amf_d3d11::drain_output"),
    "drain_output.inc": extract(amf, "  amf_encode_result\n  amf_d3d11::drain_output(",
                                "\n  bool\n  amf_d3d11::begin_drain"),
    "video_delivery.inc": extract(video, "  void deliver_amf_frames(", "\n  int encode(\n"),
    "capture_wait.inc": extract(video, "        const auto image_wait_started = std::chrono::steady_clock::now();",
                                "\n        if (img) {"),
}
methods_start = ("    bool has_fresh_conversion() const {" if "    bool has_fresh_conversion() const {" in video
                 else "    amf::amf_encode_result encode_frames(")
pieces["video_session_methods.inc"] = (
    extract(video, methods_start, "\n    bool begin_drain()") +
    extract(video, "    bool note_emitted_index(", "\n  private:")
)
pieces["encoder_class.inc"] = (
    extract(header, "namespace amf {", "  /**\n   * @brief Create an AMF D3D11 encoder instance.") + "}\n"
).replace(": public amf_encoder", "").replace(" override", "").replace("  private:", "  public:").replace(
    "std::condition_variable", "model_cv")
# Property values only identify calls at the fake AMF boundary; production
# recovery decisions and acceptance bookkeeping are compiled unchanged.
properties = sorted(set(re.findall(r"\bAMF_VIDEO_[A-Z0-9_]+", pieces["encode_frame.inc"])))
pieces["constants.inc"] = "\n".join(
    f"inline constexpr int {name} = 1;" if name.endswith(("_IDR", "_KEY")) else
    f'inline constexpr const wchar_t *{name} = L"{name}";' for name in properties
)

with tempfile.TemporaryDirectory(prefix="amf-output-first-") as temporary:
    work = Path(temporary)
    for name, source in pieces.items():
        (work / name).write_text(fake_clock(source))
    shutil.copyfile(Path(__file__).with_name("fixtures") / "amf_output_first_harness.cpp.in", work / "harness.cpp")
    binary = work / "regression"
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(root), str(work / "harness.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
