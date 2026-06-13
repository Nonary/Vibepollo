// truehdr_shim_test - validates the runtime-load path Vibeshine uses: LoadLibrary the shim,
// GetProcAddress its C exports, create a D3D11 device, load an SDR BMP, run VBSTrueHDR_Convert,
// and measure peak nits of the FP16 scRGB result. Run from a dir containing vibeshine_truehdr.dll
// + nvngx_truehdr.dll. Usage: truehdr_shim_test <in.bmp> [contrast sat mid peak]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <stdio.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;

using create_fn = void *(__cdecl *) (ID3D11Device *);
using convert_fn = ID3D11Texture2D *(__cdecl *) (void *, ID3D11Texture2D *, int, int, int, int);
using destroy_fn = void(__cdecl *) (void *);

static bool load_bmp24(const wchar_t *p, std::vector<uint8_t> &rgba, int &W, int &H) {
  FILE *f = _wfopen(p, L"rb"); if (!f) return false;
  uint8_t hd[54]; if (fread(hd, 1, 54, f) != 54) { fclose(f); return false; }
  uint32_t off = *(uint32_t *) (hd + 10); W = *(int *) (hd + 18); H = *(int *) (hd + 22);
  int rowb = (W * 3 + 3) & ~3; std::vector<uint8_t> raw((size_t) rowb * H);
  fseek(f, off, SEEK_SET); fread(raw.data(), 1, raw.size(), f); fclose(f);
  rgba.assign((size_t) W * H * 4, 255);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
    const uint8_t *s = &raw[(size_t) (H - 1 - y) * rowb + x * 3]; uint8_t *d = &rgba[((size_t) y * W + x) * 4];
    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
  }
  return true;
}

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) { printf("usage: truehdr_shim_test <in.bmp> [c s m peak]\n"); return 1; }
  int c = argc > 2 ? _wtoi(argv[2]) : 100, s = argc > 3 ? _wtoi(argv[3]) : 100,
      m = argc > 4 ? _wtoi(argv[4]) : 50, peak = argc > 5 ? _wtoi(argv[5]) : 1000;

  HMODULE dll = LoadLibraryW(L"vibeshine_truehdr.dll");
  if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
  auto create = (create_fn) GetProcAddress(dll, "VBSTrueHDR_Create");
  auto convert = (convert_fn) GetProcAddress(dll, "VBSTrueHDR_Convert");
  auto destroy = (destroy_fn) GetProcAddress(dll, "VBSTrueHDR_Destroy");
  printf("exports: create=%p convert=%p destroy=%p\n", create, convert, destroy);
  if (!create || !convert || !destroy) return 1;

  std::vector<uint8_t> rgba; int W, H;
  if (!load_bmp24(argv[1], rgba, W, H)) { printf("bmp load failed\n"); return 1; }

  ComPtr<IDXGIFactory1> fac; CreateDXGIFactory1(IID_PPV_ARGS(&fac));
  ComPtr<IDXGIAdapter1> ad, pick;
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; ++i) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); if (wcsstr(d.Description, L"NVIDIA")) { pick = ad; break; } }
  ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl;
  D3D11CreateDevice(pick.Get(), pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);

  D3D11_TEXTURE2D_DESC td = {}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = rgba.data(); sd.SysMemPitch = W * 4;
  ComPtr<ID3D11Texture2D> in; dev->CreateTexture2D(&td, &sd, &in);

  void *h = create(dev.Get());
  printf("VBSTrueHDR_Create -> %p\n", h);
  if (!h) { printf("create failed (TrueHDR unavailable?)\n"); return 1; }
  ID3D11Texture2D *out = convert(h, in.Get(), c, s, m, peak);
  printf("VBSTrueHDR_Convert(%d,%d,%d,%d) -> %p\n", c, s, m, peak, out);
  if (!out) { destroy(h); return 1; }

  D3D11_TEXTURE2D_DESC od; out->GetDesc(&od);
  od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  od.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> stg; dev->CreateTexture2D(&od, nullptr, &stg);
  ctx->CopyResource(stg.Get(), out);
  D3D11_MAPPED_SUBRESOURCE map; ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &map);
  float maxc = 0; size_t above = 0, total = 0;
  for (UINT y = 0; y < H; y += 2) { auto *row = (const uint8_t *) map.pData + (size_t) y * map.RowPitch;
    for (UINT x = 0; x < (UINT) W; x += 2) { auto *p = (const uint16_t *) (row + (size_t) x * 8);
      float r = XMConvertHalfToFloat(p[0]), g = XMConvertHalfToFloat(p[1]), b = XMConvertHalfToFloat(p[2]);
      float mx = r > g ? r : g; if (b > mx) mx = b; if (mx > maxc) maxc = mx; if (mx > 3.0f) ++above; ++total; } }
  ctx->Unmap(stg.Get(), 0);
  printf("RESULT: peak scRGB=%.3f => %.0f nits, above-SDR-white=%.2f%%\n", maxc, maxc * 80.0f, 100.0 * above / total);
  destroy(h);
  return 0;
}
