#!/usr/bin/env python3
"""Validate the packaged Vibeshine DRM source and its deterministic HDR EDID."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def require_source(source: str, needle: str, filename: str) -> None:
    require(needle in source, f"{filename} is missing {needle!r}")


def decode_manufacturer(edid: bytes) -> str:
    encoded = int.from_bytes(edid[8:10], byteorder="big")
    return "".join(chr(((encoded >> shift) & 0x1F) + ord("A") - 1) for shift in (10, 5, 0))


def cta_data_blocks(cta: bytes) -> list[tuple[int, bytes]]:
    require(cta[0] == 0x02, "EDID extension is not a CTA-861 extension")
    require(4 <= cta[2] <= 127, "CTA data-block collection has an invalid end offset")

    blocks: list[tuple[int, bytes]] = []
    offset = 4
    while offset < cta[2]:
        header = cta[offset]
        length = header & 0x1F
        end = offset + 1 + length
        require(end <= cta[2], "CTA data block extends beyond the data-block collection")
        blocks.append((header >> 5, cta[offset + 1 : end]))
        offset = end
    require(offset == cta[2], "CTA data-block collection was not parsed exactly")
    return blocks


def validate_edid(edid: bytes) -> None:
    require(len(edid) == 256, f"expected a 256-byte EDID, got {len(edid)} bytes")
    for index in range(2):
        block = edid[index * 128 : (index + 1) * 128]
        require(sum(block) & 0xFF == 0, f"EDID block {index} checksum is invalid")

    require(decode_manufacturer(edid) == "VBS", "EDID manufacturer is not VBS")
    require(b"Vibeshine HDR" in edid[:128], "EDID monitor name is not Vibeshine HDR")
    require(edid[126] == 1, "base EDID does not advertise exactly one extension block")

    blocks = cta_data_blocks(edid[128:])
    video_blocks = [payload for tag, payload in blocks if tag == 0x02]
    require(video_blocks == [bytes((97, 118, 16, 63))],
            "CTA video block must advertise valid 4K60, 4K120, 1080p60, and 1080p120 VICs")

    extended = [payload for tag, payload in blocks if tag == 0x07 and payload]
    colorimetry = [payload for payload in extended if payload[0] == 0x05]
    require(colorimetry and len(colorimetry[0]) >= 3, "CTA colorimetry block is missing")
    require(colorimetry[0][1] & 0xC0 == 0xC0,
            "CTA colorimetry block does not advertise BT.2020 RGB and YCbCr")

    hdr_metadata = [payload for payload in extended if payload[0] == 0x06]
    require(hdr_metadata and len(hdr_metadata[0]) >= 3, "CTA HDR static metadata block is missing")
    require(hdr_metadata[0][1] & 0x04, "CTA HDR metadata does not advertise SMPTE ST 2084 PQ")
    require(hdr_metadata[0][2] & 0x01, "CTA HDR metadata does not advertise static metadata type 1")


def validate_source_contract(driver_root: Path) -> None:
    connector_path = driver_root / "vkms_connector.c"
    plane_path = driver_root / "vkms_plane.c"
    configfs_path = driver_root / "vkms_configfs.c"
    config_path = driver_root / "vkms_config.c"
    connector = connector_path.read_text(encoding="utf-8")
    plane = plane_path.read_text(encoding="utf-8")
    configfs = configfs_path.read_text(encoding="utf-8")
    config = config_path.read_text(encoding="utf-8")

    for needle in (
        "drm_connector_attach_max_bpc_property(&connector->base, 8, 16)",
        "drm_connector_attach_hdr_output_metadata_property(&connector->base)",
        "DRM_MODE_COLORIMETRY_BT2020_RGB",
        "DRM_MODE_COLORIMETRY_BT2020_YCC",
        "drm_connector_attach_colorspace_property(&connector->base)",
    ):
        require_source(connector, needle, connector_path.name)

    for pixel_format in (
        "DRM_FORMAT_XRGB2101010",
        "DRM_FORMAT_XBGR2101010",
        "DRM_FORMAT_ARGB2101010",
        "DRM_FORMAT_ABGR2101010",
        "DRM_FORMAT_P010",
        "DRM_FORMAT_P012",
        "DRM_FORMAT_P016",
    ):
        require_source(plane, pixel_format, plane_path.name)

    require(re.search(r'\.ci_name\s*=\s*"vibeshine-drm"', configfs) is not None,
            "configfs root is not named vibeshine-drm")
    require("connector_cfg->status = connector_status_disconnected" in config,
            "new Vibeshine DRM connectors do not default to disconnected")

    exported = []
    for source_path in sorted(driver_root.rglob("*")):
        if source_path.suffix not in {".c", ".h"}:
            continue
        source = source_path.read_text(encoding="utf-8")
        if re.search(r"^\s*EXPORT_SYMBOL\w*\s*\(", source, flags=re.MULTILINE):
            exported.append(source_path.relative_to(driver_root).as_posix())
    require(not exported, "out-of-tree source exports kernel symbols: " + ", ".join(exported))


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} DRIVER_ROOT", file=sys.stderr)
        return 2

    driver_root = Path(sys.argv[1]).resolve()
    generator = driver_root / "generate_hdr_edid.py"
    header = driver_root / "vibeshine_hdr_edid.h"
    require(generator.is_file(), f"missing EDID generator: {generator}")
    require(header.is_file(), f"missing generated EDID header: {header}")

    subprocess.run([sys.executable, str(generator), "--check", str(header)], check=True)
    with tempfile.TemporaryDirectory(prefix="vibeshine-drm-contract-") as temporary_dir:
        binary = Path(temporary_dir) / "vibeshine-hdr.bin"
        subprocess.run([sys.executable, str(generator), "--binary", str(binary)], check=True)
        validate_edid(binary.read_bytes())

    validate_source_contract(driver_root)
    print("Vibeshine DRM source and HDR EDID contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ContractError, OSError, subprocess.CalledProcessError) as error:
        print(f"Vibeshine DRM source contract failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
