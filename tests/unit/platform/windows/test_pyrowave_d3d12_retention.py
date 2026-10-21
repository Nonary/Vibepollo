#!/usr/bin/env python3
"""Run exact production retention bodies with controlled CPU-only API leaves.

This does not execute Windows, COM, D3D or GPU code. The complete Windows
translation unit is qualified separately with genuine SDK headers.
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
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def bodies(path):
    text = path.read_text()
    start = text.index('    bool pending = false;')
    end = text.index('    bool check(HRESULT', start)
    return text[start:end], function(text, '  d3d12_conversion_t::~d3d12_conversion_t()')


PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
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

using HRESULT = int;
#define FAILED(value) ((value) < 0)
struct LUID { std::uint32_t LowPart = 0; std::int32_t HighPart = 0; };
struct device_leaf_t {
  bool removed = false;
  HRESULT GetDeviceRemovedReason() const { return removed ? -1 : 0; }
};
struct fence_leaf_t {
  std::uint64_t completed = 0;
  std::uint64_t GetCompletedValue() const { return completed; }
};
struct queue_leaf_t {
  HRESULT result = -1;
  bool completes = false;
  int signals = 0;
  HRESULT Signal(fence_leaf_t *fence, std::uint64_t value) {
    ++signals;
    if (result >= 0 && completes) fence->completed = value;
    return result;
  }
};
template<class T> struct pointer_leaf_t {
  T *value = nullptr;
  T *Get() const { return value; }
  T *operator->() const { return value; }
  explicit operator bool() const { return value != nullptr; }
};
struct resource_leaf_t {
  int *destroyed = nullptr;
  void (*on_release)() = nullptr;
  ~resource_leaf_t() {
    if (destroyed) ++*destroyed;
    if (on_release) on_release();
  }
};
struct d3d12_conversion_t {
  struct impl_t;
  std::unique_ptr<impl_t> impl;
  ~d3d12_conversion_t();
};
struct d3d12_conversion_t::impl_t {
  resource_leaf_t resource;
  LUID luid {};
  pointer_leaf_t<device_leaf_t> device;
  pointer_leaf_t<queue_leaf_t> queue;
  pointer_leaf_t<fence_leaf_t> output;
  pointer_leaf_t<fence_leaf_t> retirement;
'''

SUFFIX = r'''
static void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(91);
  }
}
struct fixture_t {
  device_leaf_t device;
  queue_leaf_t queue;
  fence_leaf_t output, retirement;
  int destroyed = 0;
  std::unique_ptr<d3d12_conversion_t> owner;
  explicit fixture_t(std::uint32_t low, std::int32_t high = 0) : owner(new d3d12_conversion_t) {
    owner->impl = std::make_unique<d3d12_conversion_t::impl_t>();
    auto &impl = *owner->impl;
    impl.luid = {low, high};
    impl.device.value = &device;
    impl.queue.value = &queue;
    impl.output.value = &output;
    impl.retirement.value = &retirement;
    impl.resource.destroyed = &destroyed;
  }
  bool admit() {
#ifdef BASELINE_OWNER
    owner->impl->admitted = d3d12_conversion_t::impl_t::admit(owner->impl->luid);
    return owner->impl->admitted;
#else
    return owner->impl->admit();
#endif
  }
  void pending(bool unknown = false) {
    owner->impl->pending = true;
    owner->impl->pending_value = 2;
    owner->impl->unknown_completion = unknown;
  }
};
static fixture_t *reentrant = nullptr;
static bool reentry_succeeded = false;
static void release_reentry() { reentry_succeeded = reentrant->admit(); }

static void normal_finished() {
  fixture_t first(1), replacement(1);
  require(first.admit(), "initial admission failed");
  first.pending();
  first.output.completed = 2;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 1 && first.queue.signals == 0, "finished owner was retained or signaled");
  require(replacement.admit(), "finished generation kept adapter admission");
}

static void unknown_quarantine() {
  fixture_t first(2), blocked(2), collector(2);
  require(first.admit(), "initial admission failed");
  first.pending(true);
  first.retirement.completed = 100;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 0, "unknown work released resources");
  require(!blocked.admit() && first.destroyed == 0, "unknown work admitted reuse or trusted an unproven value");
  first.device.removed = true;
  require(collector.admit(), "removed-device quarantine was not collected");
  require(first.destroyed == 1, "removed-device resources were not released exactly once");
}

static void known_quarantine() {
  fixture_t first(3), blocked(3), collector(3);
  require(first.admit(), "initial admission failed");
  first.pending();
  first.queue.result = 0;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 0 && first.queue.signals == 1, "unretired known tail released resources");
  require(!blocked.admit(), "unretired tail admitted same adapter");
  first.retirement.completed = std::numeric_limits<std::uint64_t>::max();
  require(!blocked.admit(), "device-removal sentinel was treated as ordinary completion");
  first.retirement.completed = 1;
  require(collector.admit() && first.destroyed == 1, "completed tail was not collected");
}

static void collection_allocation() {
  fixture_t first(4), collector(4);
  require(first.admit(), "initial admission failed");
  first.pending();
  first.queue.result = 0;
  first.owner.reset();  // Baseline insertion is allowed, isolating collection.
  require(first.destroyed == 0, "setup did not quarantine");
  first.retirement.completed = 1;
  deny_allocation = true;
  require(collector.admit() && first.destroyed == 1, "collection failed under allocation denial");
}

static void collection_reentry() {
  fixture_t first(5), collector(5), nested(6);
  require(first.admit(), "initial admission failed");
  first.pending();
  first.owner->impl->resource.on_release = release_reentry;
  reentrant = &nested;
  first.owner.reset();
  first.device.removed = true;
  deny_allocation = true;
  require(collector.admit(), "collector admission failed");
  require(first.destroyed == 1 && reentry_succeeded, "resource release could not reenter admission");
}

static void active_admission() {
  fixture_t active(7, 1), same(7, 1), different_high(7, 2);
  require(active.admit(), "initial admission failed");
  deny_allocation = true;
  require(!same.admit() && active.destroyed == 0, "active finished/pre-submit owner was collected");
  require(different_high.admit(), "adapter comparison ignored LUID HighPart");
}

int main(int argc, char **argv) {
  std::set_terminate([] { std::_Exit(86); });
  require(argc == 2, "expected one case");
  try {
    if (!std::strcmp(argv[1], "normal-finished")) normal_finished();
    else if (!std::strcmp(argv[1], "unknown-quarantine")) unknown_quarantine();
    else if (!std::strcmp(argv[1], "known-quarantine")) known_quarantine();
    else if (!std::strcmp(argv[1], "collection-allocation")) collection_allocation();
    else if (!std::strcmp(argv[1], "collection-reentry")) collection_reentry();
    else if (!std::strcmp(argv[1], "active-admission")) active_admission();
    else require(false, "unknown case");
  } catch (const std::bad_alloc &) {
    return 87;
  }
  return 0;
}
'''


def generated(owner, destructor, baseline=False):
    return ('#define BASELINE_OWNER\n' if baseline else '') + PREFIX + owner + '\n};\n' + destructor + SUFFIX


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repository', type=Path)
    parser.add_argument('compiler')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--evidence-dir', type=Path)
    args = parser.parse_args()
    path = args.repository / 'src/platform/windows/pyrowave_d3d12_conversion.cpp'
    owner, destructor = bodies(path)
    cases = ['normal-finished', 'unknown-quarantine', 'known-quarantine', 'collection-allocation', 'collection-reentry', 'active-admission']
    variants = [('corrected', owner, destructor, False, cases, False)]
    if args.baseline:
        old_owner, old_destructor = bodies(args.baseline)
        variants.append(('baseline', old_owner, old_destructor, True, ['normal-finished', 'unknown-quarantine', 'collection-allocation'], False))
    omission_controls = [
        ('omit-retention-transfer', owner, destructor.replace('impl.release();', '(void) impl.get();'), ['unknown-quarantine']),
        ('omit-unknown-guard', owner.replace('!unknown_completion && ', ''), destructor, ['unknown-quarantine']),
        ('omit-quarantine-guard', owner.replace('generation->quarantined && ', ''), destructor, ['active-admission']),
        ('delete-under-registry-lock', owner.replace('        delete generation;', '        std::lock_guard held(state.mutex);\n        delete generation;'), destructor, ['collection-reentry']),
    ]
    for name, changed_owner, changed_destructor, selected_cases in omission_controls:
        assert (changed_owner, changed_destructor) != (owner, destructor), name
        variants.append((name, changed_owner, changed_destructor, False, selected_cases, True))
    evidence = {
        'source': str(path.resolve()), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'owner_block_sha256': hashlib.sha256(owner.encode()).hexdigest(),
        'destructor_sha256': hashlib.sha256(destructor.encode()).hexdigest(),
        'scope': 'Exact production owner/admission/finished/destructor bodies; controlled CPU-only device/fence/queue/resource leaves. No Windows/GPU execution.',
        'variants': [],
    }
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pyrowave-retention-', dir='/tmp') as temporary:
        scratch = Path(temporary)
        for name, actual_owner, actual_destructor, baseline, selected_cases, omission in variants:
            source = scratch / (name + '.cpp')
            source.write_text(generated(actual_owner, actual_destructor, baseline))
            binary = scratch / name
            command = [args.compiler, '-std=c++23', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            assert compiled.returncode == 0, compiled.stderr
            row = {'variant': name, 'command': command, 'generated_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'compile_exit': compiled.returncode, 'cases': []}
            for case in selected_cases:
                timed_out = False
                try:
                    proc = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=2)
                    status, stderr = proc.returncode, proc.stderr
                except subprocess.TimeoutExpired:
                    status, stderr, timed_out = None, 'bounded timeout: registry-lock reentry deadlocked', True
                expected_red = omission or (baseline and case in ['unknown-quarantine', 'collection-allocation'])
                assert (timed_out or status != 0) if expected_red else status == 0, (name, case, status, stderr)
                if baseline and case == 'unknown-quarantine':
                    assert status == 86, 'baseline must cross its noexcept destructor boundary'
                if baseline and case == 'collection-allocation':
                    assert status == 87, 'baseline must fail the temporary collection-vector allocation'
                row['cases'].append({'case': case, 'exit': status, 'timed_out': timed_out, 'expected_red': expected_red, 'stderr': stderr})
            evidence['variants'].append(row)
            if args.evidence_dir:
                shutil.copy2(source, args.evidence_dir / source.name)
                (args.evidence_dir / (name + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
    if args.evidence_dir:
        (args.evidence_dir / 'results.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'corrected_cases_passed': len(cases), 'omission_controls_detected': len(omission_controls), 'baseline_checked': bool(args.baseline)}))


if __name__ == '__main__':
    main()
