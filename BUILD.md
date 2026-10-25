# Build Instructions

## Requirements

- Windows 10/11 x64
- Ryzen 7 7840HS (or any Zen 4 / x64) — code is generic x64 but tuned for Zen 4
- Visual Studio 2022 Build Tools (MSVC) **or** LLVM clang-cl 17+

## Option 1: MSVC (Recommended)

```bat
:: From Developer Command Prompt for VS 2022
cl /O2 /arch:AVX2 /GL /DNDEBUG /W3 src\hp_prime_stream_win.c /Fe:build\prime.exe /link synchronization.lib /LTCG

:: Or use provided script
scripts\build_msvc.bat
```

Flags explained:
- `/O2` - maximize speed
- `/arch:AVX2` - allow AVX2 (Zen4 has AVX-512 but AVX2 is safe baseline)
- `/GL /LTCG` - whole program optimization, inlines mont_mul
- `synchronization.lib` - for WaitOnAddress/WakeByAddress

## Option 2: clang-cl

```bat
clang-cl /O3 /march=znver4 /DNDEBUG src\hp_prime_stream_win.c /Fe:build\prime.exe -fuse-ld=lld -lsynchronization

:: Or
scripts\build_clang.bat
```

Why clang-cl? It understands `__asm__` MULQ wrappers and can auto-vectorize trial division better. On Zen4, clang often ~5% faster.

## Option 3: CMake

```bat
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Binary will be at `build\Release\prime.exe` or `build\prime.exe`.

## Running

```bat
.\build\prime.exe
:: Generates primegenerated1.txt, primegenerated2.txt ... next to exe
:: Press Ctrl+C - it flushes and exits cleanly
:: Run again - it resumes from last prime
```

## Verifying Output

```powershell
# PowerShell - check first file
Get-Content primegenerated1.txt | Select-Object -First 20
# Should be 2,3,5,7,11...

# Verify count vs expected
python -c "import sympy; print(list(sympy.primerange(0,100)))"
```

## Troubleshooting

- `Failed to open new file` → run as admin if exe is in Program Files, or move to writable folder.
- Low throughput → check affinity: Task Manager > Details > prime.exe > Set Affinity should show 15 cores. Disable Game Mode.
- clang error `undefined symbol __umodti3` → ensure you use clang-cl not clang (clang targets GNU ABI).

## Cross-Platform Note

Current code is Windows-only due to `WaitOnAddress` and `SetThreadAffinityMask`. Linux port would replace with `futex` + `pthread_setaffinity_np`. See Roadmap in README.
