#pragma once

#include "device.hpp"
#include "pyrowave_core.h"

#include <cstring>
#include <ffnvcodec/dynlink_loader.h>
#include <stdexcept>
#include <unistd.h>

namespace pyrowave::linux_gpu {
  // External GPU semaphores and explicit queue ownership transfers bracket each
  // CUDA invocation (CPU fences remain a compatibility fallback). Only opaque GPU allocations cross the API boundary.
  class truehdr_t {
  public:
    Vulkan::BufferHandle input, output;

    truehdr_t() = default;

    ~truehdr_t() {
      if (context) {
        CUcontext previous {};
        cuda->cuCtxPushCurrent(context);
        if (stream) {
          cuda->cuStreamSynchronize(stream);
        }
        effect.reset();
        if (cuda_timeline) {
          cuda->cuDestroyExternalSemaphore(cuda_timeline);
        }
        for (int i = 0; i < 2; ++i) {
          if (pointers[i]) {
            cuda->cuMemFree(pointers[i]);
          }
          if (memory[i]) {
            cuda->cuDestroyExternalMemory(memory[i]);
          }
        }
        if (stream) {
          platf::linux_truehdr::destroy_stream(runtime, stream);
        }
        cuda->cuCtxPopCurrent(&previous);
        cuda->cuDevicePrimaryCtxRelease(selected);
      }
      cuda_free_functions(&cuda);
    }

    void init(Vulkan::Device &device, const config_t &config) {
      runtime = config.truehdr_runtime;
      if (cuda_load_functions(&cuda, nullptr) != 0 || !cuda->cuImportExternalMemory || !cuda->cuExternalMemoryGetMappedBuffer || !cuda->cuDestroyExternalMemory || !cuda->cuDeviceGetUuid) {
        throw std::runtime_error("PyroWave RTX HDR: CUDA external memory unavailable");
      }
      check(cuda->cuInit(0));
      VkPhysicalDeviceIDProperties id {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
      vkGetPhysicalDeviceProperties2(device.get_physical_device(), &properties);
      int count = 0;
      check(cuda->cuDeviceGetCount(&count));
      selected = -1;
      for (int i = 0; i < count; ++i) {
        CUdevice candidate;
        CUuuid uuid {};
        check(cuda->cuDeviceGet(&candidate, i));
        check(cuda->cuDeviceGetUuid(&uuid, candidate));
        if (std::memcmp(uuid.bytes, id.deviceUUID, VK_UUID_SIZE) == 0) {
          selected = candidate;
        }
      }
      if (selected < 0) {
        throw std::runtime_error("PyroWave RTX HDR requires an NVIDIA capture/encode GPU");
      }
      check(cuda->cuDevicePrimaryCtxRetain(&context, selected));
      check(cuda->cuCtxPushCurrent(context));
      // Restore the caller even on failure.
      try {
        stream = static_cast<CUstream>(platf::linux_truehdr::create_stream(config.truehdr_runtime));
        effect = std::make_unique<platf::linux_truehdr::effect_t>(config.truehdr_runtime, config.width, config.height, stream);
        Vulkan::BufferCreateInfo info;
        info.size = std::size_t(config.width) * config.height * 4;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.misc = Vulkan::BUFFER_MISC_EXTERNAL_MEMORY_BIT;
        input = device.create_buffer(info);
        output = device.create_buffer(info);
        if (!input || !output) {
          throw std::runtime_error("PyroWave RTX HDR: external GPU allocation failed");
        }
        import(*input, 0);
        import(*output, 1);
        if (config.truehdr_external_sync && cuda->cuImportExternalSemaphore && cuda->cuDestroyExternalSemaphore && cuda->cuWaitExternalSemaphoresAsync && cuda->cuSignalExternalSemaphoresAsync) {
          timeline = device.request_semaphore_external(VK_SEMAPHORE_TYPE_TIMELINE, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
          if (timeline) {
            auto handle = timeline->export_to_handle();
            if (handle) {
              CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc {};
              desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
              desc.handle.fd = handle.handle;
              if (cuda->cuImportExternalSemaphore(&cuda_timeline, &desc) != CUDA_SUCCESS) {
                close(handle.handle);
                cuda_timeline = nullptr;
              }
            }
          }
        }
      } catch (...) {
        CUcontext popped {};
        cuda->cuCtxPopCurrent(&popped);
        throw;
      }
      CUcontext popped {};
      check(cuda->cuCtxPopCurrent(&popped));
    }

    bool asynchronous() const {
      return cuda_timeline != nullptr;
    }

    std::uint64_t next_value() {
      if (value > UINT64_MAX - 2) {
        throw std::runtime_error("PyroWave RTX HDR: timeline exhausted");
      }
      value += 2;
      return value;
    }

    Vulkan::Semaphore timeline;

    void run(const platf::linux_truehdr::settings_t &settings, int width, int height) {
      check(cuda->cuCtxPushCurrent(context));
      try {
        if (cuda_timeline) {
          CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wait {};
          wait.params.fence.value = value - 1;
          check(cuda->cuWaitExternalSemaphoresAsync(&cuda_timeline, &wait, 1, stream));
        }
        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = pointers[0];
        copy.srcPitch = width * 4;
        copy.dstDevice = effect->input();
        copy.dstPitch = effect->input_pitch();
        copy.WidthInBytes = width * 4;
        copy.Height = height;
        check(cuda->cuMemcpy2DAsync(&copy, stream));
        effect->run(settings);
        copy = {};
        copy.srcMemoryType = copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.WidthInBytes = width * 4;
        copy.Height = height;
        copy.srcDevice = effect->output();
        copy.srcPitch = effect->output_pitch();
        copy.dstDevice = pointers[1];
        copy.dstPitch = width * 4;
        check(cuda->cuMemcpy2DAsync(&copy, stream));
        if (cuda_timeline) {
          CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS signal {};
          signal.params.fence.value = value;
          check(cuda->cuSignalExternalSemaphoresAsync(&cuda_timeline, &signal, 1, stream));
        } else {
          check(cuda->cuStreamSynchronize(stream));
        }
      } catch (...) {
        CUcontext popped {};
        cuda->cuCtxPopCurrent(&popped);
        throw;
      }
      CUcontext popped {};
      check(cuda->cuCtxPopCurrent(&popped));
    }

  private:
    std::filesystem::path runtime;
    CudaFunctions *cuda = nullptr;
    CUcontext context = nullptr;
    CUdevice selected = -1;
    CUstream stream = nullptr;
    CUexternalSemaphore cuda_timeline = nullptr;
    std::uint64_t value = 0;
    CUexternalMemory memory[2] {};
    CUdeviceptr pointers[2] {};
    std::unique_ptr<platf::linux_truehdr::effect_t> effect;

    static void check(CUresult result) {
      if (result != CUDA_SUCCESS) {
        throw std::runtime_error("PyroWave RTX HDR: CUDA error " + std::to_string(result));
      }
    }

    void import(Vulkan::Buffer &buffer, int index) {
      auto handle = buffer.export_handle();
      if (!handle) {
        throw std::runtime_error("PyroWave RTX HDR: cannot export GPU memory");
      }
      CUDA_EXTERNAL_MEMORY_HANDLE_DESC desc {};
      desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
      desc.handle.fd = handle.handle;
      desc.size = buffer.get_allocation().get_size();
      desc.flags = 0x1;  // CUDA_EXTERNAL_MEMORY_DEDICATED (omitted by nv-codec-headers).
      auto result = cuda->cuImportExternalMemory(&memory[index], &desc);
      if (result != CUDA_SUCCESS) {
        close(handle.handle);  // CUDA owns the FD on success.
      }
      check(result);
      CUDA_EXTERNAL_MEMORY_BUFFER_DESC mapped {};
      mapped.size = buffer.get_create_info().size;
      check(cuda->cuExternalMemoryGetMappedBuffer(&pointers[index], memory[index], &mapped));
    }
  };
}  // namespace pyrowave::linux_gpu
