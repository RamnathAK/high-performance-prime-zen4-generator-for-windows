@echo off
echo Building with clang-cl for znver4...
if not exist build mkdir build
clang-cl /O3 /march=znver4 /DNDEBUG src\hp_prime_stream_win.c /Fe:build\prime.exe /link synchronization.lib
echo Build complete: build\prime.exe
