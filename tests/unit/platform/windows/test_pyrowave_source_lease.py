#!/usr/bin/env python3
"""Exercise V7 abandoned-acquire quarantine with controlled CPU leaves.

The fixture extracts exact production ownership bodies. It never executes
Windows, COM, D3D, or GPU APIs. Native teardown and driver behavior remain
separate qualification gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def extract(converter):
    text = converter.read_text()
    impl = {
        'retained': function(text, '    static retained_t &retained()'),
        'impl_destructor': function(text, '    ~impl_t()'),
        'producer_finished': function(text, '    bool producer_finished() const'),
        'consumer_finished': function(text, '    bool consumer_finished() const'),
        'finished': function(text, '    bool finished() const'),
        'release_source': function(text, '    bool release_source() noexcept'),
        'same_luid': function(text, '    static bool same_luid(const LUID &left, const LUID &right) noexcept'),
        'unlink_locked': function(text, '    static void unlink_locked(retained_t &state, impl_t *owner) noexcept'),
        'collect_completed': function(text, '    static void collect_completed()'),
        'register_sink': function(text, '    bool register_sink()'),
        'admit_conversion': function(text, '    bool admit_conversion()'),
        'same_identity': function(text, '    static bool same_identity(IUnknown *left, IUnknown *right)'),
        'stage_source': function(text, '    bool stage_source(const source_t &source, capture_mutex_guard_t &pin)'),
    }
    wrapper = {
        'prepare_capture': function(text, '  d3d12_conversion_t::capture_e d3d12_conversion_t::prepare_capture('),
        'capture_mutex': function(text, '  IDXGIKeyedMutex *d3d12_conversion_t::capture_mutex() const noexcept'),
        'arm_capture': function(text, '  void d3d12_conversion_t::arm_capture() noexcept'),
        'cancel_capture': function(text, '  void d3d12_conversion_t::cancel_capture() noexcept'),
        'release_fallback': function(text, '  bool d3d12_conversion_t::release_fallback() noexcept'),
        'abandon_capture': function(text, '  void d3d12_conversion_t::abandon_capture() noexcept'),
        'converter_destructor': function(text, '  d3d12_conversion_t::~d3d12_conversion_t()'),
        'factory_create': function(text, '  std::unique_ptr<d3d12_conversion_t> d3d12_conversion_t::create('),
    }
    wrapper['prepare_capture'] = wrapper['prepare_capture'].replace(
        'd3d12_conversion_t::capture_e d3d12_conversion_t::prepare_capture', 'capture_e prepare_capture')
    for name in ('capture_mutex', 'arm_capture', 'cancel_capture', 'release_fallback', 'abandon_capture'):
        wrapper[name] = wrapper[name].replace('d3d12_conversion_t::' + name, name)
    return impl, wrapper


PREFIX = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

static bool deny_allocation = false;
static std::size_t denied_allocation_attempts = 0;
void *operator new(std::size_t size) {
  if (deny_allocation) { ++denied_allocation_attempts; throw std::bad_alloc(); }
  if (auto *value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }
static void require(bool ok, const char *why) {
  if (!ok) { std::fprintf(stderr, "%s\n", why); std::_Exit(92); }
}
static thread_local bool registry_lock_held = false;
using owner_destroy_hook_t = void (*)();
static owner_destroy_hook_t owner_destroy_hook = nullptr;
using release_hook_t = void (*)();
static release_hook_t release_hook = nullptr;
struct registry_mutex_t {
  std::mutex inner;
  void lock() { inner.lock(); registry_lock_held = true; }
  void unlock() { registry_lock_held = false; inner.unlock(); }
};
struct owner_t {
  ~owner_t() {
    require(!registry_lock_held, "image owner destroyed under registry mutex");
    if (owner_destroy_hook) owner_destroy_hook();
  }
};
using HRESULT = int;
static constexpr HRESULT S_OK = 0, E_FAIL = -1;
#define FAILED(value) ((value) < 0)
#define SUCCEEDED(value) ((value) >= 0)
#define IID_PPV_ARGS(pointer) pointer
enum class source_lease_e { unproven, async_capture_pool, keyed_standalone };
enum class transfer_e { unorm };
enum class result_e { ok, unsupported, failed };
struct LUID { std::uint32_t LowPart = 0; std::int32_t HighPart = 0; };
struct release_state_t { int calls = 0; HRESULT result = S_OK; bool held = false; } release_state;
struct IUnknown {
  IUnknown *identity = nullptr;
  int refs = 1;
  virtual ~IUnknown() = default;
  void AddRef() { ++refs; }
  void Release() { require(!registry_lock_held, "COM reference released under registry mutex"); --refs; }
  HRESULT QueryInterface(IUnknown **out) {
    require(!registry_lock_held, "COM QueryInterface called under registry mutex");
    *out = identity ? identity : this; (*out)->AddRef(); return S_OK;
  }
};
struct IDXGIKeyedMutex: IUnknown {
  HRESULT AcquireSync(std::uint64_t key, unsigned long) {
    require(!registry_lock_held, "AcquireSync called under registry mutex");
    require(key == 0 && !release_state.held, "wrong key or overlapping acquire");
    release_state.held = true;
    return S_OK;
  }
  HRESULT ReleaseSync(std::uint64_t key) {
    require(!registry_lock_held, "ReleaseSync called under registry mutex");
    require(key == 0 && release_state.held, "wrong key, duplicate or unowned release");
    ++release_state.calls;
    release_state.held = false;
    if (release_hook) release_hook();
    return release_state.result;
  }
};
struct ID3D11Resource: IUnknown {};
struct ID3D11Texture2D: ID3D11Resource {};
struct ID3D11ShaderResourceView: IUnknown {
  ID3D11Resource *resource = nullptr;
  void GetResource(ID3D11Resource **out) {
    require(!registry_lock_held, "GetResource called under registry mutex");
    *out = resource; if (*out) (*out)->AddRef();
  }
};
template<class T> struct ComPtr {
  T *value = nullptr;
  ComPtr() = default;
  ComPtr(const ComPtr &other): value(other.value) { if (value) value->AddRef(); }
  ComPtr &operator=(const ComPtr &other) {
    if (this == &other) return *this;
    Reset(); value = other.value; if (value) value->AddRef(); return *this;
  }
  ComPtr(ComPtr &&other) noexcept: value(std::exchange(other.value, nullptr)) {}
  ComPtr &operator=(ComPtr &&other) noexcept { Reset(); value = std::exchange(other.value, nullptr); return *this; }
  ComPtr &operator=(T *other) { Reset(); value = other; if (value) value->AddRef(); return *this; }
  ~ComPtr() { Reset(); }
  T *Get() const { return value; }
  T **operator&() { Reset(); return &value; }
  explicit operator bool() const { return value != nullptr; }
  void Reset() { if (auto *old = std::exchange(value, nullptr)) old->Release(); }
  template<class U> HRESULT As(U **out) { *out = dynamic_cast<U *>(value); if (*out) (*out)->AddRef(); return *out ? S_OK : E_FAIL; }
};
__ACTUAL_CAPTURE_GUARD__
struct source_t {
  ID3D11ShaderResourceView *srv = nullptr;
  IDXGIKeyedMutex *mutex = nullptr;
  source_lease_e lease = source_lease_e::unproven;
  std::shared_ptr<const void> capture_owner;
  ID3D11Texture2D *capture_texture = nullptr;
  IDXGIKeyedMutex *capture_mutex = nullptr;
};
struct fake_device_t {
  HRESULT removal_status = S_OK;
  HRESULT GetDeviceRemovedReason() const {
    require(!registry_lock_held, "GetDeviceRemovedReason called under registry mutex");
    return removal_status;
  }
};
struct fake_fence_t {
  std::uint64_t completed = 0;
  mutable int queries = 0;
  std::uint64_t GetCompletedValue() const {
    require(!registry_lock_held, "GetCompletedValue called under registry mutex");
    ++queries; return completed;
  }
};
struct fake_queue_t {
  HRESULT signal_status = S_OK;
  bool complete_on_signal = false;
  int signal_calls = 0;
  HRESULT Signal(fake_fence_t *fence, std::uint64_t value) {
    require(!registry_lock_held, "queue Signal called under registry mutex");
    ++signal_calls;
    if (SUCCEEDED(signal_status) && complete_on_signal) fence->completed = value;
    return signal_status;
  }
};
template<class T> struct fake_ptr_t {
  T *value = nullptr;
  T *Get() const { return value; }
  T *operator->() const { return value; }
  explicit operator bool() const { return value != nullptr; }
};
'''

SKELETON = r'''
struct d3d12_conversion_t;
struct impl_t {
  struct luid_state_t {
    LUID luid {};
    std::mutex frame_mutex;
    std::atomic<bool> poisoned {false};
    std::atomic<bool> blocked {false};
    luid_state_t *next = nullptr;
  };
  struct retained_t {
    registry_mutex_t mutex;
    impl_t *generations = nullptr;
    luid_state_t *adapters = nullptr;
  };
  LUID luid {};
  std::shared_ptr<const void> capture_owner;
  ComPtr<IUnknown> native_marker;
  ComPtr<ID3D11Texture2D> capture_texture;
  ComPtr<IDXGIKeyedMutex> capture_mutex;
  ComPtr<ID3D11Texture2D> opened_source;
  ComPtr<ID3D11ShaderResourceView> source_view;
  ComPtr<IDXGIKeyedMutex> source_mutex;
  capture_mutex_guard_t capture_guard;
  bool producer_pending = false, producer_unknown = false;
  std::uint64_t producer_value = 0;
  fake_ptr_t<fake_fence_t> producer11;
  bool pending = false, unknown_completion = false, retired = false;
  std::uint64_t pending_value = 0;
  fake_ptr_t<fake_device_t> device;
  fake_ptr_t<fake_fence_t> output, retirement;
  fake_ptr_t<fake_queue_t> queue;
  std::atomic<bool> release_unknown {false};
  std::atomic<bool> acquire_abandoned {false};
  bool capture_staged = false;
  luid_state_t *adapter_state = nullptr;
  std::unique_lock<std::mutex> frame_lock;
  HRESULT failure_status = S_OK;
  const char *failure_operation = nullptr;
  bool collecting = false, registered = false, conversion_admitted = false, quarantined = false;
  impl_t *collection_next = nullptr, *registry_next = nullptr;

  __ACTUAL_IMPL_MEMBERS__
};

struct d3d12_conversion_t {
  enum class capture_e { ready, busy, failed };
  std::unique_ptr<impl_t> impl;
  capture_e prepare_capture(const source_t &, capture_mutex_guard_t &);
  IDXGIKeyedMutex *capture_mutex() const noexcept;
  void arm_capture() noexcept;
  void cancel_capture() noexcept;
  bool release_fallback() noexcept;
  void abandon_capture() noexcept;
  ~d3d12_conversion_t();
};
'''

SUFFIX = r'''
struct identity_t: IUnknown {};
struct fixture_t {
  identity_t original_identity, opened_identity, unrelated_identity;
  identity_t deletion_marker;
  ID3D11Texture2D original, opened;
  IDXGIKeyedMutex original_mutex, opened_mutex;
  ID3D11ShaderResourceView view;
  fake_device_t device;
  fake_fence_t producer11, producer, output, retirement;
  fake_queue_t queue;
  std::unique_ptr<d3d12_conversion_t> converter = std::make_unique<d3d12_conversion_t>();
  std::shared_ptr<owner_t> owner = std::make_shared<owner_t>();
  std::weak_ptr<owner_t> weak_owner = owner;
  source_t source;
  fixture_t() {
    original.identity = &original_identity; original_mutex.identity = &original_identity;
    opened.identity = &opened_identity; opened_mutex.identity = &opened_identity;
    view.resource = &opened;
    converter->impl = std::make_unique<impl_t>();
    converter->impl->luid = {7, 1};
    converter->impl->native_marker = &deletion_marker;
    converter->impl->device.value = &device;
    converter->impl->producer11.value = &producer11;
    converter->impl->output.value = &output;
    converter->impl->retirement.value = &retirement;
    converter->impl->queue.value = &queue;
    require(converter->impl->register_sink() && converter->impl->admit_conversion(), "sink/backend admission failed");
    source.srv = &view; source.mutex = &opened_mutex;
    source.lease = source_lease_e::async_capture_pool;
    source.capture_owner = owner; source.capture_texture = &original; source.capture_mutex = &original_mutex;
  }
  bool stage_without_acquire() {
    capture_mutex_guard_t pin(source.mutex);
    if (converter->prepare_capture(source, pin) != d3d12_conversion_t::capture_e::ready) return false;
    return pin.empty() && converter->capture_mutex() == &opened_mutex &&
           opened.refs > 1 && view.refs > 1 && original.refs > 1 &&
           original_mutex.refs > 1 && opened_mutex.refs > 1 && !weak_owner.expired();
  }
  bool stage_and_acquire() {
    capture_mutex_guard_t pin(source.mutex);
    if (converter->prepare_capture(source, pin) != d3d12_conversion_t::capture_e::ready) return false;
    require(pin.empty() && converter->capture_mutex() == &opened_mutex,
            "sink did not take the exact unarmed acquire interface before acquisition");
    require(opened.refs > 1 && view.refs > 1 && original.refs > 1 &&
            original_mutex.refs > 1 && opened_mutex.refs > 1 && !weak_owner.expired(),
            "pre-acquire staging omitted a source owner or COM reference");
    require(converter->capture_mutex()->AcquireSync(0, 17) == S_OK, "controlled acquire failed");
    converter->arm_capture();
    require(converter->impl->capture_guard.held() == &opened_mutex, "S_OK did not arm the exact pinned interface");
    return true;
  }
};

static bool registry_contains(impl_t *wanted);
static void failed_release_retains_and_poisons() {
  fixture_t f;
  require(f.stage_and_acquire(), "valid source was not staged");
  release_state.result = E_FAIL;
  impl_t *expected_node = f.converter->impl.get();
  deny_allocation = true;
  require(!f.converter->release_fallback(), "failed fallback release was reported as success");
  require(release_state.calls == 1 && !release_state.held, "ambiguous release was retried or not disarmed");
  require(expected_node->release_unknown.load() && expected_node->adapter_state->poisoned.load() &&
          expected_node->adapter_state->blocked.load() && !expected_node->frame_lock.owns_lock(),
          "failed release did not poison its full-LUID state and release the frame gate");
  require(expected_node->capture_guard.pinned() == &f.opened_mutex && expected_node->capture_guard.held() == nullptr &&
          expected_node->capture_owner && expected_node->capture_texture.Get() == &f.original &&
          expected_node->capture_mutex.Get() == &f.original_mutex && expected_node->opened_source.Get() == &f.opened &&
          expected_node->source_view.Get() == &f.view && expected_node->source_mutex.Get() == &f.opened_mutex &&
          !f.weak_owner.expired(), "failed release dropped exact owner or opened/original references");
  // The release path was checked with allocation denied; later registration of a new LUID may allocate its state.
  deny_allocation = false;
  impl_t peer; peer.luid = {7, 1};
  require(!peer.register_sink() && release_state.calls == 1, "same-LUID sink passed poison or retried release");
  impl_t independent; independent.luid = {7, 2};
  require(independent.register_sink() && independent.admit_conversion(),
          "failed-release poison crossed the full-LUID boundary");
  // Denial resumes only after unrelated-LUID registration has allocated its own state.
  // Count attempts so a caught bad_alloc cannot make this teardown check pass.
  deny_allocation = true;
  const std::size_t denied_before_destroy = denied_allocation_attempts;
  f.converter.reset();  // Actual destructor must quarantine the existing node without allocation.
  require(denied_allocation_attempts == denied_before_destroy,
          "converter quarantine destructor attempted scalar/array allocation");
  deny_allocation = false;
  f.source.capture_owner.reset(); f.owner.reset();
  require(registry_contains(expected_node) && expected_node->quarantined &&
          expected_node->release_unknown.load() && !f.weak_owner.expired(),
          "failed-release teardown destroyed or detached the retained node/owner");
  f.device.removal_status = E_FAIL;
  f.producer11.completed = std::numeric_limits<std::uint64_t>::max();
  f.output.completed = std::numeric_limits<std::uint64_t>::max();
  f.retirement.completed = std::numeric_limits<std::uint64_t>::max();
  impl_t::collect_completed();
  require(registry_contains(expected_node) && expected_node->release_unknown.load() &&
          expected_node->capture_guard.pinned() == &f.opened_mutex && !f.weak_owner.expired() && release_state.calls == 1,
          "collector retried or cleared sticky failed-release poison after all completion signals");
  deny_allocation = false;
}

static void abandoned_acquire_is_quarantined() {
  fixture_t f;
  require(f.stage_without_acquire(), "valid source was not staged before WAIT_ABANDONED");
  impl_t *node = f.converter->impl.get();
  node->pending = true;
  node->pending_value = 9;
  f.output.completed = 0;
  f.queue.signal_status = E_FAIL;
  f.converter->abandon_capture();
  require(node->acquire_abandoned.load(std::memory_order_acquire) &&
          node->adapter_state->poisoned.load(std::memory_order_acquire) &&
          node->adapter_state->blocked.load(std::memory_order_acquire) && node->quarantined &&
          !node->frame_lock.owns_lock() && registry_contains(node),
          "WAIT_ABANDONED did not poison, quarantine, and release the frame gate");
  require(node->capture_guard.pinned() == &f.opened_mutex && node->capture_guard.held() == nullptr &&
          node->capture_owner && node->capture_texture.Get() == &f.original &&
          node->capture_mutex.Get() == &f.original_mutex && node->opened_source.Get() == &f.opened &&
          node->source_view.Get() == &f.view && node->source_mutex.Get() == &f.opened_mutex &&
          !f.weak_owner.expired() && release_state.calls == 0,
          "abandoned acquire released or dropped an unarmed source owner/interface");
  impl_t same_luid; same_luid.luid = {7, 1};
  require(!same_luid.register_sink(), "same full-LUID sink passed abandoned-acquire poison");
  impl_t other_luid; other_luid.luid = {7, 2};
  require(other_luid.register_sink() && other_luid.admit_conversion(),
          "abandoned-acquire poison crossed to a different full LUID");
  deny_allocation = true;
  const std::size_t denied_before_destroy = denied_allocation_attempts;
  f.converter.reset();
  require(denied_allocation_attempts == denied_before_destroy,
          "abandoned-acquire quarantine destructor attempted scalar/array allocation");
  deny_allocation = false;
  f.source.capture_owner.reset(); f.owner.reset();
  require(registry_contains(node) && node->quarantined && !f.weak_owner.expired() &&
          release_state.calls == 0 && f.queue.signal_calls == 0,
          "abandoned teardown released source or emitted an unproven retirement signal");
  f.device.removal_status = E_FAIL;
  f.producer11.completed = std::numeric_limits<std::uint64_t>::max();
  f.output.completed = std::numeric_limits<std::uint64_t>::max();
  f.retirement.completed = std::numeric_limits<std::uint64_t>::max();
  impl_t::collect_completed();
  require(registry_contains(node) && node->acquire_abandoned.load(std::memory_order_acquire) &&
          node->capture_guard.pinned() == &f.opened_mutex && !f.weak_owner.expired() &&
          release_state.calls == 0 && f.queue.signal_calls == 0,
          "completion or device-removal signals cleared abandoned-acquire quarantine");
}

static void successful_release_clears_lease() {
  fixture_t f;
  impl_t *node = f.converter->impl.get();
  require(f.stage_and_acquire(), "valid source was not staged");
  release_state.result = S_OK;
  require(f.converter->release_fallback(), "successful fallback release failed");
  require(release_state.calls == 1 && !release_state.held && !node->capture_staged &&
          !node->adapter_state->poisoned.load() && !node->adapter_state->blocked.load(), "successful release retained or poisoned the lease");
  require(!node->capture_owner && !node->capture_texture && !node->capture_mutex &&
          !node->opened_source && !node->source_view && !node->source_mutex, "successful release kept source references");
  f.converter.reset(); f.source.capture_owner.reset(); f.owner.reset();
  require(f.weak_owner.expired(), "successful release left its caller owner retained");
}

static void canceled_acquire_does_not_release() {
  fixture_t f;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::ready,
          "valid source did not stage");
  f.converter->cancel_capture();
  require(release_state.calls == 0 && !release_state.held && !f.converter->impl->capture_staged &&
          !f.converter->impl->capture_owner && !f.converter->impl->frame_lock.owns_lock(),
          "non-acquired staging attempted release or stayed pinned");
}

static void invalid_source_fails_closed() {
  fixture_t f;
  capture_mutex_guard_t pin(f.source.mutex);
  f.source.capture_owner.reset();
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "missing caller owner did not fail before AcquireSync");
  require(release_state.calls == 0 && !release_state.held && !f.converter->impl->frame_lock.owns_lock() &&
          pin.pinned() == &f.opened_mutex && !f.converter->impl->capture_staged && !f.converter->impl->capture_owner &&
          !f.converter->impl->capture_texture && !f.converter->impl->capture_mutex && !f.converter->impl->opened_source &&
          !f.converter->impl->source_view && !f.converter->impl->source_mutex,
          "invalid source acquired/released, staged sink refs, or lost the caller's unarmed pin");
}

static void unproven_lease_fails_closed() {
  fixture_t f;
  f.source.lease = source_lease_e::unproven;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "unproven caller lease did not fail before AcquireSync");
  require(release_state.calls == 0 && !release_state.held && !f.converter->impl->frame_lock.owns_lock() &&
          pin.pinned() == &f.opened_mutex && !f.converter->impl->capture_staged && !f.converter->impl->capture_owner &&
          !f.converter->impl->capture_texture && !f.converter->impl->capture_mutex && !f.converter->impl->opened_source &&
          !f.converter->impl->source_view && !f.converter->impl->source_mutex,
          "unproven lease acquired/released, staged sink refs, or lost the caller's unarmed pin");
}

static void wrong_opened_mutex_fails_closed() {
  fixture_t f;
  f.opened_mutex.identity = &f.unrelated_identity;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "opened SRV and keyed mutex identity mismatch passed staging");
  require(release_state.calls == 0 && !release_state.held && !f.converter->impl->frame_lock.owns_lock() &&
          pin.pinned() == &f.opened_mutex && !f.converter->impl->capture_staged && !f.converter->impl->capture_owner &&
          !f.converter->impl->capture_texture && !f.converter->impl->capture_mutex && !f.converter->impl->opened_source &&
          !f.converter->impl->source_view && !f.converter->impl->source_mutex,
          "wrong opened mutex acquired/released, staged sink refs, or lost the caller's unarmed pin");
}

static void wrong_capture_mutex_fails_closed() {
  fixture_t f;
  f.original_mutex.identity = &f.unrelated_identity;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "original capture texture and keyed mutex identity mismatch passed staging");
  require(release_state.calls == 0 && !release_state.held && !f.converter->impl->frame_lock.owns_lock() &&
          pin.pinned() == &f.opened_mutex && !f.converter->impl->capture_staged && !f.converter->impl->capture_owner &&
          !f.converter->impl->capture_texture && !f.converter->impl->capture_mutex && !f.converter->impl->opened_source &&
          !f.converter->impl->source_view && !f.converter->impl->source_mutex,
          "wrong capture mutex acquired/released, staged sink refs, or lost the caller's unarmed pin");
}

static void missing_keyed_mutex_fails_closed() {
  fixture_t f;
  f.source.mutex = nullptr;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "nonblank source without an opened keyed mutex did not fail before AcquireSync");
  require(release_state.calls == 0 && !f.converter->impl->capture_staged &&
          !f.converter->impl->frame_lock.owns_lock(), "missing opened mutex staged or retained a lease");
}

static void missing_original_mutex_fails_closed() {
  fixture_t f;
  f.source.capture_mutex = nullptr;
  capture_mutex_guard_t pin(f.source.mutex);
  require(f.converter->prepare_capture(f.source, pin) == d3d12_conversion_t::capture_e::failed,
          "nonblank source without its original keyed mutex did not fail before AcquireSync");
  require(release_state.calls == 0 && !f.converter->impl->capture_staged &&
          !f.converter->impl->frame_lock.owns_lock(), "missing original mutex staged or retained a lease");
}

static void full_luid_comparison_uses_high_part() {
  impl_t first, same, different;
  first.luid = {7, 1}; same.luid = {7, 1}; different.luid = {7, 2};
  require(impl_t::same_luid(first.luid, same.luid), "identical full LUIDs differ");
  require(!impl_t::same_luid(first.luid, different.luid), "LUID comparison ignored HighPart");
  require(first.register_sink() && same.register_sink() && different.register_sink(), "full LUID sink registration failed");
  require(first.adapter_state != different.adapter_state, "registry merged adapters with different HighPart");
  require(first.admit_conversion() && !same.admit_conversion() && different.admit_conversion(),
          "full LUID conversion admission or fallback sink sharing changed");
}

static impl_t *collecting_node_probe = nullptr;
static std::weak_ptr<owner_t> *collecting_owner_probe = nullptr;
static impl_t *release_same_luid_probe = nullptr;
static impl_t *release_different_luid_probe = nullptr;
static impl_t *owner_same_luid_probe = nullptr;
static impl_t *owner_different_luid_probe = nullptr;
static bool release_callback_saw_collecting_node = false;
static bool release_same_luid_reentry_denied = false;
static bool release_different_luid_reentry_allowed = false;
static bool nested_collector_skipped_collecting_node = false;
static bool owner_callback_saw_collecting_node = false;
static bool owner_same_luid_reentry_denied = false;
static bool owner_different_luid_reentry_allowed = false;

static void collector_reentry(const char *message, impl_t *same_probe, impl_t *different_probe,
                              bool &same_denied, bool &different_allowed) {
  require(!registry_lock_held, message);
  release_callback_saw_collecting_node = collecting_node_probe->collecting &&
      collecting_node_probe->quarantined && collecting_node_probe->adapter_state->blocked.load() &&
      registry_contains(collecting_node_probe);
  const int releases = release_state.calls;
  impl_t::collect_completed();
  nested_collector_skipped_collecting_node = release_state.calls == releases &&
      collecting_node_probe->collecting && registry_contains(collecting_node_probe);
  same_denied = !same_probe->register_sink();
  different_allowed = different_probe->register_sink() && different_probe->admit_conversion();
}

static void collector_release_reentry() {
  require(collecting_owner_probe && !collecting_owner_probe->expired(),
          "source owner dropped before ReleaseSync returned");
  collector_reentry("ReleaseSync callback ran under registry mutex", release_same_luid_probe,
                    release_different_luid_probe, release_same_luid_reentry_denied,
                    release_different_luid_reentry_allowed);
}

static void collector_owner_reentry() {
  require(collecting_owner_probe && collecting_owner_probe->expired(),
          "successful source release did not reach the owner deleter");
  collector_reentry("owner deleter ran under registry mutex", owner_same_luid_probe,
                    owner_different_luid_probe, owner_same_luid_reentry_denied,
                    owner_different_luid_reentry_allowed);
  owner_callback_saw_collecting_node = collecting_node_probe->collecting &&
      registry_contains(collecting_node_probe);
}

static bool registry_contains(impl_t *wanted) {
  for (auto *node = impl_t::retained().generations; node; node = node->registry_next) {
    if (node == wanted) return true;
  }
  return false;
}

static void collector_retains_until_both_tails_finish() {
  fixture_t f;
  require(f.stage_and_acquire(), "valid source was not staged for collection");
  impl_t *node = f.converter->impl.get();
  auto *adapter = node->adapter_state;
  node->producer_pending = true;
  node->producer_value = 3;
  f.producer11.completed = 0;
  node->pending = true;
  node->pending_value = 9;
  f.output.completed = 0;
  impl_t release_same_probe, release_different_probe, owner_same_probe, owner_different_probe;
  release_same_probe.luid = owner_same_probe.luid = {7, 1};
  release_different_probe.luid = {7, 2};
  owner_different_probe.luid = {7, 3};
  release_same_luid_probe = &release_same_probe;
  release_different_luid_probe = &release_different_probe;
  owner_same_luid_probe = &owner_same_probe;
  owner_different_luid_probe = &owner_different_probe;
  collecting_node_probe = node;
  collecting_owner_probe = &f.weak_owner;
  f.converter.reset();
  f.source.capture_owner.reset();
  f.owner.reset();
  require(node->quarantined && registry_contains(node) && !f.weak_owner.expired() &&
          adapter->blocked.load(), "incomplete producer/consumer tails were not retained and blocked");
  const int output_queries_after_destructor = f.output.queries;
  const int retirement_queries_after_destructor = f.retirement.queries;
  require(output_queries_after_destructor > 0 && retirement_queries_after_destructor == 0,
          "fixture did not distinguish the destructor's output-fence check from collector probes");
  impl_t::collect_completed();
  require(node->quarantined && registry_contains(node) && !f.weak_owner.expired() && release_state.calls == 0,
          "incomplete producer/consumer tails were released");
  require(f.output.queries == output_queries_after_destructor &&
          f.retirement.queries == retirement_queries_after_destructor,
          "collector queried a consumer fence before the producer tail completed");
  f.producer11.completed = 3;
  const int retirement_queries_before = f.retirement.queries;
  impl_t::collect_completed();
  require(node->quarantined && registry_contains(node) && !f.weak_owner.expired() && release_state.calls == 0,
          "producer completion incorrectly discharged the consumer tail");
  require(f.retirement.queries > retirement_queries_before,
          "collector did not independently check retired consumer completion after producer completion");
  f.retirement.completed = 1;
  release_hook = collector_release_reentry;
  owner_destroy_hook = collector_owner_reentry;
  impl_t::collect_completed();
  release_hook = nullptr;
  owner_destroy_hook = nullptr;
  require(release_callback_saw_collecting_node && owner_callback_saw_collecting_node &&
          nested_collector_skipped_collecting_node && release_same_luid_reentry_denied &&
          release_different_luid_reentry_allowed && owner_same_luid_reentry_denied &&
          owner_different_luid_reentry_allowed,
          "collector callbacks lost the in-progress gate, nested claim, or independent-LUID behavior");
  require(!registry_contains(node) && release_state.calls == 1 && !release_state.held && f.weak_owner.expired() &&
          !adapter->blocked.load() && f.deletion_marker.refs == 1,
          "completed source was not released/unlinked/deleted exactly once outside registry lock");
  collecting_node_probe = nullptr;
  collecting_owner_probe = nullptr;
  release_same_luid_probe = nullptr;
  release_different_luid_probe = nullptr;
  owner_same_luid_probe = nullptr;
  owner_different_luid_probe = nullptr;
}

static void collector_release_failure_is_sticky() {
  fixture_t f;
  require(f.stage_and_acquire(), "valid source was not staged for failed collection release");
  impl_t *node = f.converter->impl.get();
  node->producer_pending = true;
  node->producer_value = 3;
  f.producer11.completed = 0;
  node->pending = true;
  node->pending_value = 9;
  f.output.completed = 0;
  f.converter.reset();
  f.source.capture_owner.reset();
  f.owner.reset();
  require(node->quarantined && !f.weak_owner.expired(), "incomplete tails were not quarantined");
  f.producer11.completed = 3;
  f.retirement.completed = 1;
  release_state.result = E_FAIL;
  impl_t::collect_completed();
  require(registry_contains(node) && node->quarantined && node->release_unknown.load() &&
          node->adapter_state->poisoned.load() && node->adapter_state->blocked.load() &&
          !f.weak_owner.expired() && node->capture_guard.pinned() == &f.opened_mutex &&
          node->capture_guard.held() == nullptr && release_state.calls == 1,
          "collector failed release detached ownership or omitted sticky poison");
  f.device.removal_status = E_FAIL;
  f.output.completed = std::numeric_limits<std::uint64_t>::max();
  f.retirement.completed = std::numeric_limits<std::uint64_t>::max();
  impl_t::collect_completed();
  require(registry_contains(node) && !f.weak_owner.expired() && release_state.calls == 1,
          "collector retried failed ReleaseSync after completion/device-removal signals");
}

int main(int argc, char **argv) {
  require(argc == 2, "expected one case");
  if (!std::strcmp(argv[1], "failed-release")) failed_release_retains_and_poisons();
  else if (!std::strcmp(argv[1], "successful-release")) successful_release_clears_lease();
  else if (!std::strcmp(argv[1], "cancel-before-acquire")) canceled_acquire_does_not_release();
  else if (!std::strcmp(argv[1], "missing-owner")) invalid_source_fails_closed();
  else if (!std::strcmp(argv[1], "unproven-lease")) unproven_lease_fails_closed();
  else if (!std::strcmp(argv[1], "wrong-opened-mutex")) wrong_opened_mutex_fails_closed();
  else if (!std::strcmp(argv[1], "wrong-capture-mutex")) wrong_capture_mutex_fails_closed();
  else if (!std::strcmp(argv[1], "missing-keyed-mutex")) missing_keyed_mutex_fails_closed();
  else if (!std::strcmp(argv[1], "missing-original-mutex")) missing_original_mutex_fails_closed();
  else if (!std::strcmp(argv[1], "full-luid")) full_luid_comparison_uses_high_part();
  else if (!std::strcmp(argv[1], "collector-retention")) collector_retains_until_both_tails_finish();
  else if (!std::strcmp(argv[1], "collector-release-failure")) collector_release_failure_is_sticky();
  else if (!std::strcmp(argv[1], "abandoned-acquire")) abandoned_acquire_is_quarantined();
  else require(false, "unknown case");
  return 0;
}
'''


def generated(converter, header):
    impl, wrapper = extract(converter)
    guard = function(header.read_text(), '  class capture_mutex_guard_t') + ';'
    actual_members = '\n'.join(impl.values())
    wrapper_methods = '\n'.join(wrapper[name] for name in ('prepare_capture', 'capture_mutex', 'arm_capture', 'cancel_capture', 'release_fallback', 'abandon_capture'))
    code = PREFIX.replace('__ACTUAL_CAPTURE_GUARD__', guard)
    code += SKELETON.replace('__ACTUAL_IMPL_MEMBERS__', actual_members)
    code = code.replace('  std::unique_ptr<impl_t> impl;\n  capture_e prepare_capture(const source_t &, capture_mutex_guard_t &);\n  IDXGIKeyedMutex *capture_mutex() const noexcept;\n  void arm_capture() noexcept;\n  void cancel_capture() noexcept;\n  bool release_fallback() noexcept;\n  void abandon_capture() noexcept;\n  ~d3d12_conversion_t();',
                        '  std::unique_ptr<impl_t> impl;\n' + wrapper_methods + '\n  ~d3d12_conversion_t();')
    code += wrapper['converter_destructor'] + '\n' + SUFFIX
    return code, impl, wrapper


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repository', type=Path)
    parser.add_argument('compiler')
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--prepare-only', action='store_true',
                        help='extract and freeze the controlled source without compiling or running it')
    parser.add_argument('--only-case', choices=['abandoned-acquire'],
                        help='focus execution on the V7-only case while preserving the full inventory')
    args = parser.parse_args()
    converter = args.repository / 'src/platform/windows/pyrowave_d3d12_conversion.cpp'
    header = args.repository / 'src/platform/windows/pyrowave_d3d11_core.h'
    code, impl, wrapper = generated(converter, header)
    fallback_bindings = [
        'state.fallback_device = device;',
        'state.fallback_context = context;',
        'state.fallback_fence = output_fence;',
        'state.fallback_planes[i] = planes[i];',
    ]
    absent = [binding for binding in fallback_bindings if binding not in wrapper['factory_create']]
    if absent:
        raise AssertionError(f'fallback sink no longer pins its D3D11 device/context/output owners: {absent}')
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    source_name = 'pyrowave-v7-source-lease-input.cpp'
    source = (args.evidence_dir / source_name if args.prepare_only and args.evidence_dir
              else Path('/tmp') / source_name)
    source.write_text(code)
    cases = ['failed-release', 'successful-release', 'cancel-before-acquire', 'missing-owner',
             'unproven-lease', 'wrong-opened-mutex', 'wrong-capture-mutex',
             'missing-keyed-mutex', 'missing-original-mutex', 'full-luid', 'collector-retention',
             'collector-release-failure', 'abandoned-acquire']
    selected_cases = [args.only_case] if args.only_case else cases
    evidence = {
        'source': str(converter.resolve()),
        'source_sha256': hashlib.sha256(converter.read_bytes()).hexdigest(),
        'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
        'extracted_body_sha256': {name: hashlib.sha256(value.encode()).hexdigest()
                                  for name, value in {**impl, **wrapper}.items()},
        'factory_fallback_owner_binding_source_strings': fallback_bindings,
        'factory_fallback_owner_binding_mode': 'static source-string presence only; the factory_create body is hashed but not executed or inserted into this fixture',
        'generated_source_sha256': hashlib.sha256(code.encode()).hexdigest(),
        'scope': 'Exact source staging/release, full-LUID admission, producer/consumer completion, collector unlink, and teardown bodies with controlled CPU-only COM/image leaves; no Windows or GPU API execution.',
        'cases': cases,
        'selected_cases': selected_cases,
        'pending_compile_command': [args.compiler, '-std=c++23', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', '/tmp/issue625-v7-source-lease'],
        'pending_run_commands': ['/tmp/issue625-v7-source-lease ' + case for case in selected_cases],
        'compiler_or_test_executed': False,
    }
    if args.evidence_dir:
        evidence_source = args.evidence_dir / source.name
        if source.resolve() != evidence_source.resolve():
            shutil.copy2(source, evidence_source)
    if args.prepare_only:
        evidence['mode'] = 'prepared; compile and execution pending root authorization'
        if args.evidence_dir:
            (args.evidence_dir / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print(json.dumps({'cases_prepared': len(selected_cases),
                          'case_inventory': len(cases),
                          'generated_source_sha256': evidence['generated_source_sha256'],
                          'compile_pending_root_go': True}))
        return

    command = evidence['pending_compile_command']
    compiled = subprocess.run(command, capture_output=True, text=True)
    if compiled.returncode != 0:
        raise RuntimeError(f"controlled source lease fixture did not compile:\n{compiled.stderr}")
    evidence['compiler_or_test_executed'] = True
    evidence['compile_exit'] = compiled.returncode
    evidence['compile_stdout'] = compiled.stdout
    evidence['compile_stderr'] = compiled.stderr
    evidence['runs'] = []
    for case in selected_cases:
        proc = subprocess.run(['/tmp/issue625-v7-source-lease', case], capture_output=True, text=True, timeout=2)
        if proc.returncode != 0:
            raise RuntimeError(f"controlled source lease case {case} failed:\n{proc.stderr}")
        evidence['runs'].append({'case': case, 'exit': proc.returncode, 'stderr': proc.stderr})
    if args.evidence_dir:
        (args.evidence_dir / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'cases_passed': len(cases), 'generated_source_sha256': evidence['generated_source_sha256']}))


if __name__ == '__main__':
    main()
