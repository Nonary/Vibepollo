#!/usr/bin/env python3
"""Refresh only PyroWave's embedded block-packing shader.

Run in the development container with glslangValidator, spirv-opt and spirv-val
on PATH.
All other SPIR-V programs and every reflection byte are preserved; only their
bank offsets move. Descriptor bindings, push constants and specialization
constants remain unchanged. Full slangmosh regeneration is also supported.
Use --shaders PATH to update a host's vendored bank without replacing its other
programs with the client's variants.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    default_shaders = root / "pyrowave/pyrowave/shaders"
    if not default_shaders.is_dir():
        default_shaders = root / "third-party/pyrowave/pyrowave/shaders"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaders", type=Path, default=default_shaders)
    shaders = parser.parse_args().shaders.resolve()
    header = shaders / "slangmosh.hpp"
    text = header.read_text()
    bank_match = re.search(r"static const uint32_t spirv_bank\[\] =\s*\{(.*?)\n\};", text, re.S)
    if not bank_match:
        raise RuntimeError("Unrecognized slangmosh bank format")
    words = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", bank_match[1])]
    program_pattern = r"this->block_packing = device\.request_program\(spirv_bank \+ (\d+), (\d+), &layout\);"
    entries = list(re.finditer(program_pattern, text))
    if len(entries) != 1:
        raise RuntimeError("Expected exactly one block-packing shader")
    entry = entries[0]
    begin, byte_size = int(entry[1]), int(entry[2])
    if byte_size % 4 or begin < 0 or begin + byte_size // 4 > len(words):
        raise RuntimeError("Invalid block-packing bank range")
    end = begin + byte_size // 4
    if words[begin] != 0x07230203:
        raise RuntimeError("Block-packing bank range is not SPIR-V")
    with tempfile.TemporaryDirectory(prefix="pyrowave-block-packing-") as directory:
        binary = Path(directory) / "block-packing.spv"
        unoptimized = Path(directory) / "block-packing-unoptimized.spv"
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1", "-g0",
                        f"-I{shaders}", str(shaders / "block_packing.comp"), "-o", str(unoptimized)], check=True)
        # glslang -Os currently folds the tail's uint8_t(0) to OpConstant uchar
        # without Int8 capability, which is illegal with storage8 alone. These
        # passes inline and remove local temporaries while retaining the valid
        # 32-bit-zero-to-8-bit-storage conversion and existing feature contract.
        subprocess.run(["spirv-opt", "--target-env=vulkan1.1",
                        "--inline-entry-points-exhaustive", "--eliminate-dead-functions",
                        "--eliminate-local-single-block", "--eliminate-local-single-store",
                        "--eliminate-dead-code-aggressive", "--scalar-replacement=0",
                        "--ssa-rewrite", "--combine-access-chains",
                        "--eliminate-dead-code-aggressive", "--compact-ids",
                        str(unoptimized), "-o", str(binary)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(binary)], check=True)
        data = binary.read_bytes()
    shader_words = struct.unpack(f"<{len(data) // 4}I", data)
    # Preserve the eight-word row alignment of every later embedded program.
    # Unreferenced zero words after this program avoid rewriting the entire
    # remaining bank merely because this shader's word count changed.
    alignment_words = ((end - begin) - len(shader_words)) % 8
    word_delta = len(shader_words) + alignment_words - (end - begin)
    words[begin:end] = [*shader_words, *([0] * alignment_words)]

    # Rewrite every later program/shader pointer, including variant aliases.
    replacements = []
    offset_pattern = r"spirv_bank \+ (\d+), (\d+)"
    for pointer in re.finditer(offset_pattern, text):
        offset, size = int(pointer[1]), int(pointer[2])
        if pointer.start() >= entry.start() and pointer.end() <= entry.end():
            replacements.append((pointer.start(), pointer.end(), f"spirv_bank + {begin}, {len(data)}"))
        elif offset >= end:
            replacements.append((pointer.start(), pointer.end(), f"spirv_bank + {offset + word_delta}, {size}"))
        elif offset < end and offset + size // 4 > begin:
            raise RuntimeError("Another shader overlaps the block-packing bank range")
    rows = "\n" + "\n".join(
        "\t" + ", ".join(f"0x{x:08x}u" for x in words[i:i + 8]) + ","
        for i in range(0, len(words), 8))
    replacements.append((bank_match.start(1), bank_match.end(1), rows))
    for start, stop, replacement in sorted(replacements, reverse=True):
        text = text[:start] + replacement + text[stop:]
    header.write_text(text)


if __name__ == "__main__":
    main()
