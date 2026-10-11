"""Build every configured component target before the full Linux package build."""
import json
from pathlib import Path
import re
import subprocess
import sys


def component_targets(build):
    reply = Path(build) / ".cmake/api/v1/reply"
    indices = list(reply.glob("index-*.json"))
    if not indices:
        raise ValueError("CMake component codemodel reply is missing")
    index = json.loads(max(indices, key=lambda p: p.stat().st_mtime_ns).read_text())
    model_file = index["reply"]["codemodel-v2"]["jsonFile"]
    model = json.loads((reply / model_file).read_text())
    configurations = model["configurations"]
    if len(configurations) != 1:
        raise ValueError("Linux package requires one configured build type")
    targets = sorted({t["name"] for t in configurations[0]["targets"] if re.fullmatch(r"test_component_[A-Za-z0-9_]+", t["name"])})
    if "test_component_linux_routed_link" not in targets:
        raise ValueError("Required Linux routed-link component was not configured")
    return targets


def main(build):
    targets = component_targets(build)
    print(f"Early component compile gate: {len(targets)} targets", flush=True)
    subprocess.run(["cmake", "--build", str(build), "--target", *targets], check=True)


if __name__ == "__main__":
    main(sys.argv[1])
