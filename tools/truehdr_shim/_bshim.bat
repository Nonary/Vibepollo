@echo off
call "D:\Software\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >/dev/null
cd /d D:\sources\sunshine\tools\truehdr_shim\out
cl /nologo /EHsc /MD /O2 /std:c++17 /LD "..\truehdr_shim.cpp" /I "D:\sources\RTX_Video_SDK\include" /Fe:vibeshine_truehdr.dll /link "D:\sources\RTX_Video_SDK\lib\Windows\x64\nvsdk_ngx_d.lib" advapi32.lib user32.lib
