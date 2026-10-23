#!/usr/bin/env python3
"""Exercise production D3D12 retention paths with CPU-only API leaves.

The generated C++ includes the production registry, completion, release, and
destructor bodies verbatim. API leaves replace only D3D/COM interfaces; this
does not execute Windows, COM, D3D, or GPU code.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def closing_brace(text, opening):
    """Return the byte after a C++ brace block, ignoring comments and strings."""
    depth = 0
    index = opening
    state = "code"
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 1
        elif state in ("string", "character"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        elif char == "/" and following == "/":
            state = "line-comment"
            index += 1
        elif char == "/" and following == "*":
            state = "block-comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    raise ValueError("unterminated C++ brace block")


def block(text, signature):
    if text.count(signature) != 1:
        raise ValueError(f"expected exactly one active fixture target for {signature!r}")
    start = text.index(signature)
    opening = start + len(signature) - 1 if signature.endswith("{") else text.index("{", start + len(signature))
    end = closing_brace(text, opening)
    return text[start:end]


def struct_block(text, signature):
    start = text.index(signature)
    result = block(text, signature)
    end = start + len(result)
    while end < len(text) and text[end].isspace():
        end += 1
    if end >= len(text) or text[end] != ";":
        raise ValueError(f"expected semicolon after {signature}")
    return text[start:end + 1]


def production_bodies(path):
    text = path.read_text()
    impl_type = struct_block(text, "  struct d3d12_conversion_t::impl_t {")
    # Keep this small test seam tied to the real lifecycle functions. In
    # particular, do not include source staging or shader/command setup here.
    nested_types = "\n\n".join(
        struct_block(impl_type, signature)
        for signature in ("    struct luid_state_t {", "    struct retained_t {")
    )
    methods = "\n\n".join(
        block(impl_type, signature)
        for signature in (
            "    static retained_t &retained()",
            "    ~impl_t()",
            "    bool producer_finished() const",
            "    bool consumer_finished() const",
            "    bool finished() const",
            "    bool release_source() noexcept",
            "    static bool same_luid(const LUID &left, const LUID &right) noexcept",
            "    static void unlink_locked(retained_t &state, impl_t *owner) noexcept",
            "    static void collect_completed()",
            "    bool register_sink()",
            "    bool admit_conversion()",
            "    void release_conversion_admission() noexcept",
        )
    )
    owner = nested_types + "\n\n" + methods
    destructor = block(text, "  d3d12_conversion_t::~d3d12_conversion_t()")
    return owner, destructor


PREFIX = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
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
struct reset_leaf_t {
  int resets = 0;
  void Reset() { ++resets; }
};
struct capture_guard_leaf_t {
  HRESULT result = 0;
  int releases = 0;
  HRESULT release() { ++releases; return result; }
  void reset() { result = 0; }
};
struct delete_probe_t {
  int *destroyed = nullptr;
  void (*on_release)() = nullptr;
  ~delete_probe_t() {
    if (destroyed) ++*destroyed;
    if (on_release) on_release();
  }
};
struct lifetime_token_t { int *destroyed; void (*on_release)() = nullptr; };
struct fixture_t;
static fixture_t *same_luid_probe = nullptr;
static bool same_luid_reentry_denied = false;
static void release_same_luid_probe();
static void release_lifetime_token(const void *value) {
  auto *token = static_cast<const lifetime_token_t *>(value);
  if (token->destroyed) ++*token->destroyed;
  if (token->on_release) token->on_release();
  delete token;
}
struct d3d12_conversion_t {
  struct impl_t;
  std::unique_ptr<impl_t> impl;
  ~d3d12_conversion_t();
};
struct d3d12_conversion_t::impl_t {
  struct luid_state_t;
  struct retained_t;
  // Delete probe checks that collection destroys the node outside registry lock.
  delete_probe_t delete_probe;
  LUID luid {};
  pointer_leaf_t<device_leaf_t> device;
  pointer_leaf_t<queue_leaf_t> queue;
  pointer_leaf_t<fence_leaf_t> producer11;
  pointer_leaf_t<fence_leaf_t> output;
  pointer_leaf_t<fence_leaf_t> retirement;
  reset_leaf_t source_mutex, source_view, opened_source, capture_mutex, capture_texture;
  std::shared_ptr<const void> capture_owner;
  capture_guard_leaf_t capture_guard;
  bool capture_staged = false;
  bool pending = false;
  bool unknown_completion = false;
  bool retired = false;
  std::uint64_t pending_value = 0;
  std::uint64_t producer_value = 0;
  bool producer_pending = false;
  bool producer_unknown = false;
  std::atomic<bool> release_unknown {false};
  std::atomic<bool> acquire_abandoned {false};
  std::unique_lock<std::mutex> frame_lock;
  HRESULT failure_status = 0;
  const char *failure_operation = nullptr;
  luid_state_t *adapter_state = nullptr;
  bool collecting = false;
  impl_t *collection_next = nullptr;
  bool registered = false;
  bool conversion_admitted = false;
  bool quarantined = false;
  impl_t *registry_next = nullptr;
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
  fence_leaf_t producer, output, retirement;
  int destroyed = 0;
  int capture_owner_destroyed = 0;
  std::unique_ptr<d3d12_conversion_t> owner;
  explicit fixture_t(std::uint32_t low, std::int32_t high = 0) : owner(new d3d12_conversion_t) {
    owner->impl = std::make_unique<d3d12_conversion_t::impl_t>();
    auto &impl = *owner->impl;
    impl.luid = {low, high};
    impl.device.value = &device;
    impl.queue.value = &queue;
    impl.producer11.value = &producer;
    impl.output.value = &output;
    impl.retirement.value = &retirement;
    impl.delete_probe.destroyed = &destroyed;
    impl.capture_owner = std::shared_ptr<const void>(
        new lifetime_token_t {&capture_owner_destroyed}, release_lifetime_token);
  }
  bool admit() {
    return owner->impl->register_sink() && owner->impl->admit_conversion();
  }
  void on_capture_owner_release(void (*callback)()) {
    auto *token = const_cast<lifetime_token_t *>(
        static_cast<const lifetime_token_t *>(owner->impl->capture_owner.get()));
    token->on_release = callback;
  }
  void pending(bool unknown = false) {
    owner->impl->pending = true;
    owner->impl->pending_value = 2;
    owner->impl->unknown_completion = unknown;
  }
};
static void release_same_luid_probe() {
  same_luid_reentry_denied = same_luid_probe && !same_luid_probe->admit() &&
                             !same_luid_probe->owner->impl->registered;
}
static fixture_t *reentrant = nullptr;
static bool reentry_succeeded = false;
static void release_reentry() { reentry_succeeded = reentrant->admit(); }

static void normal_finished() {
  fixture_t first(1), replacement(1);
  require(first.admit(), "initial registration/admission failed");
  first.pending();
  first.output.completed = 2;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 1 && first.capture_owner_destroyed == 1 && first.queue.signals == 0,
          "finished owner was retained or signaled");
  require(replacement.admit(), "finished generation kept adapter admission");
}

static void unknown_quarantine() {
  fixture_t first(2), blocked(2), collector(2);
  require(first.admit(), "initial registration/admission failed");
  first.pending();
  first.queue.result = -1;
  first.retirement.completed = 100;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 0 && first.capture_owner_destroyed == 0,
          "unknown work released resources");
  require(!blocked.admit() && first.destroyed == 0 && first.capture_owner_destroyed == 0,
          "unknown completion trusted an unproven retirement value");
  first.device.removed = true;
  require(!collector.admit() && first.destroyed == 0 && first.capture_owner_destroyed == 0,
          "device removal discharged explicitly unknown queue completion");
}

static void known_quarantine() {
  fixture_t first(3), blocked(3), collector(3);
  require(first.admit(), "initial registration/admission failed");
  first.pending();
  first.queue.result = 0;
  deny_allocation = true;
  first.owner.reset();
  require(first.destroyed == 0 && first.capture_owner_destroyed == 0 && first.queue.signals == 1,
          "unretired known tail released resources");
  require(!blocked.admit(), "unretired tail admitted same adapter");
  first.retirement.completed = std::numeric_limits<std::uint64_t>::max();
  require(!blocked.admit(), "device-removal sentinel was treated as ordinary completion");
  first.retirement.completed = 1;
  require(collector.admit() && first.destroyed == 1 && first.capture_owner_destroyed == 1,
          "completed known tail was not collected");
}

static void producer_tail() {
  fixture_t first(8), blocked(8), collector(8);
  require(first.admit(), "initial registration/admission failed");
  auto &impl = *first.owner->impl;
  impl.producer_pending = true;
  impl.producer_value = 3;
  first.producer.completed = 2;
  first.owner.reset();
  require(first.destroyed == 0 && first.capture_owner_destroyed == 0,
          "pending producer copy released source owners");
  first.producer.completed = std::numeric_limits<std::uint64_t>::max();
  require(!blocked.admit() && first.destroyed == 0 && first.capture_owner_destroyed == 0,
          "producer device-removal sentinel discharged the copy obligation");
  first.producer.completed = 3;
  require(collector.admit() && first.destroyed == 1 && first.capture_owner_destroyed == 1,
          "completed producer copy did not release retained source owners");
}

static void release_poison() {
  fixture_t first(9), blocked(9), collector(9);
  require(first.admit(), "initial registration/admission failed");
  auto *retained = first.owner->impl.get();
  auto *adapter = retained->adapter_state;
  retained->capture_guard.result = -1;
  first.owner.reset();
  require(first.destroyed == 0 && first.capture_owner_destroyed == 0 &&
              retained->capture_guard.releases == 1 && first.queue.signals == 0,
          "failed ReleaseSync dropped retained owners or submitted unrelated queue work");
  require(retained->release_unknown.load() && adapter->poisoned.load() && adapter->blocked.load(),
          "failed ReleaseSync did not poison and block the adapter gate");
  require(!blocked.admit() && !collector.admit() && first.destroyed == 0 &&
              first.capture_owner_destroyed == 0,
          "failed ReleaseSync allowed same-LUID reuse or released source owners");
}

static void collection_allocation() {
  fixture_t first(4), collector(4);
  require(first.admit(), "initial registration/admission failed");
  first.pending();
  first.queue.result = 0;
  first.owner.reset();
  require(first.destroyed == 0 && first.capture_owner_destroyed == 0, "setup did not quarantine");
  first.retirement.completed = 1;
  deny_allocation = true;
  require(collector.admit() && first.destroyed == 1 && first.capture_owner_destroyed == 1,
          "collection failed under allocation denial");
}

static void collection_reentry() {
  fixture_t first(5), collector(5), same_luid_nested(5), seed(6), nested(6);
  require(seed.admit(), "reentry adapter state setup failed");
  seed.owner.reset();
  require(first.admit(), "initial registration/admission failed");
  first.pending();
  first.queue.result = 0;
  first.owner->impl->delete_probe.on_release = release_reentry;
  reentrant = &nested;
  same_luid_probe = &same_luid_nested;
  first.on_capture_owner_release(release_same_luid_probe);
  first.owner.reset();
  first.device.removed = true;
  deny_allocation = true;
  require(collector.admit(), "collector admission failed");
  require(first.destroyed == 1 && first.capture_owner_destroyed == 1 && reentry_succeeded &&
              same_luid_reentry_denied,
          "source release reentry was not safely gated by adapter LUID");
}

static void active_admission() {
  fixture_t active(7, 1), same(7, 1), different_high(7, 2);
  require(active.admit(), "initial registration/admission failed");
  require(different_high.admit(), "adapter comparison ignored LUID HighPart");
  deny_allocation = true;
  require(!same.admit() && active.destroyed == 0, "active owner admitted duplicate conversion");
}

int main(int argc, char **argv) {
  std::set_terminate([] { std::_Exit(86); });
  require(argc == 2, "expected one case");
  try {
    if (!std::strcmp(argv[1], "normal-finished")) normal_finished();
    else if (!std::strcmp(argv[1], "unknown-quarantine")) unknown_quarantine();
    else if (!std::strcmp(argv[1], "known-quarantine")) known_quarantine();
    else if (!std::strcmp(argv[1], "producer-tail")) producer_tail();
    else if (!std::strcmp(argv[1], "release-poison")) release_poison();
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


def generated(owner, destructor):
    return PREFIX + owner + "\n};\n" + destructor + SUFFIX


def run_variant(name, owner, destructor, cases, expected_red=None, evidence_dir=None, compiler="c++"):
    expected_red = expected_red or {}
    variant = {"name": name, "cases": []}
    with tempfile.TemporaryDirectory(prefix="pyrowave-retention-", dir="/tmp") as temporary:
        scratch = Path(temporary)
        source = scratch / (name + ".cpp")
        source.write_text(generated(owner, destructor))
        binary = scratch / name
        command = [compiler, "-std=c++23", "-O0", "-Wall", "-Wextra", "-Werror", "-pthread", str(source), "-o", str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode != 0:
            raise AssertionError(f"{name} compile failed:\n{compiled.stderr}")
        variant["compile_exit"] = compiled.returncode
        variant["generated_source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        for case in cases:
            try:
                proc = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=3)
                status, stderr, timed_out = proc.returncode, proc.stderr, False
            except subprocess.TimeoutExpired as error:
                status, stderr, timed_out = None, "bounded timeout (possible lock reentry deadlock)", True
            expected = expected_red.get(case)
            if expected is None:
                assert not timed_out and status == 0, (name, case, status, stderr)
            else:
                assert not timed_out and status == 91 and expected in stderr, (name, case, status, stderr)
            variant["cases"].append({"case": case, "exit": status, "timed_out": timed_out,
                                     "expected_failure": expected, "stderr": stderr})
        if evidence_dir:
            shutil.copy2(source, evidence_dir / source.name)
            (evidence_dir / (name + "-compile.log")).write_text(compiled.stdout + compiled.stderr)
    return variant


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    path = args.repository / "src/platform/windows/pyrowave_d3d12_conversion.cpp"
    owner, destructor = production_bodies(path)
    cases = ["normal-finished", "unknown-quarantine", "known-quarantine", "producer-tail",
             "release-poison", "collection-allocation", "collection-reentry", "active-admission"]
    if args.evidence_dir:
        args.evidence_dir.mkdir(parents=True, exist_ok=True)
    variants = [run_variant("production", owner, destructor, cases, evidence_dir=args.evidence_dir,
                            compiler=args.compiler)]

    # Negative controls mutate executable production bodies, then require the
    # relevant behavioral assertion to fail (not merely source matching).
    detach_marker = "        impl.release();  // Existing node/owners, no post-copy allocation or release."
    assert destructor.count(detach_marker) == 1
    detached = destructor.replace(detach_marker, "        // regression control: unique_ptr destroys quarantined owner")
    variants.append(run_variant("drop-quarantine-owner", owner, detached, ["unknown-quarantine"],
                                {"unknown-quarantine": "unknown work released resources"}, args.evidence_dir,
                                args.compiler))

    unknown_guard = "      if (unknown_completion) return false;"
    assert owner.count(unknown_guard) == 1
    unguarded = owner.replace(unknown_guard, "      // regression control: treat unknown completion as queryable")
    variants.append(run_variant("trust-unknown-completion", unguarded, destructor, ["unknown-quarantine"],
                                {"unknown-quarantine": "unknown work released resources"}, args.evidence_dir,
                                args.compiler))

    producer_marker = "producer_finished() && "
    assert owner.count(producer_marker) == 1
    ignore_producer = owner.replace(producer_marker, "")
    variants.append(run_variant("ignore-producer-tail", ignore_producer, destructor, ["producer-tail"],
                                {"producer-tail": "pending producer copy released source owners"},
                                args.evidence_dir, args.compiler))

    evidence = {
        "source": path.relative_to(args.repository).as_posix(),
        "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "scope": "Exact production lifecycle bodies with CPU-only API leaves; no Windows/D3D/GPU execution.",
        "cases_passed": len(cases),
        "negative_controls_detected": len(variants) - 1,
        "variants": variants,
    }
    if args.evidence_dir:
        (args.evidence_dir / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({"production_cases_passed": len(cases),
                      "negative_controls_detected": len(variants) - 1,
                      "source_sha256": evidence["source_sha256"]}, sort_keys=True))


if __name__ == "__main__":
    main()
