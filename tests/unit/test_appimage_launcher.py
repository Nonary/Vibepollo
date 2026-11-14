#!/usr/bin/env python3
"""Exercise the real portable launcher without altering host rules/services."""
from pathlib import Path
import json,os,re,shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
launcher=root/'packaging/linux/AppImage/AppRun'
source=launcher.read_text()
product=re.search(r'SUNSHINE_BIN_HERE=\$HERE/usr/bin/(\S+)',source).group(1)
with tempfile.TemporaryDirectory(prefix='appimage-launcher-') as tmp:
 stage=Path(tmp)/'AppDir';stage.mkdir()
 run=stage/'AppRun';shutil.copy2(launcher,run)
 binaries=stage/'usr/bin';binaries.mkdir(parents=True)
 binary=binaries/product
 binary.write_text('#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps(sys.argv[1:]))\n');binary.chmod(0o755)
 tools=Path(tmp)/'tools';tools.mkdir()
 marker=Path(tmp)/'privileged-call'
 for name in ['sudo','setcap','systemctl','modprobe','udevadm']:
  tool=tools/name
  tool.write_text('#!/bin/sh\nprintf called > "'+str(marker)+'"\nexit 99\n');tool.chmod(0o755)
 env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],XDG_CONFIG_HOME=str(Path(tmp)/'config'))
 for option in ['--install','install','-i','--remove','remove','-r']:
  result=subprocess.run(['bash',str(run),option],env=env,capture_output=True,text=True)
  if result.returncode!=1 or 'native' not in result.stderr:raise RuntimeError('legacy privileged option did not redirect to native packaging')
  if marker.exists():raise RuntimeError('portable launcher invoked a privileged tool')
 result=subprocess.run(['bash',str(run),'--config','path with spaces.conf'],env=env,capture_output=True,text=True,check=True)
 if json.loads(result.stdout)!=['--config','path with spaces.conf']:raise RuntimeError('launcher split quoted arguments')
 if not (Path(tmp)/'config'/product).is_dir():raise RuntimeError('launcher did not respect XDG_CONFIG_HOME')
 if marker.exists():raise RuntimeError('portable launch requested privilege')
print('production AppImage launcher: privileged options refused; portable args and XDG state preserved')
