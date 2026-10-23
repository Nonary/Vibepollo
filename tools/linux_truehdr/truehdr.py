#!/usr/bin/env python3
"""Run NVIDIA's native Linux TrueHDR on raw RGBA8 frames; emit BT.2020/PQ RGB10A2."""
import argparse
import array
import ctypes as C
import hashlib
import json
from pathlib import Path
import sys
import time


# Published NvCVImage C ABI; no NVIDIA implementation code is reproduced here.
# https://docs.nvidia.com/maxine/vfx/latest/API/Reference/Structures.html
class Image(C.Structure):
    _fields_ = [
        ('width', C.c_uint), ('height', C.c_uint), ('pitch', C.c_int),
        ('pixel_format', C.c_int), ('component_type', C.c_int),
        ('pixel_bytes', C.c_ubyte), ('component_bytes', C.c_ubyte),
        ('num_components', C.c_ubyte), ('planar', C.c_ubyte),
        ('gpu_mem', C.c_ubyte), ('colorspace', C.c_ubyte),
        ('reserved', C.c_ubyte * 2), ('pixels', C.c_void_p),
        ('delete_ptr', C.c_void_p), ('delete_proc', C.c_void_p),
        ('buffer_bytes', C.c_uint64),
    ]


def bind(library, name, args, result=C.c_int):
    fn = getattr(library, name)
    fn.argtypes = args
    fn.restype = result
    return fn


class TrueHDR:
    """Single-owner native VFX effect; retains GPU buffers between frames.

    CPU transfer is provided for the command-line reference harness. The SDK's
    images and CUDA stream stay available for a future capture/encoder adapter.
    Serialize calls on the creating thread; this object is not thread-safe.
    Each input is SDR RGBA8, without an HDR transfer function or display LUT.
    """
    def __init__(self, runtime, width, height, *, contrast=100, saturation=100,
                 middle_gray=50, peak=1000, debanding_off=0):
        if sys.platform != 'linux' or sys.byteorder != 'little' or C.sizeof(C.c_void_p) != 8:
            raise RuntimeError('This adapter requires little-endian 64-bit Linux')
        if not (16 <= width <= 8192 and 16 <= height <= 8192):
            raise ValueError('width and height must be in [16, 8192]')
        self.parameters = dict(contrast=contrast, saturation=saturation,
                               middle_gray=middle_gray, peak=peak, debanding_off=debanding_off)
        limits = dict(contrast=(0, 200), saturation=(0, 200), middle_gray=(10, 100),
                      peak=(400, 2000), debanding_off=(0, 1))
        for key, value in self.parameters.items():
            lo, hi = limits[key]
            if not isinstance(value, int) or isinstance(value, bool) or not lo <= value <= hi:
                raise ValueError(f'{key} must be an integer in [{lo}, {hi}]')
        self.width, self.height = width, height
        self.size = width * height * 4
        self.handle = C.c_void_p()
        self.src, self.dst = Image(), Image()
        self.stream = C.c_void_p()
        self.runtime = Path(runtime).resolve()
        libs = self.runtime / 'nvvfx' / 'libs'
        # Load the wheel's own CUDA runtime before its dependent SDK libraries.
        cudart = list((self.runtime / 'nvidia_vfx.libs').glob('libcudart-*.so.*'))
        if len(cudart) != 1:
            raise RuntimeError('Expected exactly one CUDA runtime in the extracted NVIDIA wheel')
        self._libraries = [C.CDLL(str(cudart[0]), mode=C.RTLD_GLOBAL)]
        # This loader is shipped by NVIDIA and resolves the bundled dependencies.
        sys.path.insert(0, str(self.runtime))
        import nvvfx
        if Path(nvvfx.__file__).resolve().parent != self.runtime / 'nvvfx':
            raise RuntimeError('A different nvvfx runtime is already imported in this process')
        self.sdk_version = nvvfx.get_sdk_version()
        self.cv = C.CDLL(str(libs / 'libNVCVImage.so'))
        self.fx = C.CDLL(str(libs / 'libVideoFX.so'))
        image_ptr = C.POINTER(Image)
        self._error = bind(self.cv, 'NvCV_GetErrorStringFromCode', [C.c_int], C.c_char_p)
        self._alloc = bind(self.cv, 'NvCVImage_Alloc', [image_ptr] + [C.c_uint] * 7)
        self._init = bind(self.cv, 'NvCVImage_Init',
                          [image_ptr, C.c_uint, C.c_uint, C.c_int, C.c_void_p] + [C.c_uint] * 4)
        self._dealloc = bind(self.cv, 'NvCVImage_Dealloc', [image_ptr], None)
        self._transfer = bind(self.cv, 'NvCVImage_Transfer',
                              [image_ptr, image_ptr, C.c_float, C.c_void_p, image_ptr])
        self._create = bind(self.fx, 'NvVFX_CreateEffect', [C.c_char_p, C.POINTER(C.c_void_p)])
        self._destroy = bind(self.fx, 'NvVFX_DestroyEffect', [C.c_void_p])
        self._set_image = bind(self.fx, 'NvVFX_SetImage', [C.c_void_p, C.c_char_p, image_ptr])
        self._set_u32 = bind(self.fx, 'NvVFX_SetU32', [C.c_void_p, C.c_char_p, C.c_uint])
        self._set_stream = bind(self.fx, 'NvVFX_SetCudaStream', [C.c_void_p, C.c_char_p, C.c_void_p])
        self._stream_create = bind(self.fx, 'NvVFX_CudaStreamCreate', [C.POINTER(C.c_void_p)])
        self._stream_destroy = bind(self.fx, 'NvVFX_CudaStreamDestroy', [C.c_void_p])
        self._load = bind(self.fx, 'NvVFX_Load', [C.c_void_p])
        self._run = bind(self.fx, 'NvVFX_Run', [C.c_void_p, C.c_int])
        self._cuda = C.CDLL('libcuda.so.1')
        self._synchronize = bind(self._cuda, 'cuStreamSynchronize', [C.c_void_p])
        try:
            self.check('CreateEffect', self._create(b'TrueHDR', C.byref(self.handle)))
            self.check('CudaStreamCreate', self._stream_create(C.byref(self.stream)))
            self.check('SetCudaStream', self._set_stream(self.handle, b'CudaStream', self.stream))
            # NVCV_RGBA=6/U8=1 and NVCV_RGB10A2=13/P32=11, interleaved=0, GPU=1.
            self.check('Allocate input', self._alloc(C.byref(self.src), width, height, 6, 1, 0, 1, 1))
            self.check('Allocate output', self._alloc(C.byref(self.dst), width, height, 13, 11, 0, 1, 1))
            self.check('Set input', self._set_image(self.handle, b'SrcImage0', C.byref(self.src)))
            self.check('Set output', self._set_image(self.handle, b'DstImage0', C.byref(self.dst)))
            for key, sdk_name in [('contrast', b'Contrast'), ('saturation', b'Saturation'),
                                  ('middle_gray', b'MiddleGray'), ('peak', b'Luminance'),
                                  ('debanding_off', b'DebandingOff')]:
                self.check(key, self._set_u32(self.handle, sdk_name, self.parameters[key]))
            self.check('Load', self._load(self.handle))
        except BaseException:
            self.close()
            raise

    def check(self, operation, status):
        if status:
            message = self._error(status)
            raise RuntimeError(f'{operation}: NVIDIA status {status}: {message.decode() if message else "unknown"}')

    def run_gpu(self):
        """Enqueue resident RGBA8 -> RGB10A2 conversion on this effect's CUDA stream.

        The caller must establish input readiness on this stream and retain both
        buffers until completion. Callers can chain GPU consumers on the same
        stream; CPU readback is optional. No host buffers are allocated here.
        """
        if not self.handle:
            raise RuntimeError('Effect is closed')
        self.check('Run', self._run(self.handle, 1))

    def convert(self, rgba):
        if not self.handle:
            raise RuntimeError('Effect is closed')
        if len(rgba) != self.size:
            raise ValueError(f'Expected {self.size} RGBA bytes, got {len(rgba)}')
        src_buffer = C.create_string_buffer(rgba, self.size)
        dst_buffer = C.create_string_buffer(self.size)
        cpu_src, cpu_dst = Image(), Image()
        self.check('Wrap input', self._init(C.byref(cpu_src), self.width, self.height,
                                            self.width * 4, src_buffer, 6, 1, 0, 0))
        self.check('Wrap output', self._init(C.byref(cpu_dst), self.width, self.height,
                                             self.width * 4, dst_buffer, 13, 11, 0, 0))
        try:
            self.check('Upload', self._transfer(C.byref(cpu_src), C.byref(self.src), 1.0, self.stream, None))
            self.check('Run', self._run(self.handle, 0))
            self.check('Download', self._transfer(C.byref(self.dst), C.byref(cpu_dst), 1.0, self.stream, None))
        finally:
            # Drain even on a failed SDK call before releasing the host buffers.
            status = self._synchronize(self.stream)
            if status:
                raise RuntimeError(f'cuStreamSynchronize: CUDA status {status}')
        return dst_buffer.raw

    def close(self):
        if self.stream:
            self._synchronize(self.stream)
        if self.handle:
            self._destroy(self.handle)
            self.handle = C.c_void_p()
        for image in (self.src, self.dst):
            if image.pixels:
                self._dealloc(C.byref(image))
                image.pixels = None
        if self.stream:
            self._stream_destroy(self.stream)
            self.stream = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def test_chart(width, height):
    """SDR ramps, RGB primaries, neutral patches and a bright highlight in one frame."""
    out = bytearray(width * height * 4)
    colors = [(0, 0, 0), (32, 32, 32), (128, 128, 128), (255, 255, 255),
              (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
    for y in range(height):
        for x in range(width):
            v = round(x * 255 / (width - 1))
            band = y * 4 // height
            rgb = ((v, v, v) if band == 0 else (v, 0, 0) if band == 1 else
                   colors[min(7, x * 8 // width)] if band == 2 else
                   (255, 255, 255) if abs(x-width//2) < width//32 else (16, 16, 16))
            i = (y * width + x) * 4
            out[i:i+4] = bytes((*rgb, 255))
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime', required=True, type=Path, help='Extracted NVIDIA wheel root')
    p.add_argument('--width', type=int, default=512)
    p.add_argument('--height', type=int, default=288)
    p.add_argument('--input', type=Path, help='Raw RGBA8 frames; omit to generate the test chart; use - for stdin')
    p.add_argument('--output', type=Path, required=True, help='Raw RGB10A2 output; - for stdout')
    p.add_argument('--save-input', type=Path, help='Save the exact SDR input frames for Windows reference capture')
    p.add_argument('--report', type=Path, help='JSON manifest (timing, hashes, parameters, pixel statistics)')
    p.add_argument('--frames', type=int, default=1)
    p.add_argument('--contrast', type=int, default=100)
    p.add_argument('--saturation', type=int, default=100)
    p.add_argument('--middle-gray', type=int, default=50)
    p.add_argument('--peak', type=int, default=1000)
    p.add_argument('--debanding-off', type=int, choices=(0, 1), default=0)
    args = p.parse_args()
    if args.frames < 1:
        p.error('--frames must be positive')
    parameters = {k: getattr(args, k) for k in ('contrast', 'saturation', 'middle_gray', 'peak', 'debanding_off')}
    manifest = dict(width=args.width, height=args.height, input_format='rgba8-sdr',
                    output_format='rgb10a2-bt2020-pq-le', parameters=parameters, frames=[])
    with TrueHDR(args.runtime, args.width, args.height, **parameters) as effect:
        manifest['sdk_version'] = effect.sdk_version
        model = args.runtime / 'nvvfx/libs/libnvidia-ngx-truehdr.so.1.2.3'
        with model.open('rb') as model_file:
            manifest['runtime_sha256'] = hashlib.file_digest(model_file, 'sha256').hexdigest()
        source = (sys.stdin.buffer if str(args.input) == '-' else args.input.open('rb')) if args.input else None
        output = sys.stdout.buffer if str(args.output) == '-' else args.output.open('xb')
        saved_input = None
        try:
            if args.save_input:
                saved_input = args.save_input.open('xb')
            chart = None if source else test_chart(args.width, args.height)
            for index in range(args.frames):
                rgba = source.read(effect.size) if source else chart
                if len(rgba) != effect.size:
                    raise ValueError(f'Incomplete input frame {index}: {len(rgba)} of {effect.size} bytes')
                if saved_input:
                    saved_input.write(rgba)
                start = time.perf_counter()
                hdr = effect.convert(rgba)
                elapsed = (time.perf_counter() - start) * 1000
                output.write(hdr)
                words = array.array('I', hdr)
                minimum, maximum = 1023, 0
                for word in words:
                    for shift in (0, 10, 20):
                        code = (word >> shift) & 1023
                        minimum, maximum = min(minimum, code), max(maximum, code)
                manifest['frames'].append(dict(index=index, milliseconds=elapsed,
                    input_sha256=hashlib.sha256(rgba).hexdigest(), output_sha256=hashlib.sha256(hdr).hexdigest(),
                    min_code=minimum, max_code=maximum, unique_words=len(set(words))))
        finally:
            if saved_input:
                saved_input.close()
            if source and source is not sys.stdin.buffer:
                source.close()
            if output is not sys.stdout.buffer:
                output.close()
    report = json.dumps(manifest, indent=2) + '\n'
    if args.report:
        with args.report.open('x') as stream:
            stream.write(report)
    else:
        print(report, file=sys.stderr)


if __name__ == '__main__':
    main()
