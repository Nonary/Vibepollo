#!/usr/bin/env bash
# Migration ID: upgrade-2.0-lifecycle-v1
# Scope: 2.0.0 builds with locale-sensitive checks or helper-text-first selection.
# Retire once upgrades from these builds are no longer supported. Later
# workarounds belong in separate named scripts with their own conditions.
# No installed hook is executed or edited here. The incoming full quiescence
# guard replaces only 00-vibepollo-quiesce.hook in the private ALPM HookDir.

vibepollo_upgrade_2_0_lifecycle_compat() {
  local identity=$1 outgoing=$2 incoming=$3 reason=''
  vibepollo_compat_override_hook=0
  [[ "$identity" != *$'\n'* && "$identity" =~ ^vibepollo\ ([0-9]+:)?2\.0\.0([-+.][A-Za-z0-9.+_-]+)?$ ]] || return 0
  [[ -f "$outgoing" && ! -L "$outgoing" && -f "$incoming" && ! -L "$incoming" ]] || return 0
  # A version alone is insufficient: fixed daily builds can keep 2.0.0.
  if grep -qF 'regular file' "$outgoing" && grep -qF '%F' "$outgoing" &&
     ! grep -qx 'export LC_ALL=C' "$outgoing"; then reason=locale-sensitive; fi
  if grep -qF 'if [[ ! -e "$vibepollo_legacy_host" && ! -L "$vibepollo_legacy_host" ]]; then' "$outgoing"; then
    reason="${reason:+$reason, }helper-text-first"
  fi
  [[ -n "$reason" ]] || return 0
  grep -q '^vibepollo_quiesce_or_abort()' "$incoming" || return 1
  vibepollo_compat_override_hook=1
  export LC_ALL=C
  printf '==> Compatibility upgrade-2.0-lifecycle-v1: %s has %s checks; using C locale and the incoming exact-name shutdown guard.\n' "$identity" "$reason"
}
