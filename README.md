# PicoPerf

Lightweight hardware performance counter library for x86-64 Linux. Measures cycles, instructions, cache misses, and branch mispredictions with nanosecond precision using TSC and `perf_event_open`.

Built for low-latency applications where every cycle matters.

## Quick Start

```bash
# Setup (once per boot)
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# Build and run
make run
```

## What It Measures

- **CPU cycles** - actual work done (frequency-dependent)
- **Instructions** - retired instructions
- **Branches** - branch instructions executed
- **Branch misses** - mispredictions (15-20 cycle penalty)
- **L1D misses** - L1 data cache misses
- **L3 misses** - last-level cache misses (200+ cycle DRAM fetch)
- **TSC ticks** - time-stamp counter (frequency-independent)
- **Nanoseconds** - wall-clock time

Plus derived metrics: IPC, CPI, branch miss rate

## Example

```cpp
#include "picoperf.h"

__attribute__((noinline))
uint64_t sum_array(const uint32_t *data, size_t n) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += data[i];
    return sum;
}

int main() {
    uint32_t data[1000000];
    // ... initialize data ...
    
    start_measuring();
    uint64_t result = sum_array(data, 1000000);
    KEEP(result);  // prevent dead code elimination
    BenchResult r = stop_measuring();
    
    print_measured_results(r);
    return 0;
}
```

## Build Targets

```bash
make run      # Single run
make bench    # 1000 runs with percentiles
make overhead # Measure measurement overhead
```

## System Setup

### Required (once per boot)

```bash
# Allow unprivileged access to performance counters
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

### Optional (for consistent results)

```bash
# Set CPU governor to performance mode
sudo cpupower frequency-set -g performance

# For AMD CPUs: disable boost
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost

# For Intel CPUs: disable turbo
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
```

### Verify Setup

```bash
# Check governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# Check boost status (AMD)
cat /sys/devices/system/cpu/cpufreq/boost

# Check turbo status (Intel)
cat /sys/devices/system/cpu/intel_pstate/no_turbo
```

## API

```c
void start_measuring(void);
BenchResult stop_measuring(void);
void print_measured_results(BenchResult r);
void pin_to_cpu(int cpu_id);
BenchResult measure_overhead(void);
void print_overhead_stats(int iterations);
```

### Important Macros

```c
KEEP(x)  // Prevent dead code elimination
__attribute__((noinline))  // Prevent function inlining
```

## How It Works

### TSC (Time-Stamp Counter)

- Ticks at fixed frequency regardless of CPU frequency scaling
- Serialized with `lfence` and `rdtscp` to prevent reordering
- Converted to nanoseconds using detected TSC frequency

### PMU Counters

- Uses `perf_event_open(2)` syscall
- 6 counters fit in hardware (no multiplexing)
- Atomic start/stop via `ioctl`

### Measurement Window

```
lfence → rdtsc → enable counters → YOUR CODE → disable counters → rdtscp → lfence
```

All counters cover exactly the same instruction window.

## Caveats

- **x86-64 Linux only** - uses `rdtsc`/`rdtscp` and `perf_event_open`
- **No overhead subtraction** - raw measurements only (overhead is ~30-60 TSC ticks)
- **Requires kernel 2.6.31+** - for `perf_event_open` support
- **6 counter limit** - more counters cause kernel multiplexing (less accurate)

## Files

- `picoperf.h` - Public API
- `picoperf.c` - Implementation
- `single.cpp` - Single-run example
- `bench.cpp` - Statistical benchmark (1000 runs, percentiles)
- `overhead.cpp` - Overhead measurement tool

## Performance Tips

1. **Pin to CPU** - `pin_to_cpu(0)` prevents thread migration
2. **Warmup** - Run workload 1000 times before measuring
3. **Disable frequency scaling** - Set governor to `performance`
4. **Mark functions noinline** - Prevents compiler from moving code
5. **Use KEEP()** - Prevents dead code elimination

## Measurement Overhead

Run `make overhead` to measure the cost of `start_measuring()` → `stop_measuring()` with nothing in between.

Typical overhead:
- min: ~10ns (best case)
- p50: ~20ns (median)
- p99: ~50ns (worst case)

For workloads > 1µs, overhead is negligible (< 5%).

## Percentiles

The bench target reports min, p50, p99, and p99.9:

- **min** - best case
- **p50** - median (half faster, half slower)
- **p99** - 99% of runs faster than this
- **p99.9** - 99.9% of runs faster than this

p99 and p99.9 show tail latency - critical for understanding worst-case performance.

## Why 6 Counters?

Modern CPUs have limited hardware counters:
- Intel Skylake+: 4 general + 3 fixed = 7 total
- AMD Zen 2/3: 6 general counters

Opening more counters than available causes kernel multiplexing - counters are time-sliced and values are estimated. This reduces accuracy, especially for short workloads.

6 counters fit in hardware on all modern CPUs without multiplexing.
