#!/usr/bin/env python3
"""Exercise the exact V7 core convert body with controlled native CPU leaves.

The device/backend/context leaves inject C++ failures; no Windows or GPU APIs
execute. Complete real-header Windows compilation is a separate qualification.
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
    return (function(text, '    result_e convert(const source_t &source)'),
            function(text, '    void warning(const std::string &message) const'),
            function(text, '    void error(const std::string &message) const'))


PREFIX = r'''
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <utility>

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
using DWORD = unsigned long;
using UINT = unsigned;
static constexpr HRESULT S_OK = 0, E_FAIL = -1;
static constexpr DWORD WAIT_TIMEOUT = 258, WAIT_ABANDONED = 128;
#define FAILED(value) ((value) < 0)
enum class transfer_e { unorm };
enum class result_e { ok, skipped, failed };
struct state_t {
  const char *mode = nullptr;
  bool held = false, require_capture = true, deny_after_acquire = false;
  bool poisoned = false, conversion_enabled = true, staged = false, retained_refs = false;
  HRESULT acquire_status = S_OK, release_status = S_OK, signal_status = S_OK;
  int acquire_calls = 0, release_calls = 0, backend_calls = 0;
  int fallback_calls = 0, dispatches = 0, signals = 0, flushes = 0, resets = 0;
  int prepare_calls = 0, cancel_calls = 0, fallback_release_calls = 0, abandon_calls = 0;
} state;
static bool mode(const char *name) { return !std::strcmp(state.mode, name); }
struct IDXGIKeyedMutex {
  int refs = 1;
  void AddRef() { ++refs; }
  void Release() { --refs; }
  HRESULT AcquireSync(std::uint64_t key, DWORD timeout) {
    require(key == 0 && timeout == 17, "acquire key/timeout changed");
    ++state.acquire_calls;
    if (state.acquire_status == S_OK) {
      state.held = true;
      deny_allocation = state.deny_after_acquire;
    }
    return state.acquire_status;
  }
  HRESULT ReleaseSync(std::uint64_t key) {
    require(key == 0, "release key changed");
    require(state.held, "release without ownership or duplicate release");
    ++state.release_calls;
    state.held = false;
    return state.release_status;
  }
};
struct ID3D11ComputeShader {};
struct ID3D11Buffer {};
struct ID3D11Texture2D { int refs = 1; void AddRef() { ++refs; } void Release() { --refs; } };
static ID3D11Texture2D *retained_capture_texture = nullptr;
static IDXGIKeyedMutex *retained_capture_mutex = nullptr;
struct ID3D11ShaderResourceView {};
struct ID3D11UnorderedAccessView {};
struct ID3D11SamplerState {};
struct ID3D11Fence {};
struct source_t {
  ID3D11ShaderResourceView *srv = nullptr;
  IDXGIKeyedMutex *mutex = nullptr;
  transfer_e transfer = transfer_e::unorm;
  std::shared_ptr<const void> capture_owner;
  ID3D11Texture2D *capture_texture = nullptr;
  IDXGIKeyedMutex *capture_mutex = nullptr;
};
template<class T> struct pointer_leaf_t {
  T *value = nullptr;
  T *Get() const { return value; }
  T *operator->() const { return value; }
};
static void require_hold() {
  require(!state.require_capture || state.held, "capture released before backend/fallback completed");
}
struct context_leaf_t {
  void CSSetShader(ID3D11ComputeShader *, void *, UINT) {
    require_hold();
    ++state.fallback_calls;
    if (mode("fallback-throw")) throw std::bad_alloc();
  }
  void CSSetConstantBuffers(UINT, UINT, ID3D11Buffer **) { require_hold(); }
  void CSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView **) { require_hold(); }
  void CSSetSamplers(UINT, UINT, ID3D11SamplerState **) { require_hold(); }
  void CSSetUnorderedAccessViews(UINT, UINT, ID3D11UnorderedAccessView **, UINT *) { require_hold(); }
  void Dispatch(UINT, UINT, UINT) { require_hold(); ++state.dispatches; }
  HRESULT Signal(ID3D11Fence *, std::uint64_t) {
    require(!state.held, "D3D11 capture release boundary moved after signal");
    ++state.signals;
    return state.signal_status;
  }
  void Flush() { require(!state.held, "capture held past original flush boundary"); ++state.flushes; }
};
__ACTUAL_CAPTURE_GUARD__
static std::shared_ptr<const void> retained_capture_owner;
struct d3d12_conversion_t {
  enum class capture_e { ready, busy, failed };
  enum class result_e { ok, unsupported, failed };
  capture_mutex_guard_t staged_guard;
  std::shared_ptr<const void> capture_owner;
  ID3D11Texture2D *capture_texture = nullptr;
  IDXGIKeyedMutex *original_mutex = nullptr;
  bool staged = false, enabled = true;
  ~d3d12_conversion_t() {
    if (state.poisoned) {
      // The process-exit fixture observes the retained state before teardown;
      // the production intrusive node has the corresponding lifetime role.
      state.retained_refs = bool(capture_owner) && capture_texture && original_mutex;
      retained_capture_owner = capture_owner;
      if (capture_texture) { capture_texture->AddRef(); retained_capture_texture = capture_texture; }
      if (original_mutex) { original_mutex->AddRef(); retained_capture_mutex = original_mutex; }
    }
  }
  capture_e prepare_capture(const source_t &source, capture_mutex_guard_t &pin) {
    ++state.prepare_calls;
    if (state.staged || (source.srv && (!source.capture_owner || !source.capture_texture ||
        !source.capture_mutex || !source.mutex || pin.pinned() != source.mutex))) return capture_e::failed;
    capture_owner = source.capture_owner;
    capture_texture = source.capture_texture;
    original_mutex = source.capture_mutex;
    if (capture_texture) capture_texture->AddRef();
    if (original_mutex) original_mutex->AddRef();
    staged_guard.take(pin);
    staged = state.staged = true;
    return capture_e::ready;
  }
  IDXGIKeyedMutex *capture_mutex_pin() const { return staged_guard.pinned(); }
  IDXGIKeyedMutex *capture_mutex() const { return capture_mutex_pin(); }
  void arm_capture() { if (!staged_guard.empty()) staged_guard.arm(); }
  void cancel_capture() {
    ++state.cancel_calls;
    (void) staged_guard.release();
    staged_guard.reset();
    staged = state.staged = false;
    if (original_mutex) original_mutex->Release();
    if (capture_texture) capture_texture->Release();
    original_mutex = nullptr;
    capture_texture = nullptr;
    capture_owner.reset();
  }
  bool release_fallback() {
    ++state.fallback_release_calls;
    if (FAILED(staged_guard.release())) {
      state.poisoned = true;
      state.retained_refs = bool(capture_owner) && capture_texture && original_mutex;
      return false;
    }
    staged_guard.reset();
    staged = state.staged = false;
    if (original_mutex) original_mutex->Release();
    if (capture_texture) capture_texture->Release();
    original_mutex = nullptr;
    capture_texture = nullptr;
    capture_owner.reset();
    return true;
  }
  void abandon_capture() {
    ++state.abandon_calls;
    state.poisoned = true;
    state.retained_refs = bool(capture_owner) && capture_texture && original_mutex;
  }
  void disable_conversion() { enabled = state.conversion_enabled = false; }
  bool using_conversion() const { return enabled; }
  result_e convert(const source_t &, const void *, std::uint64_t) {
    require_hold();
    ++state.backend_calls;
    if (mode("pipeline-alloc")) { std::map<int, int> pipeline; pipeline.emplace(1, 1); }
    if (mode("barrier-alloc")) { std::vector<int> barriers; barriers.push_back(1); }
    if (mode("unsupported") || mode("unsupported-release-failed") || mode("fallback-log-alloc") || mode("fallback-log-throw") || mode("fallback-throw")) return result_e::unsupported;
    if (mode("backend-failed")) return result_e::failed;
    if (!release_fallback()) return result_e::failed;
    return result_e::ok;
  }
};
static std::string hresult_string(HRESULT) { return "leaf-error"; }
static void logging_leaf(int level, const std::string &) {
  if ((level == 2 && (mode("fallback-log-throw") || mode("timeout-log-throw"))) ||
      (level == 3 && mode("signal-log-throw"))) throw std::bad_alloc();
}
struct core_leaf_t {
  struct config_t {
    bool transform_rgb = false;
    std::chrono::milliseconds capture_wait {17};
    bool yuv444 = false;
    int width = 8, height = 8;
  } config;
  void (*log)(int, const std::string &) = logging_leaf;
  pointer_leaf_t<ID3D11Buffer> color_matrix, sdr_to_pq_params, params;
  std::array<pointer_leaf_t<ID3D11UnorderedAccessView>, 3> plane_uavs;
  pointer_leaf_t<ID3D11SamplerState> sampler;
  pointer_leaf_t<ID3D11Fence> convert_fence;
  pointer_leaf_t<context_leaf_t> context11;
  std::unique_ptr<d3d12_conversion_t> d3d12_conversion;
  ID3D11ComputeShader shader;
  int last_params = 0;
  std::uint64_t convert_value = 0;
  ~core_leaf_t() = default;
  bool process_rgb(const source_t &, capture_mutex_guard_t &, ID3D11ShaderResourceView *&, ID3D11ComputeShader *&) {
    require(false, "unexpected RGB processing");
    return false;
  }
  ID3D11ComputeShader *shader_for(transfer_e) {
    if (mode("shader-throw")) throw std::bad_alloc();
    return mode("no-shader") ? nullptr : &shader;
  }
  void update_params(const source_t *) { if (mode("params-throw")) throw std::bad_alloc(); }
'''

SUFFIX = r'''
int main(int argc, char **argv) {
  std::set_terminate([] { std::_Exit(86); });
  require(argc == 2, "expected one case");
  state.mode = argv[1];
  IDXGIKeyedMutex mutex;
  ID3D11Texture2D capture_texture;
  auto owner = std::make_shared<int>(1);
  ID3D11ShaderResourceView srv;
  context_leaf_t context;
  core_leaf_t core;
  core.context11.value = &context;
  source_t source;
  source.srv = &srv;
  source.mutex = &mutex;
  source.capture_owner = owner;
  source.capture_texture = &capture_texture;
  source.capture_mutex = &mutex;
  bool backend = mode("backend-ok") || mode("backend-failed") || mode("unsupported") ||
      mode("unsupported-release-failed") || mode("pipeline-alloc") || mode("barrier-alloc") ||
      mode("fallback-log-alloc") || mode("fallback-log-throw") || mode("fallback-throw");
#ifdef SUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12
  if (mode("no-mutex") || mode("timeout") || mode("timeout-log-throw") || mode("abandoned") ||
      mode("acquire-failed") || mode("acquire-nonsuccess")) backend = true;
#endif
  if (backend) core.d3d12_conversion = std::make_unique<d3d12_conversion_t>();
  if (mode("pipeline-alloc") || mode("barrier-alloc") || mode("fallback-log-alloc")) state.deny_after_acquire = true;
  if (mode("timeout") || mode("timeout-log-throw")) state.acquire_status = WAIT_TIMEOUT;
  if (mode("abandoned")) state.acquire_status = WAIT_ABANDONED;
  if (mode("acquire-failed")) state.acquire_status = E_FAIL;
  if (mode("acquire-nonsuccess")) state.acquire_status = 1;
  if (mode("release-failed") || mode("unsupported-release-failed")) state.release_status = E_FAIL;
  if (mode("signal-failed") || mode("signal-log-throw")) state.signal_status = E_FAIL;
  if (mode("blank")) { source.srv = nullptr; state.require_capture = false; }
  if (mode("no-mutex")) { source.mutex = nullptr; state.require_capture = false; }
  bool threw = false;
  result_e result = result_e::failed;
  try { result = core.convert(source); }
  catch (const std::bad_alloc &) { threw = true; }
  deny_allocation = false;
  // Model the failed core/session teardown boundary before checking leaked
  // sink ownership. A poisoned sink keeps its extra source refs and never
  // retries the disarmed ReleaseSync.
  core.d3d12_conversion.reset();
#ifdef SUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12
  if (mode("abandoned")) {
    require(state.abandon_calls == 1 && state.cancel_calls == 0,
            "WAIT_ABANDONED did not call abandonment without unacquired cancellation");
    require(state.release_calls == 0 && state.fallback_release_calls == 0 && !state.held &&
            state.dispatches == 0 && state.signals == 0 && state.flushes == 0 && result == result_e::failed,
            "WAIT_ABANDONED released, rendered, signaled, flushed, or published a frame");
    require(state.retained_refs && retained_capture_owner && retained_capture_texture == &capture_texture &&
            retained_capture_mutex == &mutex && capture_texture.refs > 1 && mutex.refs > 1,
            "WAIT_ABANDONED lost staged owner or exact source interfaces at teardown");
  }
#else
  if (mode("abandoned")) require(state.abandon_calls == 0 && state.release_calls == 0 &&
      state.dispatches == 0 && state.signals == 0 && state.flushes == 0 && result == result_e::failed,
      "D3D11-only WAIT_ABANDONED path continued work");
#endif
  const bool pre = mode("shader-throw") || mode("params-throw") || mode("no-shader");
  const bool optin_invalid = backend && mode("no-mutex");
  const bool acquired = !pre && !optin_invalid && state.require_capture && state.acquire_status == S_OK;
  const bool expected_throw = mode("signal-log-throw") ||
      mode("timeout-log-throw") || mode("shader-throw") || mode("params-throw");
  require(threw == expected_throw, "exception injection did not reach the expected branch");
  if (mode("unsupported-release-failed")) {
    require(acquired && state.poisoned && state.release_calls == 1 && state.retained_refs,
            "failed fallback release did not remain retained and poisoned");
    require(retained_capture_owner && mutex.refs > 1 && capture_texture.refs > 1,
            "failed fallback release dropped exact source owners");
    require(state.signals == 0 && state.flushes == 0 && result == result_e::failed,
            "failed fallback release was published");
  }
  require(state.release_calls == (acquired ? 1 : 0), "acquired capture ownership was not released exactly once");
  require(!state.held, "capture ownership remained held after conversion/unwinding");
  require(state.acquire_calls == ((!pre && !optin_invalid && state.require_capture) ? 1 : 0), "acquisition control changed");
  if (!expected_throw) {
    const auto expected_result = mode("abandoned") || mode("backend-failed") || mode("signal-failed") || mode("no-shader") ||
        mode("unsupported-release-failed") || mode("pipeline-alloc") || mode("barrier-alloc") || mode("fallback-throw") ||
        mode("fallback-log-alloc") || mode("fallback-log-throw") || optin_invalid ? result_e::failed :
        (mode("timeout") || mode("acquire-failed") || mode("acquire-nonsuccess") ? result_e::skipped : result_e::ok);
    require(result == expected_result, "normal/failed/skipped result changed");
  }
  if (mode("backend-failed") || mode("backend-ok")) require(state.dispatches == 0 && state.fallback_calls == 0, "terminal backend path used fallback");
  if (mode("unsupported")) require(state.resets == 0 && state.dispatches == 1 && state.backend_calls == 1 &&
      state.signals == 1 && state.flushes == 1 && !state.conversion_enabled,
      "unsupported source did not use retained fallback and then disable conversion");
  if (mode("unsupported-release-failed")) require(state.dispatches == 1 && state.backend_calls == 1,
      "failed-release scenario did not traverse unsupported fallback");
  if (state.prepare_calls && !acquired && !optin_invalid && !pre) {
    if (mode("abandoned")) {
      require(state.cancel_calls == 0 && state.abandon_calls == 1,
              "WAIT_ABANDONED used ordinary cancellation instead of retained abandonment");
    } else {
      require(state.cancel_calls == 1, "unacquired staged source was not canceled");
    }
  }
  return 0;
}
'''


CASES = ['fallback-ok', 'backend-ok', 'backend-failed', 'unsupported', 'unsupported-release-failed', 'pipeline-alloc', 'barrier-alloc',
         'fallback-throw', 'fallback-log-alloc', 'fallback-log-throw', 'signal-failed', 'signal-log-throw',
         'release-failed', 'timeout', 'abandoned', 'acquire-failed', 'acquire-nonsuccess', 'timeout-log-throw',
         'blank', 'no-mutex', 'no-shader', 'shader-throw', 'params-throw']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repository', type=Path)
    parser.add_argument('compiler')
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--prepare-only', action='store_true',
                        help='write exact generated source variants and recipes without compiling or running them')
    args = parser.parse_args()
    if args.prepare_only and not args.evidence_dir:
        parser.error('--prepare-only requires --evidence-dir so generated variants are preserved')
    path = args.repository / 'src/platform/windows/pyrowave_d3d11_core.cpp'
    body, warning, error = bodies(path)
    header = args.repository / 'src/platform/windows/pyrowave_d3d11_core.h'
    guard = function(header.read_text(), '  class capture_mutex_guard_t') + ';'
    variants = [('corrected', body, warning, error, CASES, [], True),
                ('corrected-OFF', body, warning, error, [case for case in CASES if case not in ['backend-ok', 'backend-failed', 'unsupported', 'unsupported-release-failed', 'pipeline-alloc', 'barrier-alloc', 'fallback-log-alloc', 'fallback-log-throw', 'fallback-throw']], [], False)]
    controls = [
        ('omit-unwind-cleanup', body, guard.replace('~capture_mutex_guard_t() { reset(); }', '~capture_mutex_guard_t() {}'), ['pipeline-alloc']),
        ('omit-disarm', body, guard.replace('if (!std::exchange(acquired_, false)) return S_OK;', 'if (!acquired_) return S_OK;'), ['fallback-ok']),
        ('omit-preacquire-staging', body.replace('capture_preparation = d3d12_conversion->prepare_capture(source, capture_guard);',
             'capture_preparation = d3d12_conversion_t::capture_e::ready;').replace(
             'if (d3d12_conversion) acquire_mutex = d3d12_conversion->capture_mutex();',
             'if (d3d12_conversion) acquire_mutex = source.mutex;'), guard, ['unsupported-release-failed']),
        ('arm-before-acquire', body.replace('        const HRESULT status = acquire_mutex->AcquireSync(', '        if (d3d12_conversion) d3d12_conversion->arm_capture();\n        const HRESULT status = acquire_mutex->AcquireSync(', 1), guard, ['timeout']),
        ('release-before-fallback', body.replace('          warning("the D3D12 conversion prototype cannot handle this source; using D3D11");', '          (void)d3d12_conversion->release_fallback();\n          warning("the D3D12 conversion prototype cannot handle this source; using D3D11");'), guard, ['unsupported']),
        ('ignore-fallback-release-error', body.replace('        if (!d3d12_conversion->release_fallback()) return result_e::failed;', '        (void)d3d12_conversion->release_fallback();'), guard, ['unsupported-release-failed']),
    ]
    for name, changed, changed_guard, cases in controls:
        assert changed != body or changed_guard != guard, name
        variants.append((name, changed, warning, error, cases, cases, True))
    guard_controls = {name: changed_guard for name, _, changed_guard, _ in controls}
    evidence = {'source': str(path.resolve()), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'convert_body_sha256': hashlib.sha256(body.encode()).hexdigest(),
                'guard_sha256': hashlib.sha256(guard.encode()).hexdigest(),
                'scope': 'Exact actual core convert and warning/error bodies, controlled native CPU leaves, no Windows/COM/GPU execution.',
                'compiler_or_test_executed': False, 'variants': []}
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pyrowave-capture-guard-', dir='/tmp') as temporary:
        temporary = Path(temporary)
        for name, convert, warn, err, cases, expected_red, enabled in variants:
            source = (args.evidence_dir / (name + '.cpp') if args.prepare_only and args.evidence_dir
                      else temporary / (name + '.cpp'))
            source.write_text(PREFIX.replace('__ACTUAL_CAPTURE_GUARD__', guard_controls.get(name, guard)) + warn + '\n' + err + '\n' + convert + '\n};\n' + SUFFIX)
            binary = (Path('/tmp') / ('issue625-v7-core-' + name) if args.prepare_only
                      else temporary / name)
            command = [args.compiler, '-std=c++23', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread']
            if enabled:
                command += ['-DSUNSHINE_EXPERIMENTAL_PYROWAVE_D3D12=1']
            command += [str(source), '-o', str(binary)]
            if args.prepare_only:
                evidence['variants'].append({'variant': name, 'recipe': 'ON' if enabled else 'OFF',
                    'command': command, 'generated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                    'cases': [{'case': case, 'expected_red': case in expected_red} for case in cases],
                    'compile_pending_root_go': True})
                continue
            compiled = subprocess.run(command, capture_output=True, text=True)
            assert compiled.returncode == 0, compiled.stderr
            record = {'variant': name, 'recipe': 'ON' if enabled else 'OFF', 'command': command,
                      'generated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'compile_exit': compiled.returncode, 'cases': []}
            for case in cases:
                proc = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=2)
                red = case in expected_red
                assert (proc.returncode != 0) if red else proc.returncode == 0, (name, case, proc.returncode, proc.stderr)
                record['cases'].append({'case': case, 'exit': proc.returncode, 'expected_red': red, 'stderr': proc.stderr})
            evidence['variants'].append(record)
            if args.evidence_dir:
                shutil.copy2(source, args.evidence_dir / source.name)
                (args.evidence_dir / (name + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
    if args.evidence_dir:
        (args.evidence_dir / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'corrected_ON_cases': len(CASES), 'corrected_OFF_cases': len(variants[1][4]),
                      'omission_controls': len(controls), 'compile_pending_root_go': args.prepare_only}))


if __name__ == '__main__':
    main()
