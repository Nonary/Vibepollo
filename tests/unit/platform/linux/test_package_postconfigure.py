#!/usr/bin/env python3
"""Execute real postinstall lifecycle callbacks with private paths and a manager model."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import test_package_removal as removal

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]

def sources(context):
    if context == 'DEB':
        text=(ROOT/'packaging/linux/vibepollo-postinst.in').read_text()
        tail=text[text.rindex('if [ "$vibepollo_machine_configured" -eq 1 ]; then'):]
    else:
        text=(ROOT/'packaging/linux/copr/Sunshine.spec').read_text().split('\n%post\n',1)[1].split('\n%preun\n',1)[0].replace('%%','%')
        start=text.index('  if "$vibepollo_machine_helper" configure-auto; then')
        tail=text[start:]
        # The configure branch is the final nested section before outer post guard.
        tail=tail.split('\nelse\n',1)[0]
    definitions=text[text.index('vibepollo_controller='):text.index('\nif ! vibepollo_quiesce_or_abort; then')]
    removal.assert_local_calls_defined(definitions,context+' postinstall')
    return definitions,tail

def main():
    count=0
    for context in ('DEB','RPM'):
        definitions,tail=sources(context)
        for shell in (['bash'] + ([shutil.which('dash')] if shutil.which('dash') else ['sh'])):
            for scenario in ('first-configure','reconfigure','reboot-no-record','admin-mask','failed-proof','start-failure','afterstart-proof','afterstart-failed-stop'):
                with tempfile.TemporaryDirectory(prefix='postconfigure-fixture-') as tmp:
                    f=Path(tmp);(f/'systemd').mkdir();(f/'runtime').mkdir(mode=0o700)
                    (f/'host').write_text('stopped');(f/'events').touch();(f/'clock').write_text('0')
                    controller=f/'controller';controller.write_text('#!/bin/sh\nexit 0\n');controller.chmod(0o755)
                    machine=f/'machine';machine.write_text('#!/bin/sh\nexit 1\n');machine.chmod(0o755)
                    if scenario=='admin-mask': (f/'systemd/vibepollo.service').symlink_to('/dev/null')
                    defs=definitions.replace('/run/systemd/system',str(f/'systemd')).replace('/run/vibepollo-package-transaction',str(f/'transaction')).replace('/run/vibepollo/',str(f/'runtime')+'/').replace('/run/vibeshine/',str(f/'runtime')+'/')
                    for prefix in ('restore-units','broker-units','control-units','host-upgrade'):
                        defs=defs.replace('/run/vibepollo-'+prefix+'.',str(f/prefix)+'.')
                    mocks=removal.MOCKS
                    # Add modeled new-start operations to the existing manager
                    # function while preserving show/mask/stop behavior.
                    mocks=mocks.replace('    list-units|daemon-reload|disable) : ;;','''    start)
      case "$scenario" in
        start-failure) return 1 ;;
        afterstart-proof) touch "$fixture/block-post-proof"; return 1 ;;
        afterstart-failed-stop) printf active >"$fixture/host"; return 1 ;;
      esac ;;

    enable|list-units|daemon-reload|disable) : ;;''')
                    mocks=mocks.replace('failed_stop) return 1', 'failed_stop|afterstart-failed-stop) return 1')
                    mocks=mocks.replace("RefuseManualStart) printf 'RefuseManualStart=yes\\n' ;;", "RefuseManualStart) if [ -f \"$fixture/fence\" ]; then printf 'RefuseManualStart=yes\\n'; else printf 'RefuseManualStart=no\\n'; fi ;;\n          KillMode) printf 'KillMode=mixed\\n' ;;\n          SendSIGKILL) printf 'SendSIGKILL=no\\n' ;;")
                    script=defs+mocks+f'\nvibepollo_controller="{controller}"\nvibepollo_legacy_host="{f}/absent-machine"\nvibepollo_legacy_handoff="{f}/absent-handoff"\nvibepollo_host_upgrade_dropin="{f}/fence"\nvibepollo_host_upgrade_dropin_dir="{f}/systemd/vibepollo.service.d"\nvibepollo_machine_helper="{machine}"\n'
                    # These are the production callbacks, not stub quiescers.
                    script+='vibepollo_quiesce_or_abort || exit 91\n'
                    if scenario=='reconfigure': script+='vibepollo_package_complete_quiescence_is_safe && vibepollo_package_release || exit 92\nvibepollo_quiesce_or_abort || exit 93\n'
                    if scenario=='failed-proof': script+='touch "$fixture/block-post-proof"\n'
                    if context=='DEB':script+='vibepollo_machine_configured='+('1' if scenario in ('start-failure','afterstart-proof','afterstart-failed-stop') else '0')+'\n'
                    elif scenario in ('start-failure','afterstart-proof','afterstart-failed-stop'): machine.write_text('#!/bin/sh\nexit 0\n')
                    script+=tail
                    result=subprocess.run([shell,'-c',script],env=dict(os.environ,fixture=tmp,scenario=scenario),capture_output=True,text=True,timeout=20)
                    failed=scenario in ('failed-proof','start-failure','afterstart-proof','afterstart-failed-stop')
                    assert (result.returncode != 0) == failed,(context,shell,scenario,result.returncode,result.stderr)
                    assert 'not found' not in result.stderr,(context,shell,scenario,result.stderr)
                    events=(f/'events').read_text()
                    if scenario=='start-failure':
                        assert not (f/'transaction').exists(),result.stderr
                        assert not (f/'fence').exists(),result.stderr
                        assert not (f/'systemd/vibepollo.service').exists()
                        assert not (f/'systemd/vibepollo-session-exec.socket').exists()
                        assert events.count('mask --runtime vibepollo.service')>=2,events
                        assert sum(line.startswith('start ') for line in events.splitlines()) == 1,events
                    elif scenario in ('failed-proof','afterstart-proof','afterstart-failed-stop'):
                        assert (f/'transaction').is_dir() and (f/'fence').exists()
                    else:
                        assert not (f/'transaction').exists(),(scenario,result.stderr)
                        assert not (f/'fence').exists()
                        assert (f/'systemd/vibepollo.service').is_symlink()==(scenario=='admin-mask')
                        assert not (f/'systemd/vibepollo-session-exec.socket').exists()
                        assert not any(x.startswith('start ') for x in events.splitlines()),events
                    count+=1
    print(f'{count} unmodified postinstall callback scenarios passed, including no-record configuration and post-release cleanup; no live services touched.')

if __name__=='__main__':main()
