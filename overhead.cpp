/*
 * overhead.cpp  --  Measure and display measurement overhead
 *
 * This program demonstrates:
 * 1. CPU pinning for deterministic measurements
 * 2. Overhead measurement (empty start/stop)
 * 3. Percentile analysis (min, p50, p99, p99.9)
 *
 * Build & run:  make overhead
 *
 * One-time per boot (lets you measure without sudo):
 *   echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
 */

#include "picoperf.h"
#include <iostream>

int main() {
    // Pin to CPU 0, lock pages, ask for real-time priority, check the environment.
    picoperf_setup(0);
    
    std::cout << "\n=== Measurement Overhead Analysis ===\n\n";
    std::cout << "This measures the cost of start_measuring() -> stop_measuring()\n";
    std::cout << "with NOTHING in between. This is the floor of measurement accuracy.\n\n";
    
    // Measure overhead with 10,000 iterations
    print_overhead_stats(10000);
    
    std::cout << "Key Takeaways:\n";
    std::cout << "  - Your measurements will ALWAYS include this overhead\n";
    std::cout << "  - For workloads > 1000ns, overhead is negligible (< 5%)\n";
    std::cout << "  - For sub-100ns workloads, overhead dominates the measurement\n";
    std::cout << "  - p99 shows worst-case (cache miss, branch misprediction)\n\n";
    
    return 0;
}
