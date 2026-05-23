#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <asm/unistd.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// ═══════════════════════════════════════════════════════════════════════
//  SERIALIZED TSC
//  START: lfence → rdtsc   (lfence drains all prior instructions first)
//  END:   rdtscp → lfence  (rdtscp partial-serializes the read;
//                           trailing lfence stops later instrs sneaking in)
//  TSC ticks at a FIXED reference frequency regardless of CPU turbo/throttle.
//  Ticks ≠ actual execution cycles. Divide by TSC freq to get nanoseconds.
// ═══════════════════════════════════════════════════════════════════════

static inline uint64_t tsc_start(void) {
    uint64_t tsc;
    __asm__ volatile(
        "lfence\n\t"
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or  %%rdx, %%rax"
        : "=a"(tsc) :: "rdx", "memory");
    return tsc;
}

static inline uint64_t tsc_end(void) {
    uint64_t tsc;
    __asm__ volatile(
        "rdtscp\n\t"
        "shl $32, %%rdx\n\t"
        "or  %%rdx, %%rax\n\t"
        "lfence"
        : "=a"(tsc) :: "rdx", "rcx", "memory");
    return tsc;
}

// ═══════════════════════════════════════════════════════════════════════
//  COUNTER DEFINITIONS
//  PERF_TYPE_HARDWARE: generic aliases, kernel maps to correct CPU MSR.
//  PERF_TYPE_HW_CACHE: encodes (cache_id | op<<8 | result<<16).
//  Not all CPUs support all combinations — unavailable ones show "n/a".
//  Run `perf list` to see every event your CPU actually supports.
// ═══════════════════════════════════════════════════════════════════════

#define CACHE_EV(c,o,r) \
    ((uint64_t)(c) | ((uint64_t)(o)<<8) | ((uint64_t)(r)<<16))

typedef struct { uint32_t type; uint64_t config; const char *name; } CDef;

// Counter index constants — use these names everywhere for clarity
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

static const CDef DEFS[NC] = {
    [CI_CYCLES]    = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,          "cycles"          },
    [CI_INSTRS]    = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,        "instructions"    },
    [CI_BRANCHES]  = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branches"        },
    [CI_BMISSES]   = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,       "branch_misses"   },
    [CI_L1D_REFS]  = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_L1D,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_ACCESS),           "l1d_read_refs"   },
    [CI_L1D_MISS]  = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_L1D,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_MISS),             "l1d_read_miss"   },
    [CI_L1I_MISS]  = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_L1I,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_MISS),             "l1i_miss"        },
    [CI_DTLB_MISS] = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_DTLB,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_MISS),             "dtlb_miss"       },
    [CI_ITLB_MISS] = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_ITLB,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_MISS),             "itlb_miss"       },
};

// ═══════════════════════════════════════════════════════════════════════
//  BENCH_RESULT  —  one complete snapshot from a single start/stop pair
// ═══════════════════════════════════════════════════════════════════════

typedef struct {
    uint64_t tsc_ticks;        // fixed-freq reference ticks (overhead subtracted)
    uint64_t c[NC];            // raw counter values, indexed by CI_* constants
    // Derived
    double   ipc;              // instructions / cycles
    double   cpi;              // cycles / instructions
    double   branch_miss_pct;  // branch_misses / branches * 100
    double   l1d_miss_pct;     // l1d_misses / l1d_refs * 100
    int      ok[NC];           // 1 if this counter was available
} BenchResult;

// ═══════════════════════════════════════════════════════════════════════
//  BENCH_CTX  —  open fds, created once at startup
// ═══════════════════════════════════════════════════════════════════════

typedef struct {
    int      fd[NC];
    int      ok[NC];
    uint64_t val[NC];
    uint64_t baseline[NC];
} BenchCtx;

static BenchCtx g_ctx;
static uint64_t g_t0;
static int g_initialized = 0;

static long _perf_open(struct perf_event_attr *a) {
    return syscall(__NR_perf_event_open, a, 0, -1, -1, 0);
}

static void _init_once(void) {
    if (g_initialized) return;
    g_initialized = 1;
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    printf("Counter availability on this CPU:\n");
    for (int i = 0; i < NC; i++) {
        struct perf_event_attr pe = {0};
        pe.type = DEFS[i].type; pe.size = sizeof(pe); pe.config = DEFS[i].config;
        pe.disabled = 1; pe.exclude_kernel = 1; pe.exclude_hv = 1;
        g_ctx.fd[i] = (int)_perf_open(&pe);
        g_ctx.ok[i] = (g_ctx.fd[i] >= 0);
        printf("  %s %s\n", g_ctx.ok[i] ? "✓" : "✗ (not on this CPU)", DEFS[i].name);
    }
    printf("\n");
}

// ═══════════════════════════════════════════════════════════════════════
//  start_measuring / stop_measuring
//
//  These are the two functions you call around your code.
//
//  Usage:
//      uint64_t t0 = start_measuring(&ctx);
//      your_code();
//      BenchResult r = stop_measuring(&ctx, t0);
//      print_result("label", &r);
//
//  The TSC token t0 is returned from start and passed into stop so
//  both TSC and PMU counters cover exactly the same code window.
//
//  ioctl(ENABLE) is called BEFORE tsc_start so its cost is outside
//  the TSC window. ioctl(DISABLE) is called AFTER tsc_end for the
//  same reason. Both are still subtracted via calibration anyway.
// ═══════════════════════════════════════════════════════════════════════

// KEEP: prevents compiler dead-code-elimination of your computed result
#define KEEP(x) __asm__ volatile("" : "+r,m"(x) :: "memory")

void start_measuring(void) {
    _init_once();
    for (int i = 0; i < NC; i++) {
        if (!g_ctx.ok[i]) continue;
        ioctl(g_ctx.fd[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(g_ctx.fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
    g_t0 = tsc_start();
}

BenchResult stop_measuring(void) {
    uint64_t t1 = tsc_end();
    
    for (int i = 0; i < NC; i++) {
        if (!g_ctx.ok[i]) { g_ctx.val[i] = 0; continue; }
        ioctl(g_ctx.fd[i], PERF_EVENT_IOC_DISABLE, 0);
        (void)!read(g_ctx.fd[i], &g_ctx.val[i], sizeof(uint64_t));
    }

    BenchResult r = {0};
    r.tsc_ticks = t1 - g_t0;
    for (int i = 0; i < NC; i++) {
        r.c[i]  = g_ctx.val[i];
        r.ok[i] = g_ctx.ok[i];
    }

    if (r.ok[CI_CYCLES] && r.c[CI_CYCLES]) {
        r.ipc = (double)r.c[CI_INSTRS]  / r.c[CI_CYCLES];
        r.cpi = (double)r.c[CI_CYCLES]  / r.c[CI_INSTRS];
    }
    if (r.ok[CI_BRANCHES] && r.c[CI_BRANCHES])
        r.branch_miss_pct = 100.0 * r.c[CI_BMISSES] / r.c[CI_BRANCHES];
    if (r.ok[CI_L1D_REFS] && r.c[CI_L1D_REFS])
        r.l1d_miss_pct = 100.0 * r.c[CI_L1D_MISS] / r.c[CI_L1D_REFS];

    return r;
}

void print_measured_results(BenchResult r) {
    printf("  --------------------------------------------------\n");
    
    if (r.ok[CI_INSTRS])
        printf("  %-32s %16llu\n", "instructions", (unsigned long long)r.c[CI_INSTRS]);
    else
        printf("  %-32s %16s\n", "instructions", "n/a");
    
    if (r.ok[CI_CYCLES])
        printf("  %-32s %16llu\n", "cycles", (unsigned long long)r.c[CI_CYCLES]);
    else
        printf("  %-32s %16s\n", "cycles", "n/a");
    
    if (r.ok[CI_CYCLES] && r.ok[CI_INSTRS] && r.c[CI_CYCLES] && r.c[CI_INSTRS])
        printf("  %-32s %16.3f\n", "IPC", r.ipc);
    else
        printf("  %-32s %16s\n", "IPC", "n/a");
    
    printf("  --------------------------------------------------\n");
    
    if (r.ok[CI_BMISSES] && r.ok[CI_BRANCHES]) {
        printf("  %-32s %16llu\n", "branch misses", (unsigned long long)r.c[CI_BMISSES]);
        if (r.c[CI_BRANCHES])
            printf("  %-32s %15.4f%%\n", "branch miss rate", r.branch_miss_pct);
        else
            printf("  %-32s %16s\n", "branch miss rate", "n/a");
    } else {
        printf("  %-32s %16s\n", "branch misses", "n/a");
        printf("  %-32s %16s\n", "branch miss rate", "n/a");
    }
    
    printf("  --------------------------------------------------\n");
    
    if (r.ok[CI_L1D_MISS])
        printf("  %-32s %16llu\n", "L1D misses", (unsigned long long)r.c[CI_L1D_MISS]);
    else
        printf("  %-32s %16s\n", "L1D misses", "n/a");
    
    printf("  %-32s %16s\n", "LLC misses", "n/a");
    
    printf("  --------------------------------------------------\n");
    
    printf("  %-32s %16llu\n", "tsc ticks", (unsigned long long)r.tsc_ticks);
    
    printf("\n");
}