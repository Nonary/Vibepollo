"""Actual standalone mask ownership helpers with private command boundaries."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve()
brand = sys.argv[2] if len(sys.argv) > 2 else 'vibeshine'
assert brand in ('vibeshine', 'vibepollo')
contexts = [(f'packaging/linux/Arch/{brand}.install', 0),
            ('packaging/linux/vibeshine-preinst.in', 0),
            (f'packaging/linux/{brand}-postinst.in', 0),
            (f'packaging/linux/{brand}-prerm.in', 0),
            ('packaging/linux/vibeshine-postrm.in', 0)]
contexts += [('packaging/linux/copr/Sunshine.spec', i) for i in range(4)]
names = ['package_mask', 'package_path_identity', 'package_write_record',
         'package_transaction_is_safe', 'package_record_is_safe',
         'package_remove_owned']
units = ['product.service', 'product-session-exec.socket',
         'product-vkms-control.socket', 'product-session-restore@.service']
shells = ['bash', 'sh']
dash = Path('/tmp/upgrade-dash/bin/dash')
if dash.is_file(): shells.append(str(dash))
elif shutil.which('dash'): shells.append(shutil.which('dash'))
count = 0
for relative, index in contexts:
    if relative.endswith('Sunshine.spec'):
        raw = (root / relative).read_text()
        assert raw.count("stat -c '%%u:%%g:%%h:%%F'") == 4
    text = (root / relative).read_text().replace('%%', '%')
    text = text.replace('vibeshine-vkms', 'product-vkms').replace(brand, 'product').replace(brand.upper(), 'PRODUCT')
    functions = []
    for name in names:
        matches = re.findall(r'^product_' + name + r'\(\) [({]\n.*?^[)}]$', text, re.M | re.S)
        assert len(matches) > index, (relative, name, index)
        functions.append(matches[index])
    code = '\n'.join(functions).replace('0:0:', f'{os.getuid()}:{os.getgid()}:')
    for shell in (['bash'] if '/Arch/' in relative else shells):
        for unit in units:
            scenarios = ['new', 'external', 'existing-owned', 'collision', 'manager-timeout', 'owned-mismatch']
            if unit == units[0]: scenarios += ['manager-replacement', 'failed-publication', 'missing-mktemp']
            for scenario in scenarios:
                with tempfile.TemporaryDirectory(prefix='package-mask-ownership-') as temp:
                    stage = Path(temp)
                    (stage / 'systemd').mkdir()
                    transaction = stage / 'transaction'
                    transaction.mkdir(mode=0o700)
                    before = transaction / (unit + '.before')
                    before.write_text('absent\n'); before.chmod(0o600)
                    body = code.replace('/run/systemd/system', str(stage / 'systemd'))
                    body += r'''
product_package_transaction="$stage/transaction"
product_host_upgrade_dropin="$stage/absent-fence"
path="$stage/systemd/$unit"
owned="$product_package_transaction/$unit.owned"
if [ "$scenario" = external ] || [ "$scenario" = existing-owned ] || [ "$scenario" = owned-mismatch ]; then
  command ln -s /dev/null "$path"
  original=$(product_package_path_identity "$path") || exit 91
  if [ "$scenario" = existing-owned ]; then product_package_write_record "$unit.owned" "$original" || exit 92; fi
  if [ "$scenario" = owned-mismatch ]; then product_package_write_record "$unit.owned" 'different-inode' || exit 93; fi
fi
timeout() { shift 2; "$@"; }
systemctl() {
  printf '%s\n' "$*" >>"$stage/events"
  [ "$1" = mask ] && [ "$2" = --runtime ] || return 94
  if [ "$scenario" = manager-replacement ]; then
    command mv "$path" "$stage/original-mask"
    command ln -s /dev/null "$path"
    original=$(product_package_path_identity "$path")
  fi
  [ "$scenario" != manager-timeout ] || return 124
}
ln() {
  if [ "$1" = -P ]; then
    if [ "$scenario" = collision ]; then
      command ln -s /dev/null "$path"
      original=$(product_package_path_identity "$path")
      printf '%s\n' "$original" >"$stage/external-identity"
    fi
    [ "$scenario" != failed-publication ] || return 1
  fi
  command ln "$@"
}
mktemp() { [ "$scenario" != missing-mktemp ] || return 1; command mktemp "$@"; }
product_package_mask "$unit"
result=$?
case "$scenario" in
  external|collision)
    [ "$result" = 0 ] || exit 95
    [ "$scenario" != collision ] || original=$(cat "$stage/external-identity")
    [ ! -e "$owned" ] && [ ! -L "$owned" ] || exit 96
    [ "$(cat "$product_package_transaction/$unit.before")" = "$original" ] || exit 97
    product_package_remove_owned "$unit" "$path" || exit 98
    [ "$(product_package_path_identity "$path")" = "$original" ] || exit 99 ;;
  new|existing-owned|manager-timeout)
    if [ "$scenario" = manager-timeout ]; then [ "$result" = 124 ] || exit 100; else [ "$result" = 0 ] || exit 101; fi
    [ "$(cat "$owned")" = "$(product_package_path_identity "$path")" ] || exit 102
    product_package_remove_owned "$unit" "$path" || exit 103
    [ ! -e "$path" ] && [ ! -L "$path" ] || exit 104 ;;
  owned-mismatch)
    [ "$result" != 0 ] || exit 105
    [ ! -e "$stage/events" ] || exit 106
    [ "$(cat "$owned")" = different-inode ] || exit 107
    [ "$(product_package_path_identity "$path")" = "$original" ] || exit 108 ;;
  manager-replacement)
    [ "$result" != 0 ] || exit 109
    ! product_package_remove_owned "$unit" "$path" || exit 110
    [ "$(product_package_path_identity "$path")" != "$(cat "$owned")" ] || exit 111
    [ -L "$path" ] || exit 112 ;;
  failed-publication|missing-mktemp)
    [ "$result" != 0 ] || exit 113
    [ ! -e "$stage/events" ] || exit 114
    [ ! -e "$path" ] && [ ! -L "$path" ] || exit 115
    [ ! -e "$owned" ] && [ ! -L "$owned" ] || exit 116 ;;
esac
'''
                    result = subprocess.run([shell, '-c', body], env=dict(os.environ, stage=temp, unit=unit, scenario=scenario), capture_output=True, text=True, timeout=10)
                    assert result.returncode == 0, (relative, index, shell, unit, scenario, result.returncode, result.stderr)
                    assert not list(transaction.glob('.mask.*')), (relative, scenario, 'staging leak')
                    count += 1
print(f'{brand}: {count} actual mask-helper cases passed across nine standalone contexts; no native services')
subprocess.run([sys.executable, str(Path(__file__).parent / 'fixtures/package-mask-retry.py'), str(root), brand], check=True)

subprocess.run([sys.executable, str(Path(__file__).parent / 'fixtures/package-idle-mask-retry.py'), str(root), brand], check=True)
subprocess.run([sys.executable, str(Path(__file__).parent / 'fixtures/package-idle-mask-faults.py'), str(root), brand], check=True)
