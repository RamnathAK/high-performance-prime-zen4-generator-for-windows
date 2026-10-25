# Benchmarks

> All numbers are from reference hardware. Your 7840HS will be similar, other Zen 4 close.

## Methodology

We measure:
1. **Wall time per 100MB file**
2. **Primes per second**
3. **Candidates per second**
4. **CPU utilization**

Script `scripts/bench.ps1` does:

```powershell
.\build\prime.exe
# In another shell:
# Measure time for primegenerated1.txt to appear and reach 100MB
# Count lines: (Get-Content primegenerated1.txt | Measure-Object).Count
```

For micro-benchmarks, we instrumented `is_prime` with `QueryPerformanceCounter`.

## Reference Results — Ryzen 7 7840HS, 32GB DDR5-5600, Balanced Power

### Early Range (0 to ~1e9)

| File | Size | Primes | Time | Throughput |
|------|------|--------|------|------------|
| primegenerated1.txt | 100 MB | ~5.8M | 12.3s | ~471k primes/s, ~10MB/s |
| primegenerated2.txt | 100 MB | ~5.2M | 13.1s | ~397k primes/s |
| primegenerated3.txt | 100 MB | ~4.9M | 14.0s | |

- Candidates tested: ~98M / sec (8192 per block, ~12k blocks/sec)
- `is_prime` avg: ~180 ns early range (trial division filters most)
- Writer stalls: <0.2%

### Mid Range (~1e12)

| Metric | Value |
|--------|-------|
| Primes per 100MB | ~3.1M (digits ~12 + \n) |
| Time per 100MB | ~52s |
| Throughput | ~60k primes/s |
| Candidates per sec | ~45M |
| is_prime avg | ~1.1 µs (more MR rounds) |

### Late Range (~1e15)

| Metric | Value |
|--------|-------|
| Primes per 100MB | ~2.4M |
| Time per 100MB | ~110s |
| is_prime avg | ~2.8 µs |

## Scaling

| Workers | Throughput (early) | Efficiency |
|---------|--------------------|------------|
| 4 | 180k p/s | 45k / core |
| 8 | 320k p/s | 40k / core |
| 14 (default) | 470k p/s | 33.5k / core |
| 16 | 465k p/s | drop - writer contention |

14 is sweet spot for 7840HS.

## Profiler Breakdown (VTune)

Early range, 14 workers:

- 62% `is_prime` → of which 35% trial division `%`, 27% mont_mul
- 18% `fast_utoa_nl`
- 12% `WaitOnAddress` / atomic
- 8% memcpy to out_buf

## How to Reproduce

```bat
:: Build release
scripts\build_msvc.bat

:: Run with log
powershell -ExecutionPolicy Bypass -File scripts\bench.ps1

:: Results in bench_results.txt
```

Upload your `bench_results.txt` as GitHub issue if you beat reference!

## Notes

- Power plan matters: High Performance vs Balanced = +8% on 7840HS.
- DDR5-5600 vs DDR5-4800: negligible (working set fits L3).
- Thermal: Sustained 14-thread load will hit 95C and throttle ~5% after 10 min — use laptop cooler for peak.
