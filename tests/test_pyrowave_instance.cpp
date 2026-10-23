/** Deterministic lifetime tests for the shared Linux PyroWave Vulkan instance. */
#include "src/platform/linux/pyrowave_instance.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
  using pyrowave::linux_gpu::instance_factory_t;

  void require(bool ok, const char *message) {
    if (!ok) {
      throw std::runtime_error(message);
    }
  }

  struct fake_driver_t {
    static inline fake_driver_t *active = nullptr;

    const VkInstance instance = reinterpret_cast<VkInstance>(std::uintptr_t {0x1234});
    std::atomic<unsigned> creates {0};
    std::atomic<unsigned> destroys {0};
    std::atomic<unsigned> bad_arguments {0};
    std::atomic<unsigned> proc_lookups {0};
    unsigned failed_attempts = 0;
    const VkInstanceCreateInfo *expected_info = nullptr;
    const void *expected_create_chain = nullptr;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool block_creation = false;
    bool creation_entered = false;
    bool release_creation = false;

    fake_driver_t() {
      require(!active, "overlapping fake Vulkan drivers");
      active = this;
    }

    ~fake_driver_t() {
      active = nullptr;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL create(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
      auto &driver = *active;
      const auto attempt = ++driver.creates;
      if (!info || allocator || !out) {
        ++driver.bad_arguments;
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      if (driver.expected_info && (info != driver.expected_info || info->pNext != driver.expected_create_chain)) {
        ++driver.bad_arguments;
      }
      {
        std::unique_lock lock(driver.gate_mutex);
        driver.creation_entered = true;
        driver.gate_cv.notify_all();
        if (driver.block_creation) {
          driver.gate_cv.wait(lock, [&driver] {
            return driver.release_creation;
          });
        }
      }
      if (attempt <= driver.failed_attempts) {
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      *out = driver.instance;
      return VK_SUCCESS;
    }

    static VKAPI_ATTR void VKAPI_CALL destroy(VkInstance instance, const VkAllocationCallbacks *allocator) {
      auto &driver = *active;
      ++driver.destroys;
      if (instance != driver.instance || allocator) {
        ++driver.bad_arguments;
      }
    }

    static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_proc(VkInstance instance, const char *name) {
      auto &driver = *active;
      ++driver.proc_lookups;
      if (instance != driver.instance || !name || std::strcmp(name, "vkDestroyInstance")) {
        ++driver.bad_arguments;
        return nullptr;
      }
      return reinterpret_cast<PFN_vkVoidFunction>(&destroy);
    }
  };

  void test_copied_create_info_and_reuse() {
    fake_driver_t driver;
    {
      instance_factory_t factory(fake_driver_t::create, nullptr, fake_driver_t::destroy);
      require(factory.factory_owns_created_instance(), "factory must own its Vulkan instance");
      require(!factory.get_existing_create_info(), "uninitialized factory exposed create info");

      char application[] = "capture application";
      char engine[] = "capture engine";
      char extension_a[] = "VK_test_extension_a";
      char extension_b[] = "VK_test_extension_b";
      char layer_a[] = "VK_test_layer_a";
      char layer_b[] = "VK_test_layer_b";
      const char *extensions[] = {extension_a, extension_b};
      const char *layers[] = {layer_a, layer_b};
      VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.pApplicationName = application;
      app.applicationVersion = 23;
      app.pEngineName = engine;
      app.engineVersion = 42;
      app.apiVersion = VK_API_VERSION_1_3;
      VkValidationFeaturesEXT validation {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
      VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      info.pNext = &validation;
      info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
      info.pApplicationInfo = &app;
      info.enabledExtensionCount = 2;
      info.ppEnabledExtensionNames = extensions;
      info.enabledLayerCount = 2;
      info.ppEnabledLayerNames = layers;
      driver.expected_info = &info;
      driver.expected_create_chain = &validation;

      require(factory.create_instance(&info) == driver.instance, "instance creation failed");
      const auto *saved = factory.get_existing_create_info();
      require(saved && saved != &info, "factory retained the caller's create-info pointer");
      require(!saved->pNext, "factory retained a transient instance-creation chain");
      require(saved->pApplicationInfo && saved->pApplicationInfo != &app, "factory retained the caller's application-info pointer");
      require(saved->pApplicationInfo->pApplicationName != application && saved->pApplicationInfo->pEngineName != engine, "factory retained the caller's application-name pointers");
      require(saved->ppEnabledExtensionNames != extensions && saved->ppEnabledLayerNames != layers, "factory retained the caller's name arrays");
      require(saved->ppEnabledExtensionNames[0] != extension_a && saved->ppEnabledExtensionNames[1] != extension_b && saved->ppEnabledLayerNames[0] != layer_a && saved->ppEnabledLayerNames[1] != layer_b, "factory retained the caller's extension or layer strings");

      std::fill_n(application, sizeof(application) - 1, 'x');
      std::fill_n(engine, sizeof(engine) - 1, 'x');
      std::fill_n(extension_a, sizeof(extension_a) - 1, 'x');
      std::fill_n(extension_b, sizeof(extension_b) - 1, 'x');
      std::fill_n(layer_a, sizeof(layer_a) - 1, 'x');
      std::fill_n(layer_b, sizeof(layer_b) - 1, 'x');
      extensions[0] = extensions[1] = "changed extension";
      layers[0] = layers[1] = "changed layer";
      app = {};
      info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

      require(saved->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO && saved->flags == VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, "create-info values changed with caller storage");
      require(saved->pApplicationInfo->sType == VK_STRUCTURE_TYPE_APPLICATION_INFO && saved->pApplicationInfo->applicationVersion == 23 && saved->pApplicationInfo->engineVersion == 42 && saved->pApplicationInfo->apiVersion == VK_API_VERSION_1_3, "application-info values changed with caller storage");
      require(!std::strcmp(saved->pApplicationInfo->pApplicationName, "capture application") && !std::strcmp(saved->pApplicationInfo->pEngineName, "capture engine"), "application names changed with caller storage");
      require(saved->enabledExtensionCount == 2 && saved->enabledLayerCount == 2 && !std::strcmp(saved->ppEnabledExtensionNames[0], "VK_test_extension_a") && !std::strcmp(saved->ppEnabledExtensionNames[1], "VK_test_extension_b") && !std::strcmp(saved->ppEnabledLayerNames[0], "VK_test_layer_a") && !std::strcmp(saved->ppEnabledLayerNames[1], "VK_test_layer_b"), "extension or layer names changed with caller storage");
      require(factory.create_instance(&info) == driver.instance, "factory did not return the existing instance");
      require(factory.get_existing_create_info() == saved, "instance reuse replaced the published create info");
      require(driver.creates == 1 && driver.destroys == 0, "instance reuse recreated or prematurely destroyed the instance");
    }
    require(driver.destroys == 1 && driver.bad_arguments == 0, "cached instance was not destroyed exactly once");
  }

  void test_empty_and_failed_factories() {
    fake_driver_t driver;
    {
      instance_factory_t empty(fake_driver_t::create, nullptr, fake_driver_t::destroy);
    }
    require(driver.creates == 0 && driver.destroys == 0, "unused factory touched the Vulkan driver");
    driver.failed_attempts = 1;
    {
      instance_factory_t factory(fake_driver_t::create, nullptr, fake_driver_t::destroy);
      VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      require(factory.create_instance(&info) == VK_NULL_HANDLE, "failed Vulkan creation returned an instance");
      require(!factory.get_existing_create_info(), "failed creation published cached create info");
      require(driver.destroys == 0, "failed instance creation destroyed an unowned instance");
    }
    require(driver.destroys == 0 && driver.bad_arguments == 0, "failed factory destruction touched an unowned instance");
  }

  void test_failure_retry() {
    fake_driver_t driver;
    driver.failed_attempts = 1;
    {
      instance_factory_t factory(fake_driver_t::create, nullptr, fake_driver_t::destroy);
      VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      require(factory.create_instance(&info) == VK_NULL_HANDLE, "first creation should fail");
      require(!factory.get_existing_create_info(), "failed attempt became the shared instance");
      VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
      app.pApplicationName = "successful retry";
      info.pApplicationInfo = &app;
      require(factory.create_instance(&info) == driver.instance, "failed creation could not retry");
      const auto *saved = factory.get_existing_create_info();
      require(saved && saved->pApplicationInfo && !std::strcmp(saved->pApplicationInfo->pApplicationName, "successful retry"), "successful retry retained create info from the failed attempt");
      require(factory.create_instance(&info) == driver.instance && driver.creates == 2, "successful retry was not cached");
    }
    require(driver.destroys == 1 && driver.bad_arguments == 0, "retry did not destroy exactly the successful instance");
  }

  void test_concurrent_creation() {
    fake_driver_t driver;
    driver.block_creation = true;
    {
      instance_factory_t factory(fake_driver_t::create, nullptr, fake_driver_t::destroy);
      constexpr std::size_t caller_count = 12;
      std::barrier start(static_cast<std::ptrdiff_t>(caller_count + 1));
      std::array<VkInstance, caller_count> results {};
      std::array<std::thread, caller_count> callers;
      VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      for (std::size_t i = 0; i < caller_count; ++i) {
        callers[i] = std::thread([&, i] {
          start.arrive_and_wait();
          results[i] = factory.create_instance(&info);
        });
      }
      start.arrive_and_wait();
      {
        std::unique_lock lock(driver.gate_mutex);
        driver.gate_cv.wait(lock, [&driver] {
          return driver.creation_entered;
        });
        driver.release_creation = true;
      }
      driver.gate_cv.notify_all();
      for (auto &caller : callers) {
        caller.join();
      }
      require(std::all_of(results.begin(), results.end(), [&driver](VkInstance instance) {
                return instance == driver.instance;
              }),
              "concurrent callers received different instances");
      require(driver.creates == 1 && driver.destroys == 0, "concurrent callers created multiple instances or destroyed the shared instance");
      const auto *saved = factory.get_existing_create_info();
      require(saved && !saved->pApplicationInfo && saved->enabledExtensionCount == 0 && saved->enabledLayerCount == 0, "minimal create info was not retained");
    }
    require(driver.destroys == 1 && driver.bad_arguments == 0, "concurrent creation did not destroy exactly one instance");
  }

  void test_shared_owner_lifetime() {
    fake_driver_t driver;

    struct core_owner_t {
      std::shared_ptr<instance_factory_t> owner;
      VkInstance instance;

      explicit core_owner_t(std::shared_ptr<instance_factory_t> factory):
          owner(std::move(factory)) {
        VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance = owner->create_instance(&info);
      }
    };

    auto owner = std::make_shared<instance_factory_t>(fake_driver_t::create, nullptr, fake_driver_t::destroy);
    std::weak_ptr<instance_factory_t> weak = owner;
    auto first = std::make_unique<core_owner_t>(owner);
    auto second = std::make_unique<core_owner_t>(owner);
    require(first->instance == driver.instance && second->instance == first->instance && driver.creates == 1, "core owners did not share a single instance");
    owner.reset();
    require(!weak.expired() && driver.destroys == 0, "dropping the outer owner destroyed an instance still used by cores");
    first.reset();
    require(!weak.expired() && driver.destroys == 0, "destroying one core destroyed an instance still used by another core");
    VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    require(second->owner->create_instance(&info) == second->instance && driver.creates == 1, "remaining core could not reuse the instance");
    second.reset();
    require(weak.expired() && driver.destroys == 1 && driver.bad_arguments == 0, "last core did not release exactly one instance");
  }

  void test_instance_scoped_destroy_lookup() {
    fake_driver_t driver;
    {
      instance_factory_t factory(fake_driver_t::create, fake_driver_t::get_proc);
      VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      require(factory.create_instance(&info) == driver.instance, "instance creation with a proc resolver failed");
    }
    require(driver.creates == 1 && driver.proc_lookups == 1 && driver.destroys == 1 && driver.bad_arguments == 0, "factory did not resolve and call destroy for the created instance");
  }

  void test_terminal_instance_lifetime() {
    fake_driver_t driver;
    pyrowave::linux_gpu::instance_lifetime_t lifetime;
    auto owner = lifetime.acquire(fake_driver_t::create, nullptr, fake_driver_t::destroy);
    auto same_owner = lifetime.acquire(fake_driver_t::create, nullptr, fake_driver_t::destroy);
    require(owner && owner == same_owner && driver.creates == 0, "process lifetime did not lazily share one factory");
    VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    require(owner->create_instance(&info) == driver.instance, "process lifetime could not create its shared instance");
    auto first_core = owner;
    auto second_core = same_owner;
    std::weak_ptr<instance_factory_t> weak = owner;
    owner.reset();
    same_owner.reset();

    auto shutdown_owner = lifetime.close();
    require(shutdown_owner == first_core && driver.destroys == 0, "process shutdown did not transfer its existing factory owner");
    require(!lifetime.acquire(fake_driver_t::create, nullptr, fake_driver_t::destroy), "late encoder creation reopened the process instance after shutdown");
    require(!lifetime.close(), "repeated process shutdown returned another owner");
    require(driver.creates == 1 && driver.destroys == 0, "terminal acquisition or repeated close touched the driver");
    shutdown_owner.reset();
    require(!weak.expired() && driver.destroys == 0, "process shutdown destroyed an instance still leased by cores");
    first_core.reset();
    require(!weak.expired() && driver.destroys == 0, "one retiring core destroyed another core's instance");
    second_core.reset();
    require(weak.expired() && driver.destroys == 1 && driver.bad_arguments == 0, "last retained core did not destroy exactly one closed instance");

    pyrowave::linux_gpu::instance_lifetime_t unused;
    require(!unused.close() && !unused.acquire(fake_driver_t::create, nullptr, fake_driver_t::destroy), "closing an unused lifetime did not permanently reject late creation");
    require(driver.creates == 1 && driver.destroys == 1, "closed unused process lifetime touched the driver");
  }

  void test_dispatch_gate_shared_admission() {
    pyrowave::linux_gpu::dispatch_gate_t gate;
    auto first = gate.lock_shared();
    std::promise<void> second_acquired;
    auto second_ready = second_acquired.get_future();
    std::promise<void> release_second;
    auto second_release = release_second.get_future();
    std::thread second([&] {
      auto lock = gate.lock_shared();
      second_acquired.set_value();
      second_release.wait();
    });
    const bool overlapped = second_ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    // Release both owners before reporting a failure so a rejected admission
    // cannot strand the worker in this test's cleanup.
    first.unlock();
    release_second.set_value();
    second.join();
    require(overlapped, "dispatch gate serialized two shared operations");
  }

  void test_dispatch_gate_writer_precedes_late_reader() {
    pyrowave::linux_gpu::dispatch_gate_t gate;
    auto initial_reader = gate.lock_shared();
    std::promise<void> writer_admitted;
    auto admitted = writer_admitted.get_future();
    std::promise<void> writer_acquired;
    auto writer_ready = writer_acquired.get_future();
    std::promise<void> release_writer;
    auto writer_release = release_writer.get_future();
    std::promise<void> late_reader_started;
    auto reader_started = late_reader_started.get_future();
    std::promise<void> late_reader_acquired;
    auto reader_ready = late_reader_acquired.get_future();
    std::atomic<unsigned> acquisition_order {0};
    unsigned writer_order = 0, reader_order = 0;
    std::thread writer([&] {
      auto lock = gate.lock_exclusive([&] {
        writer_admitted.set_value();
      });
      writer_order = ++acquisition_order;
      writer_acquired.set_value();
      writer_release.wait();
    });
    // The production admission hook runs while holding the admission mutex,
    // before the writer waits for the still-active initial shared operation.
    admitted.wait();
    std::thread late_reader([&] {
      late_reader_started.set_value();
      auto lock = gate.lock_shared();
      reader_order = ++acquisition_order;
      late_reader_acquired.set_value();
    });
    reader_started.wait();
    initial_reader.unlock();
    writer_ready.wait();
    const bool reader_blocked = reader_ready.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    release_writer.set_value();
    writer.join();
    late_reader.join();
    require(reader_blocked, "dispatch gate admitted a reader while an exclusive operation held the lock");
    require(writer_order == 1 && reader_order == 2, "dispatch gate admitted a late reader ahead of an already-waiting writer");
  }
}  // namespace

int main() {
  try {
    test_copied_create_info_and_reuse();
    test_empty_and_failed_factories();
    test_failure_retry();
    test_concurrent_creation();
    test_shared_owner_lifetime();
    test_instance_scoped_destroy_lookup();
    test_terminal_instance_lifetime();
    test_dispatch_gate_shared_admission();
    test_dispatch_gate_writer_precedes_late_reader();
    std::cout << "PyroWave Vulkan instance lifetime tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
