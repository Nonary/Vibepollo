/**
 * @file src/platform/windows/pyrowave_d3d11_core.cpp
 * @brief PyroWave encoding of Direct3D 11 frames through PyroWave's Vulkan C API.
 *
 * The D3D11 conversion half follows dimizago's Vibepollo PyroWave encoder
 * (GPLv3, https://github.com/dimizago/Vibepollo, branch pyrowave), which builds on
 * andygrundman's Windows PyroWave host. The encoder half replaces that port's
 * AMD-only Direct3D 12 codec with upstream PyroWave's Vulkan C API.
 */
#include "pyrowave_d3d11_core.h"

#include "src/pyrowave_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <span>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vulkan/vulkan_core.h>
// vulkan_core.h must precede pyrowave.h.
#include <pyrowave.h>

using Microsoft::WRL::ComPtr;

namespace pyrowave::d3d11 {
  namespace {
    using clock_type = std::chrono::steady_clock;

    constexpr DWORD CAPTURE_MUTEX_TIMEOUT_MS = 3000;

    // Matches params_cbuffer (b2) in convert_pyrowave_cs.hlsl.
    struct convert_params_t {
      std::uint32_t luma_size[2];
      float content_offset[2];
      float content_scale[2];
      std::uint32_t blank;
      std::int32_t rotate_texture_steps;
    };

    static_assert(sizeof(convert_params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

    // Matches sdr_to_pq_params_cbuffer (b1) in include/convert_sdr_to_pq_base.hlsl.
    struct sdr_to_pq_params_t {
      float sdr_white_nits;
      float padding[3];
    };

    static_assert(sizeof(pyrowave_packet) == sizeof(pyrowave::policy::packet_t), "pyrowave_packet layout changed");
    static_assert(offsetof(pyrowave_packet, offset) == offsetof(pyrowave::policy::packet_t, offset), "pyrowave_packet layout changed");
    static_assert(offsetof(pyrowave_packet, size) == offsetof(pyrowave::policy::packet_t, size), "pyrowave_packet layout changed");

    std::string hresult_string(HRESULT hr) {
      char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
      return buffer;
    }

    const char *pyrowave_result_string(pyrowave_result result) {
      switch (result) {
        case PYROWAVE_SUCCESS:
          return "success";
        case PYROWAVE_TIMEOUT:
          return "timeout";
        case PYROWAVE_ERROR_GENERIC:
          return "generic error";
        case PYROWAVE_ERROR_INVALID_ARGUMENT:
          return "invalid argument";
        case PYROWAVE_ERROR_OUT_OF_HOST_MEMORY:
          return "out of host memory";
        case PYROWAVE_ERROR_OUT_OF_DEVICE_MEMORY:
          return "out of device memory";
        case PYROWAVE_ERROR_NO_VULKAN:
          return "no Vulkan";
        case PYROWAVE_ERROR_NOT_IMPLEMENTED:
          return "not implemented";
        case PYROWAVE_ERROR_UNSUPPORTED_EXTERNAL_HANDLE:
          return "unsupported external handle";
        case PYROWAVE_ERROR_FAILED_EXTERNAL_HANDLE:
          return "failed external handle";
        default:
          return "unknown error";
      }
    }

    double elapsed_ms(clock_type::time_point start, clock_type::time_point end) {
      return std::chrono::duration<double, std::milli>(end - start).count();
    }

    pyrowave_result create_device_for(const LUID *luid, pyrowave_device *device) {
      if (luid) {
        return pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, reinterpret_cast<const pyrowave_luid *>(luid), device);
      }
      return pyrowave_create_default_device(device);
    }

    static_assert(sizeof(LUID) == sizeof(pyrowave_luid), "LUID and pyrowave_luid differ");
  }  // namespace

  struct core_t::impl_t {
    core_config_t config;
    log_fn log;

    // Conversion, on a private D3D11 device on the capture adapter.
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    std::array<ComPtr<ID3D11Texture2D>, 3> planes11;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 3> plane_uavs;
    ComPtr<ID3D11Buffer> color_matrix;
    ComPtr<ID3D11Buffer> sdr_to_pq_params;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11SamplerState> sampler;
    std::map<transfer_e, ComPtr<ID3D11ComputeShader>> shaders;
    convert_params_t last_params {};
    bool params_valid = false;

    // Handoff to PyroWave: the planes and a fence signalled once they are written.
    ComPtr<ID3D11Fence> convert_fence;
    std::uint64_t convert_value = 0;

    // Encoding.
    pyrowave_device pw_device = nullptr;
    pyrowave_encoder pw_encoder = nullptr;
    std::array<pyrowave_image, 3> pw_planes {};
    std::array<pyrowave_gpu_external_reference, 3> pw_plane_refs {};
    pyrowave_gpu_buffers pw_buffers {};
    pyrowave_sync_object pw_fence = nullptr;

    std::vector<pyrowave_packet> packets;
    std::vector<std::uint8_t> scratch;

    ~impl_t() {
      if (pw_encoder) {
        pyrowave_encoder_destroy(pw_encoder);
      }
      for (auto image : pw_planes) {
        if (image) {
          pyrowave_image_destroy(image);
        }
      }
      if (pw_fence) {
        pyrowave_sync_object_destroy(pw_fence);
      }
      if (pw_device) {
        pyrowave_device_destroy(pw_device);
      }
    }

    void error(const std::string &message) const {
      if (log) {
        log(3, "PyroWave: " + message);
      }
    }

    void warning(const std::string &message) const {
      if (log) {
        log(2, "PyroWave: " + message);
      }
    }

    void info(const std::string &message) const {
      if (log) {
        log(1, "PyroWave: " + message);
      }
    }

    bool create_d3d11(IDXGIAdapter *adapter) {
      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      ComPtr<ID3D11Device> device;
      ComPtr<ID3D11DeviceContext> context;
      HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context);
      if (FAILED(hr)) {
        error("D3D11CreateDevice failed [" + hresult_string(hr) + "]");
        return false;
      }
      if (FAILED(device.As(&device11)) || FAILED(context.As(&context11))) {
        error("the conversion device lacks ID3D11Device5/ID3D11DeviceContext4 (Windows 10 1703 or later is required)");
        return false;
      }

      ComPtr<IDXGIDevice> dxgi_device;
      if (SUCCEEDED(device.As(&dxgi_device))) {
        // Same priority as Sunshine's other encoder devices; needs elevation to take effect.
        (void) dxgi_device->SetGPUThreadPriority(7);
      }
      return true;
    }

    bool create_pyrowave(IDXGIAdapter *adapter) {
      DXGI_ADAPTER_DESC desc {};
      HRESULT hr = adapter->GetDesc(&desc);
      if (FAILED(hr)) {
        error("IDXGIAdapter::GetDesc failed [" + hresult_string(hr) + "]");
        return false;
      }

      auto result = create_device_for(&desc.AdapterLuid, &pw_device);
      if (result != PYROWAVE_SUCCESS) {
        error(std::string("no Vulkan device matches the capture adapter: ") + pyrowave_result_string(result));
        return false;
      }
      if (!pyrowave_device_confirm_interop_support(pw_device)) {
        error("the Vulkan driver cannot import Direct3D 11 textures and fences");
        return false;
      }
      // Sunshine is a separate process from the game, which keeps the graphics
      // queue busy; async compute (when the GPU has it) keeps the encode clear of it.
      (void) pyrowave_device_set_queue_type(pw_device, VK_QUEUE_COMPUTE_BIT);

      pyrowave_encoder_create_info encoder_info {};
      encoder_info.device = pw_device;
      encoder_info.width = config.width;
      encoder_info.height = config.height;
      encoder_info.chroma = config.yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      result = pyrowave_encoder_create(&encoder_info, &pw_encoder);
      if (result != PYROWAVE_SUCCESS) {
        error(std::string("encoder creation failed: ") + pyrowave_result_string(result));
        return false;
      }
      return true;
    }

    bool create_planes() {
      const DXGI_FORMAT dxgi_format = config.ten_bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
      const VkFormat vk_format = config.ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;

      for (int i = 0; i < 3; i++) {
        const bool full = i == 0 || config.yuv444;
        const UINT width = UINT(full ? config.width : config.width / 2);
        const UINT height = UINT(full ? config.height : config.height / 2);

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = dxgi_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &planes11[i]);
        if (FAILED(hr)) {
          error("plane texture creation failed [" + hresult_string(hr) + "]");
          return false;
        }
        hr = device11->CreateUnorderedAccessView(planes11[i].Get(), nullptr, &plane_uavs[i]);
        if (FAILED(hr)) {
          error("plane UAV creation failed [" + hresult_string(hr) + "]");
          return false;
        }

        ComPtr<IDXGIResource1> resource;
        HANDLE handle = nullptr;
        hr = planes11[i].As(&resource);
        if (SUCCEEDED(hr)) {
          hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        }
        if (FAILED(hr)) {
          error("sharing plane " + std::to_string(i) + " failed [" + hresult_string(hr) + "]");
          return false;
        }

        VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = vk_format;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        pyrowave_image_create_info import_info {};
        import_info.device = pw_device;
        import_info.external_handle = pyrowave_os_handle(handle);
        import_info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        import_info.image_create_info = &image_info;
        auto result = pyrowave_image_create(&import_info, &pw_planes[i]);
        if (result != PYROWAVE_SUCCESS) {
          // PyroWave takes ownership of the handle only when the import succeeds.
          CloseHandle(handle);
          error("importing plane " + std::to_string(i) + " into Vulkan failed: " + pyrowave_result_string(result));
          return false;
        }

        result = pyrowave_image_get_image_view(pw_planes[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT, &pw_buffers.planes[i]);
        if (result != PYROWAVE_SUCCESS) {
          error("plane " + std::to_string(i) + " view creation failed: " + pyrowave_result_string(result));
          return false;
        }
        pw_plane_refs[i] = {pw_planes[i], VK_QUEUE_FAMILY_EXTERNAL};
      }
      return true;
    }

    bool create_fence() {
      HRESULT hr = device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&convert_fence));
      if (FAILED(hr)) {
        error("shared fence creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      HANDLE handle = nullptr;
      hr = convert_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
      if (FAILED(hr)) {
        error("sharing the fence failed [" + hresult_string(hr) + "]");
        return false;
      }

      pyrowave_sync_object_create_info sync_info {};
      sync_info.device = pw_device;
      sync_info.external_handle = pyrowave_os_handle(handle);
      // A D3D11 fence is a D3D12 fence on Windows 10+; it must be imported as a timeline.
      sync_info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      sync_info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
      auto result = pyrowave_sync_object_create(&sync_info, &pw_fence);
      if (result != PYROWAVE_SUCCESS) {
        CloseHandle(handle);
        error(std::string("importing the fence into Vulkan failed: ") + pyrowave_result_string(result));
        return false;
      }
      return true;
    }

    bool create_constants() {
      D3D11_BUFFER_DESC cb {};
      cb.ByteWidth = sizeof(color_matrix_t);
      cb.Usage = D3D11_USAGE_IMMUTABLE;
      cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      D3D11_SUBRESOURCE_DATA init {&config.color_matrix, 0, 0};
      HRESULT hr = device11->CreateBuffer(&cb, &init, &color_matrix);
      if (FAILED(hr)) {
        error("colour matrix buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      sdr_to_pq_params_t sdr {config.sdr_white_nits, {}};
      cb.ByteWidth = sizeof(sdr);
      init = {&sdr, 0, 0};
      hr = device11->CreateBuffer(&cb, &init, &sdr_to_pq_params);
      if (FAILED(hr)) {
        error("SDR-to-PQ buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      cb.ByteWidth = sizeof(convert_params_t);
      cb.Usage = D3D11_USAGE_DEFAULT;
      hr = device11->CreateBuffer(&cb, nullptr, &params);
      if (FAILED(hr)) {
        error("parameter buffer creation failed [" + hresult_string(hr) + "]");
        return false;
      }

      D3D11_SAMPLER_DESC sd {};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      hr = device11->CreateSamplerState(&sd, &sampler);
      if (FAILED(hr)) {
        error("sampler creation failed [" + hresult_string(hr) + "]");
        return false;
      }
      return true;
    }

    ID3D11ComputeShader *shader_for(transfer_e transfer) {
      auto &shader = shaders[transfer];
      if (shader) {
        return shader.Get();
      }

      std::vector<D3D_SHADER_MACRO> macros;
      switch (transfer) {
        case transfer_e::pq:
          macros.push_back({"PQ", "1"});
          break;
        case transfer_e::linear_sdr:
          macros.push_back({"LINEAR", "1"});
          break;
        case transfer_e::sdr_to_pq:
          macros.push_back({"SDR_TO_PQ", "1"});
          break;
        case transfer_e::unorm:
          break;
      }
      if (!config.yuv444) {
        macros.push_back({"SUBSAMPLE_420", "1"});
      }
      macros.push_back({nullptr, nullptr});

      ComPtr<ID3DBlob> code;
      ComPtr<ID3DBlob> messages;
      HRESULT hr = D3DCompileFromFile(config.shader_path.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
      if (FAILED(hr)) {
        std::string detail = messages ? std::string(static_cast<const char *>(messages->GetBufferPointer()), messages->GetBufferSize()) : std::string("no compiler output");
        error("compiling convert_pyrowave_cs.hlsl failed [" + hresult_string(hr) + "]: " + detail);
        return nullptr;
      }
      hr = device11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
      if (FAILED(hr)) {
        error("CreateComputeShader failed [" + hresult_string(hr) + "]");
        shader.Reset();
        return nullptr;
      }
      return shader.Get();
    }

    void update_params(const source_t *source) {
      convert_params_t p {};
      p.luma_size[0] = std::uint32_t(config.width);
      p.luma_size[1] = std::uint32_t(config.height);
      p.blank = source ? 0 : 1;
      if (source && source->width && source->height) {
        p.rotate_texture_steps = source->rotate_texture_steps;
        // Letterbox the picture into the stream extent, preserving its aspect ratio.
        // A quarter turn swaps the displayed width and height.
        const bool swap = (source->rotate_texture_steps & 1) != 0;
        const float source_w = float(swap ? source->height : source->width);
        const float source_h = float(swap ? source->width : source->height);
        const float scale = std::min(float(config.width) / source_w, float(config.height) / source_h);
        const float content_w = source_w * scale;
        const float content_h = source_h * scale;
        p.content_offset[0] = (float(config.width) - content_w) * 0.5f;
        p.content_offset[1] = (float(config.height) - content_h) * 0.5f;
        p.content_scale[0] = 1.0f / content_w;
        p.content_scale[1] = 1.0f / content_h;
      }
      if (!params_valid || std::memcmp(&p, &last_params, sizeof(p)) != 0) {
        context11->UpdateSubresource(params.Get(), 0, nullptr, &p, 0, 0);
        last_params = p;
        params_valid = true;
      }
    }

    result_e convert(const source_t &source) {
      const bool blank = source.srv == nullptr;
      auto *shader = shader_for(blank ? transfer_e::unorm : source.transfer);
      if (!shader) {
        return result_e::failed;
      }
      update_params(blank ? nullptr : &source);

      // Synchronize with the capture side, with a finite timeout so display re-init or
      // device loss cannot wedge the stream.
      if (!blank && source.mutex) {
        const HRESULT status = source.mutex->AcquireSync(0, CAPTURE_MUTEX_TIMEOUT_MS);
        if (status == static_cast<HRESULT>(WAIT_TIMEOUT)) {
          warning("timed out acquiring the capture mutex; skipping the frame");
          return result_e::skipped;
        }
        if (status != S_OK && status != static_cast<HRESULT>(WAIT_ABANDONED)) {
          warning("acquiring the capture mutex failed [" + hresult_string(status) + "]; skipping the frame");
          return result_e::skipped;
        }
      }

      ID3D11Buffer *cbs[] = {color_matrix.Get(), sdr_to_pq_params.Get(), params.Get()};
      ID3D11ShaderResourceView *srv = source.srv;
      ID3D11UnorderedAccessView *uavs[] = {plane_uavs[0].Get(), plane_uavs[1].Get(), plane_uavs[2].Get()};
      ID3D11SamplerState *samplers[] = {sampler.Get()};
      context11->CSSetShader(shader, nullptr, 0);
      context11->CSSetConstantBuffers(0, 3, cbs);
      context11->CSSetShaderResources(0, 1, &srv);
      context11->CSSetSamplers(0, 1, samplers);
      context11->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

      const UINT work_w = UINT(config.yuv444 ? config.width : (config.width + 1) / 2);
      const UINT work_h = UINT(config.yuv444 ? config.height : (config.height + 1) / 2);
      context11->Dispatch((work_w + 7) / 8, (work_h + 7) / 8, 1);

      ID3D11ShaderResourceView *null_srv = nullptr;
      ID3D11UnorderedAccessView *null_uavs[3] = {};
      context11->CSSetShaderResources(0, 1, &null_srv);
      context11->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);

      // The frame is in the planes as far as D3D11 ordering goes, so the capture
      // side can have its texture back.
      if (!blank && source.mutex) {
        source.mutex->ReleaseSync(0);
      }

      const HRESULT hr = context11->Signal(convert_fence.Get(), ++convert_value);
      if (FAILED(hr)) {
        error("signalling the conversion fence failed [" + hresult_string(hr) + "]");
        return result_e::failed;
      }
      // Submit the conversion and the signal now: the Vulkan queue waits for the value.
      context11->Flush();
      return result_e::ok;
    }
  };

  core_t::core_t() = default;
  core_t::~core_t() = default;

  std::unique_ptr<core_t> core_t::create(IDXGIAdapter *adapter, const core_config_t &config, log_fn log) {
    auto core = std::unique_ptr<core_t>(new core_t());
    core->impl = std::make_unique<impl_t>();
    auto &impl = *core->impl;
    impl.config = config;
    impl.log = std::move(log);

    if (!adapter) {
      impl.error("no capture adapter");
      return nullptr;
    }
    if (config.width <= 0 || config.height <= 0 ||
        (!config.yuv444 && ((config.width & 1) || (config.height & 1)))) {
      impl.error("invalid encode size " + std::to_string(config.width) + "x" + std::to_string(config.height));
      return nullptr;
    }

    if (!impl.create_d3d11(adapter) ||
        !impl.create_pyrowave(adapter) ||
        !impl.create_planes() ||
        !impl.create_fence() ||
        !impl.create_constants()) {
      return nullptr;
    }

    // Compile the variants used by blank frames and the common SDR capture up front,
    // so a shader error ends session setup instead of the first streamed frame.
    if (!impl.shader_for(transfer_e::unorm)) {
      return nullptr;
    }
    return core;
  }

  ID3D11Device *core_t::device() const {
    return impl->device11.Get();
  }

  result_e core_t::encode(
    const source_t &source,
    std::size_t max_bitstream_bytes,
    const framing_params_t &framing,
    std::vector<std::uint8_t> &out,
    frame_stats_t *stats
  ) {
    auto &state = *impl;
    frame_stats_t local_stats;
    auto &s = stats ? *stats : local_stats;
    s = {};

    const auto convert_start = clock_type::now();
    const auto converted = state.convert(source);
    if (converted != result_e::ok) {
      return converted;
    }
    const auto encode_start = clock_type::now();
    s.convert_ms = elapsed_ms(convert_start, encode_start);

    pyrowave_gpu_sync_operation acquire {};
    acquire.images = state.pw_plane_refs.data();
    acquire.num_images = state.pw_plane_refs.size();
    acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(state.pw_fence);
    acquire.sync.value = state.convert_value;

    // No release semaphore: the encode is synchronous. The CPU waits for it below
    // (get_mapped_raw_bitstream) before D3D11 can overwrite the planes again.
    pyrowave_gpu_sync_operation release {};
    release.images = state.pw_plane_refs.data();
    release.num_images = state.pw_plane_refs.size();

    pyrowave_rate_control rate {};
    rate.maximum_bitstream_size = std::clamp<std::size_t>(max_bitstream_bytes, 4096, std::numeric_limits<std::uint32_t>::max() & ~3u);

    auto result = pyrowave_encoder_encode_gpu_synchronous(state.pw_encoder, &acquire, &release, &state.pw_buffers, &rate);
    if (result != PYROWAVE_SUCCESS) {
      state.error(std::string("encode failed: ") + pyrowave_result_string(result));
      return result_e::failed;
    }

    // Waits for the encode and sizes the packetizer output from the bitstream buffer.
    const void *mapped_bitstream = nullptr;
    const void *mapped_metadata = nullptr;
    std::size_t mapped_bitstream_size = 0;
    std::size_t mapped_metadata_size = 0;
    result = pyrowave_encoder_get_mapped_raw_bitstream(state.pw_encoder, &mapped_bitstream, &mapped_bitstream_size, &mapped_metadata, &mapped_metadata_size);
    if (result != PYROWAVE_SUCCESS) {
      state.error(std::string("reading the encoded frame failed: ") + pyrowave_result_string(result));
      return result_e::failed;
    }
    const auto packetize_start = clock_type::now();
    s.encode_ms = elapsed_ms(encode_start, packetize_start);

    // Record framing packs the records itself (policy::write_record_frame), so it takes
    // the whole frame as one packet: the sequence header, then every block.
    const bool records = framing.framing == pyrowave::policy::framing_e::records;
    const std::size_t boundary = records ? std::size_t(std::numeric_limits<std::uint32_t>::max()) : pyrowave::protocol::LENGTH_PREFIXED_PACKET_BOUNDARY;
    const std::size_t first_packet_reserve = 0;

    std::size_t num_packets = 0;
    result = pyrowave_encoder_compute_num_packets_with_padding(state.pw_encoder, boundary, first_packet_reserve, &num_packets);
    if (result != PYROWAVE_SUCCESS) {
      state.error(std::string("computing packets failed: ") + pyrowave_result_string(result));
      return result_e::failed;
    }

    // The packetizer copies the sequence header and every coded block; the raw
    // bitstream buffer bounds the blocks.
    const std::size_t scratch_bytes = mapped_bitstream_size + 64;
    if (state.scratch.size() < scratch_bytes) {
      state.scratch.resize(scratch_bytes);
    }
    state.packets.resize(std::max<std::size_t>(num_packets, 1));
    std::size_t out_packets = 0;
    result = pyrowave_encoder_packetize_with_padding(state.pw_encoder, state.packets.data(), boundary, first_packet_reserve, &out_packets, state.scratch.data(), state.scratch.size());
    if (result != PYROWAVE_SUCCESS || out_packets > state.packets.size()) {
      state.error(std::string("packetizing failed: ") + pyrowave_result_string(result));
      return result_e::failed;
    }

    std::span<const pyrowave::policy::packet_t> packet_span(reinterpret_cast<const pyrowave::policy::packet_t *>(state.packets.data()), out_packets);
    for (const auto &packet : packet_span) {
      s.encoded_bytes += packet.size;
    }

    const std::size_t frame_start = out.size();
    if (records) {
      if (out_packets != 1) {
        state.error("packetizing the frame produced " + std::to_string(out_packets) + " packets instead of one");
        return result_e::failed;
      }
      const auto &frame = packet_span[0];
      auto written = pyrowave::policy::write_record_frame(std::span<const std::uint8_t>(state.scratch.data() + frame.offset, frame.size), framing.shard_payload, out);
      if (!written) {
        state.error("the encoder produced a malformed bitstream");
        return result_e::failed;
      }
      s.records = *written;
    } else {
      pyrowave::policy::write_length_prefixed_frame(packet_span, state.scratch.data(), out);
    }
    s.frame_bytes = out.size() - frame_start;
    s.packets = out_packets;
    s.packetize_ms = elapsed_ms(packetize_start, clock_type::now());
    return result_e::ok;
  }

  bool probe(const LUID *luid, std::string &detail) {
    pyrowave_device device = nullptr;
    auto result = create_device_for(luid, &device);
    if (result != PYROWAVE_SUCCESS) {
      detail = std::string("no usable Vulkan device: ") + pyrowave_result_string(result);
      return false;
    }

    bool ok = true;
    if (!pyrowave_device_confirm_interop_support(device)) {
      detail = "the Vulkan driver cannot import Direct3D 11 textures and fences";
      ok = false;
    }

    if (ok) {
      // Encoder creation checks the subgroup, int16 and 8-bit storage features.
      pyrowave_encoder_create_info info {};
      info.device = device;
      info.width = 256;
      info.height = 256;
      info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
      pyrowave_encoder encoder = nullptr;
      result = pyrowave_encoder_create(&info, &encoder);
      if (result != PYROWAVE_SUCCESS) {
        detail = std::string("the GPU cannot run the encoder: ") + pyrowave_result_string(result);
        ok = false;
      } else {
        pyrowave_encoder_destroy(encoder);
      }
    }

    pyrowave_device_destroy(device);
    return ok;
  }
}  // namespace pyrowave::d3d11
