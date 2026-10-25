# Limitations — `hp_prime_stream_win.c`  

Scope: `hp_prime_stream_win.c` (14-worker / 1-writer lock-free ring buffer, 64 KiB blocks, mod-30 wheel, Montgomery + deterministic Miller–Rabin). 

## 64-bit ceiling  
- Max n = 2^64−1. Deterministic Miller–Rabin bases `{2, **325**, **9375**, **28178**, **450775**, **9780504**, **1795265022**}` are proven deterministic only for n < 2^64. Beyond that requires wider arithmetic, additional bases, or **BPSW**.  
- Max representable prime: `18,**446**,**744**,**073**,**709**,**551**,**557**`.  
- `mont_mul` is a subtractive **REDC** with `inv = n⁻¹ mod 2^64` and `res = t_hi − h; if (t_hi < h) res += n`. Correct for `a, b < n`; the `a, b < n` invariant is load-bearing and must survive any edit.  

## Overflow behavior — guard is present but flawed  
The worker guard is:  

```c  
**LONG64** ticket = InterlockedIncrement64(&g_next_ticket) - 1;  
uint64_t offset = (uint64_t)ticket * BLOCK_SIZE;  
if (UINT64_MAX - g_base_candidate <= offset + BLOCK_SIZE) {  
    InterlockedExchange(&g_stop, 1);  
    break;  
}  
```  

Problems:  
- `ticket * BLOCK_SIZE` can wrap **before** the comparison; the guard then runs against a wrapped small offset and does not fire.  
- `offset + BLOCK_SIZE` can itself overflow, defeating the comparison.  
- When the guard does fire, the ticket has already been consumed from `g_next_ticket`, and the worker returns **without publishing the slot**. The writer's stop condition (`expected >= max_ticket` → break) then never becomes true; the writer spins in 50 ms `WaitOnAddress` waits and never exits.  
- If wrap occurs without the guard firing, small primes (e.g., 35, 37, …) are re-emitted; per-ticket writer order still holds, so the file becomes globally unsorted and resume picks up a small last prime.  

Suggested fix: test `ticket > (UINT64_MAX − g_base_candidate) / BLOCK_SIZE` **before** the multiply, and publish an explicit terminal sentinel (or an empty block) for the offending ticket instead of abandoning it.  

## Shutdown can deadlock  
- A worker checks `g_stop` at the loop top, then increments `g_next_ticket`. If Ctrl+C lands in that window and the ring is congested, the worker enters the slot-wait loop, sees `g_stop`, and `return 0`s **without publishing its ticket**.  
- The writer's stop path (`expected >= max_ticket` → break) never becomes true. Writer hangs.  
- No dead-worker detection, no terminal sentinel. Shutdown is safe only if every dispatched ticket is eventually published.  
- Ctrl+C handler covers `CTRL_C_EVENT` and `CTRL_BREAK_EVENT` only; `CTRL_CLOSE_EVENT`, logoff, and shutdown are unhandled.  

## Platform & ISA coupling (Windows x64 only)  
- Synchronization: `WaitOnAddress` / `WakeByAddressAll` via `synchronization.lib` (Windows 8+); pinning via `SetThreadAffinityMask`. Linux port needs `futex(2)` and `pthread_setaffinity_np`.  
- **128**-bit multiply: `_umul128` / `_umulh` / `_udiv128` on **MSVC**, or inline `mulq` / `divq` on clang-cl. The `__uint128_t` fallback in `mulmod64` / `mont_mul` exists, but the rest of the file still requires Windows APIs.  
- No portable build; no Linux / macOS target.  

## Static hardware topology & pipeline sizing  
- Compile-time constants: `NUM_WORKERS 14`, writer on **LP14**, **LP15** left to the OS. Tuned for a single-**CCD** 8C/**16T** part (**7840HS**). Workers occupy both **SMT** threads of physical cores 0–6; the writer sits alone on core 7's first thread.  
- `SetThreadAffinityMask` / `SetPriorityClass` return values are ignored. On systems with < 15 logical processors, `1 << 14` is invalid and the writer runs unpinned; workers with `worker_id` ≥ available LPs likewise unpinned.  
- Dual-**CCD** Ryzen: cores above **LP13** sit idle. Intel hybrid (P/E): **LP0**–13 may straddle core types with different ISAs and throughput. No `GetLogicalProcessorInformationEx` discovery.  
- No **NUMA** awareness (irrelevant on **7840HS**; relevant on larger SKUs).  
- Compile-time chunk and buffer parameters: `MAX_FILE_SIZE_BYTES = **100** MiB`, `OUT_BUF_SIZE = 1 MiB`, `RING_SIZE = 64`, `BLOCK_SIZE = **30720**`, `BLOCK_TEXT_CAP = **65520**`. Not runtime-configurable.  

## Thread priority, power, thermal  
- `HIGH_PRIORITY_CLASS` + writer `THREAD_PRIORITY_HIGHEST` + 14 busy workers can starve foreground apps and OS work.  
- On a **7840HS** (35–54 W envelope), sustained 14-thread load tends to hit package power/thermal limits and reduce boost clocks; net throughput can fall. No throttling, yield, or temperature awareness. Priority is not adjustable without a rebuild.  

## Storage, file indexing, integrity  
- File index is `int`; `INT_MAX` files × **100** MiB ≈ **214**.7 PB before overflow (filenames collide/truncate beyond that).  
- Chunks may exceed **100** MiB by up to one block (~≤20 KB), since the size check runs after the block is appended.  
- Disk consumption uncapped. The *~**122**.8 MB/s* figure is an **estimate**, not a measurement; actual throughput depends on hardware, thermal state, and file system. When the volume fills, `fopen` on the next chunk fails, the writer sets `g_stop` and breaks — but this shares the deadlock surface in *Shutdown can deadlock*.  
- `fwrite`, `fflush`, and `fclose` return values are ignored in `flush_output` / `close_file`; a mid-chunk disk-full silently truncates the final write.  
- No checksum, **CRC**, length check, or write verification across chunks. A truncated or bit-rotted file is not detected.  
- Tail-based resume (`read_last_prime`, last **256** bytes of the highest-indexed `primegenerated*.txt`):  
    - A file ending mid-line (crash during `fwrite`) yields a partial number, accepted as the last prime.  
    - The parsed value is not validated (prime check, digit count, trailing `\n`).  
    - If the tail contains no `\n`, `strtoull` runs on the whole **256**-byte buffer.  
    - **Destructive empty-file case:** if the highest-indexed file is empty/unreadable, `read_last_prime` returns 0 and the `else` branch reopens `primegenerated1.txt` with `*wb*`, truncating it and restarting from 2/3/5 while later chunks remain on disk. No guard.  
    - Near `UINT64_MAX`, resume computes `last_prime + 2`, which can wrap to a small value; `align_to_wheel30` then yields a small candidate and duplicates of early primes are appended.  

## Error handling / robustness  
- No startup self-test against known primes (e.g., near 2^64) or cross-check of the MR bases.  
- No runtime check that output is strictly increasing. Ordering is enforced only by the writer's ticket sequence, so wrap or an abandoned ticket manifests as duplicates or a hang — not a detection.  
- No detection that a worker crashed or was killed mid-block; the writer waits indefinitely for that ticket.  
- `Block.text` has no bounds check. Worst case at current constants is ~20 KB (max ~3.3k primes in 30,**720** numbers × 6 bytes), well under 64 KiB — safe as configured, unsafe if `BLOCK_SIZE`, `CANDIDATES_PER_BLOCK`, or the wheel change.  
- `g_base_wheel_idx` is initialized to 1 (matches `align_to_wheel30(7) = 7, idx 1`). Per-block wheel continuity in `worker_proc` assumes `BLOCK_SIZE` is an exact multiple of 30 and this invariant holds.  
- `is_prime`'s 6-base loop uses a nonstandard `r == 1` shortcut. Logically equivalent to `x == 1 || x == n−1`, but fragile to edit.  
- The 6-base loop uses `(bases[i] < n) ? bases[i] : (bases[i] % n)`, correctly avoiding a hardware divide for large n; for **163** < n < ~1.8e9 the divide path still runs (those n are handled earlier by trial division, so in practice the divide path is hit only for a fraction of blocks).  

## Performance notes (not bugs)  
- Writer `**HIGHEST**` priority is intentional, to keep pace with 14 producers. Trades system responsiveness for throughput; tuned for a single-**CCD** 8C/**16T** **7840HS**, not for multi-**CCD** or hybrid CPUs.  
- 64 KiB blocks and a 1 MiB user-space write buffer are chosen for L2/**DTLB** behavior on Zen 4; not necessarily optimal on other SKUs (e.g., **7945HX** with more cores/CCDs, Intel hybrid parts).  

## Other  
- Windows only.  
- 14 workers fixed.  
- **100** MB chunk fixed.  
- File index `int` overflow at ~**214** PB.  
- No verification checksum.  
