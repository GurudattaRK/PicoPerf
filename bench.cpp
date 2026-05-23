/*
 * bench.cpp  --  Multi-run benchmark, last / avg / min table.
 *
 * Build:
 *   g++ -O3 -march=native -funroll-loops bench.cpp time.o -o bench
 *
 * Setup (once per boot):
 *   echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 */

#include "time.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <string>

constexpr int WARMUP = 20;
constexpr int RUNS = 100;

__attribute__((noinline))
static uint64_t sum_array(const std::vector<uint32_t>& data) {
    uint64_t s = 0;
    for (const auto& val : data) s += val;
    return s;
}

static volatile uint64_t sink;

constexpr int LCOL = 28;
constexpr int VCOL = 14;

static void print_header(const char *label, int runs) {
    std::cout << "\n" << label << "  (" << runs << " runs)\n";
    std::cout << "  " << std::left << std::setw(LCOL) << ""
              << "  " << std::right << std::setw(VCOL) << "last"
              << "  " << std::setw(VCOL) << "avg"
              << "  " << std::setw(VCOL) << "min" << "\n";
    std::cout << "  " << std::string(LCOL + 3*(VCOL+2), '-') << "\n";
}

static void print_sep() {
    std::cout << "  " << std::string(LCOL + 3*(VCOL+2), '-') << "\n";
}

static void row_u(const char *lbl, uint64_t last, uint64_t avg, uint64_t min) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << last
              << "  " << std::setw(VCOL) << avg
              << "  " << std::setw(VCOL) << min << "\n";
}

static void row_f(const char *lbl, double last, double avg) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << std::fixed << std::setprecision(3) << last
              << "  " << std::setw(VCOL) << std::fixed << std::setprecision(3) << avg
              << "  " << std::setw(VCOL) << "-" << "\n";
}

static void row_pct(const char *lbl, double last, double avg) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << last << "%"
              << "  " << std::setw(VCOL-1) << std::fixed << std::setprecision(4) << avg << "%"
              << "  " << std::setw(VCOL) << "-" << "\n";
}

static void row_na(const char *lbl) {
    std::cout << "  " << std::left << std::setw(LCOL) << lbl
              << "  " << std::right << std::setw(VCOL) << "n/a"
              << "  " << std::setw(VCOL) << "n/a"
              << "  " << std::setw(VCOL) << "n/a" << "\n";
}

static void print_table(const char *label, const BenchResult *res, int n) {
    if (n <= 0) return;
    const BenchResult *last = &res[n - 1];

    /* accumulate sums and mins */
    uint64_t s_tsc=0, s_cy=0, s_in=0, s_bm=0, s_br=0, s_l1=0;
    uint64_t m_tsc=~0ULL, m_cy=~0ULL, m_in=~0ULL;
    uint64_t m_bm=~0ULL, m_br=~0ULL, m_l1=~0ULL;

    for (int i = 0; i < n; i++) {
        s_tsc += res[i].tsc_ticks;
        s_cy  += res[i].c[CI_CYCLES];
        s_in  += res[i].c[CI_INSTRS];
        s_bm  += res[i].c[CI_BMISSES];
        s_br  += res[i].c[CI_BRANCHES];
        s_l1  += res[i].c[CI_L1D_MISS];
        if (res[i].tsc_ticks      < m_tsc) m_tsc = res[i].tsc_ticks;
        if (res[i].c[CI_CYCLES]   < m_cy)  m_cy  = res[i].c[CI_CYCLES];
        if (res[i].c[CI_INSTRS]   < m_in)  m_in  = res[i].c[CI_INSTRS];
        if (res[i].c[CI_BMISSES]  < m_bm)  m_bm  = res[i].c[CI_BMISSES];
        if (res[i].c[CI_BRANCHES] < m_br)  m_br  = res[i].c[CI_BRANCHES];
        if (res[i].c[CI_L1D_MISS] < m_l1)  m_l1  = res[i].c[CI_L1D_MISS];
    }

    uint64_t N = (uint64_t)n;
    uint64_t a_tsc = s_tsc/N, a_cy = s_cy/N, a_in = s_in/N;
    uint64_t a_bm  = s_bm/N,  a_br = s_br/N;
    uint64_t a_l1  = s_l1/N;

    double ipc_last=0, ipc_avg=0;
    if (last->c[CI_CYCLES] && last->c[CI_INSTRS])
        ipc_last = last->ipc;
    if (a_cy && a_in)
        ipc_avg = (double)a_in / (double)a_cy;

    double bmr_last=0, bmr_avg=0;
    if (last->c[CI_BRANCHES]) bmr_last = last->branch_miss_pct;
    if (a_br)           bmr_avg  = 100.0*(double)a_bm/(double)a_br;

    print_header(label, n);

    /* instructions */
    last->ok[CI_INSTRS] ? row_u("instructions", last->c[CI_INSTRS], a_in, m_in)
             : row_na("instructions");

    /* cycles + IPC */
    last->ok[CI_CYCLES] ? row_u("cycles", last->c[CI_CYCLES], a_cy, m_cy)
             : row_na("cycles");
    if (last->ok[CI_CYCLES] && last->ok[CI_INSTRS] && a_cy && a_in)
        row_f("IPC", ipc_last, ipc_avg);
    else
        row_na("IPC");

    print_sep();

    /* branches */
    if (last->ok[CI_BMISSES] && last->ok[CI_BRANCHES]) {
        row_u("branch misses", last->c[CI_BMISSES], a_bm, m_bm);
        row_pct("branch miss rate", bmr_last, bmr_avg);
    } else {
        row_na("branch misses");
        row_na("branch miss rate");
    }

    print_sep();

    /* cache */
    last->ok[CI_L1D_MISS] ? row_u("L1D misses",  last->c[CI_L1D_MISS], a_l1,  m_l1)  : row_na("L1D misses");
    row_na("LLC misses");

    print_sep();

    row_u("tsc ticks", last->tsc_ticks, a_tsc, m_tsc);

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