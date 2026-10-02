// Exercise the real D3D11 texture/fence imports, including Intel's NT-handle path.
#include "src/platform/windows/pyrowave_d3d11_core.h"

#include <vulkan/vulkan_core.h>
#include <pyrowave.h>
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
          config.shader_path = PYROWAVE_TEST_SHADER_PATH;
          // Distinct plane values verify conversion and the imported contents.
          config.color_matrix.color_vec_y[0] = config.color_matrix.color_vec_u[0] = config.color_matrix.color_vec_v[0] = 0.5f;
          config.color_matrix.color_vec_y[3] = 0.125f;
          config.color_matrix.color_vec_u[3] = 0.375f;
          config.color_matrix.color_vec_v[3] = 0.625f;
          config.color_matrix.range_y[0] = config.color_matrix.range_uv[0] = 1.0f;
          auto core = pyrowave::d3d11::core_t::create(adapter.Get(), config, [](int, const std::string &message) { std::cerr << message << '\n'; });
          require(bool(core), "D3D11 encoder initialization failed");
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

          for (unsigned frame = 0; frame < 3; ++frame) {
            // Change the input to expose missing cross-API synchronization.
            const unsigned red = frame * 32;
            std::vector<std::uint32_t> pixels(256 * 256, 0xff000000u | red);
            D3D11_TEXTURE2D_DESC texture_info {};
            texture_info.Width = texture_info.Height = 256;
            texture_info.MipLevels = texture_info.ArraySize = 1;
            texture_info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_info.SampleDesc.Count = 1;
            texture_info.Usage = D3D11_USAGE_IMMUTABLE;
            texture_info.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial {pixels.data(), 256 * 4, 0};
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> view;
            require(SUCCEEDED(core->device()->CreateTexture2D(&texture_info, &initial, &texture)), "source texture failed");
            require(SUCCEEDED(core->device()->CreateShaderResourceView(texture.Get(), nullptr, &view)), "source view failed");
            pyrowave::d3d11::source_t source;
            source.srv = view.Get();
            source.width = source.height = 256;
            pyrowave::d3d11::framing_params_t framing;
            framing.framing = pyrowave::policy::framing_e::length_prefixed;
            std::vector<std::uint8_t> bits;
            require(core->encode(source, 65536, framing, bits) == pyrowave::d3d11::result_e::ok, "encoding failed");
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
              const auto actual = planes[plane][planes[plane].size() / 2];
              require(std::abs(float(actual) - (255.0f * (0.125f + plane * 0.25f) + red * 0.5f)) <= 3, "decoded plane mismatch");
            }
            ++cases;
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
