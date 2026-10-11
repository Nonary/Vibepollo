// SPDX-License-Identifier: GPL-3.0-only
// Included inside platf::dxgi after img_d3d_t. Session opt-in only.
class d3d_gpu_hevc_encode_device_t final: public gpu_hevc_encode_device_t {
 public:
  ~d3d_gpu_hevc_encode_device_t() override {
    if (session) close_fn(session);
    if (module) FreeLibrary(module);
  }
  bool init(adapter_t::pointer adapter, const ::video::config_t& requested, bool hdr_display) {
    config = requested;
    colorspace = ::video::colorspace_from_client_config(config, hdr_display);
    if (config.videoFormat != 1 || config.chromaSamplingType != 0 || config.width < 64 || config.height < 64 ||
        config.width % 32 || config.height % 32 || colorspace.full_range ||
        (colorspace.colorspace != ::video::colorspace_e::rec709 && colorspace.colorspace != ::video::colorspace_e::bt2020)) {
      BOOST_LOG(error) << "[AURORA-GPU-HEVC] unsupported geometry/colour contract; no fallback";
      return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
    // A CAPTURE THAT IS NOT THE PANE SIZE IS FITTED, NOT REFUSED -- the hardware path's own
    // behaviour, reused here rather than reimplemented. display_vram.cpp aspect-fits the
    // capture into the encode surface and pre-clears the target so the mismatch bars are
    // black; these are its cursor_vs/cursor_ps, which are a bare fullscreen-triangle blit.
    //
    // WHY THIS EXISTS: refusing the frame froze the whole co-op stream the instant a game
    // applied its own fullscreen resolution -- MEASURED 2026-09-17, a 1920x1080 game against
    // a 3840x1088 pane produced `capture size differs` ~7x/second and zero output, and the
    // composite cannot be built without the primary's half. The pane-sized case is UNTOUCHED
    // below: it is still a 1:1 CopyResource, so nothing qualified changes. The exact-match
    // rule came from the panel gate in passover 04 ("what fails it is pillarboxing, upscaling
    // a full desktop into a half pane"), which is a statement about the CORRECT configuration
    // -- where this fit computes scalar 1.0 and scales nothing.
    auto blit_vs_blob = compile_vertex_shader(SUNSHINE_SHADERS_DIR "/cursor_vs.hlsl");
    auto blit_ps_blob = compile_pixel_shader(SUNSHINE_SHADERS_DIR "/cursor_ps.hlsl");
    if (!blit_vs_blob || !blit_ps_blob) return fail("blit shader compile", E_FAIL);
    hr = device->CreateVertexShader(blit_vs_blob->GetBufferPointer(), blit_vs_blob->GetBufferSize(), nullptr, &blit_vs);
    if (FAILED(hr)) return fail("blit vertex shader", hr);
    hr = device->CreatePixelShader(blit_ps_blob->GetBufferPointer(), blit_ps_blob->GetBufferSize(), nullptr, &blit_ps);
    if (FAILED(hr)) return fail("blit pixel shader", hr);
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler_desc, &blit_sampler);
    if (FAILED(hr)) return fail("blit sampler", hr);
    // cursor_vs reads its rotation from b2; this path never rotates.
    int32_t blit_rotation_data[16 / sizeof(int32_t)] {0};
    blit_rotation = make_buffer(device.get(), blit_rotation_data);
    if (!blit_rotation) return fail("blit rotation buffer", E_FAIL);
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return fail("executable path", E_FAIL);
    auto directory = std::filesystem::path(path).parent_path() / "gpu-hevc";
    auto library = directory / (colorspace.bit_depth == 10 ? "aurora_gpu_hevc10.dll" : "aurora_gpu_hevc8.dll");
    module = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return fail("LoadLibraryExW", HRESULT_FROM_WIN32(GetLastError()));
    open_fn = reinterpret_cast<decltype(open_fn)>(GetProcAddress(module, "agh_open"));
    encode_fn = reinterpret_cast<decltype(encode_fn)>(GetProcAddress(module, "agh_encode"));
    close_fn = reinterpret_cast<decltype(close_fn)>(GetProcAddress(module, "agh_close"));
    if (!open_fn || !encode_fn || !close_fn) return fail("GPU library exports", E_FAIL);
    kernels = (directory / "kernels").string();
    agh_config args = {sizeof(agh_config), AGH_ABI_VERSION, uint32_t(config.width), uint32_t(config.height),
      colorspace.bit_depth, ::video::colorspace_is_hdr(colorspace) ? 1u : 0u, device.get(), kernels.c_str(), uint32_t(config.bitrate), uint32_t(config.framerate)};
    char detail[1024] = {};
    session = open_fn(&args, detail, sizeof(detail));
    if (!session) {
      BOOST_LOG(error) << "[AURORA-GPU-HEVC] open failed: " << detail;
      // Context/queue creation is the GPU runtime itself, not this session's settings (see video.h).
      const std::string_view text {detail};
      if (text.starts_with("context OpenCL error") || text.starts_with("queue OpenCL error")) {
        ::video::gpu_hevc_runtime_lost(detail);
      }
      return false;
    }
    BOOST_LOG(info) << "[AURORA-GPU-HEVC] opened " << config.width << "x" << config.height << " depth=" << colorspace.bit_depth
      << " HDR=" << args.hdr << " bitrate_kbps=" << args.bitrate_kbps << " rate_control=bitrate experimental=1";
    return true;
  }
  int convert(platf::img_t& base) override {
    auto* img = dynamic_cast<img_d3d_t*>(&base);
    if (!img) {
      BOOST_LOG(error) << "[AURORA-GPU-HEVC] capture is not a D3D image; no fallback";
      return -1;
    }
    const bool exact_size = img->width == config.width && img->height == config.height;
    if (!exact_size && !fitted_warned) {
      fitted_warned = true;
      BOOST_LOG(warning) << "[AURORA-GPU-HEVC] capture " << img->width << "x" << img->height
        << " differs from the pane " << config.width << "x" << config.height
        << "; fitting aspect-correct with black bars, as the hardware path does. Set the game or"
           " desktop to the pane resolution for a 1:1 pane.";
    }
    // Apollo's startup dummy has no initialized pixel content. Generate a defined
    // black GPU texture, and do the same for a genuinely blank desktop image.
    DXGI_FORMAT format = img->dummy || img->blank
      ? (::video::colorspace_is_hdr(colorspace) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM)
      : img->format;
    if (!staged || format != staged_format) {
      staged.reset(); target.reset();
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = config.width; desc.Height = config.height; desc.MipLevels = 1; desc.ArraySize = 1;
      desc.SampleDesc.Count = 1; desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staged);
      if (FAILED(hr)) return fail("staging texture", hr), -1;
      hr = device->CreateRenderTargetView(staged.get(), nullptr, &target);
      if (FAILED(hr)) return fail("staging target", hr), -1;
      staged_format = format;
    }
    if (img->dummy || img->blank) {
      const float black[4] = {0, 0, 0, 1};
      context->ClearRenderTargetView(target.get(), black);
    } else {
      for (auto it = images.begin(); it != images.end();) {
        if (it->second.lifetime.expired()) it = images.erase(it); else ++it;
      }
      auto& mapped = images[img->id];
      if (mapped.identity != img->capture_texture.get()) {
        mapped.texture.reset(); mapped.mutex.reset(); mapped.identity = nullptr;
        device1_t device1;
        HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1), (void**)&device1);
        if (FAILED(hr)) return fail("ID3D11Device1", hr), -1;
        hr = device1->OpenSharedResource1(img->encoder_texture_handle, __uuidof(ID3D11Texture2D), (void**)&mapped.texture);
        if (FAILED(hr)) return fail("OpenSharedResource1", hr), -1;
        hr = mapped.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mapped.mutex);
        if (FAILED(hr)) return fail("IDXGIKeyedMutex", hr), -1;
        mapped.identity = img->capture_texture.get(); mapped.lifetime = img->weak_from_this();
      }
      HRESULT hr = mapped.mutex->AcquireSync(0, 1000);
      if (hr != S_OK) return fail("capture mutex", hr), -1;
      const bool copied = exact_size
        ? (context->CopyResource(staged.get(), mapped.texture.get()), true)
        : fit_into_staged(mapped.texture.get(), img->width, img->height);
      context->Flush();
      hr = mapped.mutex->ReleaseSync(0);
      if (FAILED(hr)) return fail("capture release", hr), -1;
      if (!copied) return -1;
    }
    ready = true;
    return 0;
  }
  bool encode(uint64_t index, bool force_idr, std::vector<uint8_t>& bytes, bool& idr) override {
    if (!ready || !session) return false;
    agh_packet packet = {}; packet.size = sizeof(packet); char detail[1024] = {};
    if (encode_fn(session, staged.get(), index, force_idr ? 1u : 0u, &packet, detail, sizeof(detail)) ||
        packet.frame_id != index || !packet.data || !packet.bytes) {
      BOOST_LOG(error) << "[AURORA-GPU-HEVC] encode failed: " << detail;
      return false;
    }
    bytes.assign(packet.data, packet.data + packet.bytes); idr = packet.is_idr != 0;
    if (idr || index % 300 == 0) BOOST_LOG(info) << "[AURORA-GPU-HEVC] frame=" << index << " bytes=" << packet.bytes
      << " IDR=" << idr << " QP=" << packet.qp << " passes=" << packet.encode_passes << " submit_packet_ms=" << packet.submit_to_packet_ms;
    return true;
  }
 private:
  /** Aspect-correct fit of a differently sized capture into the pane-sized encode surface.
   *  The scalar/offset arithmetic is display_vram.cpp's, unchanged, minus the co-op top-pad
   *  strip (that belongs to the hardware path's own seam guard, not here). Bars are black
   *  because the target is cleared first, exactly as the hardware path's rtvs_cleared pass. */
  bool fit_into_staged(ID3D11Texture2D* source, int in_w, int in_h) {
    shader_res_t view;
    HRESULT hr = device->CreateShaderResourceView(source, nullptr, &view);
    if (FAILED(hr)) return fail("blit view", hr);
    const float out_width = (float) config.width;
    const float out_height = (float) config.height;
    const float scalar = std::fminf(out_width / (float) in_w, out_height / (float) in_h);
    const float out_width_f = (float) in_w * scalar;
    const float out_height_f = (float) in_h * scalar;
    const D3D11_VIEWPORT viewport {(out_width - out_width_f) / 2, (out_height - out_height_f) / 2,
      out_width_f, out_height_f, 0.0f, 1.0f};
    const float black[4] = {0, 0, 0, 1};
    context->ClearRenderTargetView(target.get(), black);
    auto* srv = view.get();
    auto* sampler = blit_sampler.get();
    auto* rotation = blit_rotation.get();
    auto* rtv = target.get();
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(blit_vs.get(), nullptr, 0);
    context->VSSetConstantBuffers(2, 1, &rotation);
    context->PSSetShader(blit_ps.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &sampler);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    ID3D11RenderTargetView* detach = nullptr;
    context->OMSetRenderTargets(1, &detach, nullptr);
    return true;
  }
  bool fail(const char* operation, HRESULT hr) {
    BOOST_LOG(error) << "[AURORA-GPU-HEVC] " << operation << " failed " << util::log_hex(hr);
    return false;
  }
  struct image_t {
    texture2d_t texture; keyed_mutex_t mutex;
    const ID3D11Texture2D* identity = nullptr;
    std::weak_ptr<const platf::img_t> lifetime;
  };
  ::video::config_t config{};
  device_t device; device_ctx_t context;
  texture2d_t staged; render_target_t target;
  DXGI_FORMAT staged_format = DXGI_FORMAT_UNKNOWN;
  vs_t blit_vs; ps_t blit_ps; sampler_state_t blit_sampler; buf_t blit_rotation;
  bool fitted_warned = false;
  std::unordered_map<uint32_t, image_t> images;
  bool ready = false;
  HMODULE module = nullptr;
  decltype(&agh_open) open_fn = nullptr;
  decltype(&agh_encode) encode_fn = nullptr;
  decltype(&agh_close) close_fn = nullptr;
  agh_session* session = nullptr;
  std::string kernels;
};
