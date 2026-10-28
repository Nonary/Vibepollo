#!/usr/bin/env python3
"""Exercise real NVIDIA TrueHDR pixels, determinism, and tuning on a Linux GPU."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import time
from truehdr import TrueHDR, test_chart


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime', required=True, type=Path)
    args = p.parse_args()
    width, height = 512, 288
    frame = test_chart(width, height)
    results, outputs = [], {}
    for name, parameters in [('baseline', {}), ('peak400', {'peak': 400}),
                              ('peak2000', {'peak': 2000}), ('contrast0', {'contrast': 0}),
                              ('saturation0', {'saturation': 0}), ('middle10', {'middle_gray': 10}),
                              ('no_debanding', {'debanding_off': 1})]:
        with TrueHDR(args.runtime, width, height, **parameters) as effect:
            first = effect.convert(frame)
            start = time.perf_counter()
            second = effect.convert(frame)
            milliseconds = (time.perf_counter() - start) * 1000
            if first != second:
                raise AssertionError(f'{name}: repeated identical frame changed')
            if len(second) != width * height * 4:
                raise AssertionError('Incorrect output length')
            if not any(word & 0x3fffffff for (word,) in struct.iter_unpack('<I', second)):
                raise AssertionError(f'{name}: output has no nonzero RGB values')
            outputs[name] = second
            results.append(dict(case=name, sha256=hashlib.sha256(second).hexdigest(), milliseconds=milliseconds))
    for name in ('peak400', 'peak2000', 'contrast0', 'saturation0', 'middle10'):
        if outputs[name] == outputs['baseline']:
            raise AssertionError(f'{name}: changing the tuning dial did not change any output values')
    print(json.dumps(dict(passed=True, scope='Linux NVIDIA runtime only; not Windows parity', results=results), indent=2))


if __name__ == '__main__':
    main()
