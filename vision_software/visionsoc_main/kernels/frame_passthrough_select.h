/*
 * frame_passthrough_select.h -- activate the frame_passthrough kernel.
 *
 * The .S is self-describing: (a0) = src, (a1) = dst, no mode flips. The
 * generic dispatcher (active_kernel_dispatcher.h) auto-routes op.rs1 and
 * op.vertical_mode per word, so this file is just two macros.
 */
#pragma once

#include "frame_passthrough.h"

#define ACTIVE_KERNEL            frame_passthrough
#define ACTIVE_KERNEL_NEUTRAL_UV 0   /* keep the camera's chroma -- full colour */

#include "active_kernel_dispatcher.h"
