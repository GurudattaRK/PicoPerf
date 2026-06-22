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
    picoperf_setup(0);  // pin to CPU, mlockall, SCHED_FIFO, environment checks

    uint32_t data[1000000];
    // ... initialize data ...

    start_measuring();
    uint64_t result = sum_array(data, 1000000);
    KEEP(result);  // prevent dead code elimination
    BenchResult r = stop_measuring();

    print_measured_results(r);
    picoperf_fini();
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

## Root-Level Tuning (Recommended for Higher Accuracy)

`picoperf_setup()` automatically attempts several system optimizations and warns
to stderr when they fail or the environment is misconfigured. **Run as root for
full effect.**

### What `picoperf_setup` does automatically

| Action | Root needed? | Effect |
|--------|-------------|--------|
| `sched_setaffinity` (CPU pin) | No | Prevents thread migration between cores |
| `mlockall(MCL_CURRENT\|MCL_FUTURE)` | Yes (or `ulimit -l unlimited`) | Locks all pages in RAM, prevents page faults mid-measurement |
| `sched_setscheduler(SCHED_FIFO)` | Yes | Real-time priority, OS scheduler won't preempt the thread |
| sysfs SMT check | No (read-only) | Warns if Hyper-Threading is active |
| sysfs governor check | No (read-only) | Warns if CPU frequency scaling is on |
| sysfs turbo/boost check | No (read-only) | Warns if turbo/boost can skew cycle counts |

### What requires manual root intervention

These cannot be done from user code and must be configured system-wide:

```bash
# 1. Disable SMT/Hyper-Threading (sibling shares PMU, pollutes counts)
echo off | sudo tee /sys/devices/system/cpu/smt/control

# 2. Isolate benchmark CPU from kernel scheduler and IRQs
#    Add to kernel boot params (e.g. GRUB_CMDLINE_LINUX in /etc/default/grub):
#    isolcpus=3 nohz_full=3 rcu_nocbs=3
#    Then rebuild grub: sudo update-grub && sudo reboot

# 3. Pin all IRQs away from the isolated core (after isolcpus)
sudo systemctl stop irqbalance
for irq in /proc/irq/*/smp_affinity; do
    echo 7 | sudo tee $irq > /dev/null  # avoid CPU 3 (binary 0111 = cores 0,1,2)
done

# 4. Disable CPU frequency scaling (governor + turbo/boost)
sudo cpupower frequency-set -g performance
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo  # Intel
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost          # AMD
```

### Accuracy without root

Running without root still gives useful results:
- CPU pinning works (no root required)
- Multiplexing is detected and aborts immediately - no silent data corruption
- Group leader mode ensures all 6 counters start/stop atomically
- Remaining noise sources: OS scheduler preemption, IRQs hitting your core,
  page faults, turbo frequency jitter

For workloads > 10µs, non-root results are reliable. For sub-microsecond
workloads, the full root setup above is required for p99/p99.9 accuracy.

## API

```c
// Core measurement
void        start_measuring(void);          // reset+enable all counters, read TSC
BenchResult stop_measuring(void);           // read TSC, disable all counters, read counts
void        print_measured_results(BenchResult r);

// Setup (call once before any measurement)
void        picoperf_setup(int cpu_id);     // pin + mlockall + SCHED_FIFO + sysfs checks
void        picoperf_fini(void);            // close all perf fds, reset state

// Lower-level
void        pin_to_cpu(int cpu_id);         // affinity only, no other setup
BenchResult measure_overhead(void);         // cost of empty start→stop
void        print_overhead_stats(int n);    // percentile overhead report over n iterations
```

### Error behaviour

- **Multiplexing detected** (`time_running < time_enabled`): prints to stderr and `exit(1)`.
  This means counter values would be interpolated, not real hardware reads.
- **`read()` failure on a counter fd**: prints to stderr and `exit(1)`.
- **Group leader open failure** (cycles counter): prints instructions and `exit(1)`.
- Member counters that fail to open are silently marked unavailable (`ok[i]=0`).

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
- **Group leader mode**: `CI_CYCLES` is the group leader; the other 5 counters are
  group members. One `ioctl` with `PERF_IOC_FLAG_GROUP` starts and stops all 6 atomically
- 6 counters fit in hardware on all modern x86 without multiplexing (Intel uses
  fixed-function counters for cycles+instructions, leaving 4 general-purpose PMCs
  for the rest; AMD Zen 2/3/4 has 6 general-purpose PMCs)
- Each counter fd is opened with `PERF_FORMAT_TOTAL_TIME_ENABLED |
  PERF_FORMAT_TOTAL_TIME_RUNNING`; if `time_running < time_enabled`, multiplexing
  is detected and the process aborts immediately

### Measurement Window

```
lfence → rdtsc → [all 6 counters enabled atomically] → YOUR CODE → [all 6 counters disabled atomically] → rdtscp → lfence
```

All counters see the same instruction window. The TSC window is slightly narrower
than the PMU window by one `ioctl` round-trip at each end — this is the
irreducible overhead measured by `measure_overhead()`.

## Caveats

- **x86-64 Linux only** - uses `rdtsc`/`rdtscp` and `perf_event_open`
- **No overhead subtraction** - raw measurements only; use `measure_overhead()` to
  characterize the floor and subtract manually if needed
- **Requires kernel 2.6.31+** - for `perf_event_open` support
- **6 counter limit** - more counters cause kernel multiplexing; PicoPerf detects
  multiplexing and aborts rather than returning interpolated values
- **`exit(1)` on any data integrity failure** - no silent garbage values. Multiplexing,
  counter read errors, and group leader open failures all abort immediately

## Files

- `picoperf.h` - Public API
- `picoperf.c` - Implementation
- `single.cpp` - Single-run example
- `bench.cpp` - Statistical benchmark example(1000 runs, percentiles)
- `overhead.cpp` - Overhead measurement tool

## Performance Tips

1. **Use `picoperf_setup(0)`** - call once at program start; handles pinning,
   memory locking, real-time scheduling, and environment validation in one call
2. **Warmup** - Run workload 1000 times before measuring (see `bench.cpp`)
3. **Disable frequency scaling** - Set governor to `performance` and disable turbo/boost
4. **Mark measured functions `noinline`** - Prevents compiler from moving code
   across the start/stop boundary
5. **Use `KEEP(result)`** - Prevents the compiler from eliminating your workload
   as dead code
6. **Run as root** - Enables `mlockall` and `SCHED_FIFO` for lowest jitter
7. **Isolate the core** - `isolcpus=N` boot param removes the core from OS
   scheduler and interrupt routing entirely

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
