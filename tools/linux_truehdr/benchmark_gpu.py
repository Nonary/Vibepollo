#!/usr/bin/env python3
"""Measure resident NVIDIA TrueHDR work separately from host transfers/allocations."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import statistics
import time
from truehdr import Image, TrueHDR, bind, test_chart


def summary(values):
    values = sorted(values)
    return dict(min_ms=values[0], median_ms=statistics.median(values),
                p95_ms=values[min(len(values)-1, int(0.95*len(values)))], max_ms=values[-1])


def measure(runtime, width, height, debanding_off, samples, warmup):
    frame = test_chart(width, height)
    with TrueHDR(runtime, width, height, debanding_off=debanding_off) as effect:
        # Populate the GPU source and establish the synchronous reference once.
        reference = effect.convert(frame)
        cuda = effect._cuda
        create = bind(cuda, 'cuEventCreate', [C.POINTER(C.c_void_p), C.c_uint])
        record = bind(cuda, 'cuEventRecord', [C.c_void_p, C.c_void_p])
        wait = bind(cuda, 'cuEventSynchronize', [C.c_void_p])
        elapsed = bind(cuda, 'cuEventElapsedTime', [C.POINTER(C.c_float), C.c_void_p, C.c_void_p])
        destroy = bind(cuda, 'cuEventDestroy_v2', [C.c_void_p])
        begin, end = C.c_void_p(), C.c_void_p()
        def check(status):
            if status:
                raise RuntimeError(f'CUDA event operation failed: {status}')
        try:
            check(create(C.byref(begin), 0))
            check(create(C.byref(end), 0))
            gpu, wall, submit = [], [], []
            for index in range(warmup + samples):
                check(record(begin, effect.stream))
                start = time.perf_counter()
                effect.run_gpu()
                submitted = time.perf_counter()
                check(record(end, effect.stream))
                check(wait(end))
                done = time.perf_counter()
                ms = C.c_float()
                check(elapsed(C.byref(ms), begin, end))
                if index >= warmup:
                    gpu.append(ms.value)
                    wall.append((done-start)*1000)
                    submit.append((submitted-start)*1000)
            # Cross-check the event span with a batch that pays the CPU wait once.
            check(record(begin, effect.stream))
            batch_start = time.perf_counter()
            for _ in range(samples):
                effect.run_gpu()
            check(record(end, effect.stream))
            check(wait(end))
            batch_wall_ms = (time.perf_counter() - batch_start) * 1000
            batch_gpu_ms = C.c_float()
            check(elapsed(C.byref(batch_gpu_ms), begin, end))
            # Copy the last asynchronously evaluated output once, outside timing.
            host = C.create_string_buffer(effect.size)
            image = Image()
            effect.check('Wrap benchmark output', effect._init(C.byref(image), width, height,
                width*4, host, 13, 11, 0, 0))
            effect.check('Download benchmark output', effect._transfer(C.byref(effect.dst),
                C.byref(image), 1.0, effect.stream, None))
            check(effect._synchronize(effect.stream))
            if host.raw != reference:
                raise AssertionError('GPU-resident output differs from the synchronous reference')
            return dict(width=width, height=height, debanding_off=debanding_off,
                samples=samples, warmup=warmup, cuda_event_span=summary(gpu),
                wall_completion=summary(wall), host_submit=summary(submit),
                batch_cuda_ms_per_frame=batch_gpu_ms.value/samples,
                batch_wall_ms_per_frame=batch_wall_ms/samples,
                output_matches_synchronous_reference=True,
                output_sha256=hashlib.sha256(host.raw).hexdigest())
        finally:
            effect._synchronize(effect.stream)
            for event in (begin, end):
                if event:
                    destroy(event)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=100)
    parser.add_argument('--warmup', type=int, default=10)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.samples < 1 or args.warmup < 0:
        parser.error('samples must be positive and warmup nonnegative')
    results = []
    for width, height in ((1280, 720), (1920, 1080), (3840, 2160)):
        for debanding_off in (0, 1):
            result = measure(args.runtime, width, height, debanding_off, args.samples, args.warmup)
            results.append(result)
            print(json.dumps(result), flush=True)
    with args.output.open('x') as out:
        json.dump(dict(scope='Resident RGBA8 -> RGB10A2 effect only; excludes capture, copies, encoding and display',
                       results=results), out, indent=2)


if __name__ == '__main__':
    main()
