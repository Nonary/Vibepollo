"""Verify only the retained-record mask fix against the current source.

Actual Arch retry and postinstall callers run with private paths and modeled
unit/process probes. No native package entrypoint, service, or live scope runs.
Success verifies preserved masks, deferred activation and completed recovery.
"""
from pathlib import Path
import os, re, subprocess, tempfile, sys, itertools

ROOT=Path(sys.argv[1]).resolve()
BRAND=sys.argv[2]
assert BRAND in ('vibeshine', 'vibepollo')
TIPS={BRAND:'current'}
failures=[]

def function(source,name):
    match=re.search(r'^'+name+r'\(\) ([{(])\n',source,re.M)
    assert match,name
    end=re.search(r'^'+re.escape('}' if match[1]=='{' else ')')+r'\s*$',source[match.end():],re.M)
    assert end,name
    return source[match.start():match.end()+end.end()]+'\n'

results=[]
for brand,tip in TIPS.items():
  relative=f'packaging/linux/Arch/{brand}.install'
  text=(ROOT/relative).read_text()
  package_names=list(dict.fromkeys(re.findall(r'^('+brand+r'_package_\w+)\(\)',text,re.M)))
  extras=['prepare_host_upgrade_fence','activate_host_upgrade_fence','quiesce_or_abort','abort_quiesce','unmask_host_for_controller','preserved_quiesce_is_safe']
  code='\n'.join(function(text,n) for n in package_names)
  code+='\n'.join(function(text,brand+'_'+n) for n in extras)
  code+='\n'.join(function(text,n) for n in ('vq','do_quiesce_machine_host','do_install_prelogin'))
  code=code.replace('vibeshine-vkms','product-vkms').replace(brand,'product').replace(brand.upper(),'PRODUCT')
  for unit,cut in itertools.product(('product.service','product-session-exec.socket','product-vkms-control.socket'),('none','before-baseline','after-baseline')):
    with tempfile.TemporaryDirectory(prefix='kept-record-unowned-mask-') as tmp:
      stage=Path(tmp)
      for name in ('systemd','etc-systemd','runtime','fence-dir'):(stage/name).mkdir()
      helper=stage/'machine-host'
      helper.write_text('#!/bin/sh\nexit 0\n');helper.chmod(0o755)
      body=code.replace('0:0:',f'{os.getuid()}:{os.getgid()}:')
      body=body.replace('/run/systemd/system',str(stage/'systemd')).replace('/etc/systemd/system',str(stage/'etc-systemd'))
      body=body.replace('/run/product',str(stage/'runtime'))
      body=body.replace('/usr/libexec/vibeshine/product-machine-host',str(helper))
      body=body.replace('/usr/libexec/product/product-machine-host',str(helper))
      body+='''
product_package_transaction="$stage/transaction"
product_package_lock_held=0
product_host_upgrade_dropin_dir="$stage/fence-dir"
product_host_upgrade_dropin="$stage/fence-dir/90-product-safe-upgrade.conf"
product_legacy_handoff="$stage/absent-handoff"
product_legacy_handoff_directory="$stage/handoffs"
product_legacy_restore_directory="$stage/restores"
product_legacy_transition_lock="$stage/transition"
product_legacy_prelogin_marker="$stage/prelogin-marker"
product_broker_socket="$stage/runtime/broker"
product_control_socket="$stage/runtime/control"
product_controller="$stage/absent-controller"
product_controller_was_frozen=0
controller_idle=0
PRODUCT_PACKAGE_RUN_ID=11111111111111111111111111111111
product_package_capture_host_executables() { echo '[]'; }
product_package_unmanaged_host_absent() { return 0; }
product_unit_is_quiescent() {
  if [ "$1" = product-session-controller.service ]; then [ "$controller_idle" = 1 ]; else return 0; fi
}
product_host_is_stable_or_quiescent() { return 0; }
product_select_upgrade_kill_mode() { product_upgrade_kill_mode=mixed; }
product_freeze_controller() { return 0; }
product_controller_remains_frozen() { return 0; }
product_thaw_controller() { return 0; }
product_stop_exact_unit() { [ "$1" != product-session-controller.service ] || controller_idle=1; return 0; }
product_stop_restore_instances() { return 0; }
product_stop_brokers() { return 0; }
product_control_instances_are_quiescent() { return 0; }
product_brokers_are_quiescent() { return 0; }
product_restore_instances_are_quiescent() { return 0; }
product_disable_legacy_handoff() { return 0; }
product_runtime_root_is_safe_or_absent() { return 0; }
product_cleanup_legacy_transition_state() { return 0; }
product_run_optional_legacy_command() { return 0; }
product_retire_obsolete_boot_links() { return 0; }
product_restore_template_is_masked() { [ -L "$stage/systemd/product-session-restore@.service" ]; }
product_control_socket_is_masked() { [ -L "$stage/systemd/product-vkms-control.socket" ]; }
product_broker_socket_is_masked() { [ -L "$stage/systemd/product-session-exec.socket" ]; }
product_host_unit_is_masked() { [ -L "$stage/systemd/product.service" ]; }
timeout() { while [ "${1#--}" != "$1" ]; do shift; done; shift; "$@"; }
install() {
  local -a kept=()
  while (( $# )); do case "$1" in -o|-g) shift 2 ;; *) kept+=("$1"); shift ;; esac; done
  command install "${kept[@]}"
}
systemctl() {
  printf '%s\\n' "$*" >>"$stage/events"
  case "$1" in
    mask)
      shift 2
      for name do [ -L "$stage/systemd/$name" ] || ln -s /dev/null "$stage/systemd/$name"; done ;;
    show)
      shift; name=$1; shift
      for property do case "$property" in
        --property=Job) echo Job= ;;
        --property=MainPID) echo MainPID=0 ;;
        --property=ControlGroup) echo ControlGroup= ;;
        --property=InvocationID) echo InvocationID= ;;
        --property=FreezerState) echo FreezerState= ;;
        --property=LoadState) if [ -L "$stage/systemd/$name" ]; then echo LoadState=masked; else echo LoadState=loaded; fi ;;
        --property=RefuseManualStart) if [ -e "$product_host_upgrade_dropin" ]; then echo RefuseManualStart=yes; else echo RefuseManualStart=no; fi ;;
        --property=KillMode) echo KillMode=mixed ;;
        --property=SendSIGKILL) echo SendSIGKILL=no ;;
        --property=Restart) echo Restart=no ;;
        *) return 90 ;;
      esac; done ;;
    is-enabled) if [ -L "$stage/systemd/$2" ]; then echo masked-runtime; else echo disabled; fi ;;
    daemon-reload|disable|enable|start|is-active) return 0 ;;
    *) return 91 ;;
  esac
}
product_package_begin || exit 92
printf '[Unit]\\nRefuseManualStart=yes\\n\\n[Service]\\nKillMode=mixed\\nSendSIGKILL=no\\nRestart=no\\n' >"$product_host_upgrade_dropin"
chmod 644 "$product_host_upgrade_dropin"
product_package_record_owned fence "$product_host_upgrade_dropin" || exit 93
for name in product.service product-session-exec.socket product-vkms-control.socket 'product-session-restore@.service'; do
  [ "$name" != "$external_unit" ] || continue
  product_package_mask "$name" || exit 94
done
product_package_write_record phase late || exit 95
ln -s /dev/null "$stage/systemd/$external_unit"
external_identity=$(product_package_path_identity "$stage/systemd/$external_unit") || exit 96
[ "$(cat "$stage/transaction/$external_unit.before")" = absent ] || exit 97
[ ! -e "$stage/transaction/$external_unit.owned" ] || exit 98
if [ "$external_unit" = product.service ]; then
  product_package_preserve_unowned_host_mask || exit 99
  [ "$product_package_preserve_host_mask" = 1 ] || exit 100
fi
controller_idle=1
product_preserved_quiesce_is_safe || exit 120
mask_calls_before=$(grep -c '^mask ' "$stage/events" || true)
# Repeated actual retries must take the preserved-quiescence return.
for retry in 1 2 3; do product_quiesce_or_abort || exit 101; done
[ "$mask_calls_before" = "$(grep -c '^mask ' "$stage/events" || true)" ] || exit 121
# The actual retry must preserve the external inode and never own it.
[ ! -e "$stage/transaction/$external_unit.owned" ] || exit 102
[ "$(cat "$stage/transaction/$external_unit.before")" = absent ] || exit 108
[ "$external_identity" = "$(product_package_path_identity "$stage/systemd/$external_unit")" ] || exit 103
controller_idle=1
# Inject a one-time interruption immediately before/after the baseline writer's
# atomic rename. All writer and release logic outside this fault remains real.
# Use a durable marker because package_write_record is a subshell.
mv() {
  local destination
  for destination do :; done
  if [ "$destination" = "$stage/transaction/$external_unit.before" ] && [ ! -e "$stage/interrupted" ] && [ "$cut" != none ]; then
    : >"$stage/interrupted"
    if [ "$cut" = before-baseline ]; then return 123; fi
    command mv "$@" || return 1
    return 124
  fi
  command mv "$@"
}
if [ "$cut" != none ]; then
  do_install_prelogin && exit 122
  [ -e "$stage/interrupted" ] || exit 123
  [ ! -e "$stage/transaction/$external_unit.owned" ] || exit 124
  [ "$external_identity" = "$(product_package_path_identity "$stage/systemd/$external_unit")" ] || exit 125
  ! grep -Eq '^(enable|start) ' "$stage/events" || exit 126
  [ -d "$stage/transaction" ] || exit 127
fi
# The original postinstall engine must recover on repeated retries.
do_install_prelogin || exit 104
[ "$external_identity" = "$(product_package_path_identity "$stage/systemd/$external_unit")" ] || exit 105
! grep -Eq '^(enable|start) ' "$stage/events" || exit 106
[ ! -d "$stage/transaction" ] || exit 107
for retired in product.service product-session-exec.socket product-vkms-control.socket; do
  [ "$retired" = "$external_unit" ] || { [ ! -L "$stage/systemd/$retired" ] || exit 128; }
done
[ ! -e "$product_host_upgrade_dropin" ] || exit 129
printf 'PASS external mask preserved, deferred postinstall succeeded: %s\\n' "$external_unit"
'''
      result=subprocess.run(['bash','-c',body],env=dict(os.environ,stage=tmp,external_unit=unit,cut=cut),text=True,capture_output=True,timeout=15)
      if result.returncode:
        line=f'{brand} {tip} {unit} {cut}: FAIL {result.returncode} {result.stderr.strip()}'
        print(line,flush=True); failures.append(line); continue
      line=f'{brand} {tip} {unit} {cut}: {result.stdout.strip()}'
      print(line,flush=True);results.append(line)
print('SUMMARY',len(results),'PASS',len(failures),'FAIL; actual idle preserved path and full postinstall; private only')
raise SystemExit(bool(failures))
