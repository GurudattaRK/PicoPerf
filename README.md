# LLHPC - Low-Latency Hardware Performance Counter Benchmarking

A minimal, zero-overhead benchmarking library for Linux x86-64 that reads **hardware PMU (Performance Monitoring Unit) counters** directly. No `clock_gettime`, no `rdtsc` guesswork — just real CPU counters: instructions retired, cycles, cache misses, branch misses, and more.

## What It Does

This project measures your code using the CPU's built-in hardware performance counters via Linux `perf_event_open(2)`. Unlike software timers, these counters are counted by the CPU itself, giving you nanosecond-precise, deterministic measurements of exactly what your code did.

**Measured metrics:**
- **Instructions** retired
- **Cycles** elapsed
- **IPC** (instructions per cycle)
- **Branches** and **branch misses**
- **L1D read references** and **L1D read misses**
- **L1 instruction cache misses**
- **DTLB** and **ITLB misses**
- **TSC ticks** (high-resolution timestamp)

## Quick Start

### 1. One-Time System Setup

The kernel restricts access to hardware PMU counters. Run this once per boot:

```bash
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

> **Why?** `perf_event_paranoid` defaults to `2` (only allow user-space perf for root). `-1` disables the restriction entirely so any user can open hardware counters.

To make it permanent across reboots:

```bash
echo 'kernel.perf_event_paranoid = -1' | sudo tee -a /etc/sysctl.d/99-perf.conf
sudo sysctl --system
```

### 2. Build

```bash
git clone <your-repo-url>
cd LLHPC
make run    # plain build, runs single-run benchmark
make bench  # plain build, runs multi-run benchmark
```

For profile-guided optimization (PGO), which produces faster binaries:

```bash
make pgo       # PGO build, runs single
make pgo-bench # PGO build, runs benchmark
```

### 3. Output Example (`make run`)

```
Counter availability on this CPU:
  ✓ cycles
  ✓ instructions
  ✓ branches
  ✓ branch_misses
  ✓ l1d_read_refs
  ✓ l1d_read_miss
  ✓ l1i_miss
  ✓ dtlb_miss
  ✓ itlb_miss

sum_array  4 MB  (single run, no warmup)
  --------------------------------------------------
  instructions                              836112
  cycles                                    628180
  IPC                                        1.331
  --------------------------------------------------
  branch misses                                 64
  branch miss rate                         0.3880%
  --------------------------------------------------
  L1D misses                                    21
  LLC misses                                   n/a
  --------------------------------------------------
  tsc ticks                                1341210
```

## File Structure

| File | Purpose |
|------|---------|
| `time.h` | Public API header — include this in your code |
| `time.c` | Core measurement engine — compile and link with your code |
| `single.cpp` | Single-run example (measures one function call) |
| `bench.cpp` | Multi-run statistical benchmark (100 runs, reports avg/min/last) |
| `prime.c` | Example: finding the next prime number with timing |
| `makefile` | Build system with plain and PGO targets |
| `pgo_data/` | Profile data directory (auto-generated) |

## How to Use in Your Own Code

### Minimal Example (C)

```c
#include "time.h"
#include <stdio.h>

int main() {
    start_measuring();               // auto-initializes on first call

    int sum = 0;
    for (int i = 0; i < 1000000; i++)
        sum += i;
    KEEP(sum);                       // prevent dead-code elimination

    print_measured_results(stop_measuring());
    return 0;
}
```

### Minimal Example (C++)

```cpp
#include "time.h"
#include <vector>

__attribute__((noinline))
static uint64_t sum_array(const std::vector<uint32_t>& data) {
    uint64_t s = 0;
    for (auto v : data) s += v;
    return s;
}

int main() {
    std::vector<uint32_t> data(1024 * 1024);
    // ... fill data ...

    start_measuring();
    auto result = sum_array(data);
    KEEP(result);

    print_measured_results(stop_measuring());
    return 0;
}
```

### Build Your Code

```bash
# Compile time.c once
gcc -O3 -march=native -c time.c -o time.o

# Link with your C code
gcc -O3 your_code.c time.o -o your_binary

# Or your C++ code
g++ -O3 your_code.cpp time.o -o your_binary
```

### Using `make` for Your Code

Add to the `makefile`:

```make
your_binary: your_code.c time.o time.h
	$(CC) $(CFLAGS) your_code.c time.o -o your_binary
```

Then run `make your_binary`.

## API Reference

### `start_measuring()`

Resets all hardware counters, enables counting, and records the TSC start timestamp. **Automatically initializes counters on the first call** — no `init()` needed.

### `stop_measuring()`

Stops counting, reads all hardware counters and TSC end, and returns a `BenchResult` struct.

### `print_measured_results(BenchResult r)`

Pretty-prints all metrics from a `BenchResult`.

### `KEEP(x)`

Macro to prevent the compiler from deleting your computation as dead code. **Always wrap the result** of your measured code with this when compiling with `-O3`.

```c
uint64_t result = my_function();
KEEP(result);   // forces compiler to materialize the value
```

## Build Targets

| Target | Description |
|--------|-------------|
| `make run` | Plain build, runs `single.cpp` |
| `make bench` | Plain build, runs `bench.cpp` |
| `make pgo` | PGO build, runs `single.cpp` (2-pass optimized) |
| `make pgo-bench` | PGO build, runs `bench.cpp` (2-pass optimized) |
| `make clean` | Remove all binaries and profile data |

### What is PGO?

Profile-Guided Optimization. The build runs your code twice:

1. **Pass 1**: Compiles with instrumentation (`-fprofile-generate`), runs to collect execution data
2. **Pass 2**: Recompiles using that data (`-fprofile-use`) for better inlining, register allocation, and branch layout

PGO binaries are typically 5-15% faster. The overhead is only in build time, not runtime.

## Requirements

| Requirement | Details |
|-------------|---------|
| **OS** | Linux only (uses `perf_event_open(2)`, available since kernel 2.6.31) |
| **Architecture** | x86-64 only (uses `rdtsc`/`rdtscp` instructions) |
| **Compiler** | GCC or Clang with C11 and C++17 support |
| **CPU** | Any modern AMD or Intel CPU with invariant TSC (Zen 2+, Intel Nehalem+) |
| **Permissions** | `echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid` |

## Common Pitfalls & How to Fix Them

### 1. "✗ (not on this CPU)" for some counters

**What it means:** Your CPU doesn't support that specific event, or the kernel can't map it.

**Fix:** Check what your CPU actually supports:

```bash
perf list
```

If a counter shows `✗`, you can't use it. The `n/a` placeholder will appear in output for that metric. This is normal — not all CPUs support all events.

### 2. All counters show `n/a`

**Cause:** `perf_event_paranoid` is too restrictive.

**Fix:**

```bash
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

If it still doesn't work, check if your kernel has `CONFIG_PERF_EVENTS=y`:

```bash
zgrep PERF_EVENTS /proc/config.gz 2>/dev/null || grep PERF_EVENTS /boot/config-$(uname -r)
```

### 3. `undefined reference to 'start_measuring'`

**Cause:** You compiled your code but didn't link `time.o`.

**Fix:** Link `time.o` with your code:

```bash
# Wrong:
gcc your_code.c -o binary

# Right:
gcc your_code.c time.o -o binary
```

### 4. Instruction count is 0 or suspiciously low

**Cause 1:** The compiler optimized away your entire computation (dead-code elimination).

**Fix:** Use `KEEP(result)` on the return value of your measured function:

```c
uint64_t result = my_function();
KEEP(result);
```

**Cause 2:** You didn't mark your function `noinline`, so the compiler inlined and reordered it outside the measurement window.

**Fix:** Add `__attribute__((noinline))`:

```c
__attribute__((noinline))
static uint64_t my_function(...) { ... }
```

**Cause 3:** The measurement window is too short. For very fast code (< 1000 instructions), the `ioctl(RESET)` → `ioctl(ENABLE)` → `ioctl(DISABLE)` overhead can dominate, and some runs may catch the counter during reset.

**Fix:** Run your function in a loop or use larger input data. The minimum instruction count in multi-run benchmarks (`bench.cpp`) may show artifacts — use the **average** instead.

### 5. Different instruction counts across identical runs

**Cause:** You're using the `bench.cpp` multi-run output and looking at the `min` column. Some iterations catch the counter during reset transition.

**Fix:** Look at the **average** or **last** column instead. The average is statistically stable. If you need perfectly deterministic single-run counts, use `single.cpp` (one measurement, no reset race).

Alternatively, ensure your workload is large enough (> 10,000 instructions) so measurement overhead is negligible.

### 6. High `IPC` with very few instructions (e.g., IPC = 463)

**Cause:** The compiler hoisted your computation out of the measurement window (Common Subexpression Elimination or loop-invariant code motion).

**Fix:** Use `__attribute__((noinline))` on the measured function and pass data as a pointer/reference so the compiler can't prove the result is constant.

### 7. `make` fails with "missing profile data" warnings

**Cause:** PGO build is looking for `.gcda` files that don't exist because you interrupted the build or ran `make clean`.

**Fix:** Just run `make clean && make pgo` again. The `.gcda` files are generated during pass 1 and consumed in pass 2.

### 8. Permission denied when running `echo -1 | sudo tee ...`

**Cause:** You're not in the sudoers file.

**Fix:** Run the command as root, or add yourself to sudoers, or ask your system administrator to set `perf_event_paranoid`.

## How It Works Internally

### Hardware Counters via `perf_event_open`

Linux provides `perf_event_open(2)` to access the CPU's hardware Performance Monitoring Unit (PMU). This project opens 9 counters:

| Index | Counter | Type |
|-------|---------|------|
| 0 | CPU cycles | `PERF_COUNT_HW_CPU_CYCLES` |
| 1 | Instructions retired | `PERF_COUNT_HW_INSTRUCTIONS` |
| 2 | Branches | `PERF_COUNT_HW_BRANCH_INSTRUCTIONS` |
| 3 | Branch misses | `PERF_COUNT_HW_BRANCH_MISSES` |
| 4 | L1D read references | `PERF_TYPE_HW_CACHE` (L1D read) |
| 5 | L1D read misses | `PERF_TYPE_HW_CACHE` (L1D read miss) |
| 6 | L1 instruction cache misses | `PERF_TYPE_HW_CACHE` (L1I read miss) |
| 7 | DTLB misses | `PERF_TYPE_HW_CACHE` (DTLB read miss) |
| 8 | ITLB misses | `PERF_TYPE_HW_CACHE` (ITLB read miss) |

### Serialized TSC Timing

The TSC (Time-Stamp Counter) is read with CPU serialization to prevent out-of-order execution from leaking into the measurement window:

- **Start:** `lfence` → `rdtsc` — drains all prior instructions before reading TSC
- **End:** `rdtscp` → `lfence` — reads TSC, then blocks later instructions

### Compiler Barriers

`asm volatile` with `"memory"` clobber is a full compiler barrier. The optimizer cannot move any load, store, or function call across `start_measuring()` or `stop_measuring()`. This works even at `-O3`, `-Ofast`, and with LTO.

### No Overhead Subtraction

The library does **not** subtract a calibrated "empty run" overhead. This is intentional:

- Overhead varies per-run due to cache state, branch predictor state, etc.
- Subtracting an average adds noise and can produce negative or clamped-to-zero results
- The harness cost is ~30-60 TSC ticks — negligible for any real workload
- Raw hardware counts are perfectly deterministic for identical binaries and inputs

## License

MIT License — use it, modify it, ship it.

## See Also

- `perf_event_open(2)` man page
- `perf list` — list all events your CPU supports
- `perf stat ./your_binary` — quick one-shot measurement with `perf` CLI
- Intel SDM Vol. 3B, Ch. 18 — Performance Monitoring
- AMD BIOS and Kernel Developer Guide — PMU section
