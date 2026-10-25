# Architecture Deep Dive — PrimeStream Zen 4

This document explains *why* every line in `prime_stream_win.c` exists.

## 1. Target Hardware: Ryzen 7 7840HS

- 8 cores / 16 threads, single CCD, Zen 4
- 32KB L1d per core, 32KB L1i, 1MB L2 per core, 16MB shared L3
- Fast `MULX` (unsigned high mul), `ADOX/ADCX` - ideal for Montgomery math
- High `DIV` latency (20-40 cycles) - we avoid it at all costs

Decision: Use 14 workers = 7 full physical cores. Why not 16?
- Writer needs its own core to avoid head-of-line blocking
- OS + interrupts need breathing room
- 7 cores keep us on 7 L2 slices, no cross-core L2 spill

Core pinning:
```c
SetThreadAffinityMask(thread, 1 << worker_id) // workers 0-13
SetThreadAffinityMask(writer, 1 << 14)        // writer on core 14
SetPriorityClass(HIGH_PRIORITY_CLASS)
worker -> THREAD_PRIORITY_TIME_CRITICAL
writer -> THREAD_PRIORITY_ABOVE_NORMAL
```

## 2. Ring Buffer — Lock-Free Ordered Pipeline

```
struct Block {
  volatile LONG64 seq; // sequence state
  uint32_t text_len;
  char text[65520];    // 64KiB - 16 = exactly 64KiB block
} __declspec(align(64))
```

- `align(64)` = cache line size on Zen 4, prevents false sharing between blocks.
- `RING_SIZE = 64` power of 2 → `index = ticket & 63` is single AND, not DIV.
- Must be > NUM_WORKERS (14) so workers never stall on full ring.
- Sequence protocol (classic Disruptor pattern):
  - Init: seq = index (0..63)
  - Worker claims ticket T, spins until `seq == T` (slot free)
  - Worker fills, sets `seq = -T-1` (ready)
  - Writer waits for `seq == -expected-1` in order
  - Writer consumes, sets `seq = expected + RING_SIZE` (free again)

Wait mechanism: `WaitOnAddress` is Windows futex - sleeps in kernel, 0% CPU spin. Writer uses 50ms timeout to check `g_stop`.

## 3. Block Sizing Math

- `BLOCK_SIZE = 30720 = 1024 * 30`
  - Multiple of 30 → wheel alignment constant across blocks!
  - If block size weren't multiple of 30, each ticket would need to recompute wheel_idx from scratch.

- `CANDIDATES_PER_BLOCK = 8192 = 1024 * 8`
  - 8 residues per 30 numbers, 1024 cycles of wheel → 8192 candidates
  - 8192 fits in L1, loop unrolls nicely

Derivation:
```
tickets: 0 -> candidates [7, 7+30720)
ticket 1 -> [30727, 61447) etc.
Because 30720 % 30 == 0, wheel_idx stays same for all tickets!
```

## 4. Wheel-30 Sieve

Residues coprime to 30: 1,7,11,13,17,19,23,29
Steps: 6,4,2,4,2,4,6,2 (difference to next residue, wraps around)

This eliminates all multiples of 2,3,5 upfront — 22/30 numbers never tested.
Implementation: `next_candidate` adds step and `&7` wraps index.

## 5. Primality Test — Two Layers

### Layer A: Trial Division 23..163

Why not 2..163? Wheel already removed 2,3,5. We also skip 7,11,13,17,19 by same logic? Actually code keeps them for safety but starts at 23 for efficiency — most composites divisible by small primes are caught here with single `%` (compiler turns `%` by constant into magic-number mul, no DIV).

### Layer B: Deterministic Miller-Rabin for 64-bit

Theory:
- Write n-1 = d * 2^r
- For base a, compute a^d mod n
- If ==1 or ==n-1, probably prime, else square r-1 times

Deterministic set for <2^64: [2] + [325,9375,28178,450775,9780504,1795265022] — proven by Jim Sinclair. We do base 2 first as fast reject.

Montgomery optimization:
- Normal modular mul needs DIV. Montgomery transforms numbers to `a*R mod n` where `R=2^64`
- Then `mont_mul(aR, bR) = abR mod n` using only MULH, no DIV
- Need `inv = -n^-1 mod R` computed via Newton-Raphson: `inv = (3*n)^2; repeat inv *= 2 - n*inv` (4 iterations for 64 bits)
- `mont_one = R mod n = (0 - n) % n` trick to avoid 128-bit mod
- `mont_two = 2*R mod n`

All hot loops use `mont_mul` which on Zen4 is:
```
MULQ  -> low, high
IMUL  low*inv
MULH  m*n high
SUB   t_hi - h
```

## 6. Fast Integer to String

Naive `sprintf` is ~200 cycles. Our `fast_utoa_nl`:

- LUT of "00" to "99" as 200 chars
- Process val % 100 → 2 digits at once
- Store reversed in temp[20], then reverse copy + '\n'
- No division by 10, division by 100 is cheaper (compiler uses magic)
- Writes directly into block's text buffer — zero extra copy until final memcpy to 1MB out_buf

## 7. Writer & File Rotation

- 1 MiB `g_out_buf` — accumulates many blocks, single fwrite per MiB
- `setvbuf(..., _IONBF, 0)` disables CRT buffering — we own buffering
- `g_current_file_bytes` counter tracks exact file size in memory — no `ftell` syscall per block (which would be 10k syscalls/sec)
- At >=100 MiB: flush, fclose, fopen next file `primegenerated{N}.txt`
- 100 MiB chosen: NTFS allocation unit friendly, fits in Git LFS if needed, easy to grep

## 8. Resume Logic

On startup:
- Scan exe directory for `primegenerated*.txt` via `FindFirstFileA`
- Find max index, read last 256 bytes, parse backwards to last '\n' → last prime
- `align_to_wheel30(last_prime+2)` sets new base_candidate and wheel_idx
- Open file in append mode `"ab"` and seek to end to get `g_current_file_bytes`

Crash safe: if killed mid-block, last incomplete line is ignored (no newline) and regenerated.

## 9. Synchronization Primitives

- `InterlockedIncrement64` for ticket dispenser — lock xadd on Zen4 is ~20 cycles
- `ReadAcquire64` / `InterlockedExchange64` — acquire/release semantics prevent reordering
- `WaitOnAddress` / `WakeByAddressAll` — user-mode futex, Windows 8+

No mutex, no critical section, no SRW lock in hot path.

## 10. What Could Be Faster?

- Use AVX2 to test 4 candidates in parallel for trial division (GCD batch)
- Use shared `g_base_candidate` as atomic and fetch-add BLOCK_SIZE — already done via ticket * BLOCK_SIZE
- Huge pages for ring buffer (2MB pages reduce DTLB misses, but 64*64KiB = 4MiB already fits in L3)

## Diagram

```
Core 0-13: WORKER[i] loop
  ticket = atomic_inc()
  Block* b = ring[ticket & 63] wait seq==ticket
  c = 7 + ticket*30720
  for 8192 steps:
    if is_prime(c): fast_utoa_nl(c, b->text)
    c += wheel_step
  b->text_len = pos
  seq = -ticket-1; wake

Core 14: WRITER loop
  expected=0
  b = ring[expected & 63] wait seq==-expected-1
  out_buf += b->text; bytes+=len
  if bytes>=100MB: rotate file
  seq = expected+64; wake; expected++
```
