# Limitations

## 64-bit ceiling
Max n = 2^64-1. Deterministic MR set only valid <2^64. Max prime 18446744073709551557.

## Overflow behavior
Unsigned wrap -> duplicates, not crash. After reaching max, g_base_candidate + ticket*BLOCK_SIZE wraps to small number (e.g., 35), re-emits early primes, output becomes unsorted. Writer order still holds per ticket but values are small. Resume will then pick small last prime.

Fix: check before ticket dispatch:
if (g_base_candidate > UINT64_MAX - (uint64_t)1<<40) stop.

## Other
- Windows only
- 14 threads fixed
- 100MB chunk fixed
- File index int overflow at 214PB
- No verification checksum
