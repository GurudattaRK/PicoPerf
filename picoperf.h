// picoperf.h - Low-latency hardware counter measurement
// x86-64 Linux only

#pragma once
#include <stdint.h>
#include <stddef.h>

// Prevent dead code elimination
#define KEEP(x) __asm__ volatile("" : "+r,m"(x) :: "memory")

// Counter indices
#define CI_CYCLES     0
#define CI_INSTRS     1
#define CI_BRANCHES   2
#define CI_BMISSES    3
#define CI_L1D_MISS   4
#define CI_L3_MISS    5
#define NC            6

typedef struct {
    uint64_t tsc_ticks;
    uint64_t nanoseconds;
    uint64_t c[NC];
    double   ipc;
    double   cpi;
    double   branch_miss_pct;
    int      ok[NC];
} BenchResult;

#ifdef __cplusplus
extern "C" {
#endif

void        start_measuring(void);
BenchResult stop_measuring(void);
void        print_measured_results(BenchResult r);
void        pin_to_cpu(int cpu_id);
void        picoperf_setup(int cpu_id);
void        picoperf_lock_memory(void *addr, size_t len);
void        picoperf_fini(void);
BenchResult measure_overhead(void);
void        print_overhead_stats(int iterations);

#ifdef __cplusplus
}
#endif
