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
#include <sched.h>
#include <time.h>

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

// Counter definitions
// Not all events work on all CPUs - use `perf list` to check

#define CACHE_EV(c,o,r) \
    ((uint64_t)(c) | ((uint64_t)(o)<<8) | ((uint64_t)(r)<<16))

typedef struct { uint32_t type; uint64_t config; const char *name; } CDef;

// 6 counters - fits in hardware without multiplexing
#define CI_CYCLES     0
#define CI_INSTRS     1
#define CI_BRANCHES   2
#define CI_BMISSES    3
#define CI_L1D_MISS   4
#define CI_L3_MISS    5
#define NC            6

static const CDef DEFS[NC] = {
    [CI_CYCLES]    = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,          "cycles"          },
    [CI_INSTRS]    = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,        "instructions"    },
    [CI_BRANCHES]  = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, "branches"        },
    [CI_BMISSES]   = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,       "branch_misses"   },
    [CI_L1D_MISS]  = { PERF_TYPE_HW_CACHE,
                       CACHE_EV(PERF_COUNT_HW_CACHE_L1D,
                                PERF_COUNT_HW_CACHE_OP_READ,
                                PERF_COUNT_HW_CACHE_RESULT_MISS),             "l1d_miss"        },
    [CI_L3_MISS]   = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES,        "l3_miss"         },
};

// Internal result structure

typedef struct {
    uint64_t tsc_ticks;        // fixed-freq reference ticks
    uint64_t nanoseconds;      // TSC converted to nanoseconds
    uint64_t c[NC];            // raw counter values, indexed by CI_* constants
    // Derived
    double   ipc;              // instructions / cycles
    double   cpi;              // cycles / instructions
    double   branch_miss_pct;  // branch_misses / branches * 100
    double   l1d_miss_pct;     // l1d_misses / l1d_refs * 100
    int      ok[NC];           // 1 if this counter was available
} BenchResult;

// Global context - file descriptors for perf counters

typedef struct {
    int      fd[NC];
    int      ok[NC];
    uint64_t val[NC];
    uint64_t baseline[NC];
} BenchCtx;

static BenchCtx g_ctx;
static uint64_t g_t0;
static int g_initialized = 0;
static uint64_t g_tsc_freq_hz = 0;  // TSC frequency in Hz

static long _perf_open(struct perf_event_attr *a) {
    return syscall(__NR_perf_event_open, a, 0, -1, -1, 0);
}

// TSC frequency detection:
// 1. Read from sysfs (best)
// 2. Calibrate against CLOCK_MONOTONIC
// 3. Fail if neither works

static uint64_t _detect_tsc_freq(void) {
    // Method 1: Read from sysfs
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
    if (f) {
        uint64_t khz = 0;
        if (fscanf(f, "%llu", &khz) == 1 && khz > 0) {
            fclose(f);
            return khz * 1000;  // convert kHz to Hz
        }
        fclose(f);
    }

    // Method 2: Calibrate against CLOCK_MONOTONIC
    // Run for 100ms and measure TSC ticks
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t tsc_begin = tsc_start();
    
    // Busy wait for ~100ms
    struct timespec sleep_time = {0, 100000000};  // 100ms
    nanosleep(&sleep_time, NULL);
    
    uint64_t tsc_finish = tsc_end();
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    
    uint64_t tsc_delta = tsc_finish - tsc_begin;
    uint64_t ns_delta = (ts_end.tv_sec - ts_start.tv_sec) * 1000000000ULL +
                        (ts_end.tv_nsec - ts_start.tv_nsec);
    
    if (ns_delta > 0) {
        uint64_t freq = (tsc_delta * 1000000000ULL) / ns_delta;
        if (freq > 1000000000ULL && freq < 10000000000ULL) {  // sanity check: 1-10 GHz
            return freq;
        }
    }
    
    // Method 3: Fatal error - cannot proceed without TSC frequency
    fprintf(stderr, "ERROR: Could not detect TSC frequency.\n");
    fprintf(stderr, "       TSC frequency is required for accurate nanosecond conversion.\n");
    fprintf(stderr, "       Please report this issue with your CPU model.\n");
    exit(1);
}

static void _init_once(void) {
    if (g_initialized) return;
    g_initialized = 1;
    
    // Detect TSC frequency
    g_tsc_freq_hz = _detect_tsc_freq();
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    printf("TSC frequency: %.3f GHz\n", g_tsc_freq_hz / 1e9);
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

// Main API - wrap your code with start/stop

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
    r.nanoseconds = (r.tsc_ticks * 1000000000ULL) / g_tsc_freq_hz;
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
    
    if (r.ok[CI_L3_MISS])
        printf("  %-32s %16llu\n", "L3 misses", (unsigned long long)r.c[CI_L3_MISS]);
    else
        printf("  %-32s %16s\n", "L3 misses", "n/a");
    
    printf("  --------------------------------------------------\n");
    
    printf("  %-32s %16llu\n", "tsc ticks", (unsigned long long)r.tsc_ticks);
    printf("  %-32s %16llu\n", "nanoseconds", (unsigned long long)r.nanoseconds);
    
    printf("\n");
}

// CPU pinning - bind thread to specific core

void pin_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        printf("Thread pinned to CPU %d\n", cpu_id);
    } else {
        fprintf(stderr, "Warning: Failed to pin to CPU %d\n", cpu_id);
    }
}

// Overhead measurement - cost of empty start/stop

BenchResult measure_overhead(void) {
    start_measuring();
    BenchResult r = stop_measuring();
    return r;
}

static int _cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

void print_overhead_stats(int iterations) {
    if (iterations <= 0) iterations = 10000;
    
    uint64_t *tsc_samples = malloc(iterations * sizeof(uint64_t));
    uint64_t *ns_samples = malloc(iterations * sizeof(uint64_t));
    uint64_t *instr_samples = malloc(iterations * sizeof(uint64_t));
    
    if (!tsc_samples || !ns_samples || !instr_samples) {
        fprintf(stderr, "Failed to allocate memory for overhead measurement\n");
        free(tsc_samples);
        free(ns_samples);
        free(instr_samples);
        return;
    }
    
    printf("Measuring overhead (%d iterations)...\n", iterations);
    
    for (int i = 0; i < iterations; i++) {
        BenchResult r = measure_overhead();
        tsc_samples[i] = r.tsc_ticks;
        ns_samples[i] = r.nanoseconds;
        instr_samples[i] = r.ok[CI_INSTRS] ? r.c[CI_INSTRS] : 0;
    }
    
    qsort(tsc_samples, iterations, sizeof(uint64_t), _cmp_u64);
    qsort(ns_samples, iterations, sizeof(uint64_t), _cmp_u64);
    qsort(instr_samples, iterations, sizeof(uint64_t), _cmp_u64);
    
    uint64_t tsc_min = tsc_samples[0];
    uint64_t tsc_p50 = tsc_samples[iterations / 2];
    uint64_t tsc_p99 = tsc_samples[(iterations * 99) / 100];
    
    uint64_t ns_min = ns_samples[0];
    uint64_t ns_p50 = ns_samples[iterations / 2];
    uint64_t ns_p99 = ns_samples[(iterations * 99) / 100];
    
    uint64_t instr_min = instr_samples[0];
    uint64_t instr_p50 = instr_samples[iterations / 2];
    uint64_t instr_p99 = instr_samples[(iterations * 99) / 100];
    
    printf("\nMeasurement Overhead Statistics:\n");
    printf("  --------------------------------------------------\n");
    printf("  %-20s %10s %10s %10s\n", "Metric", "min", "p50", "p99");
    printf("  --------------------------------------------------\n");
    printf("  %-20s %10llu %10llu %10llu\n", "TSC ticks", 
           (unsigned long long)tsc_min, (unsigned long long)tsc_p50, (unsigned long long)tsc_p99);
    printf("  %-20s %10llu %10llu %10llu\n", "Nanoseconds", 
           (unsigned long long)ns_min, (unsigned long long)ns_p50, (unsigned long long)ns_p99);
    printf("  %-20s %10llu %10llu %10llu\n", "Instructions", 
           (unsigned long long)instr_min, (unsigned long long)instr_p50, (unsigned long long)instr_p99);
    printf("  --------------------------------------------------\n");
    printf("\nInterpretation:\n");
    printf("  - min    = best-case overhead (hot caches, perfect branch prediction)\n");
    printf("  - p50    = typical overhead (median of all runs)\n");
    printf("  - p99    = worst-case overhead (99%% of runs are faster than this)\n");
    printf("\n");
    
    free(tsc_samples);
    free(ns_samples);
    free(instr_samples);
}