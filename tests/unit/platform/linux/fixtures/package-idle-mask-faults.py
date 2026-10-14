"""Actual Arch unowned-mask reconcile fault checks with private paths/probes.

Usage: package-idle-mask-faults.py REPOSITORY_ROOT BRAND
BRAND is vibeshine or vibepollo; no native services or package actions run."""
from pathlib import Path
import os,re,subprocess,tempfile,sys

ROOT=Path(sys.argv[1]).resolve()
BRAND=sys.argv[2]
assert BRAND in ('vibeshine','vibepollo')
source=(ROOT/f'packaging/linux/Arch/{BRAND}.install').read_text()
source=source.replace('vibeshine-vkms','product-vkms').replace(BRAND,'product').replace(BRAND.upper(),'PRODUCT')
def function(name):
 m=re.search(r'^'+name+r'\(\) ([{(])\n',source,re.M);assert m,name
 e=re.search(r'^'+re.escape('}' if m[1]=='{' else ')')+r'\s*$',source[m.end():],re.M);assert e
 return source[m.start():m.end()+e.end()]+'\n'
names=list(dict.fromkeys(re.findall(r'^(product_package_\w+)\(\)',source,re.M)))
helpers='\n'.join(function(n) for n in names)
writer=function('product_package_write_record').replace('product_package_write_record()','product_real_write_record()',1)
count=0
for unit in ('product.service','product-session-exec.socket','product-vkms-control.socket','product-session-restore@.service'):
 for scenario in ('ok','uncertain','changed-proof','changed-publish','owned-corrupt','owned-symlink','lock-unavailable','wrong-path','fence-unowned','owned-during-proof','baseline-during-proof'):
  with tempfile.TemporaryDirectory(prefix='idle-mask-reconcile-') as tmp:
   stage=Path(tmp);(stage/'systemd').mkdir();(stage/'runtime').mkdir()
   code=(helpers+writer).replace('0:0:',f'{os.getuid()}:{os.getgid()}:').replace('/run/systemd/system',str(stage/'systemd')).replace('/run/product/session.env',str(stage/'runtime/session.env'))
   code+='''
product_package_transaction="$stage/transaction"
product_package_lock_held=0
product_host_upgrade_dropin="$stage/fence"
product_legacy_handoff="$stage/handoff"
product_legacy_handoff_directory="$stage/handoffs"
product_legacy_restore_directory="$stage/restores"
product_legacy_transition_lock="$stage/transition"
product_legacy_prelogin_marker="$stage/prelogin"
product_broker_socket="$stage/broker"
product_control_socket="$stage/control"
product_package_capture_host_executables() { echo '[]'; }
product_host_is_stable_or_quiescent() { return 0; }
product_unit_is_quiescent() { return 0; }
product_control_instances_are_quiescent() { return 0; }
product_brokers_are_quiescent() { return 0; }
product_restore_instances_are_quiescent() { return 0; }
product_package_unmanaged_host_absent() {
  [ "$scenario" != uncertain ] || return 1
  if [ "$scenario" = changed-proof ]; then
    mv "$mask_path" "$stage/original-mask"
    ln -s /dev/null "$mask_path"
    product_package_path_identity "$mask_path" >"$stage/replacement.identity"
  fi
  case "$scenario" in
    owned-during-proof) product_package_write_record "$unit.owned" "$original_identity" || return 1 ;;
    baseline-during-proof) product_package_write_record "$unit.before" "$original_identity" || return 1 ;;
  esac
  return 0
}
timeout() { shift 2; "$@"; }
systemctl() {
  case "$*" in
    *--property=Job*) echo Job= ;;
    *--property=MainPID*) printf 'MainPID=0\\nControlGroup=\\nFreezerState=\\nInvocationID=\\n' ;;
    *) return 91 ;;
  esac
}
PRODUCT_PACKAGE_RUN_ID=11111111111111111111111111111111
product_package_begin || exit 92
mask_path="$stage/systemd/$unit"
ln -s /dev/null "$mask_path"
original_identity=$(product_package_path_identity "$mask_path") || exit 93
case "$scenario" in
  owned-corrupt) product_package_write_record "$unit.owned" different-inode || exit 94 ;;
  owned-symlink)
    printf 'unchanged private sentinel\\n' >"$stage/owned-target"
    chmod 600 "$stage/owned-target"
    ln -s "$stage/owned-target" "$stage/transaction/$unit.owned" ;;
  lock-unavailable) product_package_lock() { return 1; } ;;
  wrong-path)
    mv "$mask_path" "$stage/systemd/unrelated.service"
    mask_path="$stage/systemd/unrelated.service" ;;
  fence-unowned)
    rm "$mask_path"
    printf 'administrator fence\\n' >"$stage/fence"; chmod 644 "$stage/fence"
    unit=fence; mask_path="$stage/fence"
    original_identity=$(product_package_path_identity "$mask_path") || exit 95 ;;
esac
product_package_write_record() {
  product_real_write_record "$@" || return 1
  if [ "$scenario:$1" = "changed-publish:$unit.before" ]; then
    mv "$mask_path" "$stage/original-mask"
    ln -s /dev/null "$mask_path"
    product_package_path_identity "$mask_path" >"$stage/replacement.identity"
  fi
}
product_package_remove_owned "$unit" "$mask_path"
status=$?
if [ "$scenario" = ok ]; then
  [ "$status" = 0 ] || exit 96
  [ "$(cat "$stage/transaction/$unit.before")" = "$original_identity" ] || exit 97
else
  [ "$status" != 0 ] || exit 98
  expected_before=absent
  case "$scenario" in
    changed-publish|baseline-during-proof) expected_before=$original_identity ;;
  esac
  [ "$(cat "$stage/transaction/$unit.before")" = "$expected_before" ] || exit 99
fi
expected_current=$original_identity
[ ! -e "$stage/replacement.identity" ] || expected_current=$(cat "$stage/replacement.identity")
[ "$(product_package_path_identity "$mask_path")" = "$expected_current" ] || exit 100
case "$scenario" in
  owned-during-proof) [ "$(cat "$stage/transaction/$unit.owned")" = "$original_identity" ] || exit 105 ;;
  owned-corrupt) [ "$(cat "$stage/transaction/$unit.owned")" = different-inode ] || exit 101 ;;
  owned-symlink)
    [ -L "$stage/transaction/$unit.owned" ] || exit 102
    [ "$(cat "$stage/owned-target")" = 'unchanged private sentinel' ] || exit 103 ;;
  *) [ ! -e "$stage/transaction/$unit.owned" ] && [ ! -L "$stage/transaction/$unit.owned" ] || exit 104 ;;
esac
'''
   r=subprocess.run(['bash','-c',code],env=dict(os.environ,stage=tmp,unit=unit,scenario=scenario),capture_output=True,text=True,timeout=10)
   assert r.returncode==0,(unit,scenario,r.returncode,r.stderr)
   print('PASS',unit,scenario,flush=True);count+=1
print(BRAND, 'PASS',count,'actual Arch reconcile fault cases; private only')
