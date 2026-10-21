// Bounded, opt-in D3D12 conversion prototype. The source is snapshotted by
// D3D11 while the caller holds its keyed mutex; only conversion moves queues.
#include "pyrowave_d3d12_conversion.h"

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pyrowave::d3d11 {
  namespace {
    struct handle_t {
      HANDLE value = nullptr;
      ~handle_t() {
        if (value) {
          CloseHandle(value);
        }
      }
    };

    std::string hex(HRESULT hr) {
      char value[16];
      std::snprintf(value, sizeof(value), "0x%08lX", static_cast<unsigned long>(hr));
      return value;
    }

    D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
      D3D12_RESOURCE_BARRIER barrier {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = resource;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter = after;
      return barrier;
    }
  }  // namespace

  struct d3d12_conversion_t::impl_t {
    core_config_t config;
    log_fn log;
    LUID luid {};
    DXGI_ADAPTER_DESC adapter_desc {};
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> producer11;
    ComPtr<ID3D11Fence> output11;
    std::array<ComPtr<ID3D11Texture2D>, 3> planes11;
    ComPtr<ID3D11Texture2D> bridge11;
    D3D11_TEXTURE2D_DESC bridge_desc {};
    // The sink exists even when D3D12 setup is unavailable. If fallback
    // ReleaseSync is ambiguous, it keeps the exact D3D11 path and its outputs
    // alive without depending on the core object's lifetime.
    ComPtr<ID3D11Device> fallback_device;
    ComPtr<ID3D11DeviceContext> fallback_context;
    std::array<ComPtr<ID3D11Texture2D>, 3> fallback_planes;
    ComPtr<ID3D11Fence> fallback_fence;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Fence> producer;
    ComPtr<ID3D12Fence> output;
    ComPtr<ID3D12Fence> retirement;
    ComPtr<ID3D12Resource> bridge;
    std::array<ComPtr<ID3D12Resource>, 3> planes;
    std::map<transfer_e, ComPtr<ID3D12PipelineState>> pipelines;
    handle_t event;
    UINT increment = 0;
    bool pending = false;
    bool unknown_completion = false;
    bool retired = false;
    std::uint64_t pending_value = 0;
    std::uint64_t producer_value = 0;
    // Source reservation is installed in this already-admitted node before copy.
    // No owner allocation or caller/cache lookup is needed during failure teardown.
    std::shared_ptr<const void> capture_owner;
    ComPtr<ID3D11Texture2D> capture_texture;
    ComPtr<IDXGIKeyedMutex> capture_mutex;
    ComPtr<ID3D11Texture2D> opened_source;
    ComPtr<ID3D11ShaderResourceView> source_view;
    ComPtr<IDXGIKeyedMutex> source_mutex;
    capture_mutex_guard_t capture_guard;
    bool producer_pending = false;
    bool producer_unknown = false;
    std::atomic<bool> release_unknown {false};
    // AcquireSync(WAIT_ABANDONED) leaves both mutex ownership and source
    // consistency unknown. This is a terminal full-LUID quarantine state.
    std::atomic<bool> acquire_abandoned {false};
    bool capture_staged = false;
    struct luid_state_t {
      LUID luid {};
      std::mutex frame_mutex;
      std::atomic<bool> poisoned {false};
      std::atomic<bool> blocked {false};
      luid_state_t *next = nullptr;
    };
    luid_state_t *adapter_state = nullptr;
    std::unique_lock<std::mutex> frame_lock;
    HRESULT failure_status = S_OK;
    const char *failure_operation = nullptr;
    bool collecting = false;
    impl_t *collection_next = nullptr;
    bool registered = false;
    bool conversion_admitted = false;
    std::atomic<bool> conversion_enabled {false};
    bool quarantined = false;
    impl_t *registry_next = nullptr;

    // Every opted-in core registers its own preallocated sink. D3D12 queue
    // admission remains unique per full LUID; fallback sinks can coexist, and
    // any sticky release poison remains visible without allocating at failure.
    struct retained_t {
      std::mutex mutex;
      impl_t *generations = nullptr;
      luid_state_t *adapters = nullptr;
    };

    static retained_t &retained() {
      static auto *state = new retained_t;
      return *state;
    }

    ~impl_t() {
      if (registered) {
        auto &state = retained();
        std::lock_guard guard(state.mutex);
        for (auto **entry = &state.generations; *entry; entry = &(*entry)->registry_next) {
          if (*entry == this) {
            *entry = registry_next;
            break;
          }
        }
        registered = false;
      }
    }

    bool producer_finished() const {
      if (producer_unknown) return false;
      if (!producer_pending) return true;
      const auto done = producer11->GetCompletedValue();
      // No device-removal shortcut discharges the D3D11 copy obligation.
      return done != std::numeric_limits<std::uint64_t>::max() && done >= producer_value;
    }

    bool consumer_finished() const {
      if (!pending) return true;
      // Device removal may stand in for a known queue tail, but it cannot
      // discharge work whose completion is still explicitly unknown.
      if (unknown_completion) return false;
      if (device && FAILED(device->GetDeviceRemovedReason())) return true;
      auto *fence = retired ? retirement.Get() : output.Get();
      const auto done = fence->GetCompletedValue();
      return done != std::numeric_limits<std::uint64_t>::max() && done >= pending_value;
    }

    bool finished() const {
      // Producer unknown and failed release poison admission even when the
      // consumer is absent, complete, device-removed, or has a completed tail.
      return !acquire_abandoned.load(std::memory_order_acquire) &&
             !release_unknown.load(std::memory_order_acquire) && producer_finished() && consumer_finished();
    }

    bool release_source() noexcept {
      // WAIT_ABANDONED is not a successful acquisition. Do not call
      // ReleaseSync and do not drop any staged source owner or interface.
      if (acquire_abandoned.load(std::memory_order_acquire)) return false;
      if (release_unknown.load(std::memory_order_acquire)) {
        if (frame_lock.owns_lock()) frame_lock.unlock();
        return false;
      }
      const HRESULT status = capture_guard.release();
      if (FAILED(status)) {
        failure_status = status;
        failure_operation = "release capture ownership";
        release_unknown.store(true, std::memory_order_release);
        if (adapter_state) {
          adapter_state->poisoned.store(true, std::memory_order_release);
          adapter_state->blocked.store(true, std::memory_order_release);
        }
        if (frame_lock.owns_lock()) frame_lock.unlock();
        return false;  // Never retry an ambiguous release or drop its owners.
      }
      capture_guard.reset();
      source_mutex.Reset();
      source_view.Reset();
      opened_source.Reset();
      capture_mutex.Reset();
      capture_texture.Reset();
      capture_owner.reset();
      producer_pending = false;
      capture_staged = false;
      if (frame_lock.owns_lock()) frame_lock.unlock();
      return true;
    }

    static bool same_luid(const LUID &left, const LUID &right) noexcept {
      return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
    }

    static void unlink_locked(retained_t &state, impl_t *owner) noexcept {
      for (auto **entry = &state.generations; *entry; entry = &(*entry)->registry_next) {
        if (*entry == owner) {
          *entry = owner->registry_next;
          owner->registry_next = nullptr;
          owner->registered = false;
          return;
        }
      }
    }

    // Completion/native queries and COM/shared-owner releases run outside the
    // registry mutex. collecting pins each quarantined node until revalidation.
    static void collect_completed() {
      auto &state = retained();
      impl_t *complete = nullptr;
      {
        std::lock_guard guard(state.mutex);
        for (auto *generation = state.generations; generation; generation = generation->registry_next) {
          if (generation->quarantined && !generation->collecting) {
            generation->collecting = true;
            generation->collection_next = complete;
            complete = generation;
          }
        }
      }
      while (complete) {
        auto *generation = complete;
        complete = generation->collection_next;
        generation->collection_next = nullptr;
        bool released = false;
        std::unique_lock operation(generation->adapter_state->frame_mutex, std::try_to_lock);
        if (operation.owns_lock()) {
          generation->frame_lock = std::move(operation);
          if (generation->finished()) released = generation->release_source();
          else generation->frame_lock.unlock();
        }
        {
          std::lock_guard guard(state.mutex);
          if (released) {
            unlink_locked(state, generation);
            generation->conversion_admitted = false;
            generation->quarantined = false;
            bool blocked = false;
            for (auto *owner = state.generations; owner; owner = owner->registry_next) {
              if (same_luid(owner->luid, generation->luid) &&
                  (owner->quarantined || owner->release_unknown.load(std::memory_order_acquire))) {
                blocked = true;
                break;
              }
            }
            generation->adapter_state->blocked.store(blocked, std::memory_order_release);
          }
          generation->collecting = false;
        }
        // Native releases and image deleters may reenter registration. The
        // still-linked collecting node keeps same-LUID reuse blocked meanwhile.
        if (released) delete generation;
      }
    }

    bool register_sink() {
      collect_completed();
      auto &state = retained();
      std::lock_guard guard(state.mutex);
      adapter_state = state.adapters;
      while (adapter_state && !same_luid(adapter_state->luid, luid)) adapter_state = adapter_state->next;
      if (!adapter_state) {
        adapter_state = new (std::nothrow) luid_state_t;
        if (!adapter_state) return false;
        adapter_state->luid = luid;
        adapter_state->next = state.adapters;
        state.adapters = adapter_state;  // Process-lifetime poison/gate state.
      }
      if (adapter_state->poisoned.load(std::memory_order_acquire) || adapter_state->blocked.load(std::memory_order_acquire)) return false;
      for (auto *owner = state.generations; owner; owner = owner->registry_next) {
        if (same_luid(owner->luid, luid) &&
            (owner->release_unknown.load(std::memory_order_acquire) || owner->quarantined)) return false;
      }
      registry_next = state.generations;
      state.generations = this;
      registered = true;
      return true;
    }

    bool admit_conversion() {
      if (!adapter_state || adapter_state->poisoned.load(std::memory_order_acquire) ||
          adapter_state->blocked.load(std::memory_order_acquire)) return false;
      std::unique_lock operation(adapter_state->frame_mutex, std::try_to_lock);
      if (!operation.owns_lock()) return false;
      auto &state = retained();
      std::lock_guard guard(state.mutex);
      for (auto *owner = state.generations; owner; owner = owner->registry_next) {
        if (owner == this || !same_luid(owner->luid, luid)) continue;
        if (owner->release_unknown.load(std::memory_order_acquire) || owner->quarantined || owner->conversion_admitted) return false;
      }
      conversion_admitted = true;
      return true;
    }

    void release_conversion_admission() noexcept {
      if (!registered) return;
      auto &state = retained();
      std::lock_guard guard(state.mutex);
      conversion_admitted = false;
    }

    // Post-copy failure recording has no strings, formatting, allocation or log
    // callbacks. Native method effects/exceptions remain native qualification.
    bool submitted_check(HRESULT status, const char *operation) noexcept {
      if (SUCCEEDED(status)) return true;
      failure_status = status;
      failure_operation = operation;
      return false;
    }

    static bool same_identity(IUnknown *left, IUnknown *right) {
      ComPtr<IUnknown> a, b;
      return left && right && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
             SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
    }

    bool stage_source(const source_t &source, capture_mutex_guard_t &pin) {
      if (!source.srv) {
        capture_staged = true;
        capture_guard.take(pin);
        return true;
      }
      if ((source.lease != source_lease_e::async_capture_pool && source.lease != source_lease_e::keyed_standalone) ||
          !source.capture_owner || !source.capture_texture || !source.capture_mutex ||
          !source.mutex || pin.pinned() != source.mutex ||
          !same_identity(source.capture_texture, source.capture_mutex)) return false;
      ComPtr<ID3D11Resource> resource;
      ComPtr<ID3D11Texture2D> texture;
      source.srv->GetResource(&resource);
      if (!resource || FAILED(resource.As(&texture)) || !same_identity(texture.Get(), source.mutex)) return false;
      capture_owner = source.capture_owner;  // Existing control block, no new wrapper.
      capture_texture = source.capture_texture;
      capture_mutex = source.capture_mutex;
      opened_source = texture;
      source_view = source.srv;
      source_mutex = source.mutex;  // Exact interface acquired on the encode device.
      capture_guard.take(pin);  // Pointer/scalar-only transfer before AcquireSync.
      capture_staged = true;
      return true;
    }

    bool check(HRESULT hr, const char *operation) const {
      if (SUCCEEDED(hr)) {
        return true;
      }
      if (log) {
        log(2, std::string("PyroWave D3D12 prototype: ") + operation + " [" + hex(hr) + "]");
      }
      return false;
    }

    bool open_texture(ID3D11Texture2D *texture, ComPtr<ID3D12Resource> &resource) {
      ComPtr<IDXGIResource1> dxgi;
      handle_t handle;
      return check(texture->QueryInterface(IID_PPV_ARGS(&dxgi)), "query shared texture") &&
             check(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle.value), "share texture") &&
             check(device->OpenSharedHandle(handle.value, IID_PPV_ARGS(&resource)), "open shared texture");
    }

    bool open_fence(ID3D11Fence *fence, ComPtr<ID3D12Fence> &shared) {
      handle_t handle;
      return check(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle.value), "share fence") &&
             check(device->OpenSharedHandle(handle.value, IID_PPV_ARGS(&shared)), "open shared fence");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE descriptor(UINT index) const {
      auto value = heap->GetCPUDescriptorHandleForHeapStart();
      value.ptr += SIZE_T(index) * increment;
      return value;
    }

    bool initialize_backend(IDXGIAdapter *adapter) {
      if (!check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "create device")) {
        return false;
      }
      D3D12_COMMAND_QUEUE_DESC queue_desc {};
      queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
      // Normal priority bounds this first prototype; realtime priority is never requested.
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      if (!check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create compute queue") ||
          !check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "create command allocator") ||
          !check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create command list") ||
          !check(list->Close(), "close initial command list") ||
          !check(device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producer11)), "create producer fence") ||
          !open_fence(producer11.Get(), producer) || !open_fence(output11.Get(), output) ||
          !check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retirement)), "create retirement fence")) {
        return false;
      }
      event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!event.value) {
        return false;
      }

      D3D12_DESCRIPTOR_HEAP_DESC heap_desc {};
      heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      heap_desc.NumDescriptors = 4;
      heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      if (!check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "create descriptor heap")) {
        return false;
      }
      increment = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
      for (unsigned i = 0; i < planes.size(); ++i) {
        if (!open_texture(planes11[i].Get(), planes[i])) {
          return false;
        }
        D3D11_TEXTURE2D_DESC plane_desc {};
        planes11[i]->GetDesc(&plane_desc);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support {plane_desc.Format};
        if (!check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "query plane format") ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
          return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.Format = plane_desc.Format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(planes[i].Get(), nullptr, &view, descriptor(i + 1));
      }

      D3D12_DESCRIPTOR_RANGE ranges[2] {};
      ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors = 1;
      ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors = 3;
      D3D12_ROOT_PARAMETER parameters[5] {};
      for (unsigned i = 0; i < 3; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[i].Constants.ShaderRegister = i;
        parameters[i].Constants.Num32BitValues = i == 0 ? 16 : (i == 1 ? 4 : 8);
      }
      for (unsigned i = 0; i < 2; ++i) {
        parameters[i + 3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i + 3].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i + 3].DescriptorTable.pDescriptorRanges = &ranges[i];
      }
      D3D12_STATIC_SAMPLER_DESC sampler {};
      sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      sampler.MaxAnisotropy = 1;
      sampler.MaxLOD = D3D12_FLOAT32_MAX;
      D3D12_ROOT_SIGNATURE_DESC root_desc {};
      root_desc.NumParameters = UINT(std::size(parameters));
      root_desc.pParameters = parameters;
      root_desc.NumStaticSamplers = 1;
      root_desc.pStaticSamplers = &sampler;
      ComPtr<ID3DBlob> code;
      ComPtr<ID3DBlob> messages;
      return check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &messages), "serialize root signature") &&
             check(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root)), "create root signature") &&
             pipeline(transfer_e::unorm) != nullptr;
    }

    ID3D12PipelineState *pipeline(transfer_e transfer) {
      auto &value = pipelines[transfer];
      if (value) {
        return value.Get();
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
      if (!check(D3DCompileFromFile(config.shader_path.c_str(), macros.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages), "compile conversion shader")) {
        return nullptr;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
      desc.pRootSignature = root.Get();
      desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      return check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&value)), "create conversion pipeline") ? value.Get() : nullptr;
    }

    bool prepare_source(const source_t &source, ComPtr<ID3D11Texture2D> &snapshot_source) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view {};
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2D.MipLevels = 1;
      if (!source.srv) {
        device->CreateShaderResourceView(nullptr, &view, descriptor(0));
        return true;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC source_view {};
      source.srv->GetDesc(&source_view);
      ComPtr<ID3D11Resource> resource;
      ComPtr<ID3D11Texture2D> texture;
      source.srv->GetResource(&resource);
      if (FAILED(resource.As(&texture))) {
        return false;
      }
      D3D11_TEXTURE2D_DESC desc {};
      texture->GetDesc(&desc);
      if (desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
          source_view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
          source_view.Texture2D.MostDetailedMip != 0 || source_view.Texture2D.MipLevels != 1 ||
          source_view.Format != desc.Format ||
          (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        return false;
      }
      if (!bridge || bridge_desc.Width != desc.Width || bridge_desc.Height != desc.Height || bridge_desc.Format != desc.Format) {
        bridge.Reset();
        bridge11.Reset();
        bridge_desc = desc;
        bridge_desc.Usage = D3D11_USAGE_DEFAULT;
        bridge_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bridge_desc.CPUAccessFlags = 0;
        bridge_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (!check(device11->CreateTexture2D(&bridge_desc, nullptr, &bridge11), "create source snapshot") ||
            !open_texture(bridge11.Get(), bridge)) {
          return false;
        }
      }
      view.Format = desc.Format;
      device->CreateShaderResourceView(bridge.Get(), &view, descriptor(0));
      // Keep the validated source alive while command preparation can still fail.
      snapshot_source = std::move(texture);
      return true;
    }

    result_e convert(const source_t &source, const void *params, std::uint64_t value) {
      // Every completed call retires the sole allocator and descriptor set.
      if (pending || producer_pending || release_unknown.load(std::memory_order_acquire) || !capture_staged ||
          value == std::numeric_limits<std::uint64_t>::max() ||
          producer_value >= std::numeric_limits<std::uint64_t>::max() - 1) {
        return result_e::failed;
      }
      ComPtr<ID3D11Texture2D> snapshot_source;
      ID3D12PipelineState *shader = nullptr;
      try {
        shader = pipeline(source.srv ? source.transfer : transfer_e::unorm);
        if (!shader || !prepare_source(source, snapshot_source)) {
          return result_e::unsupported;
        }
        if (!check(allocator->Reset(), "reset retired allocator") ||
            !check(list->Reset(allocator.Get(), shader), "reset command list")) {
          return result_e::unsupported;
        }
        // Three output planes plus an optional source; no per-frame heap growth.
        std::array<D3D12_RESOURCE_BARRIER, 1 + std::tuple_size<decltype(planes)>::value> barriers {};
        UINT barrier_count = 0;
        if (source.srv) {
          barriers[barrier_count++] = transition(bridge.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for (auto &plane : planes) {
          barriers[barrier_count++] = transition(plane.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        list->ResourceBarrier(barrier_count, barriers.data());
        list->SetComputeRootSignature(root.Get());
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRoot32BitConstants(0, 16, &config.color_matrix, 0);
        const float sdr_white[4] = {config.sdr_white_nits, 0, 0, 0};
        list->SetComputeRoot32BitConstants(1, 4, sdr_white, 0);
        list->SetComputeRoot32BitConstants(2, 8, params, 0);
        auto table = heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(3, table);
        table.ptr += increment;
        list->SetComputeRootDescriptorTable(4, table);
        const UINT width = UINT(config.yuv444 ? config.width : config.width / 2);
        const UINT height = UINT(config.yuv444 ? config.height : config.height / 2);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        for (UINT i = 0; i < barrier_count; ++i) {
          std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
        }
        list->ResourceBarrier(barrier_count, barriers.data());
        if (!check(list->Close(), "close conversion commands")) {
          return result_e::unsupported;
        }
      } catch (...) {
        // All throw-capable preparation precedes CopyResource/queue submission.
        return result_e::unsupported;
      }
      if (!capture_staged) {
        return result_e::unsupported;
      }
      // The exact acquired interface and complete source lease were staged
      // before AcquireSync. Mark unknown before the void asynchronous copy.
      producer_pending = true;
      producer_unknown = true;
      if (source.srv) context11->CopyResource(bridge11.Get(), opened_source.Get());
      const auto ticket = ++producer_value;
      const HRESULT producer_status = context11->Signal(producer11.Get(), ticket);
      if (!submitted_check(producer_status, "signal source snapshot")) return result_e::failed;
      producer_unknown = false;  // Accepted covering ticket, not observed completion.
      context11->Flush();
      // A failed Wait may have queue effects. Consumer obligations are separate.
      pending = true;
      unknown_completion = true;
      if (!submitted_check(queue->Wait(producer.Get(), ticket), "wait for source snapshot")) return result_e::failed;
      ID3D12CommandList *commands[] = {list.Get()};
      queue->ExecuteCommandLists(1, commands);
      if (!submitted_check(queue->Signal(output.Get(), value), "signal converted planes")) return result_e::failed;
      pending_value = value;
      unknown_completion = false;
      if (!submitted_check(output->SetEventOnCompletion(value, event.value), "arm conversion wait")) return result_e::failed;
      const DWORD waited = WaitForSingleObject(event.value, 2000);
      if (waited != WAIT_OBJECT_0 || FAILED(device->GetDeviceRemovedReason()) ||
          output->GetCompletedValue() == std::numeric_limits<std::uint64_t>::max() || !finished()) {
        failure_status = E_FAIL;
        failure_operation = "conversion completion not proven";
        return result_e::failed;
      }
      if (!release_source()) return result_e::failed;
      pending = false;
      return result_e::ok;
    }
  };

  d3d12_conversion_t::d3d12_conversion_t() = default;

  d3d12_conversion_t::~d3d12_conversion_t() {
    if (!impl) return;
    if (!impl->acquire_abandoned.load(std::memory_order_acquire) && impl->queue && !impl->consumer_finished()) {
      // This tail covers only the consumer queue, never an unknown D3D11 copy.
      impl->retired = true;
      impl->unknown_completion = FAILED(impl->queue->Signal(impl->retirement.Get(), 1));
      impl->pending_value = 1;
    }
    bool safe_to_destroy = impl->finished();
    if (safe_to_destroy) safe_to_destroy = impl->release_source();
    if (!safe_to_destroy) {
      // Unknown queue/copy completion blocks this LUID while quarantine is
      // retained. A failed ReleaseSync additionally sets permanent poison.
      auto &state = impl_t::retained();
      {
        // Publish quarantine before opening the per-LUID gate. The collector
        // may clear a temporary block only after scanning these linked nodes.
        // Lock order is frame gate -> registry, matching admit_conversion().
        std::lock_guard guard(state.mutex);
        if (impl->adapter_state) {
          impl->adapter_state->blocked.store(true, std::memory_order_release);
          if (impl->release_unknown.load(std::memory_order_acquire)) {
            impl->adapter_state->poisoned.store(true, std::memory_order_release);
          }
        }
        impl->quarantined = true;
        if (impl->frame_lock.owns_lock()) impl->frame_lock.unlock();
        impl.release();  // Existing node/owners, no post-copy allocation or release.
      }
    }
  }

  std::unique_ptr<d3d12_conversion_t> d3d12_conversion_t::create(
    IDXGIAdapter *adapter,
    ID3D11Device *device,
    ID3D11DeviceContext4 *context,
    const std::array<ID3D11Texture2D *, 3> &planes,
    ID3D11Fence *output_fence,
    const core_config_t &config,
    log_fn log
  ) {
    try {
      auto converter = std::unique_ptr<d3d12_conversion_t>(new d3d12_conversion_t);
      converter->impl = std::make_unique<impl_t>();
      auto &state = *converter->impl;
      state.config = config;
      state.log = std::move(log);
      if (!adapter || FAILED(adapter->GetDesc(&state.adapter_desc))) return nullptr;
      state.luid = state.adapter_desc.AdapterLuid;
      state.fallback_device = device;
      state.fallback_context = context;
      state.fallback_fence = output_fence;
      state.context11 = context;
      state.output11 = output_fence;
      for (unsigned i = 0; i < planes.size(); ++i) {
        state.planes11[i] = planes[i];
        state.fallback_planes[i] = planes[i];
      }
      if (!state.register_sink()) return nullptr;

      // Reserve the unique D3D12 backend slot before initialization. If backend
      // setup fails, keep this already-registered object as a D3D11-only sink.
      if (state.admit_conversion()) {
        bool ready = false;
        try {
          ready = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&state.device11))) && state.initialize_backend(adapter);
        } catch (...) {
          ready = false;
        }
        if (ready) {
          state.conversion_enabled.store(true, std::memory_order_release);
        } else {
          state.release_conversion_admission();
        }
      }
      return converter;
    } catch (...) {
      // Factory/setup allocation failures happen before capture acquisition;
      // fail the requested core session instead of creating an uncovered path.
      return nullptr;
    }
  }

  d3d12_conversion_t::capture_e d3d12_conversion_t::prepare_capture(const source_t &source, capture_mutex_guard_t &pin) {
    if (!impl || !impl->registered || !impl->adapter_state || impl->frame_lock.owns_lock()) {
      return capture_e::failed;
    }
    try {
      impl->frame_lock = std::unique_lock<std::mutex>(impl->adapter_state->frame_mutex, std::try_to_lock);
    } catch (...) {
      return capture_e::failed;
    }
    if (!impl->frame_lock.owns_lock()) return capture_e::busy;
    if (impl->adapter_state->poisoned.load(std::memory_order_acquire) ||
        impl->adapter_state->blocked.load(std::memory_order_acquire) ||
        impl->release_unknown.load(std::memory_order_acquire) || impl->capture_staged || !impl->capture_guard.empty()) {
      impl->frame_lock.unlock();
      return capture_e::failed;
    }
    if (source.srv && (!source.mutex || !impl->stage_source(source, pin))) {
      impl->frame_lock.unlock();
      return capture_e::failed;
    }
    if (!source.srv) {
      impl->capture_staged = true;
      impl->capture_guard.take(pin);
    }
    return capture_e::ready;
  }

  IDXGIKeyedMutex *d3d12_conversion_t::capture_mutex() const noexcept {
    return impl ? impl->capture_guard.pinned() : nullptr;
  }

  void d3d12_conversion_t::arm_capture() noexcept {
    if (impl && impl->capture_staged && !impl->capture_guard.empty()) impl->capture_guard.arm();
  }

  void d3d12_conversion_t::cancel_capture() noexcept {
    if (!impl || !impl->capture_staged || impl->capture_guard.held()) return;
    impl->release_source();
  }

  void d3d12_conversion_t::abandon_capture() noexcept {
    if (!impl) return;
    impl->acquire_abandoned.store(true, std::memory_order_release);
    if (impl->adapter_state) {
      impl->adapter_state->poisoned.store(true, std::memory_order_release);
      impl->adapter_state->blocked.store(true, std::memory_order_release);
    }
    if (impl->registered) {
      auto &state = impl_t::retained();
      // The normal caller still owns the frame gate, so lock order is
      // frame gate -> registry. The already-linked node retains the staged
      // image, original/opened textures, views, and exact mutex pins.
      std::lock_guard guard(state.mutex);
      impl->quarantined = true;
    }
    // Poison/quarantine are visible before the gate opens. Future same-LUID
    // frames fail admission, while collection can observe the terminal state.
    if (impl->frame_lock.owns_lock()) impl->frame_lock.unlock();
  }

  bool d3d12_conversion_t::release_fallback() noexcept {
    return impl && impl->release_source();
  }

  void d3d12_conversion_t::disable_conversion() noexcept {
    if (!impl) return;
    impl->conversion_enabled.store(false, std::memory_order_release);
    impl->release_conversion_admission();
  }

  bool d3d12_conversion_t::using_conversion() const noexcept {
    return impl && impl->conversion_enabled.load(std::memory_order_acquire);
  }

  d3d12_conversion_t::result_e d3d12_conversion_t::convert(const source_t &source, const void *params, std::uint64_t value) {
    if (!impl || !impl->conversion_enabled.load(std::memory_order_acquire) ||
        !impl->frame_lock.owns_lock() || impl->adapter_state->poisoned.load(std::memory_order_acquire)) return result_e::failed;
    return impl->convert(source, params, value);
  }
}  // namespace pyrowave::d3d11
