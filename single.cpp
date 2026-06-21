/*
 * single.cpp  --  Run workload once, print results. No warmup.
 *
 * Build:
 *   g++ -O3 -march=native -funroll-loops single.cpp time.o -o single
 *
 * Setup (once per boot):
 *   echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 */

#include "picoperf.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>

__attribute__((noinline))
static uint64_t sum_array(const std::vector<uint32_t>& data) {
    uint64_t s = 0;
    for (const auto& val : data) s += val;
    return s;
}

constexpr int COL = 32;

static void print_u(const char *label, uint64_t v) {
    std::cout << "  " << std::left << std::setw(COL) << label 
              << std::right << std::setw(16) << v << "\n";
}
static void print_f(const char *label, double v) {
    std::cout << "  " << std::left << std::setw(COL) << label 
              << std::right << std::setw(16) << std::fixed << std::setprecision(3) << v << "\n";
}
static void print_pct(const char *label, double v) {
    std::cout << "  " << std::left << std::setw(COL) << label 
              << std::right << std::setw(15) << std::fixed << std::setprecision(4) << v << "%\n";
}
static void print_na(const char *label) {
    std::cout << "  " << std::left << std::setw(COL) << label 
              << std::right << std::setw(16) << "n/a" << "\n";
}
static void print_sep() {
    std::cout << "  " << std::string(COL + 18, '-') << "\n";
}

static void print_result(const char *label, const BenchResult *r) {
    std::cout << "\n" << label << "\n";
    print_sep();

    r->ok[CI_INSTRS] ? print_u("instructions", r->c[CI_INSTRS]) : print_na("instructions");

    r->ok[CI_CYCLES] ? print_u("cycles", r->c[CI_CYCLES]) : print_na("cycles");
    if (r->ok[CI_CYCLES] && r->ok[CI_INSTRS] && r->c[CI_CYCLES] && r->c[CI_INSTRS])
        print_f("IPC", r->ipc);
    else
        print_na("IPC");

    print_sep();

    if (r->ok[CI_BMISSES] && r->ok[CI_BRANCHES]) {
        print_u("branch misses", r->c[CI_BMISSES]);
        if (r->c[CI_BRANCHES])
            print_pct("branch miss rate", r->branch_miss_pct);
        else
            print_na("branch miss rate");
    } else {
        print_na("branch misses");
        print_na("branch miss rate");
    }

    print_sep();

    r->ok[CI_L1D_MISS] ? print_u("L1D misses", r->c[CI_L1D_MISS]) : print_na("L1D misses");
    r->ok[CI_L3_MISS] ? print_u("L3 misses", r->c[CI_L3_MISS]) : print_na("L3 misses");

    print_sep();

    print_u("tsc ticks", r->tsc_ticks);
    print_u("nanoseconds", r->nanoseconds);

    std::cout << "\n";
}

int main() {
    constexpr size_t N = 1024 * 1024;
    std::vector<uint32_t> data(N);
    
    for (size_t i = 0; i < N; ++i)
        data[i] = static_cast<uint32_t>(i * 2654435761ULL);

    start_measuring();
    uint64_t s = sum_array(data);
    KEEP(s);
    BenchResult r = stop_measuring();

    print_result("sum_array  4 MB  (single run, no warmup)", &r);

    return 0;
}