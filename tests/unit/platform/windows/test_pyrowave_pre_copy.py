#!/usr/bin/env python3
"""Exact converter preparation/convert bodies with controlled native CPU leaves.

No Windows, COM or GPU API executes. Complete real-header COFF qualification is
separate, and producer Signal failure/lifetime remains an explicit native gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def function(text, name):
    start = text.index(name)
    position = text.index('{', start)
    depth = 1
    end = position + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def bodies(path):
    text = path.read_text()
    names = [
        '    bool check(HRESULT hr, const char *operation) const',
        '    bool prepare_source(const source_t &source',
        '    bool producer_finished() const',
        '    bool consumer_finished() const',
        '    bool finished() const',
        '    result_e convert(const source_t &source, const void *params, std::uint64_t value)',
    ]
    return [function(text, name) for name in names]


PREFIX = r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
static bool deny_allocation = false;
void *operator new(std::size_t size) {
  if (deny_allocation) throw std::bad_alloc();
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
using HRESULT = int;
using UINT = unsigned;
using DWORD = unsigned long;
using SIZE_T = std::size_t;
static constexpr HRESULT S_OK = 0, E_FAIL = -1;
static constexpr DWORD WAIT_OBJECT_0 = 0;
#define FAILED(value) ((value) < 0)
#define SUCCEEDED(value) ((value) >= 0)
enum class transfer_e { unorm };
enum class result_e { ok, unsupported, failed };
static constexpr int DXGI_FORMAT_R8G8B8A8_UNORM = 1, DXGI_FORMAT_B8G8R8A8_UNORM = 2,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 3, D3D12_SRV_DIMENSION_TEXTURE2D = 1,
    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING = 2, D3D11_SRV_DIMENSION_TEXTURE2D = 1,
    D3D11_USAGE_DEFAULT = 0, D3D11_BIND_SHADER_RESOURCE = 1,
    D3D11_RESOURCE_MISC_SHARED = 2, D3D11_RESOURCE_MISC_SHARED_NTHANDLE = 4,
    D3D12_RESOURCE_STATE_COMMON = 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 2,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 3;
struct state_t {
  const char *mode = nullptr;
  int copies = 0, closes = 0, barriers = 0, signals = 0, flushes = 0, executes = 0;
  bool source_present = true, copy_before_close = false;
} state;
static bool mode(const char *name) { return !std::strcmp(state.mode, name); }
struct D3D11_TEXTURE2D_DESC {
  UINT Width = 8, Height = 8, MipLevels = 1, ArraySize = 1;
  struct { UINT Count = 1; } SampleDesc;
  int Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  UINT Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
};
struct D3D11_SHADER_RESOURCE_VIEW_DESC {
  int Format = DXGI_FORMAT_B8G8R8A8_UNORM, ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  struct { UINT MostDetailedMip = 0, MipLevels = 1; } Texture2D;
};
struct D3D12_SHADER_RESOURCE_VIEW_DESC {
  int Format = 0, ViewDimension = 0, Shader4ComponentMapping = 0;
  struct { UINT MipLevels = 0; } Texture2D;
};
struct ID3D11Resource { virtual ~ID3D11Resource() = default; };
struct ID3D11Texture2D: ID3D11Resource {
  void GetDesc(D3D11_TEXTURE2D_DESC *desc) { *desc = {}; if (mode("source-unsupported")) desc->Format = 99; }
} source_texture, snapshot_texture;
template<class T> struct ComPtr {
  T *value = nullptr;
  T *Get() const { return value; }
  T *operator->() const { return value; }
  T **operator&() { return &value; }
  explicit operator bool() const { return value != nullptr; }
  void Reset() { value = nullptr; }
  template<class U> HRESULT As(U **target) { *target = static_cast<U *>(value); return S_OK; }
};
struct ID3D11ShaderResourceView {
  void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *desc) { *desc = {}; }
  void GetResource(ID3D11Resource **resource) { *resource = &source_texture; }
};
struct source_t { ID3D11ShaderResourceView *srv = nullptr; transfer_e transfer = transfer_e::unorm; };
struct ID3D12Resource { int index = 0; } bridge_resource {0}, plane_resources[] {{1}, {2}, {3}};
struct D3D12_RESOURCE_BARRIER {
  struct { ID3D12Resource *pResource = nullptr; int StateBefore = 0, StateAfter = 0; } Transition;
};
static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, int before, int after) {
  D3D12_RESOURCE_BARRIER barrier; barrier.Transition = {resource, before, after}; return barrier;
}
struct handle_leaf_t { void *value = nullptr; };
struct device11_leaf_t {
  HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC *, void *, ID3D11Texture2D **target) {
    if (mode("source-create-failed")) return E_FAIL;
    *target = &snapshot_texture; return S_OK;
  }
};
struct pipeline_leaf_t {};
using ID3D12PipelineState = pipeline_leaf_t;
struct descriptor_leaf_t { SIZE_T ptr = 0; };
struct device_leaf_t {
  HRESULT removal_status = S_OK;
  void CreateShaderResourceView(ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, descriptor_leaf_t) {}
  HRESULT GetDeviceRemovedReason() { return mode("consumer-removed") ? E_FAIL : removal_status; }
};
struct root_leaf_t {};
struct ID3D12DescriptorHeap {
  descriptor_leaf_t GetGPUDescriptorHandleForHeapStart() { return {}; }
};
struct fence_leaf_t {
  std::uint64_t completed = 1;
  HRESULT SetEventOnCompletion(std::uint64_t, void *) { return S_OK; }
  std::uint64_t GetCompletedValue() { return completed; }
};
struct allocator_leaf_t {
  HRESULT Reset() {
    if (mode("allocator-log-alloc")) deny_allocation = true;
    return mode("allocator-failed") || mode("allocator-log-alloc") ? E_FAIL : S_OK;
  }
};
struct ID3D12CommandList {};
struct list_leaf_t: ID3D12CommandList {
  HRESULT Reset(allocator_leaf_t *, pipeline_leaf_t *) {
    if (mode("list-reset-log-alloc")) deny_allocation = true;
    if (mode("deny-after-reset")) deny_allocation = true;
    return mode("list-reset-log-alloc") ? E_FAIL : S_OK;
  }
  void ResourceBarrier(UINT count, D3D12_RESOURCE_BARRIER *barriers) {
    const UINT expected = state.source_present ? 4 : 3;
    require(count == expected, "active source/plane barrier count changed");
    for (UINT i = 0; i < count; ++i) {
      const int index = int(i) + (state.source_present ? 0 : 1);
      require(barriers[i].Transition.pResource->index == index, "barrier resource identity/order changed");
      const int compute = index == 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      require(barriers[i].Transition.StateBefore == (state.barriers == 0 ? D3D12_RESOURCE_STATE_COMMON : compute) &&
              barriers[i].Transition.StateAfter == (state.barriers == 0 ? compute : D3D12_RESOURCE_STATE_COMMON), "barrier transition changed");
    }
    ++state.barriers;
  }
  void SetComputeRootSignature(root_leaf_t *) {}
  void SetDescriptorHeaps(UINT, ID3D12DescriptorHeap **) {}
  void SetComputeRoot32BitConstants(UINT, UINT, const void *, UINT) {}
  void SetComputeRootDescriptorTable(UINT, descriptor_leaf_t) {}
  void Dispatch(UINT, UINT, UINT) {}
  HRESULT Close() {
    ++state.closes;
    if (mode("close-log-alloc")) deny_allocation = true;
    return mode("close-failed") || mode("close-log-alloc") ? E_FAIL : S_OK;
  }
};
struct context_leaf_t {
  void CopyResource(ID3D11Texture2D *destination, ID3D11Texture2D *source) {
    require(destination == &snapshot_texture && source == &source_texture, "validated copy source/destination identity changed");
    ++state.copies;
    state.copy_before_close = state.closes == 0;
    if (mode("after-copy-deny")) deny_allocation = true;
    if (mode("copy-throw")) { deny_allocation = true; throw std::bad_alloc(); }
  }
  HRESULT Signal(fence_leaf_t *, std::uint64_t) {
    ++state.signals;
    require(state.copies == (state.source_present ? 1 : 0), "producer Signal preceded actual snapshot");
    if (mode("producer-signal-log-alloc")) deny_allocation = true;
    return mode("producer-signal-failed") || mode("producer-signal-log-alloc") || mode("pool-pin") ||
        mode("producer-unknown-removed") || mode("producer-unknown-tail") || mode("producer-unknown-sentinel") ? E_FAIL : S_OK;
  }
  void Flush() { ++state.flushes; }
};
struct queue_leaf_t {
  HRESULT Wait(fence_leaf_t *, std::uint64_t) {
    require(state.signals == 1 && state.flushes == 1, "consumer wait reordered before producer signal/flush");
    if (mode("queue-wait-log-alloc")) { deny_allocation = true; return E_FAIL; }
    if (mode("queue-wait-failed")) return E_FAIL;
    return S_OK;
  }
  void ExecuteCommandLists(UINT, ID3D12CommandList **) {
    ++state.executes;
    if (mode("execute-throw")) { deny_allocation = true; throw std::bad_alloc(); }
  }
  HRESULT Signal(fence_leaf_t *, std::uint64_t) { return mode("consumer-signal-failed") ? E_FAIL : S_OK; }
};
static DWORD WaitForSingleObject(void *, DWORD) { return WAIT_OBJECT_0; }
static std::string hex(HRESULT) { return "leaf-error"; }
static void logging_leaf(int, const std::string &) {}
struct core_leaf_t {
  struct config_t { bool yuv444 = false; int width = 8, height = 8; float color_matrix[16] {}; float sdr_white_nits = 100; } config;
  void (*log)(int, const std::string &) = logging_leaf;
  ComPtr<device11_leaf_t> device11;
  ComPtr<device_leaf_t> device;
  ComPtr<context_leaf_t> context11;
  ComPtr<ID3D11Texture2D> bridge11;
  ComPtr<ID3D11Texture2D> opened_source;
  D3D11_TEXTURE2D_DESC bridge_desc;
  ComPtr<ID3D12Resource> bridge;
  std::array<ComPtr<ID3D12Resource>, 3> planes;
  ComPtr<allocator_leaf_t> allocator;
  ComPtr<list_leaf_t> list;
  ComPtr<root_leaf_t> root;
  ComPtr<ID3D12DescriptorHeap> heap;
  ComPtr<fence_leaf_t> producer11, producer, output;
  ComPtr<fence_leaf_t> retirement;
  ComPtr<queue_leaf_t> queue;
  UINT increment = 1;
  std::uint64_t producer_value = 0, pending_value = 0;
  bool pending = false, unknown_completion = false, retired = false;
  bool producer_pending = false, producer_unknown = false, capture_staged = true;
  std::atomic<bool> release_unknown {false};
  std::atomic<bool> acquire_abandoned {false};
  HRESULT failure_status = S_OK;
  const char *failure_operation = nullptr;
  handle_leaf_t event;
  pipeline_leaf_t shader;
  pipeline_leaf_t *pipeline(transfer_e) {
    if (mode("pipeline-alloc")) { deny_allocation = true; std::vector<int> macros; macros.push_back(1); }
    return &shader;
  }
  descriptor_leaf_t descriptor(UINT) { return {}; }
  bool open_texture(ID3D11Texture2D *, ComPtr<ID3D12Resource> &resource) { resource.value = &bridge_resource; return true; }
  bool submitted_check(HRESULT status, const char *) noexcept { return SUCCEEDED(status); }
  bool release_source() noexcept { capture_staged = false; producer_pending = false; return true; }
'''

SUFFIX = r'''
int main(int argc, char **argv) {
  require(argc == 2, "expected one case"); state.mode = argv[1];
  device11_leaf_t device11; device_leaf_t device; context_leaf_t context;
  allocator_leaf_t allocator; list_leaf_t list; root_leaf_t root; ID3D12DescriptorHeap heap;
  fence_leaf_t producer11, producer, output, retirement; queue_leaf_t queue; ID3D11ShaderResourceView srv;
  core_leaf_t core;
  core.device11.value = &device11; core.device.value = &device; core.context11.value = &context;
  core.bridge11.value = &snapshot_texture; core.opened_source.value = &source_texture; core.bridge.value = &bridge_resource;
  for (int i = 0; i < 3; ++i) core.planes[i].value = &plane_resources[i];
  core.allocator.value = &allocator; core.list.value = &list; core.root.value = &root;
  core.heap.value = &heap; core.producer11.value = &producer11; core.producer.value = &producer;
  core.output.value = &output; core.retirement.value = &retirement; core.queue.value = &queue;
  if (mode("source-create-failed")) { core.bridge.Reset(); core.bridge11.Reset(); }
  if (mode("blank-normal")) state.source_present = false;
  if (mode("producer-overflow")) core.producer_value = std::numeric_limits<std::uint64_t>::max() - 1;
  if (mode("producer-incomplete")) producer11.completed = 0;
  if (mode("producer-sentinel")) producer11.completed = std::numeric_limits<std::uint64_t>::max();
  if (mode("consumer-incomplete")) output.completed = 0;
  if (mode("consumer-sentinel")) output.completed = std::numeric_limits<std::uint64_t>::max();
  source_t source {state.source_present ? &srv : nullptr, transfer_e::unorm};
  result_e result = result_e::failed; bool threw = false; int params[8] {};
  try { result = core.convert(source, params, 1); } catch (const std::bad_alloc &) { threw = true; }
  deny_allocation = false;
  const bool setup_exception = mode("pipeline-alloc") || mode("allocator-log-alloc") || mode("list-reset-log-alloc") || mode("close-log-alloc");
  const bool producer_exception = mode("copy-throw");
  const bool consumer_exception = mode("execute-throw");
  require(threw == (producer_exception || consumer_exception), "unexpected allocation exception/control reachability");
  const bool unsupported = setup_exception || mode("allocator-failed") || mode("close-failed") || mode("source-unsupported") || mode("source-create-failed");
  const bool failed_after_prepare = mode("producer-overflow") || mode("producer-signal-failed") || mode("producer-signal-log-alloc") ||
      mode("pool-pin") || mode("producer-unknown-removed") || mode("producer-unknown-tail") || mode("producer-unknown-sentinel") || mode("producer-incomplete") ||
      mode("producer-sentinel") || mode("queue-wait-failed") || mode("queue-wait-log-alloc") || mode("execute-throw") ||
      mode("consumer-signal-failed") || mode("consumer-incomplete") || mode("consumer-sentinel") || mode("consumer-removed");
  if (setup_exception || unsupported) require(state.copies == 0 && state.signals == 0 && state.executes == 0, "preparation failure/exception issued snapshot or queue work");
  if (!threw) require(result == (unsupported ? result_e::unsupported : failed_after_prepare ? result_e::failed : result_e::ok), "result/fallback policy changed");
  if (mode("source-normal")) require(state.copies == 1 && !state.copy_before_close && state.barriers == 2 && state.executes == 1, "copy did not follow successful complete preparation");
  if (mode("blank-normal")) require(state.copies == 0 && state.barriers == 2 && state.executes == 1, "blank conversion/barriers changed");
  if (mode("producer-signal-failed") || mode("producer-signal-log-alloc") || mode("pool-pin") ||
      mode("producer-unknown-removed") || mode("producer-unknown-tail") || mode("producer-unknown-sentinel") || producer_exception)
    require(state.copies == 1 && !core.pending && state.executes == 0 && core.producer_pending && core.producer_unknown,
            "producer-failure uncertainty tracking changed");
  if (mode("queue-wait-failed") || mode("queue-wait-log-alloc") || mode("consumer-signal-failed") || consumer_exception)
    require(core.pending && core.unknown_completion, "consumer unknown-completion tracking changed");
  if (mode("producer-unknown-removed")) {
    device.removal_status = E_FAIL;
    producer11.completed = std::numeric_limits<std::uint64_t>::max();
  }
  if (mode("producer-unknown-tail")) {
    core.pending = true; core.retired = true; core.pending_value = 1; retirement.completed = 1;
  }
  if (mode("producer-unknown-sentinel")) {
    producer11.completed = std::numeric_limits<std::uint64_t>::max();
    device.removal_status = E_FAIL;
  }
  if (mode("pool-pin") || mode("producer-unknown-removed") || mode("producer-unknown-tail") || mode("producer-unknown-sentinel"))
    require(core.capture_staged && core.producer_pending && core.producer_unknown && !core.finished(),
            "unknown producer work lost its source pin after another completion signal");
  if (mode("queue-wait-failed") || mode("queue-wait-log-alloc") || mode("consumer-signal-failed") || consumer_exception)
    require(!core.finished(), "unknown consumer work was treated as complete");
  if (mode("producer-overflow")) require(state.copies == 0 && state.signals == 0 && state.executes == 0,
                                          "producer counter overflow performed work");
  if (mode("producer-incomplete") || mode("producer-sentinel"))
    require(state.executes == 1 && core.producer_pending && !core.producer_unknown && core.pending && !core.finished(),
            "incomplete producer completion did not retain the submitted frame");
  if (mode("consumer-incomplete") || mode("consumer-sentinel"))
    require(core.pending && !core.unknown_completion && !core.finished(),
            "incomplete consumer completion did not retain its known output ticket");
  if (mode("consumer-removed"))
    require(core.pending && !core.unknown_completion,
            "consumer completion failure did not retain its known output ticket");
  if (mode("producer-signal-failed") || mode("producer-signal-log-alloc") || producer_exception)
    require(!core.finished(), "unknown producer work was treated as complete");
  return 0;
}
'''

CASES = ['source-normal', 'blank-normal', 'after-copy-deny', 'deny-after-reset', 'pipeline-alloc',
         'allocator-failed', 'close-failed', 'allocator-log-alloc', 'list-reset-log-alloc', 'close-log-alloc',
         'source-unsupported', 'source-create-failed', 'producer-overflow', 'producer-signal-failed',
         'producer-signal-log-alloc', 'copy-throw', 'pool-pin', 'producer-incomplete', 'producer-sentinel',
         'producer-unknown-removed', 'producer-unknown-tail', 'producer-unknown-sentinel',
         'queue-wait-failed', 'queue-wait-log-alloc', 'execute-throw', 'consumer-signal-failed',
         'consumer-incomplete', 'consumer-sentinel', 'consumer-removed']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repository', type=Path)
    parser.add_argument('compiler')
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--prepare-only', action='store_true',
                        help='extract and record source cases without compiling or running them')
    args = parser.parse_args()
    path = args.repository / 'src/platform/windows/pyrowave_d3d12_conversion.cpp'
    check, prepare, producer_finished, consumer_finished, finished, convert = bodies(path)
    source_bodies = [check, prepare, producer_finished, consumer_finished, finished, convert]
    variants = [('corrected', source_bodies, CASES, [])]
    copy = '    if (source.srv) context11->CopyResource(bridge11.Get(), opened_source.Get());'
    early = convert.replace(copy, '').replace('        if (!check(allocator->Reset()', copy + '\n        if (!check(allocator->Reset()', 1)
    heap = convert.replace('std::array<D3D12_RESOURCE_BARRIER, 1 + std::tuple_size<decltype(planes)>::value> barriers {};', 'std::vector<D3D12_RESOURCE_BARRIER> barriers(4);')
    omit_close = convert.replace('        if (!check(list->Close(), "close conversion commands")) {\n          return result_e::unsupported;\n        }', '        list->Close();')
    short_count = convert.replace('      list->ResourceBarrier(barrier_count, barriers.data());', '      list->ResourceBarrier(barrier_count - 1, barriers.data());', 1)
    for name, changed, cases in [('early-copy', early, ['close-failed']), ('heap-barriers', heap, ['deny-after-reset']),
                                 ('omit-close-validation', omit_close, ['close-failed']), ('omit-one-barrier', short_count, ['source-normal', 'blank-normal'])]:
        assert changed != convert, name
        variants.append((name, [check, prepare, producer_finished, consumer_finished, finished, changed], cases, cases))
    evidence = {'source': str(path.resolve()), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'body_sha256': {name: hashlib.sha256(value.encode()).hexdigest() for name, value in zip(
                    ['check', 'prepare_source', 'producer_finished', 'consumer_finished', 'finished', 'convert'], source_bodies)},
                'scope': 'Exact actual check, prepare_source, producer_finished, consumer_finished, finished and convert bodies with controlled native CPU leaves. No Windows/COM/GPU execution.',
                'compiler_or_test_executed': False, 'variants': []}
    if args.prepare_only and not args.evidence_dir:
        parser.error('--prepare-only requires --evidence-dir so the generated inputs are reviewable')
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pyrowave-pre-copy-', dir='/tmp') as temporary:
        temporary = Path(temporary)
        for name, extracted, cases, expected_red in variants:
            checking, preparing, producer_body, consumer_body, finished_body, converting = extracted
            source_text = PREFIX + '\n'.join([checking, preparing, producer_body, consumer_body, finished_body,
                                                converting]) + '\n};\n' + SUFFIX
            source = (args.evidence_dir / (name + '.cpp') if args.prepare_only else temporary / (name + '.cpp'))
            source.write_text(source_text)
            binary = Path('/tmp') / ('pyrowave-v6-pre-copy-' + name) if args.prepare_only else temporary / name
            command = [args.compiler, '-std=c++23', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(binary)]
            if args.prepare_only:
                evidence['variants'].append({
                    'variant': name,
                    'compile_command': command,
                    'generated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                    'cases': [{'case': case, 'expected_red': case in expected_red} for case in cases],
                    'compile_and_run_pending_root_go': True,
                })
                continue
            compiled = subprocess.run(command, capture_output=True, text=True)
            if args.evidence_dir:
                shutil.copy2(source, args.evidence_dir / source.name)
                (args.evidence_dir / (name + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
            assert compiled.returncode == 0, compiled.stderr
            record = {'variant': name, 'command': command, 'generated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'compile_exit': compiled.returncode, 'cases': []}
            for case in cases:
                proc = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=2)
                red = case in expected_red
                assert (proc.returncode != 0) if red else proc.returncode == 0, (name, case, proc.returncode, proc.stderr)
                record['cases'].append({'case': case, 'exit': proc.returncode, 'expected_red': red, 'stderr': proc.stderr})
            evidence['variants'].append(record)
    if args.prepare_only:
        evidence['mode'] = 'prepared; compile and execution pending root authorization'
        evidence['pending_run_commands'] = [
            f"/tmp/pyrowave-v6-pre-copy-{variant[0]} {case}"
            for variant in variants for case in variant[2]
        ]
    if args.evidence_dir:
        (args.evidence_dir / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'corrected_cases': len(CASES), 'omission_variants': 4, 'omission_probes': 5,
                      'compile_pending_root_go': args.prepare_only}))


if __name__ == '__main__':
    main()
