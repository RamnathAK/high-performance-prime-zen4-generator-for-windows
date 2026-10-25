/*
 * hp_prime_stream_win.c - High-Performance Lock-Free Zen 4 Prime Stream Generator
 *
 * Target: AMD Ryzen 7 7840HS (Zen 4, 8C/16T single-CCD) - 14 workers + 1 writer
 * Author: RamnathAK - Optimized for Windows x64 (MSVC / clang-cl)
 *
 * ------------------------------------------------------------------
 * ARCHITECTURE OVERVIEW:
 * 1. Lock-free ring buffer (64 slots x 64 KiB) connects 14 workers -> 1 writer
 * 2. Each worker acquires a ticket (block index) via cache-isolated atomic increment
 * 3. Block = 30,720 integers = 8,192 mod-30 wheel candidates (skips multiples of 2,3,5)
 * 4. Primality: Trial division (primes 7..163) + deterministic 64-bit Miller-Rabin
 * 5. Montgomery multiplication (R = 2^64) replaces division with MULX/MULH
 * 6. fast_utoa_nl formats primes using a 2-digit LUT (zero sprintf/CRT overhead)
 * 7. Writer streams tickets sequentially to 100 MiB chunked files with auto-resume
 * ------------------------------------------------------------------
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <synchapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

// ------------------------------------------------------------------
// PLATFORM ABSTRACTION: 128-bit unsigned arithmetic
// Zen 4 executes MULX/MULQ at 2/cycle. MSVC provides _umulh/_umul128.
// For clang-cl on Windows, inline raw MULQ/DIVQ to avoid compiler-rt calls.
// ------------------------------------------------------------------
#if defined(__clang__) && defined(_WIN64)
static inline uint64_t my_umulh(uint64_t a, uint64_t b) {
    uint64_t low, high;
    __asm__("mulq %3" : "=a"(low), "=d"(high) : "%a"(a), "r"(b));
    return high;
}
static inline uint64_t my_udiv128_rem(uint64_t high, uint64_t low, uint64_t divisor) {
    uint64_t q, rem;
    __asm__("divq %4" : "=a"(q), "=d"(rem) : "d"(high), "a"(low), "r"(divisor));
    return rem;
}
static inline uint64_t my_umul128(uint64_t a, uint64_t b, uint64_t* high) {
    uint64_t low, hi;
    __asm__("mulq %3" : "=a"(low), "=d"(hi) : "%a"(a), "r"(b));
    *high = hi;
    return low;
}
#elif defined(_MSC_VER)
#define my_umulh _umulh
#define my_umul128 _umul128
static inline uint64_t my_udiv128_rem(uint64_t high, uint64_t low, uint64_t divisor) {
    uint64_t rem;
    _udiv128(high, low, divisor, &rem);
    return rem;
}
#endif

#pragma comment(lib, "synchronization.lib")

// --- Pipeline & Topology Configuration ---
// Ryzen 7 7840HS has 8 physical cores / 16 logical processors (SMT) on a single CCD.
// Logical processors 0..13 (Physical Cores 0..6) run worker threads.
// Logical processor 14 (Physical Core 7, Thread 0) runs the sequential I/O writer.
// Logical processor 15 (Physical Core 7, Thread 1) is left free for OS interrupts.
#define NUM_WORKERS 14
#define RING_SIZE 64                     // Power of 2 for bitmask indexing
#define CANDIDATES_PER_BLOCK 8192        // 1024 * 8 wheel steps per block
#define BLOCK_SIZE 30720ULL              // 1024 * 30 (exact mod-30 multiple keeps wheel aligned)
#define BLOCK_TEXT_CAP 65520             // 64 KiB minus 16-byte header -> exact 64 KiB struct
#define OUT_BUF_SIZE (1024 * 1024)       // 1 MiB user-space write buffer
#define MAX_FILE_SIZE_BYTES (100ULL * 1024 * 1024) // 100 MiB chunk size

// 64 KiB cache-aligned Block:
// seq == ticket             : slot is free for worker with `ticket` to populate
// seq == -ticket - 1        : slot is populated and ready for writer to consume
// seq == ticket + RING_SIZE : slot has been flushed by writer and recycled
typedef __declspec(align(64)) struct {
    volatile LONG64 seq;
    uint32_t        text_len;
    uint32_t        pad;
    char            text[BLOCK_TEXT_CAP];
} Block;

static Block g_blocks[RING_SIZE];

// Isolate hot atomic state and read-only globals on distinct 64-byte cache lines
// to eliminate L1D coherence invalidations (false sharing) across cores.
static __declspec(align(64)) volatile LONG64 g_next_ticket = 0;
static __declspec(align(64)) volatile LONG   g_stop = 0;
static __declspec(align(64)) uint64_t        g_base_candidate = 7;
static int                                   g_base_wheel_idx = 1;

// --- Math & Primality (Montgomery Multiplication + Deterministic Miller-Rabin) ---

/* mulmod64: Computes (a * b) % m using 128-bit intermediate product.
 * Used once per prime test to compute R^2 mod n for Montgomery domain entry.
 */
static inline uint64_t mulmod64(uint64_t a, uint64_t b, uint64_t m) {
#if defined(_MSC_VER)
    uint64_t high, low;
    low = my_umul128(a, b, &high);
    if (high == 0) return low % m;
    return my_udiv128_rem(high, low, m);
#else
    return (uint64_t)(((__uint128_t)a * b) % m);
#endif
}

/* mont_mul: Montgomery REDC multiplication: (a * b * R^-1) mod n, with R = 2^64.
 * Replaces 64-bit hardware division with two 64x64->128 multiplications.
 * Requires precomputed inv = -n^-1 mod 2^64.
 */
static inline uint64_t mont_mul(uint64_t a, uint64_t b, uint64_t n, uint64_t inv) {
#if defined(_MSC_VER)
    uint64_t t_hi, t_lo, h;
    t_lo = my_umul128(a, b, &t_hi);
    uint64_t m = t_lo * inv;
    h = my_umulh(m, n);
#else
    __uint128_t t = (__uint128_t)a * b;
    uint64_t t_hi = (uint64_t)(t >> 64);
    uint64_t m = (uint64_t)t * inv;
    uint64_t h = (uint64_t)(((__uint128_t)m * n) >> 64);
#endif
    uint64_t res = t_hi - h;
    return (t_hi < h) ? (res + n) : res; // Branchless CMOV on x86-64
}

/* mont_pow: Binary modular exponentiation in Montgomery domain */
static inline uint64_t mont_pow(uint64_t base, uint64_t exp, uint64_t n,
                                uint64_t inv, uint64_t mont_one) {
    uint64_t result = mont_one;
    while (exp) {
        if (exp & 1) result = mont_mul(result, base, n, inv);
        base = mont_mul(base, base, n, inv);
        exp >>= 1;
    }
    return result;
}

/* is_prime: Deterministic Miller-Rabin primality test for all 64-bit integers.
 * 1. Small prime lookup for n <= 163
 * 2. Fast trial division by primes 7..163 (2, 3, 5 already excluded by mod-30 wheel)
 * 3. Montgomery base-2 strong pseudoprime quick reject
 * 4. Jim Sinclair's 6 remaining bases {325, 9375, 28178, 450775, 9780504, 1795265022},
 *    proven deterministic for all n < 2^64.
 */
static int is_prime(uint64_t n) {
    if (n < 2) return 0;

    if (n <= 163) {
        static const uint8_t p_lut[38] = {
            2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
            59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113,
            127, 131, 137, 139, 149, 151, 157, 163
        };
        for (int i = 0; i < 38; ++i) {
            if (n == p_lut[i]) return 1;
        }
        return 0;
    }

    // Trial division for primes 7..163 (compiler lowers constant modulo to magic-number MUL)
    if (n %  7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0 || n % 19 == 0 ||
        n % 23 == 0 || n % 29 == 0 || n % 31 == 0 || n % 37 == 0 ||
        n % 41 == 0 || n % 43 == 0 || n % 47 == 0 || n % 53 == 0 ||
        n % 59 == 0 || n % 61 == 0 || n % 67 == 0 || n % 71 == 0 ||
        n % 73 == 0 || n % 79 == 0 || n % 83 == 0 || n % 89 == 0 ||
        n % 97 == 0 || n % 101 == 0 || n % 103 == 0 || n % 107 == 0 ||
        n % 109 == 0 || n % 113 == 0 || n % 127 == 0 || n % 131 == 0 ||
        n % 137 == 0 || n % 139 == 0 || n % 149 == 0 || n % 151 == 0 ||
        n % 157 == 0 || n % 163 == 0) {
        return 0;
    }

    uint64_t d = n - 1;
    int r = 0;
    while ((d & 1) == 0) { d >>= 1; ++r; }

    // Compute modular inverse inv = -n^-1 mod 2^64 via quadratic Newton-Raphson
    uint64_t inv = (3 * n) ^ 2;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;

    uint64_t mont_one = (0ULL - n) % n;       // R mod n (1 in Montgomery form)
    uint64_t mont_minus_one = n - mont_one;   // n - 1 in Montgomery form
    uint64_t mont_two = (mont_one >= mont_minus_one)
        ? (mont_one - mont_minus_one)
        : (mont_one + mont_one);              // 2 in Montgomery form

    // Base-2 test eliminates >99.9% of surviving composites
    uint64_t x = mont_pow(mont_two, d, n, inv, mont_one);
    if (x != mont_one && x != mont_minus_one) {
        int composite = 1;
        for (int j = 0; j < r - 1; ++j) {
            x = mont_mul(x, x, n, inv);
            if (x == mont_minus_one) { composite = 0; break; }
        }
        if (composite) return 0;
    }

    // Convert remaining 6 Sinclair bases into Montgomery domain via R^2 mod n
    uint64_t r2 = mulmod64(mont_one, mont_one, n);
    static const uint64_t bases[6] = {
        325, 9375, 28178, 450775, 9780504, 1795265022
    };

    for (int i = 0; i < 6; ++i) {
        // Avoid hardware DIV instruction whenever n > bases[i]
        uint64_t a = (bases[i] < n) ? bases[i] : (bases[i] % n);
        if (a == 0) continue;

        uint64_t mont_a = mont_mul(a, r2, n, inv);
        x = mont_pow(mont_a, d, n, inv, mont_one);
        if (x == mont_one || x != mont_minus_one && r == 1) {
            if (x == mont_one) continue;
        }
        if (x == mont_minus_one) continue;

        int composite = 1;
        for (int j = 0; j < r - 1; ++j) {
            x = mont_mul(x, x, n, inv);
            if (x == mont_minus_one) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

// --- Mod-30 Wheel Factorization ---
// Coprime residues mod 30: {1, 7, 11, 13, 17, 19, 23, 29} (8 candidates per 30 integers).
// Skips 73.3% of integers (all multiples of 2, 3, and 5) in O(1) register steps.
static const uint8_t wheel_rem[8]  = { 1, 7, 11, 13, 17, 19, 23, 29 };
static const uint8_t wheel_step[8] = { 6, 4,  2,  4,  2,  4,  6,  2 };

static inline uint64_t align_to_wheel30(uint64_t n, int* wheel_idx) {
    uint64_t base = (n / 30) * 30;
    uint64_t rem  = n % 30;
    for (int i = 0; i < 8; ++i) {
        if (wheel_rem[i] >= rem) {
            *wheel_idx = i;
            return base + wheel_rem[i];
        }
    }
    *wheel_idx = 0;
    return base + 31;
}

static inline uint64_t next_candidate(uint64_t n, int* wheel_idx) {
    uint64_t r = n + wheel_step[*wheel_idx];
    *wheel_idx = (*wheel_idx + 1) & 7;
    return r;
}

// --- Fast 2-Digit Lookup Table Integer-to-ASCII ---
static const char g_digits_lut[200] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

static inline size_t fast_utoa_nl(uint64_t val, char* out) {
    char temp[20];
    int len = 0;
    while (val >= 100) {
        uint64_t idx = (val % 100) * 2;
        val /= 100;
        temp[len++] = g_digits_lut[idx + 1];
        temp[len++] = g_digits_lut[idx];
    }
    if (val < 10) {
        temp[len++] = (char)('0' + val);
    } else {
        uint64_t idx = val * 2;
        temp[len++] = g_digits_lut[idx + 1];
        temp[len++] = g_digits_lut[idx];
    }

    size_t out_len = (size_t)len;
    while (len > 0) {
        *out++ = temp[--len];
    }
    *out = '\n';
    return out_len + 1;
}

// --- Sequential I/O Writer Thread ---
static FILE*    g_file = NULL;
static char     g_out_buf[OUT_BUF_SIZE];
static size_t   g_out_used = 0;
static uint64_t g_current_file_bytes = 0;

static char     g_dir_path[MAX_PATH];
static int      g_file_index = 1;
static char     g_current_path[MAX_PATH];

static void flush_output(void) {
    if (g_out_used > 0 && g_file) {
        fwrite(g_out_buf, 1, g_out_used, g_file);
        g_out_used = 0;
    }
}

static void close_file(void) {
    if (g_file) {
        flush_output();
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
}

/* writer_proc: Single consumer pinned to Logical Processor 14 (Core 7, Thread 0).
 * Drains ring slots strictly in ticket order (`expected++`) so output files are
 * guaranteed sorted despite out-of-order worker completion.
 */
static DWORD WINAPI writer_proc(LPVOID param) {
    (void)param;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 14);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    LONG64 expected = 0;
    for (;;) {
        if (ReadAcquire(&g_stop)) {
            LONG64 max_ticket = InterlockedCompareExchange64(&g_next_ticket, 0, 0);
            if (expected >= max_ticket) break;
        }

        Block* b = &g_blocks[expected & (RING_SIZE - 1)];
        LONG64 wait_seq = -expected - 1;
        LONG64 current;

        while ((current = ReadAcquire64(&b->seq)) != wait_seq) {
            WaitOnAddress((PVOID)&b->seq, &current, sizeof(LONG64), 50);
            if (ReadAcquire(&g_stop)) {
                LONG64 max_ticket = InterlockedCompareExchange64(&g_next_ticket, 0, 0);
                if (expected >= max_ticket) goto done;
            }
        }

        if (g_out_used + b->text_len > sizeof(g_out_buf)) {
            flush_output();
        }
        memcpy(g_out_buf + g_out_used, b->text, b->text_len);

        g_out_used += b->text_len;
        g_current_file_bytes += b->text_len;

        if (g_current_file_bytes >= MAX_FILE_SIZE_BYTES) {
            flush_output();
            fclose(g_file);

            g_file_index++;
            snprintf(g_current_path, MAX_PATH, "%sprimegenerated%d.txt", g_dir_path, g_file_index);

            g_file = fopen(g_current_path, "wb");
            if (!g_file) {
                fprintf(stderr, "Failed to open new chunk file: %s\n", g_current_path);
                InterlockedExchange(&g_stop, 1);
                break;
            }
            setvbuf(g_file, NULL, _IONBF, 0);
            g_current_file_bytes = 0;
        }

        InterlockedExchange64(&b->seq, expected + RING_SIZE);
        WakeByAddressAll((PVOID)&b->seq);
        expected++;
    }

done:
    close_file();
    // Wake any workers waiting on ring slots during shutdown
    for (int i = 0; i < RING_SIZE; ++i) {
        WakeByAddressAll((PVOID)&g_blocks[i].seq);
    }
    return 0;
}

// --- Worker Threads ---
/* worker_proc: 14 workers pinned to Logical Processors 0..13 (Physical Cores 0..6). */
static DWORD WINAPI worker_proc(LPVOID param) {
    int worker_id = (int)(intptr_t)param;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << worker_id);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    for (;;) {
        if (ReadAcquire(&g_stop)) break;

        LONG64 ticket = InterlockedIncrement64(&g_next_ticket) - 1;
        uint64_t offset = (uint64_t)ticket * BLOCK_SIZE;

        // Guard against 64-bit unsigned integer wrap-around near UINT64_MAX
        if (UINT64_MAX - g_base_candidate <= offset + BLOCK_SIZE) {
            InterlockedExchange(&g_stop, 1);
            break;
        }

        Block* b = &g_blocks[ticket & (RING_SIZE - 1)];
        LONG64 current_seq;

        while ((current_seq = ReadAcquire64(&b->seq)) != ticket) {
            if (ReadAcquire(&g_stop)) return 0;
            WaitOnAddress((PVOID)&b->seq, &current_seq, sizeof(LONG64), 50);
        }

        // Because BLOCK_SIZE (30720) is an exact multiple of 30, every block starts
        // at the exact same mod-30 wheel index as g_base_candidate.
        uint64_t c = g_base_candidate + offset;
        int wheel_idx = g_base_wheel_idx;

        size_t pos = 0;
        for (int i = 0; i < CANDIDATES_PER_BLOCK; ++i) {
            if (is_prime(c)) {
                pos += fast_utoa_nl(c, b->text + pos);
            }
            c = next_candidate(c, &wheel_idx);
        }
        b->text_len = (uint32_t)pos;

        InterlockedExchange64(&b->seq, -ticket - 1);
        WakeByAddressAll((PVOID)&b->seq);
    }
    return 0;
}

// --- Graceful Shutdown & Auto-Resume ---
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

/* read_last_prime: Reads the tail 256 bytes of the latest file to find the last committed prime */
static uint64_t read_last_prime(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    _fseeki64(f, 0, SEEK_END);
    __int64 size = _ftelli64(f);
    if (size <= 0) {
        fclose(f);
        return 0;
    }

    __int64 read_size = size < 256 ? size : 256;
    _fseeki64(f, -read_size, SEEK_END);

    char buf[257];
    size_t bytes_read = fread(buf, 1, (size_t)read_size, f);
    fclose(f);

    buf[bytes_read] = '\0';

    for (__int64 i = (__int64)bytes_read - 2; i >= 0; --i) {
        if (buf[i] == '\n') {
            return strtoull(&buf[i + 1], NULL, 10);
        }
    }
    return strtoull(buf, NULL, 10);
}

int main(void) {
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // 1. Resolve executable directory for output chunk files
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcpy(g_dir_path, path);

    // 2. Scan for existing primegenerated*.txt files to support seamless resume
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%sprimegenerated*.txt", g_dir_path);

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(search_path, &findData);
    int max_index = 0;
    uint64_t last_prime = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            int current_idx = 0;
            if (sscanf(findData.cFileName, "primegenerated%d.txt", &current_idx) == 1) {
                if (current_idx > max_index) max_index = current_idx;
            }
        } while (FindNextFileA(hFind, &findData) != 0);
        FindClose(hFind);

        if (max_index > 0) {
            g_file_index = max_index;
            snprintf(g_current_path, MAX_PATH, "%sprimegenerated%d.txt", g_dir_path, g_file_index);
            last_prime = read_last_prime(g_current_path);
        }
    }

    // 3. Initialize wheel state for either resume or fresh start
    if (last_prime >= 5) {
        printf("Resuming after prime %llu in file %s\n",
               (unsigned long long)last_prime, g_current_path);
        g_file = fopen(g_current_path, "ab");
        if (g_file) {
            _fseeki64(g_file, 0, SEEK_END);
            g_current_file_bytes = (uint64_t)_ftelli64(g_file);
        }
        uint64_t start_n = (last_prime < 7) ? 7ULL : (last_prime + 2ULL);
        g_base_candidate = align_to_wheel30(start_n, &g_base_wheel_idx);
    } else {
        g_base_candidate = align_to_wheel30(7ULL, &g_base_wheel_idx);
        g_file_index = 1;
        snprintf(g_current_path, MAX_PATH, "%sprimegenerated%d.txt", g_dir_path, g_file_index);
        g_file = fopen(g_current_path, "wb");
        if (g_file) {
            fprintf(g_file, "2\n3\n5\n");
            fflush(g_file);
            g_current_file_bytes = 6;
        }
    }

    if (!g_file) {
        fprintf(stderr, "Error opening output file: %s\n", g_current_path);
        return 1;
    }

    setvbuf(g_file, NULL, _IONBF, 0);

    for (int i = 0; i < RING_SIZE; ++i) {
        g_blocks[i].seq = i;
    }

    HANDLE workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i) {
        workers[i] = CreateThread(NULL, 0, worker_proc, (LPVOID)(intptr_t)i, 0, NULL);
    }

    HANDLE writer = CreateThread(NULL, 0, writer_proc, NULL, 0, NULL);

    printf("Generating primes across 14 worker threads. Press Ctrl+C to flush and stop...\n");

    WaitForMultipleObjects(NUM_WORKERS, workers, TRUE, INFINITE);
    WaitForSingleObject(writer, INFINITE);

    for (int i = 0; i < NUM_WORKERS; ++i) CloseHandle(workers[i]);
    CloseHandle(writer);

    printf("Flushed and stopped safely.\n");
    return 0;
}
