#include "frame_push_d3d11.h"
#ifdef _WIN32
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace platf::frame_push {
  namespace {
    std::atomic<std::uint64_t> next_cookie {1};
    std::uint64_t allocate_cookie() {
      auto current = next_cookie.load(std::memory_order_relaxed);
      while (current && !next_cookie.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {}
      return current; // permanently exhausted after UINT64_MAX, never reuse zero
    }
    class D3D11ConsumerRing final: public ConsumerResources {
    public:
      ~D3D11ConsumerRing() override {
        if (mapping_) UnmapViewOfFile(mapping_);
        for (const auto handle: handles_) if (handle) CloseHandle(handle);
      }
      vdd::FrameRingRegisterRequest request() const override { return request_; }
      Access wait(std::uint32_t timeout_ms) override {
        const auto result = WaitForSingleObject(handles_[5], (std::min)(timeout_ms, 100u));
        return result == WAIT_OBJECT_0 ? Access::Acquired : result == WAIT_TIMEOUT ? Access::Timeout : Access::Lost;
      }
      Access acquire(std::uint32_t slot, std::uint64_t key, std::uint32_t timeout_ms) override {
        if (slot >= vdd::kFrameRingSlotCount || timeout_ms != 0) return Access::Lost;
        const auto result = mutexes_[slot]->AcquireSync(key, 0);
        // WAIT_TIMEOUT and WAIT_ABANDONED are nonnegative HRESULT values.
        // SUCCEEDED(result) alone would incorrectly grant access to both.
        if (result == S_OK) { MemoryBarrier(); return Access::Acquired; }
        return result == WAIT_TIMEOUT ? Access::Timeout : Access::Lost;
      }
      vdd::FrameSlotMetadata metadata(std::uint32_t slot) const override {
        return mapping_->slots[slot]; // caller holds this slot's keyed mutex
      }
      PrivateCopy copy_to_private(std::uint32_t slot) override {
        for (std::size_t i = 0; i < private_frames_.size(); ++i) {
          auto &outstanding = private_frames_[i];
          if (!outstanding.expired()) continue;
          auto output = std::make_shared<D3D11PrivateFrame>();
          output->texture = private_textures_[i];
          context_->CopyResource(output->texture.Get(), textures_[slot].Get());
          context_->Flush();
          if (FAILED(device_->GetDeviceRemovedReason())) return {Access::Lost, {}};
          // Same immediate context submission precedes ReleaseSync(0). Never
          // return the shared ring texture itself to an asynchronous encoder.
          outstanding = output;
          return {Access::Acquired, std::move(output)};
        }
        return {}; // two retained private frames: drop without blocking
      }
      bool release(std::uint32_t slot, std::uint64_t key) override {
        MemoryBarrier();
        return slot < vdd::kFrameRingSlotCount && mutexes_[slot]->ReleaseSync(key) == S_OK;
      }
      bool initialize(ID3D11Device1 *device, ID3D11DeviceContext *context, vdd::FrameRingRegisterRequest target) {
        if (!device || !context || !target.lease_id || !target.display_id || !target.display_generation ||
            !target.width || target.width > 16384 || !target.height || target.height > 16384 ||
            !((target.format == 87 && target.color_space == 0) || (target.format == 10 && target.color_space == 1))) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> context_device;
        context->GetDevice(&context_device);
        Microsoft::WRL::ComPtr<IUnknown> context_identity, device_identity;
        if (FAILED(context_device.As(&context_identity)) || FAILED(device->QueryInterface(IID_PPV_ARGS(&device_identity))) ||
            context_identity.Get() != device_identity.Get() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc {};
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&adapter_desc)) || adapter_desc.AdapterLuid.LowPart != target.adapter_luid.low_part ||
            adapter_desc.AdapterLuid.HighPart != target.adapter_luid.high_part) return false;
        target.client_cookie = allocate_cookie();
        if (!target.client_cookie) return false;
        device_ = device; context_ = context;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = target.width; desc.Height = target.height;
        desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = static_cast<DXGI_FORMAT>(target.format);
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        for (std::size_t i = 0; i < vdd::kFrameRingSlotCount; ++i) {
          Microsoft::WRL::ComPtr<IDXGIResource1> resource;
          if (FAILED(device_->CreateTexture2D(&desc, nullptr, &textures_[i])) ||
              FAILED(textures_[i].As(&mutexes_[i])) || FAILED(textures_[i].As(&resource)) ||
              FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                   nullptr, &handles_[i]))) return false;
          target.texture_handles[i] = reinterpret_cast<std::uintptr_t>(handles_[i]);
        }
        desc.MiscFlags = 0;
        for (auto &texture: private_textures_) {
          if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture))) return false;
        }
        handles_[4] = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, vdd::kFrameRingMetadataBytes, nullptr);
        if (!handles_[4]) return false;
        mapping_ = static_cast<vdd::FrameRingMetadata *>(MapViewOfFile(handles_[4], FILE_MAP_READ | FILE_MAP_WRITE,
                                                                     0, 0, vdd::kFrameRingMetadataBytes));
        if (!mapping_) return false;
        vdd::FrameRingMetadata header {};
        header.client_cookie = target.client_cookie;
        std::memcpy(mapping_, &header, sizeof(header));
        handles_[5] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!handles_[5]) return false;
        target.metadata_handle = reinterpret_cast<std::uintptr_t>(handles_[4]);
        target.event_handle = reinterpret_cast<std::uintptr_t>(handles_[5]);
        request_ = target;
        return vdd::valid_frame_ring_request(request_);
      }
    private:
      vdd::FrameRingRegisterRequest request_ {};
      Microsoft::WRL::ComPtr<ID3D11Device1> device_;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
      std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, vdd::kFrameRingSlotCount> textures_;
      std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, vdd::kFrameRingSlotCount> mutexes_;
      std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> private_textures_;
      std::array<std::weak_ptr<D3D11PrivateFrame>, 2> private_frames_;
      std::array<HANDLE, 6> handles_ {};
      vdd::FrameRingMetadata *mapping_ {};
    };
  }
  std::shared_ptr<ConsumerResources> create_d3d11_consumer_ring(
    ID3D11Device1 *device, ID3D11DeviceContext *context, vdd::FrameRingRegisterRequest target) {
    auto ring = std::make_shared<D3D11ConsumerRing>();
    return ring->initialize(device, context, target) ? ring : nullptr;
  }
}
#endif
