/*
 * Selects which T1 kernel main.c runs. To swap kernels:
 *
 *   1. Write kernels/<name>.S -- vector ops, plus inline `csrwi 0x7c0, 0/1`
 *      to toggle horizontal/vertical mode, plus the libt1 PA convention:
 *      `(a0)` is the source pointer and `(a1)` is the destination pointer
 *      on every vle/vse you want routed automatically.
 *
 *   2. Copy kernels/sobel_select.h to kernels/<name>_select.h, change two
 *      macros (the kernel name and ACTIVE_KERNEL_NEUTRAL_UV). The shared
 *      dispatcher (active_kernel_dispatcher.h) handles the per-word
 *      op.rs1 / op.vertical_mode routing automatically -- no schedule
 *      table to maintain.
 *
 *   3. Run sync_kernel.sh <name> from the dev host. It scp's the .S to the
 *      Kria, runs build_kernel.sh to generate <name>.h on the board, scp's
 *      it back, and rewrites the #include below to point at <name>_select.h.
 *
 *   4. On the board (PuTTY/SSH): ./run_after_power_cycle.sh
 */
#pragma once

#include "frame_passthrough_select.h"
