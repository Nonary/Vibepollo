#include <cuda_runtime.h>

namespace platf::linux_truehdr {
  namespace {
    __global__ void map_peak(unsigned char *pixels, unsigned pitch, unsigned width, unsigned height, const unsigned *lut) {
      const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
      const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
      if (x < width && y < height) {
        auto &pixel = reinterpret_cast<unsigned *>(pixels + y * pitch)[x];
        const unsigned value = pixel;
        pixel = (value & 0xc0000000u) | lut[value & 1023u] | (lut[(value >> 10) & 1023u] << 10) | (lut[(value >> 20) & 1023u] << 20);
      }
    }
  }

  int create_peak_lut(void **lut) {
    return cudaMalloc(lut, 1024 * sizeof(unsigned));
  }

  void destroy_peak_lut(void *lut) {
    cudaFree(lut);
  }

  int upload_peak_lut(void *lut, const unsigned *host) {
    return cudaMemcpy(lut, host, 1024 * sizeof(unsigned), cudaMemcpyHostToDevice);
  }

  int apply_peak_lut(void *pixels, unsigned pitch, unsigned width, unsigned height, const void *lut, void *stream) {
    map_peak<<<dim3((width + 31) / 32, (height + 7) / 8), dim3(32, 8), 0, static_cast<cudaStream_t>(stream)>>>(
      static_cast<unsigned char *>(pixels), pitch, width, height, static_cast<const unsigned *>(lut));
    return cudaGetLastError();
  }
}  // namespace platf::linux_truehdr
