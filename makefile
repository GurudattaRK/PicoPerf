# Makefile
#
# make           clean, compile with PGO, run single
# make run       clean, compile, run single
# make bench     clean, compile, run bench
# make pgo       clean, compile with PGO, run single
# make pgo-bench clean, compile with PGO, run bench

CC      = gcc
CXX     = g++
PROFDIR = pgo_data

CFLAGS   = -O3 -march=native -funroll-loops -fstrict-aliasing -fno-plt -Wall -Wextra -std=c11
CXXFLAGS = -O3 -march=native -funroll-loops -fstrict-aliasing -fno-plt -Wall -Wextra -std=c++17

# All user-facing targets are phony. Binary names are out_single / out_bench
# so they never clash with the phony "bench" and "run" targets.
.PHONY: all run bench pgo pgo-bench clean _build _build_pgo _run_inst _pass2

# default: PGO build, run single
all: clean _build_pgo
	./out_single

# plain build, run single
run: clean _build
	./out_single

# plain build, run bench
bench: clean _build
	./out_bench

# PGO build, run single
pgo: clean _build_pgo_single
	./out_single

# PGO build, run bench
pgo-bench: clean _build_pgo_bench
	./out_bench

# ----------------------------------------------------------------
# Plain compile
#   time.c  -> gcc  (C11)   -> time.o
#   *.cpp   -> g++  (C++17) -> out_single / out_bench, linked with time.o
# ----------------------------------------------------------------

_build: time.o out_single out_bench

time.o: time.c time.h
	$(CC) $(CFLAGS) -c time.c -o time.o

out_single: single.cpp time.o time.h
	$(CXX) $(CXXFLAGS) single.cpp time.o -o out_single

out_bench: bench.cpp time.o time.h
	$(CXX) $(CXXFLAGS) bench.cpp time.o -o out_bench

# ----------------------------------------------------------------
# Two-pass PGO
#
# pass 1  -fprofile-generate
#   Inserts probes at every branch and call site.
#   Running the _inst binaries writes one .gcda file per TU.
#
# pass 2  -fprofile-use  -fprofile-correction
#   Reads .gcda. Uses real execution frequencies to:
#     inline hot calls the default size limit would normally skip
#     reorder basic blocks so the hot path has no taken branches
#     tighten register allocation in hot loops
#   -fprofile-correction tolerates minor count mismatches.
#
# Profile data -> $(PROFDIR)/. Instrumented binaries are intentionally slow.
# ----------------------------------------------------------------

_build_pgo: $(PROFDIR) time_inst.o single_inst bench_inst _run_inst time_pgo.o _pass2

_build_pgo_single: $(PROFDIR) time_inst.o single_inst _run_inst_single time_pgo.o _pass2

_build_pgo_bench: $(PROFDIR) time_inst.o bench_inst _run_inst_bench time_pgo.o _pass2

$(PROFDIR):
	mkdir -p $(PROFDIR)

time_inst.o: time.c time.h
	$(CC) $(CFLAGS) -fprofile-generate=$(PROFDIR) -c time.c -o time_inst.o

single_inst: single.cpp time_inst.o time.h
	$(CXX) $(CXXFLAGS) -fprofile-generate=$(PROFDIR) single.cpp time_inst.o -o single_inst

bench_inst: bench.cpp time_inst.o time.h
	$(CXX) $(CXXFLAGS) -fprofile-generate=$(PROFDIR) bench.cpp time_inst.o -o bench_inst

_run_inst: single_inst bench_inst
	@echo ""
	@echo "--- pass 1: running instrumented binaries ---"
	./single_inst
	./bench_inst
	@echo "--- profile data written to $(PROFDIR)/ ---"
	@echo ""

_run_inst_single: single_inst
	@echo ""
	@echo "--- pass 1: running instrumented binary (single) ---"
	./single_inst
	@echo "--- profile data written to $(PROFDIR)/ ---"
	@echo ""

_run_inst_bench: bench_inst
	@echo ""
	@echo "--- pass 1: running instrumented binary (bench) ---"
	./bench_inst
	@echo "--- profile data written to $(PROFDIR)/ ---"
	@echo ""

time_pgo.o: time.c time.h
	$(CC) $(CFLAGS) -fprofile-use=$(PROFDIR) -fprofile-correction -c time.c -o time_pgo.o

_pass2: time_pgo.o
	@echo "--- pass 2: building optimized binaries ---"
	$(CXX) $(CXXFLAGS) -fprofile-use=$(PROFDIR) -fprofile-correction single.cpp time_pgo.o -o out_single
	$(CXX) $(CXXFLAGS) -fprofile-use=$(PROFDIR) -fprofile-correction bench.cpp  time_pgo.o -o out_bench

clean:
	rm -f time.o time_inst.o time_pgo.o
	rm -f out_single out_bench single_inst bench_inst
	rm -rf $(PROFDIR)