"""Compile the real PyroWave include block with MinGW's Winsock warning fatal."""
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    root = Path(sys.argv[1])
    compiler = sys.argv[2]
    source = (root / "src/platform/windows/pyrowave_encode.cpp").read_text(encoding="utf-8")
    prefix, marker, _ = source.partition("// local includes")
    if not marker or "#include <winsock2.h>" not in prefix:
        raise AssertionError("PyroWave platform include block was not found")

    with tempfile.TemporaryDirectory() as tmp:
        fixture = Path(tmp) / "header_order.cpp"

        def compile_text(text: str) -> subprocess.CompletedProcess[str]:
            fixture.write_text(text, encoding="utf-8")
            return subprocess.run(
                [compiler, "-std=gnu++23", "-Werror=cpp", "-fsyntax-only", str(fixture)],
                capture_output=True, text=True, check=False,
            )

        result = compile_text(prefix + "\nint main() { return 0; }\n")
        if result.returncode:
            raise AssertionError(f"PyroWave include block did not compile:\n{result.stderr}")

        # Prove this compiler catches the original regression, not merely that
        # the extracted source happens to compile on this runner.
        reversed_order = compile_text("#include <windows.h>\n#include <winsock2.h>\n")
        if reversed_order.returncode == 0 or "winsock2.h" not in reversed_order.stderr:
            raise AssertionError("MinGW did not reject the reversed Winsock include order")


if __name__ == "__main__":
    main()
