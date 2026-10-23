#!/usr/bin/env bash
# Migration ID: upgrade-2.0.1-lifecycle-v1
# Qualified source: Vibepollo tag 2.0.1 (66e169c32), Vibeshine tag 2.0.1
# (f078104b2). Both outgoing hooks select from helper text before proving the
# host stopped and compare a fence lacking Restart=no. Retire this step when
# upgrades from these builds are no longer supported; do not widen its version
# match without reproducing the corresponding outgoing hook behavior.

vibepollo_upgrade_2_0_1_lifecycle_compat() {
  local identity=$1 outgoing=$2 incoming=$3
  [[ "$identity" != *$'\n'* && "$identity" =~ ^vibepollo\ ([0-9]+:)?2\.0\.1([-+.][A-Za-z0-9.+_-]+)?$ ]] || return 0
  [[ -f "$outgoing" && ! -L "$outgoing" && -f "$incoming" && ! -L "$incoming" ]] || return 0
  grep -Fq 'if [[ ! -e "$vibepollo_legacy_host" && ! -L "$vibepollo_legacy_host" ]]; then' "$outgoing" || return 0
  grep -Fq 'KillMode=%s\nSendSIGKILL=no\n' "$outgoing" || return 0
  if grep -Fq 'SendSIGKILL=no\nRestart=no' "$outgoing"; then return 0; fi
  grep -q '^vibepollo_quiesce_or_abort()' "$incoming" || return 1
  vibepollo_compat_override_hook=1
  export LC_ALL=C
  printf '==> Compatibility upgrade-2.0.1-lifecycle-v1: %s has the qualified old selector/fence contract; using the incoming exact-name shutdown guard.\n' "$identity"
}
