/**
 * @file src/platform/linux/cuda.cu
 * @brief CUDA implementation for Linux.
 */
// standard includes
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

// platform includes
#include <helper_math.h>

// local includes
#include "cuda.h"

using namespace std::literals;

#define SUNSHINE_STRINGVIEW_HELPER(x) x##sv
#define SUNSHINE_STRINGVIEW(x) SUNSHINE_STRINGVIEW_HELPER(x)

#define CU_CHECK(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
  return -1

#define CU_CHECK_VOID(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return;

#define CU_CHECK_PTR(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return nullptr;

#define CU_CHECK_OPT(x, y) \
  if (check((x), SUNSHINE_STRINGVIEW(y ": "))) \
    return std::nullopt;

#define CU_CHECK_IGNORE(x, y) \
  check((x), SUNSHINE_STRINGVIEW(y ": "))

using namespace std::literals;

// Special declarations
/**
 * NVCC tends to have problems with standard headers.
 * Don't include common.h, instead use bare minimum
 * of standard headers and duplicate declarations of necessary classes.
 * Not pretty and extremely error-prone, fix at earliest convenience.
 */
namespace platf {
  struct img_t: std::enable_shared_from_this<img_t> {
  public:
    std::uint8_t *data {};
    std::int32_t width {};
    std::int32_t height {};
    std::int32_t pixel_pitch {};
    std::int32_t row_pitch {};

    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

    virtual ~img_t() = default;
  };
}  // namespace platf

// End special declarations

namespace cuda {

  struct alignas(16) cuda_color_t {
    float4 color_vec_y;
    float4 color_vec_u;
    float4 color_vec_v;
    float2 range_y;
    float2 range_uv;
  };

  static_assert(sizeof(video::color_t) == sizeof(cuda::cuda_color_t), "color matrix struct mismatch");

  auto constexpr INVALID_TEXTURE = std::numeric_limits<cudaTextureObject_t>::max();

  template<class T>
  inline T div_align(T l, T r) {
    return (l + r - 1) / r;
  }

  void pass_error(const std::string_view &sv, const char *name, const char *description);

  inline static int check(cudaError_t result, const std::string_view &sv) {
    if (result) {
      auto name = cudaGetErrorName(result);
      auto description = cudaGetErrorString(result);

      pass_error(sv, name, description);
      return -1;
    }

    return 0;
  }

  template<class T>
  ptr_t make_ptr() {
    void *p;
    CU_CHECK_PTR(cudaMalloc(&p, sizeof(T)), "Couldn't allocate color matrix");

    ptr_t ptr {p};

    return ptr;
  }

  void freeCudaPtr_t::operator()(void *ptr) {
    CU_CHECK_IGNORE(cudaFree(ptr), "Couldn't free cuda device pointer");
  }

  void freeCudaStream_t::operator()(cudaStream_t ptr) {
    CU_CHECK_IGNORE(cudaStreamDestroy(ptr), "Couldn't free cuda stream");
  }

  stream_t make_stream(int flags) {
    cudaStream_t stream;

    if (!flags) {
      CU_CHECK_PTR(cudaStreamCreate(&stream), "Couldn't create cuda stream");
    } else {
      CU_CHECK_PTR(cudaStreamCreateWithFlags(&stream, flags), "Couldn't create cuda stream with flags");
    }

    return stream_t {stream};
  }

  inline __device__ float3 bgra_to_rgb(uchar4 vec) {
    return make_float3((float) vec.z, (float) vec.y, (float) vec.x);
  }

  inline __device__ float3 bgra_to_rgb(float4 vec) {
    return make_float3(vec.z, vec.y, vec.x);
  }

  inline __device__ float2 calcUV(float3 pixel, const cuda_color_t *const color_matrix) {
    float4 vec_u = color_matrix->color_vec_u;
    float4 vec_v = color_matrix->color_vec_v;

    float u = dot(pixel, make_float3(vec_u)) + vec_u.w;
    float v = dot(pixel, make_float3(vec_v)) + vec_v.w;

    u = u * color_matrix->range_uv.x + color_matrix->range_uv.y;
    v = v * color_matrix->range_uv.x + color_matrix->range_uv.y;

    return make_float2(u, v);
  }

  inline __device__ float calcY(float3 pixel, const cuda_color_t *const color_matrix) {
    float4 vec_y = color_matrix->color_vec_y;

    return (dot(pixel, make_float3(vec_y)) + vec_y.w) * color_matrix->range_y.x + color_matrix->range_y.y;
  }

  __global__ void RGBA_to_NV12(
    cudaTextureObject_t srcImage,
    std::uint8_t *dstY,
    std::uint8_t *dstUV,
    std::uint32_t dstPitchY,
    std::uint32_t dstPitchUV,
    float scale,
    const viewport_t viewport,
    const cuda_color_t *const color_matrix
  ) {
    int idX = (threadIdx.x + blockDim.x * blockIdx.x) * 2;
    int idY = (threadIdx.y + blockDim.y * blockIdx.y) * 2;

    if (idX >= viewport.width) {
      return;
    }
    if (idY >= viewport.height) {
      return;
    }

    float x = idX * scale;
    float y = idY * scale;

    idX += viewport.offsetX;
    idY += viewport.offsetY;

    uint8_t *dstY0 = dstY + idX + idY * dstPitchY;
    uint8_t *dstY1 = dstY + idX + (idY + 1) * dstPitchY;
    dstUV = dstUV + idX + (idY / 2 * dstPitchUV);

    float3 rgb_lt = bgra_to_rgb(tex2D<float4>(srcImage, x, y));
    float3 rgb_rt = bgra_to_rgb(tex2D<float4>(srcImage, x + scale, y));
    float3 rgb_lb = bgra_to_rgb(tex2D<float4>(srcImage, x, y + scale));
    float3 rgb_rb = bgra_to_rgb(tex2D<float4>(srcImage, x + scale, y + scale));

    float2 uv_lt = calcUV(rgb_lt, color_matrix) * 256.0f;
    float2 uv_rt = calcUV(rgb_rt, color_matrix) * 256.0f;
    float2 uv_lb = calcUV(rgb_lb, color_matrix) * 256.0f;
    float2 uv_rb = calcUV(rgb_rb, color_matrix) * 256.0f;

    float2 uv = (uv_lt + uv_lb + uv_rt + uv_rb) * 0.25f;

    dstUV[0] = uv.x;
    dstUV[1] = uv.y;
    dstY0[0] = calcY(rgb_lt, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY0[1] = calcY(rgb_rt, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY1[0] = calcY(rgb_lb, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
    dstY1[1] = calcY(rgb_rb, color_matrix) * 245.0f;  // 245.0f is a magic number to ensure slight changes in luminosity are more visible
  }

  __global__ void detect_pitched_frame_change(
    const std::uint8_t *current,
    const std::uint8_t *previous,
    std::size_t pitch,
    std::size_t row_bytes,
    std::uint32_t chunks_per_row,
    std::uint32_t rows,
    std::uint32_t *changed
  ) {
    __shared__ std::uint32_t skip_block;
    if (threadIdx.x == 0) {
      // Once any earlier block finds a difference, later blocks can stop.
      skip_block = atomicAdd(changed, 0U);
    }
    __syncthreads();
    if (skip_block) {
      return;
    }

    constexpr std::size_t chunk_bytes = sizeof(uint4);
    const auto chunk = blockIdx.x * blockDim.x + threadIdx.x;
    const auto row = blockIdx.y;
    bool differs = false;
    if (chunk < chunks_per_row && row < rows) {
      const auto column = static_cast<std::size_t>(chunk) * chunk_bytes;
      const auto *current_row = current + static_cast<std::size_t>(row) * pitch;
      const auto *previous_row = previous + static_cast<std::size_t>(row) * pitch;

      if (column + chunk_bytes <= row_bytes) {
        const auto current_chunk = *reinterpret_cast<const uint4 *>(current_row + column);
        const auto previous_chunk = *reinterpret_cast<const uint4 *>(previous_row + column);
        differs = current_chunk.x != previous_chunk.x ||
                  current_chunk.y != previous_chunk.y ||
                  current_chunk.z != previous_chunk.z ||
                  current_chunk.w != previous_chunk.w;
      } else if (column < row_bytes) {
        for (auto byte = column; byte < row_bytes; ++byte) {
          differs |= current_row[byte] != previous_row[byte];
        }
      }
    }

    if (__syncthreads_or(differs) && threadIdx.x == 0) {
      atomicExch(changed, 1U);
    }
  }

  int compare_pitched_frames(
    const std::uint8_t *current,
    const std::uint8_t *previous,
    const std::size_t pitch,
    const std::size_t row_bytes,
    const std::size_t rows,
    std::uint32_t *changed_device,
    cudaStream_t stream,
    bool &changed
  ) {
    if (!current || !previous || !changed_device || !stream || !pitch || !row_bytes || !rows || row_bytes > pitch) {
      return -1;
    }

    constexpr std::size_t chunk_bytes = sizeof(uint4);
    constexpr unsigned int threads_per_block = 256;
    const auto chunks_per_row = div_align(row_bytes, chunk_bytes);
    // CUDA grid Y is limited to 65535 on supported devices. Video frame row
    // counts are far below that, but reject an invalid launch rather than
    // narrowing dimensions and risking a false duplicate.
    if (chunks_per_row > std::numeric_limits<std::uint32_t>::max() || rows > 65535U) {
      return -1;
    }
    const dim3 blocks(
      static_cast<unsigned int>(div_align(chunks_per_row, static_cast<std::size_t>(threads_per_block))),
      static_cast<unsigned int>(rows)
    );

    CU_CHECK(cudaMemsetAsync(changed_device, 0, sizeof(*changed_device), stream), "Couldn't reset CUDA duplicate-frame flag");
    detect_pitched_frame_change<<<blocks, threads_per_block, 0, stream>>>(
      current,
      previous,
      pitch,
      row_bytes,
      static_cast<std::uint32_t>(chunks_per_row),
      static_cast<std::uint32_t>(rows),
      changed_device
    );
    CU_CHECK(cudaGetLastError(), "Couldn't launch CUDA duplicate-frame comparison");

    std::uint32_t host_changed = 0;
    CU_CHECK(
      cudaMemcpyAsync(&host_changed, changed_device, sizeof(host_changed), cudaMemcpyDeviceToHost, stream),
      "Couldn't read CUDA duplicate-frame result"
    );
    CU_CHECK(cudaStreamSynchronize(stream), "Couldn't synchronize CUDA duplicate-frame comparison");
    changed = host_changed != 0;
    return 0;
  }

  int tex_t::copy(std::uint8_t *src, int height, int pitch) {
    CU_CHECK(cudaMemcpy2DToArray(array, 0, 0, src, pitch, pitch, height, cudaMemcpyDeviceToDevice), "Couldn't copy to cuda array from deviceptr");

    return 0;
  }

  std::optional<tex_t> tex_t::make(int height, int pitch) {
    tex_t tex;

    auto format = cudaCreateChannelDesc<uchar4>();
    CU_CHECK_OPT(cudaMallocArray(&tex.array, &format, pitch, height, cudaArrayDefault), "Couldn't allocate cuda array");

    cudaResourceDesc res {};
    res.resType = cudaResourceTypeArray;
    res.res.array.array = tex.array;

    cudaTextureDesc desc {};

    desc.readMode = cudaReadModeNormalizedFloat;
    desc.filterMode = cudaFilterModePoint;
    desc.normalizedCoords = false;

    std::fill_n(std::begin(desc.addressMode), 2, cudaAddressModeClamp);

    CU_CHECK_OPT(cudaCreateTextureObject(&tex.texture.point, &res, &desc, nullptr), "Couldn't create cuda texture that uses point interpolation");

    desc.filterMode = cudaFilterModeLinear;

    CU_CHECK_OPT(cudaCreateTextureObject(&tex.texture.linear, &res, &desc, nullptr), "Couldn't create cuda texture that uses linear interpolation");

    return tex;
  }

  tex_t::tex_t():
      array {},
      texture {INVALID_TEXTURE, INVALID_TEXTURE} {
  }

  tex_t::tex_t(tex_t &&other):
      array {other.array},
      texture {other.texture} {
    other.array = 0;
    other.texture.point = INVALID_TEXTURE;
    other.texture.linear = INVALID_TEXTURE;
  }

  tex_t &tex_t::operator=(tex_t &&other) {
    std::swap(array, other.array);
    std::swap(texture, other.texture);

    return *this;
  }

  tex_t::~tex_t() {
    if (texture.point != INVALID_TEXTURE) {
      CU_CHECK_IGNORE(cudaDestroyTextureObject(texture.point), "Couldn't deallocate cuda texture that uses point interpolation");

      texture.point = INVALID_TEXTURE;
    }

    if (texture.linear != INVALID_TEXTURE) {
      CU_CHECK_IGNORE(cudaDestroyTextureObject(texture.linear), "Couldn't deallocate cuda texture that uses linear interpolation");

      texture.linear = INVALID_TEXTURE;
    }

    if (array) {
      CU_CHECK_IGNORE(cudaFreeArray(array), "Couldn't deallocate cuda array");

      array = cudaArray_t {};
    }
  }

  sws_t::sws_t(int in_width, int in_height, int out_width, int out_height, int pitch, int threadsPerBlock, ptr_t &&color_matrix):
      threadsPerBlock {threadsPerBlock},
      color_matrix {std::move(color_matrix)} {
    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(out_width / (float) in_width, out_height / (float) in_height);
    auto out_width_f = in_width * scalar;
    auto out_height_f = in_height * scalar;

    // result is always positive
    auto offsetX_f = (out_width - out_width_f) / 2;
    auto offsetY_f = (out_height - out_height_f) / 2;

    viewport.width = out_width_f;
    viewport.height = out_height_f;

    viewport.offsetX = offsetX_f;
    viewport.offsetY = offsetY_f;

    scale = 1.0f / scalar;
  }

  std::optional<sws_t> sws_t::make(int in_width, int in_height, int out_width, int out_height, int pitch) {
    cudaDeviceProp props;
    int device;
    CU_CHECK_OPT(cudaGetDevice(&device), "Couldn't get cuda device");
    CU_CHECK_OPT(cudaGetDeviceProperties(&props, device), "Couldn't get cuda device properties");

    auto ptr = make_ptr<cuda_color_t>();
    if (!ptr) {
      return std::nullopt;
    }

    return std::make_optional<sws_t>(in_width, in_height, out_width, out_height, pitch, props.maxThreadsPerMultiProcessor / props.maxBlocksPerMultiProcessor, std::move(ptr));
  }

  int sws_t::convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream) {
    return convert(Y, UV, pitchY, pitchUV, texture, stream, viewport);
  }

  int sws_t::convert(std::uint8_t *Y, std::uint8_t *UV, std::uint32_t pitchY, std::uint32_t pitchUV, cudaTextureObject_t texture, stream_t::pointer stream, const viewport_t &viewport) {
    int threadsX = viewport.width / 2;
    int threadsY = viewport.height / 2;

    dim3 block(threadsPerBlock);
    dim3 grid(div_align(threadsX, threadsPerBlock), threadsY);

    RGBA_to_NV12<<<grid, block, 0, stream>>>(texture, Y, UV, pitchY, pitchUV, scale, viewport, (cuda_color_t *) color_matrix.get());

    return CU_CHECK_IGNORE(cudaGetLastError(), "RGBA_to_NV12 failed");
  }

  void sws_t::apply_colorspace(const video::sunshine_colorspace_t &colorspace) {
    auto color_p = video::color_vectors_from_colorspace(colorspace, true);
    CU_CHECK_IGNORE(cudaMemcpy(color_matrix.get(), color_p, sizeof(video::color_t), cudaMemcpyHostToDevice), "Couldn't copy color matrix to cuda");
  }

  int sws_t::load_ram(platf::img_t &img, cudaArray_t array) {
    return CU_CHECK_IGNORE(cudaMemcpy2DToArray(array, 0, 0, img.data, img.row_pitch, img.width * img.pixel_pitch, img.height, cudaMemcpyHostToDevice), "Couldn't copy to cuda array");
  }

}  // namespace cuda
