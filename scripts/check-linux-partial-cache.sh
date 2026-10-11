#!/usr/bin/env bash
# Read-only eligibility checks for trusted manual validation cache retention.
set -euo pipefail
recipe=$1
installer=$2
patch_file=$3
compiler_log=$4
[[ "${VALIDATION_ONLY:-}" == true && "${GITHUB_EVENT_NAME:-}" == workflow_dispatch && "${EVENT_REPOSITORY:-}" == "${GITHUB_REPOSITORY:-}" && -n "${TOOLCHAIN_ABI:-}" && "${PACKAGE_BUILD_OUTCOME:-}" == failure ]] || exit 0
# Partial archive/toolkit caches require the original pinned checksum and receipt.
if [[ -f "$installer" && -f "$patch_file" && -f "$CUDA_CACHE_DIR/toolkit/.verified-recipe" ]]; then
  expected=$( _use_cuda=true CARCH=x86_64 bash -c 'source "$1"; printf "%s" "${sha256sums_x86_64[0]}"' _ "$recipe" )
  actual=$(sha256sum "$installer" | cut -d' ' -f1)
  receipt=$( { sha256sum "$installer" "$patch_file" | cut -d' ' -f1; uname -m; } | sha256sum | cut -d' ' -f1)
  if [[ "$expected" == "$actual" && $(cat "$CUDA_CACHE_DIR/toolkit/.verified-recipe") == "$receipt" ]] &&
     "$CUDA_CACHE_DIR/toolkit/bin/nvcc" --version | grep -Fq 'release 12.9,'; then
    echo 'cuda=true'
  fi
fi
# Preserve only ccache's content store, never build directories/results/packages.
if [[ -s "$compiler_log" ]] && grep -q "Result:" "$compiler_log" && [[ -d "$CCACHE_DIR" ]] && [[ -n $(find "$CCACHE_DIR" -type f ! -name stats ! -name ccache.conf ! -name CACHEDIR.TAG -size +0c -print -quit) ]]; then
  echo 'compiler=true'
fi
