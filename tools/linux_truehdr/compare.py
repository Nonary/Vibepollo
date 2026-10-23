#!/usr/bin/env python3
"""Compare TrueHDR RGB channel values, optionally normalizing Windows FP16 scRGB."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

# Linear Rec.709/scRGB -> Rec.2020, D65. scRGB 1.0 represents 80 cd/m2.
REC709_TO_2020 = ((0.627403896, 0.329283038, 0.043313066),
                 (0.069097289, 0.919540395, 0.011362316),
                 (0.016391439, 0.088013308, 0.895595253))


def pq_encode(nits):
    value = (max(0.0, min(10000.0, nits)) / 10000.0) ** (2610 / 16384)
    return ((3424 / 4096 + (2413 / 128) * value) / (1 + (2392 / 128) * value)) ** (2523 / 32)


def pixels(data, fmt):
    if fmt == 'rgb10a2':
        for (word,) in struct.iter_unpack('<I', data):
            yield tuple((word >> shift) & 1023 for shift in (0, 10, 20))
    else:
        for r, g, b, _ in struct.iter_unpack('<eeee', data):
            if not all(math.isfinite(v) for v in (r, g, b)):
                raise ValueError('Nonfinite channel in Windows scRGB reference')
            yield tuple(round(pq_encode(80 * (row[0] * r + row[1] * g + row[2] * b)) * 1023)
                        for row in REC709_TO_2020)


def compare(candidate, reference, reference_format='rgb10a2', max_code_error=0.0):
    if not math.isfinite(max_code_error) or max_code_error < 0:
        raise ValueError('max_code_error must be finite and nonnegative')
    reference_pixel_bytes = 4 if reference_format == 'rgb10a2' else 8
    if not candidate or len(candidate) % 4 or len(reference) % reference_pixel_bytes:
        raise ValueError('Empty or incomplete pixel data')
    if len(candidate) // 4 != len(reference) // reference_pixel_bytes:
        raise ValueError('Pixel counts differ')
    errors, squared, maximum, count, failed = 0.0, 0.0, 0.0, 0, 0
    for actual, expected in zip(pixels(candidate, 'rgb10a2'), pixels(reference, reference_format)):
        for a, e in zip(actual, expected):
            error = abs(a - e)
            errors += error
            squared += error * error
            maximum = max(maximum, error)
            count += 1
            failed += error > max_code_error
    return dict(passed=failed == 0, pixels=count // 3, channels=count,
                channels_outside_tolerance=failed, max_code_error=maximum,
                mean_absolute_code_error=errors/count, rms_code_error=(squared/count)**0.5,
                allowed_max_code_error=max_code_error, reference_format=reference_format,
                candidate_sha256=hashlib.sha256(candidate).hexdigest(),
                reference_sha256=hashlib.sha256(reference).hexdigest(),
                scope='RGB values only; input identity, geometry, parameters, versions, and frame history must also match')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('candidate', type=Path)
    p.add_argument('reference', type=Path)
    p.add_argument('--reference-format', choices=('rgb10a2', 'scrgb-f16'), default='rgb10a2')
    p.add_argument('--max-code-error', type=float, default=0,
                   help='Maximum absolute 10-bit RGB code error; default requires exact RGB10 equality')
    args = p.parse_args()
    result = compare(args.candidate.read_bytes(), args.reference.read_bytes(),
                     args.reference_format, args.max_code_error)
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
