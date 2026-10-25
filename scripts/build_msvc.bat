@echo off
echo Building with MSVC for Zen 4...
if not exist build mkdir build
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" 2>nul || call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /O2 /arch:AVX2 /GL /DNDEBUG /W3 src\hp_prime_stream_win.c /Fe:build\prime.exe /link synchronization.lib /LTCG
echo Build complete: build\prime.exe
