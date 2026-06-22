# PicoPerf

Lightweight hardware performance counter library for x86-64 Linux. Measures cycles, instructions, cache misses, and branch mispredictions with nanosecond precision using TSC and `perf_event_open`.

Built for low-latency applications where every cycle matters.

## Quick Start

```bash
# One-time per boot: let non-root programs read performance counters
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# Build and run
make run
```

That's enough to get going. For the most accurate numbers (and all six counters
at once), run as root and tune the machine first — see
[Running it properly](#running-it-properly).

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
    picoperf_setup(0);  // optional - picks the core; start_measuring() would call this for you

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

## Setup happens automatically

You don't have to call any setup function. The first time you call
`start_measuring()`, PicoPerf does the one-time groundwork for you:

- pins the thread to a CPU (core 0 by default),
- locks its pages in RAM so a page fault can't stall a measurement,
- asks the kernel for real-time priority (`SCHED_FIFO`),
- and prints a one-line heads-up for anything in the environment that would add
  jitter (frequency governor, turbo/boost, SMT).

If you want to choose the core yourself, call `picoperf_setup(core)` once before
you measure. It's optional and idempotent — calling it (or letting
`start_measuring()` call it) more than once is harmless.

Some of this needs privilege: real-time priority and locking *future*
allocations need root (or the right `ulimit`/capabilities). Without root, PicoPerf
still pins the thread, locks current pages, and measures real counters — it just
warns about what it couldn't do.

## Running it properly

PicoPerf reads real hardware counters either way, but the numbers are steadiest
on a quiet, fixed-frequency core. Here's the full pre-flight, with `sudo`. It's
the same on Intel and AMD; the boost/turbo knob just lives in a different file.

```bash
# 1. Let programs read the counters (and run as root to get all six at once)
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid

# 2. Lock the clock to base frequency
sudo cpupower frequency-set -g performance
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost           # AMD / acpi-cpufreq
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo   # Intel pstate

# 3. Turn off SMT/Hyper-Threading so a sibling thread can't share the PMU
echo off | sudo tee /sys/devices/system/cpu/smt/control

# 4. Run as root so PicoPerf can free the watchdog's counter and use all six
sudo make run
sudo make bench
sudo make overhead
```

Why root for "all six"? See [Counters and the NMI watchdog](#counters-and-the-nmi-watchdog).

For the lowest possible tail latency you can also isolate the core from the
scheduler and interrupts (a boot-time change, optional):

```bash
# In /etc/default/grub, add to GRUB_CMDLINE_LINUX, then update-grub && reboot:
#   isolcpus=3 nohz_full=3 rcu_nocbs=3
sudo systemctl stop irqbalance
for irq in /proc/irq/*/smp_affinity; do echo 7 | sudo tee "$irq" > /dev/null; done
# then measure on core 3:  picoperf_setup(3);
```

### Accuracy without root

Without root you still get real, unscaled counts — PicoPerf just measures five
events instead of six and can't grab real-time priority. CPU pinning and current-page
locking still work. For workloads longer than ~10 µs this is plenty; for
sub-microsecond code, do the full pre-flight above as root for clean p99/p99.9.

## API

```c
// Core measurement
void        start_measuring(void);          // enable the counter group, read TSC
BenchResult stop_measuring(void);           // read TSC, disable the group, read counts
void        print_measured_results(BenchResult r);

// Setup (optional - start_measuring() calls picoperf_setup(0) for you)
void        picoperf_setup(int cpu_id);     // pin + lock pages + SCHED_FIFO + env checks
void        picoperf_fini(void);            // close counters, restore the NMI watchdog

// Lower-level
void        pin_to_cpu(int cpu_id);              // affinity only, no other setup
void        picoperf_lock_memory(void*, size_t); // mlock your data buffer (non-root)
BenchResult measure_overhead(void);              // cost of an empty start->stop
void        print_overhead_stats(int n);         // percentile overhead over n iterations
```

Each `BenchResult` has an `ok[i]` flag per counter. A disabled event (for
example `l3_miss` when you're not root) reports `ok[i] == 0` and prints as `n/a`
rather than a misleading zero.

### How it reports problems

- **Not enough counters** for all six events (the NMI watchdog is holding one and
  you're not root): PicoPerf drops the lowest-priority event (`l3_miss`) so the
  rest fit with no multiplexing, and prints which events are off and how to get
  them back. It never multiplexes and never scales.
- **The measured region got descheduled** (a context switch slid the counters
  off the PMU): the counts are reported raw and a one-time warning tells you the
  sample was disturbed. Nothing is scaled or invented.
- **TSC frequency can't be determined**: `exit(1)`, because nanosecond
  conversion would be meaningless (raw TSC ticks would still be valid).

### Important Macros

```c
KEEP(x)  // Prevent dead code elimination
__attribute__((noinline))  // Prevent function inlining
```

## How It Works

### TSC (Time-Stamp Counter) — and why the frequency is detected once

The TSC is a free-running counter that ticks at a fixed rate no matter what the
CPU clock is doing. PicoPerf reads it with `lfence` + `rdtsc`/`rdtscp` around your
code, and the tick delta is a **real, live hardware read on every single
measurement** — nothing cached, nothing estimated.

The only thing detected once per process is the TSC *frequency* (ticks per
second), which is just the constant used to turn ticks into nanoseconds. It's a
fixed property of the machine, so there's no reason to re-measure it each run.
PicoPerf gets it from the kernel when that's exposed
(`/sys/devices/system/cpu/cpu0/tsc_freq_khz`), otherwise it calibrates once
against `CLOCK_MONOTONIC`. If you only trust raw hardware, read `tsc_ticks` — the
`nanoseconds` field is just `tsc_ticks ÷ frequency`.

### PMU counters

- All events are opened as **one group** led by the cycles counter, so a single
  `ioctl` with `PERF_IOC_FLAG_GROUP` starts and stops every counter on the same
  instruction window.
- At startup PicoPerf opens the **largest group that physically fits the PMU
  with zero multiplexing** and verifies it (a group that needs more counters than
  are free is never scheduled). On a typical box that's all six events; if the
  NMI watchdog is holding a counter and you're not root, it's five (`l3_miss`
  dropped). Counts are always raw — never time-sliced, never scaled.
- This is the same on Intel and AMD. Intel spends fixed-function counters on
  cycles and instructions and has ~4 general-purpose PMCs; AMD Zen 2/3/4 has 6
  general-purpose PMCs. Either way PicoPerf just uses what fits.

### Measurement window

```
lfence → rdtsc → [counter group enabled] → YOUR CODE → [counter group disabled] → rdtscp → lfence
```

Every counter sees the same window. The TSC window is narrower than the PMU
window by one `ioctl` at each end — that gap is the irreducible overhead you can
measure with `measure_overhead()`.

## Caveats

- **x86-64 Linux only** — uses `rdtsc`/`rdtscp` and `perf_event_open` (kernel 2.6.31+).
- **User-space counts.** Counters are opened with `exclude_kernel`, so time spent
  in syscalls, page faults, and the kernel is *not* counted. Great for tight CPU
  code; if your section makes heavy syscalls, expect cycles/instructions to look
  lower than wall-clock suggests.
- **One thread at a time.** State is a single global context — measure from one
  thread; don't nest or overlap start/stop.
- **No automatic overhead subtraction** — measurements are raw; use
  `measure_overhead()` to characterize the floor and subtract if you care.
- **Six counters max** by design — PicoPerf fits the group to the hardware
  instead of multiplexing, so you never get interpolated values.

## Files

- `picoperf.h` - Public API
- `picoperf.c` - Implementation
- `single.cpp` - Single-run example
- `bench.cpp` - Statistical benchmark example(1000 runs, percentiles)
- `overhead.cpp` - Overhead measurement tool

## Performance Tips

1. **Setup is automatic** - `start_measuring()` pins, locks pages, and grabs
   real-time priority on first use. Call `picoperf_setup(core)` yourself only if
   you want to choose the core.
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

## Counters and the NMI watchdog

PicoPerf measures six events, and modern CPUs have enough counters for all six:

- Intel Skylake+: 3 fixed-function + 4 general-purpose
- AMD Zen 2/3/4: 6 general-purpose

The catch is the Linux **NMI watchdog** (`/proc/sys/kernel/nmi_watchdog`). The
hard-lockup detector quietly keeps one general-purpose counter for itself on
every core. That's usually fine, but it leaves one fewer counter for us — and a
perf group is all-or-nothing, so a six-event group simply won't schedule when
the watchdog is holding a counter.

PicoPerf handles this honestly instead of multiplexing:

- **As root**, it disables the watchdog before opening counters (and puts it
  back in `picoperf_fini()` / on exit), freeing the counter so all six events fit.
- **Without root**, it can't touch the watchdog, so it opens five events
  (dropping `l3_miss`) — every one a real, unscaled count — and tells you that
  running as root brings the sixth back.

Either way you never get time-sliced or estimated values.
