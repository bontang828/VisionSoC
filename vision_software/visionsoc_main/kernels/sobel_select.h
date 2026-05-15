/*
 * sobel_select.h -- activate the Sobel edge kernel.
 *
 * The .S is self-describing: (a0) = src, (a1) = dst, `csrwi 0x7c0, X` marks
 * the H<->V mode flips inline. The generic dispatcher
 * (active_kernel_dispatcher.h) auto-routes op.rs1 and op.vertical_mode per
 * word -- no schedule table.
 */
#pragma once

#include "sobel.h"

#define ACTIVE_KERNEL            sobel
#define ACTIVE_KERNEL_NEUTRAL_UV 1   /* edges are luma-only; show grayscale */

#include "active_kernel_dispatcher.h"
