/*
 * bench.cpp  --  Multi-run benchmark, last / avg / min table.
 *
 * Build:
 *   g++ -O3 -march=native -funroll-loops bench.cpp time.o -o bench
 *
 * Setup (once per boot):
 *   echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 */

#include "picoperf.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>

constexpr int WARMUP = 1000;  // Stabilize branch predictor, TLB, caches
constexpr int RUNS = 1000;    // More samples for better percentile accuracy

__attribute__((noinline))
static uint64_t sum_array(const std::vector<uint32_t>& data) {
    uint64_t s = 0;
    for (const auto& val : data) s += val;
    return s;
}

static volatile uint64_t sink;

constexpr int LCOL = 28;
constexpr int VCOL = 12;

static void print_header(const char *label, int runs) {
    std::cout << "\n" << label << "  (" << runs << " runs)\n";
    std::cout << "  " << std::left << std::setw(LCOL) << ""
              << "  " << std::right << std::setw(VCOL) << "min"
              << "  " << std::setw(VCOL) << "p50"
              << "  " << std::setw(VCOL) << "p99"
              << "  " << std::setw(VCOL) << "p99.9" << "\n";
    std::cout << "  " << std::string(LCOL + 4*(VCOL+2), '-') << "\n";
}

static void print_sep() {
    std::cout << "  " << std::string(LCOL + 4*(VCOL+2), '-') << "\n";
}

static void row_u(const char *lbl, uint64_t min, uint64_t p50, uint64_t p99, uint64_t p999) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << min
              << "  " << std::setw(VCOL) << p50
              << "  " << std::setw(VCOL) << p99
              << "  " << std::setw(VCOL) << p999 << "\n";
}

static void row_f(const char *lbl, double min, double p50, double p99, double p999) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << std::fixed << std::setprecision(3) << min
              << "  " << std::setw(VCOL) << std::fixed << std::setprecision(3) << p50
              << "  " << std::setw(VCOL) << std::fixed << std::setprecision(3) << p99
              << "  " << std::setw(VCOL) << std::fixed << std::setprecision(3) << p999 << "\n";
}

static void row_pct(const char *lbl, double min, double p50, double p99, double p999) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << min << "%"
              << "  " << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << p50 << "%"
              << "  " << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << p99 << "%"
              << "  " << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << p999 << "%" << "\n";
}

static void row_na(const char *lbl) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << "n/a"
              << "  " << std::setw(VCOL) << "n/a"
              << "  " << std::setw(VCOL) << "n/a"
              << "  " << std::setw(VCOL) << "n/a" << "\n";
}

// Helper to extract percentiles from sorted array
static uint64_t percentile_u64(const std::vector<uint64_t>& sorted, double pct) {
    size_t idx = (size_t)((sorted.size() - 1) * pct / 100.0);
    return sorted[idx];
}

static double percentile_dbl(const std::vector<double>& sorted, double pct) {
    size_t idx = (size_t)((sorted.size() - 1) * pct / 100.0);
    return sorted[idx];
}

static void print_table(const char *label, const BenchResult *res, int n) {
    if (n <= 0) return;

    // Extract and sort all metrics for percentile calculation
    std::vector<uint64_t> tsc_vals, ns_vals, cy_vals, in_vals, bm_vals, br_vals, l1d_vals, l3_vals;
    std::vector<double> ipc_vals, bmr_vals;
    
    for (int i = 0; i < n; i++) {
        tsc_vals.push_back(res[i].tsc_ticks);
        ns_vals.push_back(res[i].nanoseconds);
        cy_vals.push_back(res[i].c[CI_CYCLES]);
        in_vals.push_back(res[i].c[CI_INSTRS]);
        bm_vals.push_back(res[i].c[CI_BMISSES]);
        br_vals.push_back(res[i].c[CI_BRANCHES]);
        l1d_vals.push_back(res[i].c[CI_L1D_MISS]);
        l3_vals.push_back(res[i].c[CI_L3_MISS]);
        if (res[i].c[CI_CYCLES] && res[i].c[CI_INSTRS])
            ipc_vals.push_back(res[i].ipc);
        if (res[i].c[CI_BRANCHES])
            bmr_vals.push_back(res[i].branch_miss_pct);
    }
    
    std::sort(tsc_vals.begin(), tsc_vals.end());
    std::sort(ns_vals.begin(), ns_vals.end());
    std::sort(cy_vals.begin(), cy_vals.end());
    std::sort(in_vals.begin(), in_vals.end());
    std::sort(bm_vals.begin(), bm_vals.end());
    std::sort(br_vals.begin(), br_vals.end());
    std::sort(l1d_vals.begin(), l1d_vals.end());
    std::sort(l3_vals.begin(), l3_vals.end());
    std::sort(ipc_vals.begin(), ipc_vals.end());
    std::sort(bmr_vals.begin(), bmr_vals.end());
    
    print_header(label, n);

    // Instructions
    if (res[0].ok[CI_INSTRS])
        row_u("instructions", percentile_u64(in_vals, 0), percentile_u64(in_vals, 50),
              percentile_u64(in_vals, 99), percentile_u64(in_vals, 99.9));
    else
        row_na("instructions");

    // Cycles
    if (res[0].ok[CI_CYCLES])
        row_u("cycles", percentile_u64(cy_vals, 0), percentile_u64(cy_vals, 50),
              percentile_u64(cy_vals, 99), percentile_u64(cy_vals, 99.9));
    else
        row_na("cycles");
    
    // IPC
    if (!ipc_vals.empty())
        row_f("IPC", percentile_dbl(ipc_vals, 0), percentile_dbl(ipc_vals, 50),
              percentile_dbl(ipc_vals, 99), percentile_dbl(ipc_vals, 99.9));
    else
        row_na("IPC");

    print_sep();

    // Branches
    if (res[0].ok[CI_BMISSES] && res[0].ok[CI_BRANCHES]) {
        row_u("branch misses", percentile_u64(bm_vals, 0), percentile_u64(bm_vals, 50),
              percentile_u64(bm_vals, 99), percentile_u64(bm_vals, 99.9));
        if (!bmr_vals.empty())
            row_pct("branch miss rate", percentile_dbl(bmr_vals, 0), percentile_dbl(bmr_vals, 50),
                    percentile_dbl(bmr_vals, 99), percentile_dbl(bmr_vals, 99.9));
        else
            row_na("branch miss rate");
    } else {
        row_na("branch misses");
        row_na("branch miss rate");
    }

    print_sep();

    // Cache
    if (res[0].ok[CI_L1D_MISS])
        row_u("L1D misses", percentile_u64(l1d_vals, 0), percentile_u64(l1d_vals, 50),
              percentile_u64(l1d_vals, 99), percentile_u64(l1d_vals, 99.9));
    else
        row_na("L1D misses");
    
    if (res[0].ok[CI_L3_MISS])
        row_u("L3 misses", percentile_u64(l3_vals, 0), percentile_u64(l3_vals, 50),
              percentile_u64(l3_vals, 99), percentile_u64(l3_vals, 99.9));
    else
        row_na("L3 misses");

    print_sep();

    row_u("tsc ticks", percentile_u64(tsc_vals, 0), percentile_u64(tsc_vals, 50),
          percentile_u64(tsc_vals, 99), percentile_u64(tsc_vals, 99.9));
    row_u("nanoseconds", percentile_u64(ns_vals, 0), percentile_u64(ns_vals, 50),
          percentile_u64(ns_vals, 99), percentile_u64(ns_vals, 99.9));

    std::cout << "\n";
}

static void run(const char *label, const std::vector<uint32_t>& data) {
    for (int i = 0; i < WARMUP; ++i) 
        sink = sum_array(data);

    std::vector<BenchResult> results(RUNS);

    for (int i = 0; i < RUNS; ++i) {
        start_measuring();
        uint64_t s = sum_array(data);
        KEEP(s);
        results[i] = stop_measuring();
        sink = s;
    }
    print_table(label, results.data(), RUNS);
}

int main() {
    picoperf_setup(0);  // pin, mlockall, SCHED_FIFO, sysfs checks, init counters

    constexpr size_t NS =     4*1024;        // 16 KB -- L1 resident
    constexpr size_t NM =  1024*1024;        //  4 MB -- spills to L3
    constexpr size_t NL = 16*1024*1024;      // 64 MB -- DRAM pressure

    std::vector<uint32_t> small(NS);
    std::vector<uint32_t> medium(NM);
    std::vector<uint32_t> large(NL);

    for (size_t i = 0; i < NS; ++i) 
        small[i] = static_cast<uint32_t>(i * 2654435761ULL);
    for (size_t i = 0; i < NM; ++i) 
        medium[i] = static_cast<uint32_t>(i * 2654435761ULL);
    for (size_t i = 0; i < NL; ++i) 
        large[i] = static_cast<uint32_t>(i * 2654435761ULL);

    run("SMALL   16 KB  (L1 resident)",   small);
    run("MEDIUM   4 MB  (spills L2->L3)", medium);
    run("LARGE   64 MB  (DRAM pressure)", large);

    return 0;
}