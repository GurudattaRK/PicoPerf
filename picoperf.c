#define _GNU_SOURCE
#include "picoperf.h"
#include <linux/perf_event.h>
#include <sys/mman.h>
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

// Serialized TSC reads
// lfence prevents reordering, rdtscp reads counter
// TSC ticks at fixed frequency (not affected by turbo/throttle)

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


// Read format for PERF_FORMAT_GROUP (group leader read)
typedef struct {
    uint64_t nr;
    uint64_t time_enabled;
    uint64_t time_running;
    struct {
        uint64_t value;
        uint64_t id;
    } values[NC];
} PerfGroupRead;

// Global context - file descriptors for perf counters
typedef struct {
    int      fd[NC];   // fd[CI_CYCLES] is the group leader
    int      ok[NC];   // 1 if event i is open and being measured
    uint64_t id[NC];   // event IDs for matching group-read values
    uint64_t val[NC];
} BenchCtx;

static BenchCtx g_ctx;
static uint64_t g_t0;
static int g_initialized = 0;
static uint64_t g_tsc_freq_hz = 0;  // TSC frequency in Hz
static int g_n_events = NC;         // events actually opened (<= NC, never multiplexed)
static int g_nmi_prev = -1;         // previous nmi_watchdog value (-1 = untouched)
static int g_setup_done = 0;        // picoperf_setup() (or the lazy fallback) has run
static int g_target_cpu = 0;        // core the measured thread is pinned to

static long _perf_open(struct perf_event_attr *a, int group_fd) {
    return syscall(__NR_perf_event_open, a, 0, -1, group_fd, 0);
}

// The NMI hardlockup watchdog permanently occupies one general-purpose PMC per
// CPU. On CPUs with N core counters this leaves only N-1 for us. Because a perf
// event group is scheduled all-or-nothing, a full NC-event group cannot be
// placed on hardware when the watchdog holds a counter, so it never counts.
// Free the counter by disabling the watchdog (root only); restored in fini.
static void _restore_nmi_watchdog(void);

static void _disable_nmi_watchdog(void) {
    if (geteuid() != 0) return;  // requires root
    FILE *f = fopen("/proc/sys/kernel/nmi_watchdog", "r");
    if (!f) return;
    int cur = -1;
    if (fscanf(f, "%d", &cur) != 1) { fclose(f); return; }
    fclose(f);
    if (cur == 0) return;        // already off, nothing to restore
    f = fopen("/proc/sys/kernel/nmi_watchdog", "w");
    if (!f) return;
    if (fprintf(f, "0\n") > 0) {
        g_nmi_prev = cur;
        // Ensure the watchdog is restored even if picoperf_fini() is never called.
        atexit(_restore_nmi_watchdog);
    }
    fclose(f);
}

static void _restore_nmi_watchdog(void) {
    if (g_nmi_prev < 0) return;
    FILE *f = fopen("/proc/sys/kernel/nmi_watchdog", "w");
    if (f) {
        fprintf(f, "%d\n", g_nmi_prev);
        fclose(f);
    }
    g_nmi_prev = -1;
}

// Work out how many TSC ticks happen per second on this machine.
//
// This is just a conversion constant (ticks -> nanoseconds), not the
// measurement itself. The TSC runs at a fixed rate that doesn't change with CPU
// frequency, so we only need to learn it once per process. The per-measurement
// tick delta (tsc_end - tsc_start) is always a real, live hardware read.
//
//   1. Ask the kernel directly (exact, when the sysfs file exists).
//   2. Otherwise calibrate against the monotonic clock for 100 ms (once).
//   3. If both fail, we can't convert to ns reliably, so bail out.
static uint64_t _detect_tsc_freq(void) {
    // 1. Exact value from the kernel, if this build exposes it.
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
    if (f) {
        unsigned long long khz = 0;
        if (fscanf(f, "%llu", &khz) == 1 && khz > 0) {
            fclose(f);
            return (uint64_t)khz * 1000;  // kHz -> Hz
        }
        fclose(f);
    }

    // 2. Calibrate: busy-wait 100 ms against CLOCK_MONOTONIC and see how many
    // TSC ticks elapsed. Busy-waiting keeps the core awake so it can't drop into
    // a C-state and skew the ratio.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t0_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    uint64_t tsc_begin = tsc_start();

    uint64_t now_ns;
    do {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    } while (now_ns - t0_ns < 100000000ULL);

    uint64_t tsc_finish = tsc_end();
    uint64_t ns_delta = now_ns - t0_ns;

    if (ns_delta > 0) {
        uint64_t freq = (tsc_finish - tsc_begin) * 1000000000ULL / ns_delta;
        if (freq > 1000000000ULL && freq < 10000000000ULL)  // sanity: 1-10 GHz
            return freq;
    }

    // 3. Give up rather than report bogus nanoseconds.
    fprintf(stderr, "picoperf: ERROR: could not determine the TSC frequency.\n");
    fprintf(stderr, "          Nanosecond conversion needs it; raw TSC ticks would still be valid.\n");
    fprintf(stderr, "          Please report this with your CPU model.\n");
    exit(1);
}

static void _init_once(void) {
    if (g_initialized) return;
    g_initialized = 1;
    g_tsc_freq_hz = _detect_tsc_freq();
    // Free a PMC for the group before any counter is opened (root only).
    _disable_nmi_watchdog();
    memset(&g_ctx, 0, sizeof(g_ctx));
}

// Counter setup helpers

static void _close_all(void) {
    for (int i = 0; i < NC; i++) {
        if (g_ctx.ok[i] && g_ctx.fd[i] >= 0) close(g_ctx.fd[i]);
        g_ctx.fd[i] = -1;
        g_ctx.ok[i] = 0;
        g_ctx.id[i] = 0;
    }
}

// Open the first `n` events (DEFS[0..n)) as ONE hardware group led by
// CI_CYCLES. Returns 1 only if every requested event opened.
static int _open_group(int n) {
    struct perf_event_attr pe = {0};
    pe.type           = DEFS[CI_CYCLES].type;
    pe.size           = sizeof(pe);
    pe.config         = DEFS[CI_CYCLES].config;
    pe.disabled       = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    pe.read_format    = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
                        PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    g_ctx.fd[CI_CYCLES] = (int)_perf_open(&pe, -1);
    g_ctx.ok[CI_CYCLES] = (g_ctx.fd[CI_CYCLES] >= 0);
    if (!g_ctx.ok[CI_CYCLES]) return 0;
    ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_ID, &g_ctx.id[CI_CYCLES]);

    for (int i = 1; i < n; i++) {
        memset(&pe, 0, sizeof(pe));
        pe.type           = DEFS[i].type;
        pe.size           = sizeof(pe);
        pe.config         = DEFS[i].config;
        pe.disabled       = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv     = 1;
        pe.read_format    = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
        g_ctx.fd[i] = (int)_perf_open(&pe, g_ctx.fd[CI_CYCLES]);
        g_ctx.ok[i] = (g_ctx.fd[i] >= 0);
        if (!g_ctx.ok[i]) return 0;
        ioctl(g_ctx.fd[i], PERF_EVENT_IOC_ID, &g_ctx.id[i]);
    }
    return 1;
}

// Verify the open group physically fits on the PMU. A group is scheduled
// all-or-nothing: if it needs more counters than are free, it is NEVER placed
// and time_running stays ~0. A group that fits runs essentially the whole burst
// (time_running ~= time_enabled). We retry a few times so a stray context
// switch on a non-RT thread can't cause a false negative.
static int _group_fits(void) {
    for (int attempt = 0; attempt < 8; attempt++) {
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        volatile uint64_t sink = 0;
        for (int i = 0; i < 100000; i++) sink += i;
        KEEP(sink);
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

        PerfGroupRead buf;
        memset(&buf, 0, sizeof(buf));
        if (read(g_ctx.fd[CI_CYCLES], &buf, sizeof(buf)) < 0) return 0;
        // Fits => placed for (almost) the entire burst. Doesn't fit => ~0.
        if (buf.time_enabled > 0 && buf.time_running * 2 >= buf.time_enabled)
            return 1;
    }
    return 0;
}

static void _report_events(int n) {
    if (n >= NC) return;  // all events available, nothing to report
    fprintf(stderr,
        "picoperf: only %d of %d PMU counters are free; these events are OFF:\n", n, NC);
    for (int i = n; i < NC; i++)
        fprintf(stderr, "          - %s\n", DEFS[i].name);
    fprintf(stderr,
        "          The NMI watchdog holds one counter. Run as root and picoperf\n"
        "          will free it automatically, enabling all %d events. Data is\n"
        "          always real with zero multiplexing - never scaled.\n", NC);
}

// Open the largest event group (DEFS[0..n)) that fits the PMU with NO
// multiplexing. As root the watchdog counter is freed first, so all NC events
// fit; otherwise the lowest-priority trailing events (e.g. l3_miss) are dropped
// until the group fits. We never multiplex and never scale. The thread is
// already pinned by picoperf_setup() before we get here.
static void _open_counters(void) {
    for (int n = NC; n >= 1; n--) {
        if (_open_group(n) && _group_fits()) {
            g_n_events = n;
            _report_events(n);
            return;
        }
        _close_all();
    }

    g_n_events = 0;
    fprintf(stderr, "picoperf: ERROR: could not schedule any PMU counter.\n");
}

// Main API - wrap your code with start/stop

void start_measuring(void) {
    // If the caller never ran picoperf_setup() explicitly, do the full setup
    // now with a sensible default (pin to CPU 0). This makes start_measuring()
    // self-contained: pinning, page locking, real-time priority and the
    // environment sanity checks all happen automatically on first use.
    if (!g_setup_done) picoperf_setup(0);

    // Open the counter group on first use (after the thread is pinned).
    if (g_ctx.fd[CI_CYCLES] == 0 || (g_ctx.fd[CI_CYCLES] < 0 && !g_ctx.ok[CI_CYCLES])) {
        _open_counters();
    }

    if (g_n_events >= 1) {
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    g_t0 = tsc_start();
}

BenchResult stop_measuring(void) {
    uint64_t t1 = tsc_end();

    for (int i = 0; i < NC; i++) g_ctx.val[i] = 0;

    if (g_n_events >= 1) {
        ioctl(g_ctx.fd[CI_CYCLES], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

        // One atomic group read returns every member value. The group was
        // verified to fit the PMU with no multiplexing, so these are exact,
        // raw hardware counts - never scaled.
        PerfGroupRead buf;
        memset(&buf, 0, sizeof(buf));
        if (read(g_ctx.fd[CI_CYCLES], &buf, sizeof(buf)) < 0)
            memset(&buf, 0, sizeof(buf));

        for (int j = 0; j < (int)buf.nr && j < NC; j++) {
            for (int i = 0; i < NC; i++) {
                if (g_ctx.ok[i] && buf.values[j].id == g_ctx.id[i]) {
                    g_ctx.val[i] = buf.values[j].value;
                    break;
                }
            }
        }

        // If a context switch slid the group off the PMU mid-measurement, the
        // counters did not run for the full enabled window. We never scale or
        // fabricate - we surface it so the caller knows the sample is disturbed.
        if (buf.time_enabled > 0 && buf.time_running != buf.time_enabled) {
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr,
                    "picoperf: WARNING: measured region was descheduled "
                    "(PMU ran %.2f%% of the enabled time); sample disturbed.\n"
                    "          Pin the thread and use SCHED_FIFO (picoperf_setup) "
                    "for clean samples.\n",
                    100.0 * (double)buf.time_running / (double)buf.time_enabled);
            }
        }
    }

    BenchResult r = {0};
    r.tsc_ticks   = t1 - g_t0;
    r.nanoseconds = (r.tsc_ticks * 1000000000ULL) / g_tsc_freq_hz;
    for (int i = 0; i < NC; i++) {
        r.c[i]  = g_ctx.val[i];
        r.ok[i] = g_ctx.ok[i];
    }

    if (r.ok[CI_CYCLES] && r.ok[CI_INSTRS] && r.c[CI_CYCLES] && r.c[CI_INSTRS]) {
        r.ipc = (double)r.c[CI_INSTRS] / r.c[CI_CYCLES];
        r.cpi = (double)r.c[CI_CYCLES] / r.c[CI_INSTRS];
    }
    if (r.ok[CI_BRANCHES] && r.ok[CI_BMISSES] && r.c[CI_BRANCHES]) {
        r.branch_miss_pct = 100.0 * r.c[CI_BMISSES] / r.c[CI_BRANCHES];
    }

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
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        fprintf(stderr, "picoperf: failed to pin to CPU %d: %s\n",
                cpu_id, strerror(errno));
}

void picoperf_lock_memory(void *addr, size_t len) {
    // Lock specific memory region to prevent page faults during measurement
    // Call this AFTER allocating your data structures
    if (mlock(addr, len) != 0) {
        fprintf(stderr, "picoperf: mlock(%p, %zu) failed: %s\n", 
                addr, len, strerror(errno));
        fprintf(stderr, "         page faults may occur during measurement\n");
        fprintf(stderr, "         try: sudo sysctl vm.max_map_count=262144\n");
        fprintf(stderr, "         or run with sudo for unlimited mlock\n");
    }
}

// One-time setup: pin the thread, lock its pages, ask for real-time priority,
// and warn about anything in the environment that would make cycle counts
// jittery. Safe to call more than once - only the first call does the work, and
// start_measuring() calls it automatically if you forget. Works the same on
// Intel and AMD.
void picoperf_setup(int cpu_id) {
    if (g_setup_done) return;
    g_setup_done = 1;
    g_target_cpu = cpu_id;

    pin_to_cpu(cpu_id);  // pin first so the counters land on this core
    _init_once();        // detect TSC frequency, free a PMU counter if root

    // Keep our pages in RAM so a page fault can't stall a measurement. As root
    // we can also lock future allocations; otherwise lock what's mapped now and
    // let the caller pin their data with picoperf_lock_memory().
    if (geteuid() == 0) mlockall(MCL_CURRENT | MCL_FUTURE);
    else                mlockall(MCL_CURRENT);

    // Real-time priority so the kernel scheduler leaves us alone mid-measurement.
    struct sched_param sp = { .sched_priority = 1 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        fprintf(stderr,
            "picoperf: couldn't get SCHED_FIFO (%s).\n"
            "          The scheduler may preempt the measured thread; run as root to fix.\n",
            strerror(errno));

    // From here on we only *warn* - these knobs need system-wide changes the
    // library shouldn't make for you.

    // Hyper-Threading / SMT: a sibling on the same core shares the PMU.
    FILE *f = fopen("/sys/devices/system/cpu/smt/active", "r");
    if (f) {
        int active = 0;
        if (fscanf(f, "%d", &active) == 1 && active)
            fprintf(stderr,
                "picoperf: heads up - SMT/Hyper-Threading is on; the sibling thread shares\n"
                "          the PMU and skews counts. Disable: echo off | sudo tee /sys/devices/system/cpu/smt/control\n");
        fclose(f);
    }

    // Frequency governor: anything but 'performance' lets the clock wander.
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu_id);
    f = fopen(path, "r");
    if (f) {
        char gov[64] = {0};
        if (fscanf(f, "%63s", gov) == 1 && strcmp(gov, "performance") != 0)
            fprintf(stderr,
                "picoperf: heads up - CPU governor is '%s', not 'performance'; cycle counts\n"
                "          will vary. Fix: sudo cpupower frequency-set -g performance\n", gov);
        fclose(f);
    }

    // Intel turbo (intel_pstate driver).
    f = fopen("/sys/devices/system/cpu/intel_pstate/no_turbo", "r");
    if (f) {
        int no_turbo = 0;
        if (fscanf(f, "%d", &no_turbo) == 1 && !no_turbo)
            fprintf(stderr,
                "picoperf: heads up - Intel turbo is on; the clock can exceed base speed.\n"
                "          Disable: echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo\n");
        fclose(f);
    }

    // AMD (and acpi-cpufreq) boost.
    f = fopen("/sys/devices/system/cpu/cpufreq/boost", "r");
    if (f) {
        int boost = 0;
        if (fscanf(f, "%d", &boost) == 1 && boost)
            fprintf(stderr,
                "picoperf: heads up - CPU boost is on; the clock can exceed base speed.\n"
                "          Disable: echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost\n");
        fclose(f);
    }
}

void picoperf_fini(void) {
    for (int i = 0; i < NC; i++) {
        if (g_ctx.fd[i] >= 0) {
            close(g_ctx.fd[i]);
            g_ctx.fd[i] = -1;
            g_ctx.ok[i] = 0;
        }
    }
    // Re-enable the NMI watchdog if we disabled it.
    _restore_nmi_watchdog();
    g_n_events = NC;
    g_initialized = 0;
    g_setup_done = 0;
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