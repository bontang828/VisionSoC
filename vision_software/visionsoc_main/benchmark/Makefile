# Makefile for benchmark_instructions_perf -- standalone FPGA instruction
# benchmark. Lives in visionsoc_main/benchmark/, so libt1 is two levels up.
#
# Build (on the board, or any host with the rv32 assembler for the kernel):
#   make                       # builds kernels/bench_instructions.h + the binary
# Run:
#   ./run_bench.sh [N_ITERS]   # board-side; or run ./benchmark_instructions_perf directly

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -g
CFLAGS  += -I../../libt1
LDLIBS  ?= ../../libt1/libt1.a

BUILD_KERNEL := ../../libt1/build_kernel.sh

all: benchmark_instructions_perf

# rv32 assembly -> uint32_t[] header. Needs riscv64-linux-gnu-as (present on
# the Kria; on a dev host without it, generate on the board and scp back).
kernels/bench_instructions.h: kernels/bench_instructions.S
	$(BUILD_KERNEL) $< $@ bench_instructions

benchmark_instructions_perf.o: benchmark_instructions_perf.c kernels/bench_instructions.h

benchmark_instructions_perf: benchmark_instructions_perf.o ../../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ benchmark_instructions_perf.o $(LDLIBS)

../../libt1/libt1.a:
	$(MAKE) -C ../../libt1 libt1.a

clean:
	$(RM) benchmark_instructions_perf.o benchmark_instructions_perf

# Remove the generated kernel header too (forces a re-assemble next build).
distclean: clean
	$(RM) kernels/bench_instructions.h

.PHONY: all clean distclean
