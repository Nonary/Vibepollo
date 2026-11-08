#!/usr/bin/env python3
"""Pin that native AMF treats the client's slices per frame as a request.

Usage: python3 test_amf_client_slices.py [repository]
Moonlight for Nintendo Switch asks for four slices per frame. A driver that
refuses or rounds that count must not fail H.264 or HEVC encoder setup; the
host logs it and keeps the driver's own count. The policy itself is covered by
test_native_amf_review.cpp.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
source = (root / "src/amf/amf_d3d11.cpp").read_text()

for prop in ("AMF_VIDEO_ENCODER_SLICES_PER_FRAME", "AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME"):
    assert f"request_client_slices({prop}," in source, f"{prop} must go through request_client_slices"
    assert f"set_verified_int64({prop}" not in source, f"a refused {prop} must not fail encoder setup"

start = source.index("auto request_client_slices = [&]")
body = source[start:source.index("\n    };\n", start)]
assert "config_policy::request_client_slices(" in body, "the encoder must use the shared slice policy"
assert "return false;" not in body, "a refused slice count must not fail encoder setup"
assert "continuing with the driver's slice count" in body, "a refused slice count must be logged"
