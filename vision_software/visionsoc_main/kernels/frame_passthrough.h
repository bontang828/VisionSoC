/* Pre-generated from frame_passthrough.S. Regenerate with:
 *   ../libt1/build_kernel.sh kernels/frame_passthrough.S kernels/frame_passthrough.h frame_passthrough
 *
 * Load-and-store passthrough: copies one 128x128 image (the LSU fans the
 * single vle8.v/vse8.v across all 128 hardware rows at the fixed 128-element
 * row pitch). No compute. The host issues word 0 with op.rs1 = source PA and
 * word 1 with op.rs1 = destination PA; the (a0) in both is just a placeholder
 * register, exactly as in grid_vadd.
 */
#pragma once

#include <stdint.h>

static const uint32_t frame_passthrough[] = {
    0x02050407, /* vle8.v  v8,  (a0)  */
    0x02050427, /* vse8.v  v8,  (a0)  */
};
static const uint32_t frame_passthrough_count = sizeof(frame_passthrough) / sizeof(frame_passthrough[0]);
