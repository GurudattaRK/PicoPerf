/*
 * time.h  --  Hardware PMU measurement library
 *
 * Measures 4 counters: cycles, instructions, cache_misses, branch_misses.
 *
 * WHY ONLY 4 COUNTERS
 *   AMD Zen 2 has 6 physical GP PMC registers per core.
 *   Intel Skylake+ has 4 GP + 3 fixed-function = 7 usable.
 *   Opening more counters than fit in hardware causes the kernel to
 *   time-multiplex them. On short workloads some counters get zero
 *   time slots and read back 0. 4 counters = no multiplexing ever.
 *
 * WHY NO OVERHEAD SUBTRACTION
 *   Subtracting a calibrated average from a single-run measurement
 *   adds noise: actual per-run overhead varies, and when it exceeds
 *   the average the result clamps to 0. The harness costs ~30-60 TSC
 *   ticks -- negligible against any real workload.
 *
 * WHY PERF GROUPS
 *   Counters opened as a group start/stop atomically via one ioctl
 *   on the leader. All four cover exactly the same instruction window.
 *
 * COMPILER SAFETY
 *   tsc_start / tsc_stop use `asm volatile` with a "memory" clobber.
 *   This is a full compiler barrier: the optimizer cannot move any
 *   load, store, or function call across it. Combined with the hardware
 *   lfence/rdtscp instructions, no optimization level (-O3, -Ofast,
 *   -funroll-loops, -fstrict-aliasing, PGO, even LTO) can reorder
 *   your code outside the measurement window.
 *
 *   LTO specifically: start_measuring / stop_measuring are extern "C"
 *   symbols. LTO may inline them into your caller, but the asm volatile
 *   barriers travel with the inlining -- the window boundaries remain
 *   exactly where you placed them.
 *
 * REQUIRED USER-SIDE DEFENSES
 *   Two things in your measured code that YOU must do:
 *
 *   1. KEEP(x) -- prevent dead-code elimination of your result:
 *        uint64_t s = compute();
 *        KEEP(s);
 *      Without this, -O3 deletes loops whose result is never used.
 *
 *   2. __attribute__((noinline)) on the measured function -- prevent
 *      the compiler from hoisting its body before start_measuring():
 *        __attribute__((noinline))
 *        uint64_t my_func(const int *data, size_t n) { ... }
 *      Without this, the compiler can inline and reorder the loop
 *      relative to your start/stop calls.
 *      Note: with LTO, noinline only protects within one translation
 *      unit. If you compile bench.cpp and your code in the same LTO
 *      unit, also add __attribute__((noinline, noclone)).
 *
 * BUILD
 *   g++ -O3 -march=native [flags] your_code.cpp time.cpp -o binary
 *
 * SETUP (once per boot)
 *   echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 *
 * PLATFORM
 *   Linux only. Requires perf_event_open(2), available since kernel 2.6.31.
 *   x86-64 only (uses rdtsc/rdtscp). TSC is invariant on all Zen 2+ and
 *   Intel Nehalem+ processors.
 */

#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>

/* ── KEEP ────────────────────────────────────────────────────────────
 * Prevents -O3 from treating your measured result as dead code.
 * Wrap every value produced inside the start/stop window:
 *   uint64_t s = compute();
 *   KEEP(s);
 * The "+r,m" constraint forces the compiler to materialise the value
 * in a register or memory location, making it "observed". The
 * "memory" clobber also acts as a barrier for surrounding loads/stores.
 */
#define KEEP(x) __asm__ volatile("" : "+r,m"(x) :: "memory")

/* ── BenchResult ─────────────────────────────────────────────────────
 * Returned by stop_measuring(). All fields valid immediately.
 * ok == 0 means perf_event_open failed (check paranoid setting).
 */
#define CI_CYCLES     0
#define CI_INSTRS     1
#define CI_BRANCHES   2
#define CI_BMISSES    3
#define CI_L1D_REFS   4
#define CI_L1D_MISS   5
#define CI_L1I_MISS   6
#define CI_DTLB_MISS  7
#define CI_ITLB_MISS  8
#define NC            9

typedef struct {
    uint64_t tsc_ticks;
    uint64_t c[NC];
    double   ipc;
    double   cpi;
    double   branch_miss_pct;
    double   l1d_miss_pct;
    int      ok[NC];
} BenchResult;

#ifdef __cplusplus
extern "C" {
#endif

void        start_measuring(void);
BenchResult stop_measuring(void);
void        print_measured_results(BenchResult r);

#ifdef __cplusplus
}
#endif