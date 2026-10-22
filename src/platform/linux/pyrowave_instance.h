/** @file src/platform/linux/pyrowave_instance.h
 * @brief Shared Vulkan instance ownership for Linux PyroWave devices.
 */
#pragma once

#include "context.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace pyrowave::linux_gpu {
  // Once a writer reaches admission, later readers cannot overtake it while
  // existing encodes drain. Reader work itself remains parallel.
  class dispatch_gate_t {
  public:
    std::shared_lock<std::shared_mutex> lock_shared() {
      std::lock_guard admitted(admission);
      return std::shared_lock(dispatch);
    }

    std::unique_lock<std::shared_mutex> lock_exclusive() {
      return lock_exclusive([] {
      });
    }

    template<class OnAdmitted>
    std::unique_lock<std::shared_mutex> lock_exclusive(OnAdmitted on_admitted) {
      std::lock_guard admitted(admission);
      on_admitted();
      return std::unique_lock(dispatch);
    }

  private:
    std::mutex admission;
    std::shared_mutex dispatch;
  };

  /**
   * @brief Let each Granite context own its device while sharing one instance.
   *
   * The process keeps one factory alive until supervised cleanup; every device
   * context also holds a lease until its resources and context are destroyed.
   * Releasing a stream therefore cannot enter the Vulkan instance teardown that
   * stalled the video join in the October 21 incident.
   */
  class instance_factory_t final: public Vulkan::InstanceFactory {
  public:
    instance_factory_t(PFN_vkCreateInstance create, PFN_vkGetInstanceProcAddr get_proc, PFN_vkDestroyInstance destroy = nullptr):
        create(create),
        get_proc(get_proc),
        destroy(destroy) {}

    ~instance_factory_t() override {
      if (instance != VK_NULL_HANDLE) {
        destroy(instance, nullptr);
      }
    }

    VkInstance create_instance(const VkInstanceCreateInfo *info) override {
      std::lock_guard lock(mutex);
      if (instance != VK_NULL_HANDLE) {
        return instance;
      }

      // Allocate/copy before creating the driver object. Failed creation leaves
      // no inherited configuration, so a later probe can retry normally.
      auto saved = std::make_unique<create_info_t>(*info);
      VkInstance created = VK_NULL_HANDLE;
      if (create(info, nullptr, &created) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      if (!destroy) {
        destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(created, "vkDestroyInstance"));
      }
      inherited_info = std::move(saved);
      instance = created;
      return instance;
    }

    const VkInstanceCreateInfo *get_existing_create_info() override {
      std::lock_guard lock(mutex);
      return inherited_info ? &inherited_info->info : nullptr;
    }

    bool factory_owns_created_instance() override {
      return true;
    }

  private:
    struct create_info_t {
      explicit create_info_t(const VkInstanceCreateInfo &source):
          info(source) {
        // Granite inherits only the application API version and enabled names.
        // Creation-only chains can point to its stack (validation features in a
        // debug build); the original chain is consumed by vkCreateInstance but
        // must not escape that call.
        info.pNext = nullptr;
        if (source.pApplicationInfo) {
          application_info = *source.pApplicationInfo;
          application_info.pNext = nullptr;
          if (application_info.pApplicationName) {
            application_name = application_info.pApplicationName;
            application_info.pApplicationName = application_name.c_str();
          }
          if (application_info.pEngineName) {
            engine_name = application_info.pEngineName;
            application_info.pEngineName = engine_name.c_str();
          }
          info.pApplicationInfo = &application_info;
        }
        for (std::uint32_t i = 0; i < source.enabledExtensionCount; ++i) {
          extensions.emplace_back(source.ppEnabledExtensionNames[i]);
        }
        for (std::uint32_t i = 0; i < source.enabledLayerCount; ++i) {
          layers.emplace_back(source.ppEnabledLayerNames[i]);
        }
        for (const auto &extension : extensions) {
          extension_names.push_back(extension.c_str());
        }
        for (const auto &layer : layers) {
          layer_names.push_back(layer.c_str());
        }
        info.ppEnabledExtensionNames = extension_names.empty() ? nullptr : extension_names.data();
        info.ppEnabledLayerNames = layer_names.empty() ? nullptr : layer_names.data();
      }

      VkInstanceCreateInfo info;
      VkApplicationInfo application_info {};
      std::string application_name, engine_name;
      std::vector<std::string> extensions, layers;
      std::vector<const char *> extension_names, layer_names;
    };

    std::mutex mutex;
    PFN_vkCreateInstance create;
    PFN_vkGetInstanceProcAddr get_proc;
    PFN_vkDestroyInstance destroy;
    VkInstance instance {VK_NULL_HANDLE};
    std::unique_ptr<create_info_t> inherited_info;
  };

  /** Process admission and an explicit handoff to the supervised shutdown path. */
  class instance_lifetime_t {
  public:
    bool is_closed() {
      std::lock_guard lock(mutex);
      return closed;
    }

    std::shared_ptr<instance_factory_t> acquire(PFN_vkCreateInstance create, PFN_vkGetInstanceProcAddr get_proc, PFN_vkDestroyInstance destroy = nullptr) {
      std::lock_guard lock(mutex);
      if (closed) {
        return {};
      }
      if (!factory) {
        factory = std::make_shared<instance_factory_t>(create, get_proc, destroy);
      }
      return factory;
    }

    // The caller releases this lease after dropping the global dispatch lock,
    // with the process shutdown deadline armed around possible driver waits.
    std::shared_ptr<instance_factory_t> close() {
      std::lock_guard lock(mutex);
      closed = true;
      return std::exchange(factory, {});
    }

  private:
    std::mutex mutex;
    bool closed {false};
    std::shared_ptr<instance_factory_t> factory;
  };
}  // namespace pyrowave::linux_gpu
