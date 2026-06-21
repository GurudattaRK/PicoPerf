# PicoPerf - Low-latency hardware performance counter library
# x86-64 Linux only

CC      = gcc
CXX     = g++
CFLAGS  = -O3 -march=native -funroll-loops -fstrict-aliasing -fno-plt -Wall -Wextra -std=c11
CXXFLAGS = -O3 -march=native -funroll-loops -fstrict-aliasing -fno-plt -Wall -Wextra -std=c++17

.PHONY: all run bench overhead clean

# Default target
all: run

# Single run
run: clean build
	./out_single

# Statistical benchmark (1000 runs, percentiles)
bench: clean build
	./out_bench

# Measure measurement overhead
overhead: clean build
	./out_overhead

# Build all binaries
build: picoperf.o out_single out_bench out_overhead

picoperf.o: picoperf.c picoperf.h
	$(CC) $(CFLAGS) -c picoperf.c -o picoperf.o

out_single: single.cpp picoperf.o picoperf.h
	$(CXX) $(CXXFLAGS) single.cpp picoperf.o -o out_single

out_bench: bench.cpp picoperf.o picoperf.h
	$(CXX) $(CXXFLAGS) bench.cpp picoperf.o -o out_bench

out_overhead: overhead.cpp picoperf.o picoperf.h
	$(CXX) $(CXXFLAGS) overhead.cpp picoperf.o -o out_overhead

clean:
	rm -f picoperf.o
	rm -f out_single out_bench out_overhead
