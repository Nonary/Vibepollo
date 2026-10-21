// Exercise the real D3D11 texture/fence imports, including Intel's NT-handle path.
#include "src/platform/windows/pyrowave_d3d11_core.h"

#include <vulkan/vulkan_core.h>
#include <pyrowave.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
  void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  std::uint32_t read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated framing");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      value |= std::uint32_t(bytes[offset++]) << (8 * i);
    }
    return value;
  }
}

int main(int argc, char **argv) {
  try {
    ComPtr<IDXGIFactory1> factory;
    require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory failed");
    unsigned cases = 0;
    const unsigned requested_vendor = argc > 1 ? std::stoul(argv[1], nullptr, 16) : 0;
    const bool request_d3d12 = argc > 2 && std::string(argv[2]) == "d3d12";
    for (unsigned index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc {};
      require(SUCCEEDED(adapter->GetDesc1(&desc)), "adapter description failed");
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (requested_vendor && requested_vendor != desc.VendorId)) {
        continue;
      }
      std::string detail;
      if (!pyrowave::d3d11::probe(&desc.AdapterLuid, detail)) {
        std::cerr << "Adapter " << std::hex << desc.VendorId << std::dec << ": " << detail << '\n';
        continue;
      }
      ComPtr<ID3D11Device> capture_device;
      ComPtr<ID3D11DeviceContext> capture_context;
      require(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &capture_device, nullptr, &capture_context)), "capture device failed");
      pyrowave_device device = nullptr;
      require(pyrowave_create_device_by_compat(0, 0, nullptr, nullptr, reinterpret_cast<const pyrowave_luid *>(&desc.AdapterLuid), &device) == PYROWAVE_SUCCESS, "decoder device failed");
      struct device_guard_t {
        pyrowave_device device;
        ~device_guard_t() { pyrowave_device_destroy(device); }
      } device_guard {device};

      for (bool ten_bit : {false, true}) {
        for (bool yuv444 : {false, true}) {
          pyrowave::d3d11::core_config_t config;
          config.width = config.height = 256;
          config.ten_bit = ten_bit;
          config.yuv444 = yuv444;
          config.experimental_d3d12_conversion = request_d3d12;
          config.shader_path = PYROWAVE_TEST_SHADER_PATH;
          // Distinct plane values verify conversion and the imported contents.
          config.color_matrix.color_vec_y[0] = config.color_matrix.color_vec_u[0] = config.color_matrix.color_vec_v[0] = 0.5f;
          config.color_matrix.color_vec_y[3] = 0.125f;
          config.color_matrix.color_vec_u[3] = 0.375f;
          config.color_matrix.color_vec_v[3] = 0.625f;
          config.color_matrix.range_y[0] = config.color_matrix.range_uv[0] = 1.0f;
          auto core = pyrowave::d3d11::core_t::create(adapter.Get(), config, [](int, const std::string &message) { std::cerr << message << '\n'; });
          require(bool(core), "D3D11 encoder initialization failed");
          require(core->using_d3d12_conversion() == request_d3d12, "requested conversion backend was not selected");
          if (request_d3d12) {
            // Only one prototype generation may own an adapter. A second
            // session falls back before submitting work to shared resources.
            auto second = pyrowave::d3d11::core_t::create(adapter.Get(), config, {});
            require(bool(second) && !second->using_d3d12_conversion(), "prototype adapter admission was not bounded");
          }
          pyrowave_decoder_create_info info {};
          info.device = device;
          info.width = info.height = 256;
          info.chroma = yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
          pyrowave_decoder decoder = nullptr;
          require(pyrowave_decoder_create(&info, &decoder) == PYROWAVE_SUCCESS, "decoder creation failed");
          struct decoder_guard_t {
            pyrowave_decoder decoder;
            ~decoder_guard_t() { pyrowave_decoder_destroy(decoder); }
          } decoder_guard {decoder};

          for (unsigned frame = 0; frame < 16; ++frame) {
            // Change the input to expose missing cross-API synchronization.
            const unsigned red = (frame % 4) * 32;
            const unsigned width = (frame % 2) ? 128 : 256;
            const bool blank = frame == 15;
            std::vector<std::uint32_t> pixels(width * 256, 0xff000000u | red);
            D3D11_TEXTURE2D_DESC texture_info {};
            texture_info.Width = width;
            texture_info.Height = 256;
            texture_info.MipLevels = texture_info.ArraySize = 1;
            texture_info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_info.SampleDesc.Count = 1;
            texture_info.Usage = D3D11_USAGE_DEFAULT;
            texture_info.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            texture_info.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            D3D11_SUBRESOURCE_DATA initial {pixels.data(), width * 4, 0};
            // A genuine shared owner survives frame/caller exception unwind.
            // The only writer above encode uses this resource's capture mutex;
            // no unguarded write or replacement occurs while the lease is retained.
            struct source_owner_t {
              ComPtr<ID3D11Texture2D> texture;
              ComPtr<IDXGIKeyedMutex> mutex;
            };
            auto source_owner = std::make_shared<source_owner_t>();
            auto &texture = source_owner->texture;
            ComPtr<ID3D11Texture2D> opened;
            ComPtr<ID3D11ShaderResourceView> view;
            auto &capture_mutex = source_owner->mutex;
            ComPtr<IDXGIKeyedMutex> encode_mutex;
            ComPtr<IDXGIResource1> shared;
            ComPtr<ID3D11Device1> encode_device;
            require(SUCCEEDED(capture_device->CreateTexture2D(&texture_info, nullptr, &texture)), "source texture failed");
            require(SUCCEEDED(texture.As(&capture_mutex)), "capture mutex missing");
            require(capture_mutex->AcquireSync(0, 100) == S_OK, "capture ownership failed");
            capture_context->UpdateSubresource(texture.Get(), 0, nullptr, initial.pSysMem, initial.SysMemPitch, 0);
            require(capture_mutex->ReleaseSync(0) == S_OK, "capture release failed");
            capture_context->Flush();
            require(SUCCEEDED(texture.As(&shared)), "source sharing interface failed");
            HANDLE handle = nullptr;
            require(SUCCEEDED(shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)), "source sharing failed");
            const auto device_result = core->device()->QueryInterface(IID_PPV_ARGS(&encode_device));
            const auto open_result = SUCCEEDED(device_result) ? encode_device->OpenSharedResource1(handle, IID_PPV_ARGS(&opened)) : device_result;
            CloseHandle(handle);
            require(SUCCEEDED(open_result), "opening capture on conversion device failed");
            require(SUCCEEDED(opened.As(&encode_mutex)), "encode mutex missing");
            require(SUCCEEDED(core->device()->CreateShaderResourceView(opened.Get(), nullptr, &view)), "source view failed");
            pyrowave::d3d11::source_t source;
            source.lease = pyrowave::d3d11::source_lease_e::keyed_standalone;
            source.capture_owner = source_owner;
            source.capture_texture = texture.Get();
            source.capture_mutex = capture_mutex.Get();
            source.srv = blank ? nullptr : view.Get();
            source.mutex = blank ? nullptr : encode_mutex.Get();
            source.width = width;
            source.height = 256;
            source.rotate_texture_steps = int(frame % 4);
            pyrowave::d3d11::framing_params_t framing;
            framing.framing = pyrowave::policy::framing_e::length_prefixed;
            std::vector<std::uint8_t> bits;
            // A too-small budget skips encode only after conversion. The next
            // call must still retire/reset its producer safely and use fresh data.
            require(core->encode(source, 0, framing, bits) == pyrowave::d3d11::result_e::skipped, "zero-budget frame was not skipped");
            require(bits.empty(), "skipped frame produced bytes");
            require(core->encode(source, 65536, framing, bits) == pyrowave::d3d11::result_e::ok, "encoding failed");
            require(core->using_d3d12_conversion() == request_d3d12, "supported source silently changed backend");
            require(capture_mutex->AcquireSync(0, 100) == S_OK, "conversion did not release capture ownership");
            require(capture_mutex->ReleaseSync(0) == S_OK, "capture ownership probe release failed");
            pyrowave_decoder_clear(decoder);
            std::size_t offset = 0;
            const auto count = read_u32(bits, offset);
            require(count > 0, "no encoded packets");
            for (unsigned packet = 0; packet < count; ++packet) {
              const auto size = read_u32(bits, offset);
              require(size <= bits.size() - offset, "packet exceeds frame");
              require(pyrowave_decoder_push_packet(decoder, bits.data() + offset, size) == PYROWAVE_SUCCESS, "packet rejected");
              offset += size;
            }
            require(offset == bits.size(), "unexpected framing tail");
            require(pyrowave_decoder_decode_is_ready(decoder, false), "incomplete frame");
            pyrowave_cpu_buffer output {};
            output.width = output.height = 256;
            output.format = yuv444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
            std::vector<std::uint8_t> planes[3];
            for (unsigned plane = 0; plane < 3; ++plane) {
              const unsigned dim = plane && !yuv444 ? 128 : 256;
              planes[plane].resize(dim * dim);
              output.data[plane] = planes[plane].data();
              output.row_stride_in_bytes[plane] = dim;
              output.plane_size_in_bytes[plane] = planes[plane].size();
            }
            require(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &output) == PYROWAVE_SUCCESS, "decode failed");
            for (unsigned plane = 0; plane < 3; ++plane) {
              const unsigned dim = plane && !yuv444 ? 128 : 256;
              const auto actual = planes[plane][(dim / 2) * dim + dim / 2];
              const float black = 255.0f * (0.125f + plane * 0.25f);
              require(std::abs(float(actual) - (black + (blank ? 0 : red * 0.5f))) <= 3, "decoded centre plane mismatch");
              if (width != 256 || blank) {
                require(std::abs(float(planes[plane][0]) - black) <= 3, "letterbox or blank plane mismatch");
              }
            }
            ++cases;
          }
          if (request_d3d12) {
            // A legal D3D11 SRGB source is outside the prototype's supported
            // raw capture formats. Fallback must occur before queue submission.
            std::vector<std::uint32_t> pixels(256 * 256, 0xff202020u);
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = desc.Height = 256;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            D3D11_SUBRESOURCE_DATA initial {pixels.data(), 256 * 4, 0};
            struct unsupported_owner_t {
              ComPtr<ID3D11Texture2D> texture;
              ComPtr<IDXGIKeyedMutex> mutex;
            };
            auto owner = std::make_shared<unsupported_owner_t>();
            auto &texture = owner->texture;
            auto &mutex = owner->mutex;
            ComPtr<ID3D11ShaderResourceView> view;
            require(SUCCEEDED(core->device()->CreateTexture2D(&desc, &initial, &texture)), "SRGB source texture failed");
            require(SUCCEEDED(texture.As(&mutex)), "SRGB source keyed mutex missing");
            require(SUCCEEDED(core->device()->CreateShaderResourceView(texture.Get(), nullptr, &view)), "SRGB source view failed");
            pyrowave::d3d11::source_t source;
            source.lease = pyrowave::d3d11::source_lease_e::keyed_standalone;
            source.capture_owner = owner;
            source.capture_texture = texture.Get();
            source.capture_mutex = mutex.Get();
            source.srv = view.Get();
            source.mutex = mutex.Get();
            source.width = source.height = 256;
            std::vector<std::uint8_t> bits;
            require(core->encode(source, 65536, {}, bits) == pyrowave::d3d11::result_e::ok, "unsupported-source D3D11 fallback failed");
            require(!core->using_d3d12_conversion() && !bits.empty(), "unsupported source did not select D3D11 fallback");
          }
        }
      }
      std::cout << "Adapter " << std::hex << desc.VendorId << std::dec << " passed\n";
    }
    std::cout << cases << " Windows GPU encode/decode cases passed\n";
    return cases ? 0 : (requested_vendor ? 1 : 77);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
