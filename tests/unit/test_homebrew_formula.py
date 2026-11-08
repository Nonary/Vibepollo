"""Render the real Homebrew formula and check the installed host path contract."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
product=re.search(r"project\((\w+)",(root/'CMakeLists.txt').read_text()).group(1)
with tempfile.TemporaryDirectory(prefix='homebrew-formula-') as tmp:
    subprocess.run(['cmake','-S',str(root),'-B',tmp,'-DSUNSHINE_CONFIGURE_ONLY=ON',
        '-DSUNSHINE_CONFIGURE_HOMEBREW=ON','-DBUILD_VERSION=2.0.2-beta.3'],
        check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    formula=(Path(tmp)/'sunshine.rb').read_text()
    assert 'HOST_BINARY = (OS.linux? ? "'+product+'".downcase : "sunshine").freeze' in formula
    assert 'bin/"sunshine"' not in formula
    for consumer in ('bin/HOST_BINARY if OS.mac?', 'run [opt_bin/HOST_BINARY,', 'system bin/HOST_BINARY, "--version"'):
        assert consumer in formula,consumer
    linux=(root/'cmake/targets/linux.cmake').read_text()
    assert re.search(r'OUTPUT_NAME\s+'+product.lower()+r'\)',linux)
    mac=(root/'cmake/targets/macos.cmake').read_text().split('else()',1)[0]
    assert 'SUNSHINE_BUILD_HOMEBREW' in mac and 'OUTPUT_NAME' not in mac
    print('Rendered Homebrew formula uses the actual Linux and macOS host names for signing, service and test paths.')
