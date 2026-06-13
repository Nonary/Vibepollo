@echo off
call "D:\Software\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >/dev/null
dumpbin /exports D:\sources\sunshine\tools\truehdr_shim\out\vibeshine_truehdr.dll
