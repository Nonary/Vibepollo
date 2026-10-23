#!/usr/bin/env bash
# Migration ID: upgrade-2.0.2-fuse-scan-v1
# Qualified daily: 2.0.2daily.d20261017.2, package release 1. The complete
# outgoing Arch hook is pinned to 416e6dec2b515b2e0f38f6329397a4d393dcd94f.
# Retire after upgrades from this daily are no longer supported. Never widen
# this predicate/hash without qualifying the additional outgoing artifact.
# The incoming full AbortOnFail guard replaces only our exact hook name in
# the installer's private HookDir; no installed file or mask is edited here.

vibepollo_upgrade_2_0_2_fuse_scan_compat() {
  local identity=$1 outgoing=$2 incoming=$3 digest
  vibepollo_compat_fuse_scan_recheck=0
  [[ "$identity" != *$'\n'* && "$identity" =~ ^vibepollo\ ([0-9]+:)?2\.0\.2daily\.d20261017\.2\.416e6dec2b51(\.r416e6dec2b515b2e0f38f6329397a4d393dcd94f)?-1$ ]] || return 0
  [[ -f "$outgoing" && ! -L "$outgoing" && -f "$incoming" && ! -L "$incoming" ]] || return 0
  [[ -x /usr/bin/sha256sum ]] || return 1
  digest=$(/usr/bin/sha256sum -- "$outgoing") || return 1
  [[ "${digest%% *}" == 02165129f73459129847324700b74d9ff8b9f209b477427e3a62e65e238560a5 ]] || return 0
  # The signed/chosen incoming package must carry the corrected proof. Its
  # actual full quiescence guard still runs under ALPM before replacement.
  grep -q '^vibepollo_quiesce_or_abort()' "$incoming" &&
    grep -Fq 'fd = os.open(executable, os.O_PATH | os.O_CLOEXEC)' "$incoming" &&
    grep -Fq 'if inaccessible_fuse_is_unrelated(fd, target, identities):' "$incoming" || return 1
  vibepollo_compat_override_hook=1
  vibepollo_compat_fuse_scan_recheck=1
  export LC_ALL=C
  printf '==> Compatibility upgrade-2.0.2-fuse-scan-v1: %s has the qualified FUSE-stat guard; using the incoming full shutdown guard before replacement.\n' "$identity"
}
