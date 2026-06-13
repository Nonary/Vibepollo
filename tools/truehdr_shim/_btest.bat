@echo off
call "D:\Software\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >/dev/null
cd /d D:\sources\sunshine\tools\truehdr_shim\out
cl /nologo /EHsc /MD /std:c++17 /DUNICODE /D_UNICODE "..\truehdr_shim_test.cpp" /Fe:shimtest.exe
