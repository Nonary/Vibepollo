// Capture the existing Windows Vibeshine TrueHDR shim's FP16 scRGB output.
// Build in an x64 MSVC developer prompt:
// cl /std:c++17 /EHsc windows_reference.cpp /link d3d11.lib dxgi.lib
// Put vibeshine_truehdr.dll and nvngx_truehdr.dll beside the resulting executable.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static void checked(HRESULT result, const char *operation) {
  if (FAILED(result)) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed: HRESULT 0x%08lx", operation, static_cast<unsigned long>(result));
    throw std::runtime_error(message);
  }
}
static int number(const wchar_t *text, int low, int high) {
  wchar_t *end = nullptr;
  const long value = std::wcstol(text, &end, 10);
  if (!end || *end || end == text || value < low || value > high) throw std::runtime_error("Invalid numeric argument");
  return static_cast<int>(value);
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 9) {
    std::fprintf(stderr, "usage: windows_reference width height input.rgba output.scrgb-f16 contrast saturation middle_gray peak\n");
    return 2;
  }
  try {
    const auto width = number(argv[1], 16, 8192), height = number(argv[2], 16, 8192);
    const int contrast = number(argv[5], 0, 200), saturation = number(argv[6], 0, 200);
    const int middle = number(argv[7], 10, 100), peak = number(argv[8], 400, 2000);
    std::vector<std::uint8_t> input(static_cast<size_t>(width) * height * 4);
    FILE *source = nullptr;
    if (_wfopen_s(&source, argv[3], L"rb") || !source) throw std::runtime_error("Cannot open SDR input");
    const auto size = std::fread(input.data(), 1, input.size(), source);
    const auto trailing = std::fgetc(source);
    std::fclose(source);
    if (size != input.size() || trailing != EOF) throw std::runtime_error("Expected exactly one complete RGBA8 frame");

    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) throw std::runtime_error("Cannot locate executable directory");
    const std::wstring path(executable, length);
    const auto shim_path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"vibeshine_truehdr.dll";
    HMODULE shim = LoadLibraryExW(shim_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!shim) throw std::runtime_error("Cannot load vibeshine_truehdr.dll and its dependencies");
    using Create = void *(__cdecl *)(ID3D11Device *);
    using Convert = ID3D11Texture2D *(__cdecl *)(void *, ID3D11Texture2D *, int, int, int, int);
    using Destroy = void(__cdecl *)(void *);
    const auto create = reinterpret_cast<Create>(GetProcAddress(shim, "VBSTrueHDR_Create"));
    const auto convert = reinterpret_cast<Convert>(GetProcAddress(shim, "VBSTrueHDR_Convert"));
    const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(shim, "VBSTrueHDR_Destroy"));
    if (!create || !convert || !destroy) throw std::runtime_error("Missing TrueHDR shim exports");

    ComPtr<IDXGIFactory1> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
      ComPtr<IDXGIAdapter1> candidate;
      const auto result = factory->EnumAdapters1(index, &candidate);
      if (result == DXGI_ERROR_NOT_FOUND) break;
      checked(result, "EnumAdapters1");
      DXGI_ADAPTER_DESC1 description {};
      checked(candidate->GetDesc1(&description), "GetDesc1");
      if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        adapter = candidate;
        break;
      }
    }
    if (!adapter) throw std::runtime_error("No NVIDIA adapter");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    checked(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
    D3D11_TEXTURE2D_DESC description {};
    description.Width = width; description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data {input.data(), static_cast<UINT>(width * 4), 0};
    ComPtr<ID3D11Texture2D> texture;
    checked(device->CreateTexture2D(&description, &data, &texture), "Create input texture");
    void *handle = create(device.Get());
    if (!handle) throw std::runtime_error("NVIDIA TrueHDR creation failed");
    // Keep ownership until all borrowed output has been copied and unmapped.
    struct Owner { void *handle; Destroy destroy; ~Owner() { destroy(handle); } } owner {handle, destroy};
    auto *output = convert(handle, texture.Get(), contrast, saturation, middle, peak);
    if (!output) throw std::runtime_error("NVIDIA TrueHDR conversion failed");
    output->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || description.Width != width || description.Height != height)
      throw std::runtime_error("Unexpected TrueHDR output format or dimensions");
    description.Usage = D3D11_USAGE_STAGING; description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    checked(device->CreateTexture2D(&description, nullptr, &staging), "Create staging texture");
    context->CopyResource(staging.Get(), output);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read TrueHDR output");
    FILE *destination = nullptr;
    // C11 x mode prevents accidentally overwriting an existing reference.
    if (_wfopen_s(&destination, argv[4], L"wbx") || !destination) {
      context->Unmap(staging.Get(), 0);
      throw std::runtime_error("Cannot create new output file");
    }
    bool ok = true;
    for (int y = 0; y < height; ++y)
      ok = (std::fwrite(static_cast<const std::uint8_t *>(mapped.pData) + y * mapped.RowPitch, 8, width, destination) == width) && ok;
    const auto closed = std::fclose(destination);
    context->Unmap(staging.Get(), 0);
    if (!ok || closed) throw std::runtime_error("Output write failed");
    std::printf("width=%d height=%d contrast=%d saturation=%d middle_gray=%d peak=%d format=RGBA16F-scRGB-80nits\n",
                width, height, contrast, saturation, middle, peak);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
