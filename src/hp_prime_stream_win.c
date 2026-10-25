/*
 * hp_prime_stream_win.c - The Ultimate High Performance Zen 4 Prime Generator
 * 
 * Target: AMD Ryzen 7 7840HS (Zen 4, 8C/16T) - Uses 14 workers + 1 writer
 * Author: RamnathAK - Optimized for Windows x64
 * 
 * ------------------------------------------------------------------
 * HIGH-LEVEL OVERVIEW:
 * 1. Lock-free ring buffer (64 slots) connects 14 workers -> 1 writer
 * 2. Each worker gets a ticket (block index) via atomic increment
 * 3. Block = 30720 numbers = 8192 wheel-30 candidates (filters multiples of 2,3,5)
 * 4. Primality: Trial division by primes <=163 + deterministic Miller-Rabin for 64-bit
 * 5. Montgomery multiplication avoids expensive DIV instructions
 * 6. fast_utoa_nl formats primes using 2-digit LUT (no sprintf)
 * 7. Writer streams blocks in order to 100MB chunked files, supports resume from last generated prime
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
// PLATFORM ABSTRACTION: 128-bit math
// Zen4 has fast MULX/ADOX. MSVC provides _umulh/_umul128. clang-cl doesn't,
// so we inline raw MULQ/DIVQ to avoid compiler-rt __umodti3 calls.
// ------------------------------------------------------------------
#if defined(__clang__) && defined(_WIN64)
static inline uint64_t my_umulh(uint64_t a, uint64_t b) {
    uint64_t low, high;
    __asm__("mulq %3" : "=a"(low), "=d"(high) : "%a"(a), "r"(b));
    return high; // Returns high 64 bits of 128-bit product
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

// --- Configuration ---
// NUM_WORKERS = 14 : Ryzen 7 7840HS has 8 cores / 16 threads. We leave 1 core (2 threads) free
// for writer + OS. Using 7 physical cores avoids cross-CCD latency (7840HS is single CCD).
#define NUM_WORKERS 14
#define RING_SIZE 64                     // Power of 2 for fast & masking, > NUM_WORKERS to hide latency
#define CANDIDATES_PER_BLOCK 8192        // 1024 * 8 wheel steps -> keeps L1i hot
#define BLOCK_SIZE 30720                 // 1024 * 30 (exact mod-30 multiple) -> ensures wheel alignment across tickets
#define BLOCK_TEXT_CAP 65520             // 64 KiB minus 16-byte header -> fits exactly in 64KiB, TLB friendly
#define OUT_BUF_SIZE (1024 * 1024)       // 1 MiB output buffer -> amortizes syscall cost

// 64 KiB aligned Block: Cache-friendly, no DTLB spills
// Each block is 64-byte aligned to avoid false sharing. seq is the lock-free state machine:
// seq == ticket        : block is free for worker to write
// seq == -ticket-1     : block is ready for writer to consume
// seq == ticket+RING   : block returned to free pool
typedef __declspec(align(64)) struct {
    volatile LONG64 seq;                // Sequence counter for WaitOnAddress/WakeByAddress
    uint32_t        text_len;           // Valid bytes in text[]
    uint32_t        pad;
    char            text[BLOCK_TEXT_CAP];
} Block;

static Block g_blocks[RING_SIZE];
static volatile LONG64 g_next_ticket = 0; // Atomic ticket dispenser - each block gets unique index
static volatile LONG   g_stop = 0;
static uint64_t        g_base_candidate = 7;
static int             g_base_wheel_idx = 0;

// --- Math & Primality (Montgomery + Magic-Number Trial Division) ---

/* mulmod64: (a*b) % m using 128-bit intermediate.
 * On MSVC we avoid __uint128_t which would call compiler-rt.
 * If high==0, we save a DIV.
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

/* mont_mul: Montgomery multiplication: a*b*R^-1 mod n, R=2^64
 * This is the HOT PATH. Avoids division entirely, uses only MULH.
 * Requires precomputed inv = -n^-1 mod R.
 * Zen4 MULX throughput is 2/cycle, so this is ~4 cycles.
 */
static inline uint64_t mont_mul(uint64_t a, uint64_t b, uint64_t n, uint64_t inv) {
#if defined(_MSC_VER)
    uint64_t t_hi, t_lo, h;
    t_lo = my_umul128(a, b, &t_hi);
    uint64_t m = t_lo * inv;            // Low part * inv -> reduction factor
    h = my_umulh(m, n);                 // High of m*n
#else
    __uint128_t t = (__uint128_t)a * b;
    uint64_t t_hi = (uint64_t)(t >> 64);
    uint64_t m = (uint64_t)t * inv;
    uint64_t h = (uint64_t)(((__uint128_t)m * n) >> 64);
#endif
    uint64_t res = t_hi - h;
    return (t_hi < h) ? (res + n) : res; // Conditional add, no branch predictor miss
}

/* mont_pow: Binary exponentiation in Montgomery domain */
static inline uint64_t mont_pow(uint64_t base, uint64_t exp, uint64_t n,
    uint64_t inv, uint64_t mont_one) {
    uint64_t result = mont_one; // 1 in Montgomery form = R mod n
    while (exp) {
        if (exp & 1) result = mont_mul(result, base, n, inv);
        base = mont_mul(base, base, n, inv);
        exp >>= 1;
    }
    return result;
}

/* is_prime: Deterministic Miller-Rabin for <2^64
 * Steps:
 * 1. LUT for n<=163 (fast path)
 * 2. Trial division by primes 23..163 (divisibility by 2,3,5,7,11,13,17,19 already excluded by wheel)
 * 3. Miller-Rabin base 2 quick reject
 * 4. Miller-Rabin bases {325, 9375, 28178, 450775, 9780504, 1795265022} - known deterministic set for 64-bit (Jim Sinclair)
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

    // Fast trial division - eliminates ~90% of composites before expensive MR
    if (n % 23 == 0 || n % 29 == 0 || n % 31 == 0 || n % 37 == 0 ||
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
    while ((d & 1) == 0) { d >>= 1; ++r; } // Factor n-1 = d*2^r

    // Compute modular inverse inv = -n^-1 mod 2^64 via Newton-Raphson (4 iterations = 64 bits)
    uint64_t inv = (3 * n) ^ 2;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;
    inv *= 2 - n * inv;

    uint64_t mont_one = (0ULL - n) % n; // R mod n
    uint64_t mont_minus_one = n - mont_one; // -1 in Montgomery form
    uint64_t mont_two = (mont_one >= mont_minus_one)
        ? (mont_one - mont_minus_one)
        : (mont_one + mont_one); // 2*R mod n

    // Base 2 test - most composites fail here
    uint64_t x = mont_pow(mont_two, d, n, inv, mont_one);
    if (x != mont_one && x != mont_minus_one) {
        int composite = 1;
        for (int j = 0; j < r - 1; ++j) {
            x = mont_mul(x, x, n, inv);
            if (x == mont_minus_one) { composite = 0; break; }
        }
        if (composite) return 0;
    }

    // Prepare R^2 mod n for converting bases into Montgomery form
    uint64_t r2 = mulmod64(mont_one, mont_one, n);
    static const uint64_t bases[6] = {
        325, 9375, 28178, 450775, 9780504, 1795265022
    };

    for (int i = 0; i < 6; ++i) {
        uint64_t a = bases[i] % n;
        if (a == 0) continue;

        uint64_t mont_a = mont_mul(a, r2, n, inv); // a*R mod n
        x = mont_pow(mont_a, d, n, inv, mont_one);
        if (x == mont_one || x == mont_minus_one) continue;

        int composite = 1;
        for (int j = 0; j < r - 1; ++j) {
            x = mont_mul(x, x, n, inv);
            if (x == mont_minus_one) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

// --- Mod-30 Wheel ---
// Numbers coprime to 30 are 1,7,11,13,17,19,23,29 -> 8 residues per 30 numbers
// This skips 73.3% of numbers (all multiples of 2,3,5)
static const uint8_t wheel_rem[8] = { 1, 7, 11, 13, 17, 19, 23, 29 };
static const uint8_t wheel_step[8] = { 6, 4,  2,  4,  2,  4,  6,  2 }; // Gaps to next residue

static inline uint64_t align_to_wheel30(uint64_t n, int* wheel_idx) {
    uint64_t base = (n / 30) * 30;
    uint64_t rem = n % 30;
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
    *wheel_idx = (*wheel_idx + 1) & 7; // Fast mod 8
    return r;
}

// --- Fast 2-Digits-at-a-Time Integer to String ---
// LUT for 00-99, processes 2 digits per iteration -> 2x faster than div-by-10 loop
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
    }
    else {
        uint64_t idx = val * 2;
        temp[len++] = g_digits_lut[idx + 1];
        temp[len++] = g_digits_lut[idx];
    }

    size_t out_len = len;
    while (len > 0) {
        *out++ = temp[--len];
    }
    *out = '\n';
    return out_len + 1;
}

// --- Writer Thread ---
#define MAX_FILE_SIZE_BYTES (100LL * 1024 * 1024) // 100 MB per file - balances filesystem & resume granularity

static FILE* g_file = NULL;
static char     g_out_buf[OUT_BUF_SIZE];
static size_t   g_out_used = 0;
static uint64_t g_current_file_bytes = 0;

static char     g_dir_path[MAX_PATH];
static int      g_file_index = 1;
static char     g_current_path[MAX_PATH];

static void flush_output(void) {
    if (g_out_used > 0) {
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

/* writer_proc: Single writer, pinned to core 14 (last core of 7840HS)
 * Waits in order (expected ticket) - ensures prime file is sorted despite out-of-order workers
 * Uses WaitOnAddress (futex-like) - zero CPU while waiting
 */
static DWORD WINAPI writer_proc(LPVOID param) {
    (void)param;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 14);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

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

        // Zero-copy streaming: memcpy from block to 1MB buffer, then chunk
        if (g_out_used + b->text_len > sizeof(g_out_buf)) flush_output();
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
                printf("Failed to open new file: %s\n", g_current_path);
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
    return 0;
}

// --- Worker Threads ---
/* worker_proc: 14 workers pinned to cores 0-13
 * Each worker:
 * 1. Atomically grabs next ticket
 * 2. Waits for its ring slot to be free (seq == ticket)
 * 3. Computes candidate range: base + ticket*BLOCK_SIZE (block size is multiple of 30, so wheel stays aligned!)
 * 4. Tests CANDIDATES_PER_BLOCK numbers via wheel
 * 5. Formats primes directly into block.text (no malloc, no shared state)
 * 6. Marks block ready: seq = -ticket-1, wakes writer
 */
static DWORD WINAPI worker_proc(LPVOID param) {
    int worker_id = (int)(intptr_t)param;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << worker_id);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    for (;;) {
        if (ReadAcquire(&g_stop)) break;

        LONG64 ticket = InterlockedIncrement64(&g_next_ticket) - 1;
        Block* b = &g_blocks[ticket & (RING_SIZE - 1)];
        LONG64 current_seq;

        while ((current_seq = ReadAcquire64(&b->seq)) != ticket) {
            WaitOnAddress((PVOID)&b->seq, &current_seq, sizeof(LONG64), INFINITE);
        }

        // Wheel index is constant across all blocks! Because BLOCK_SIZE % 30 == 0
        uint64_t c = g_base_candidate + (uint64_t)ticket * BLOCK_SIZE;
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

// --- Ctrl+C Handler & File Resume ---
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_stop, 1);
        return TRUE;
    }
    return FALSE;
}

/* read_last_prime: Resume support - reads last 256 bytes of latest file to find last complete prime */
static uint64_t read_last_prime(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    _fseeki64(f, 0, SEEK_END);
    __int64 size = _ftelli64(f);
    if (size == 0) { fclose(f); return 0; }

    __int64 read_size = size < 256 ? size : 256;
    _fseeki64(f, -read_size, SEEK_END);

    char buf[257];
    size_t bytes_read = fread(buf, 1, read_size, f);
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

    // 1. Get exe directory for output files
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcpy(g_dir_path, path);

    // 2. Find latest primegenerated*.txt to resume
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

    // 3. Setup base candidate after resume
    if (last_prime >= 5) {
        printf("Resuming after prime %llu in file %s\n", last_prime, g_current_path);
        g_file = fopen(g_current_path, "ab");
        _fseeki64(g_file, 0, SEEK_END);
        g_current_file_bytes = (uint64_t)_ftelli64(g_file);
        g_base_candidate = (last_prime < 7) ? 7 : align_to_wheel30(last_prime + 2, &g_base_wheel_idx);
    }
    else {
        g_base_candidate = 7;
        g_base_wheel_idx = 1;
        g_file_index = 1;
        snprintf(g_current_path, MAX_PATH, "%sprimegenerated%d.txt", g_dir_path, g_file_index);
        g_file = fopen(g_current_path, "wb");
        if (g_file) {
            fprintf(g_file, "2\n3\n5\n");
            fflush(g_file);
            g_current_file_bytes = 6;
        }
    }

    if (!g_file) { printf("Error opening file.\n"); return 1; }

    setvbuf(g_file, NULL, _IONBF, 0);

    for (int i = 0; i < RING_SIZE; ++i) g_blocks[i].seq = i;

    HANDLE workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i)
        workers[i] = CreateThread(NULL, 0, worker_proc, (LPVOID)(intptr_t)i, 0, NULL);

    HANDLE writer = CreateThread(NULL, 0, writer_proc, NULL, 0, NULL);

    printf("Generating primes using 14 threads. Press Ctrl+C to stop safely...\n");

    WaitForMultipleObjects(NUM_WORKERS, workers, TRUE, INFINITE);
    WaitForSingleObject(writer, INFINITE);

    for (int i = 0; i < NUM_WORKERS; ++i) CloseHandle(workers[i]);
    CloseHandle(writer);

    printf("Safely stopped.\n");
    return 0;
}
