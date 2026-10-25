# Contributing

Thanks for wanting to make PrimeStream faster!

## Quick Rules

1. Do not change `src/hp_prime_stream_win.c` logic for benchmarking PRs — keep it as reference. Put optimizations in new file.
2. Run `scripts/bench.ps1` before and after.
3. Use clang-format: 4 spaces, 100 col.

## Ideas

- Linux port (futex)
- ARM64 port (Apple M-series)
- AVX-512 trial division
- Configurable workers via command line
- Binary output

## PR Process

1. Fork, branch `feat/your-idea`
2. Build with both MSVC and clang-cl
3. Include bench_results.txt
4. Open PR with description of Zen 4 counters (use `vtune` or `perf`)

## Code Style

- Keep hot path branch-free where possible
- Comment why, not what
- No malloc in worker loop
