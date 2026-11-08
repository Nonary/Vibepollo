#!/usr/bin/env python3
"""Execute production CMake TrueHDR staging with controlled downloader edges."""
from pathlib import Path
from itertools import product
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'cmake/packaging/windows.cmake').read_text()
a=source.index('# NVIDIA TrueHDR runtime.')
b=source.index('# ARM64:',a)
block=source[a:b]
with tempfile.TemporaryDirectory(prefix='truehdr-install-') as tmp:
 base=Path(tmp)
 tools=base/'bin';tools.mkdir()
 fake=tools/'powershell'
 fake.write_text('''#!/usr/bin/env python3
import os,sys
from pathlib import Path
from itertools import product
args=sys.argv[1:]
assert args[args.index('-Repository')+1] == 'Nonary/vibeshine_truehdr_runtime'
assert args[args.index('-Tag')+1] == 'v1.0.0'
mode=os.environ['TRUEHDR_INSTALL_TEST_MODE']
if mode=='failure':sys.exit(17)
out=Path(args[args.index('-OutDir')+1]);out.mkdir(parents=True,exist_ok=True)
for name in ['vibeshine_truehdr.dll','nvngx_truehdr.dll']:
 if mode=='missing' and name=='nvngx_truehdr.dll':continue
 (out/name).write_bytes(b'' if mode=='empty' else b'fixture-runtime')
''')
 fake.chmod(0o755)
 for mode, component in product(['success','failure','missing','empty'], [None, 'application']):
  src=base/(mode+'-'+(component or 'all'));src.mkdir();build=src/'build';prefix=src/'stage'
  (src/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(TrueHdrInstall NONE)\n'
    +f'include("{root}/cmake/packaging/windows_virtual_display_contract.cmake")\n'+block)
  # Old build trees may explicitly cache OFF; production must override it.
  subprocess.run(['cmake','-S',str(src),'-B',str(build),'-DSUNSHINE_REQUIRE_TRUEHDR_RUNTIME=OFF'],check=True,capture_output=True,text=True)
  cache=(build/'CMakeCache.txt').read_text()
  if 'SUNSHINE_REQUIRE_TRUEHDR_RUNTIME:BOOL=ON' not in cache:raise RuntimeError('required runtime remained optional')
  env=dict(os.environ,PATH=str(tools)+os.pathsep+os.environ['PATH'],TRUEHDR_INSTALL_TEST_MODE=mode)
  command=['cmake','--install',str(build),'--prefix',str(prefix)]
  if component:command.extend(['--component',component])
  result=subprocess.run(command,env=env,capture_output=True,text=True)
  if mode=='success':
   if result.returncode:raise RuntimeError(result.stdout+result.stderr)
   for name in ['vibeshine_truehdr.dll','nvngx_truehdr.dll']:
    if (prefix/name).read_bytes()!=b'fixture-runtime':raise RuntimeError('runtime was not installed')
  elif result.returncode==0:raise RuntimeError(f'{mode} runtime incorrectly packaged')
  elif mode=='failure' and 'Failed to stage required TrueHDR runtime' not in result.stderr:raise RuntimeError(result.stderr)
  elif mode=='missing' and 'Required TrueHDR runtime file missing' not in result.stderr:raise RuntimeError(result.stderr)
  elif mode=='empty' and 'Required TrueHDR runtime file is empty' not in result.stderr:raise RuntimeError(result.stderr)
print('production TrueHDR install staging: success, downloader failure, missing, empty all passed')
