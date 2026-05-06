/* Pre-generated from grid_vadd.S. Regenerate with:
 *   ../libt1/build_kernel.sh kernels/grid_vadd.S kernels/grid_vadd.h grid_vadd
 */
#pragma once

#include <stdint.h>

static const uint32_t grid_vadd[] = {
    0x02050407, /* vle8.v  v8,  (a0)  */
    0x02058607, /* vle8.v  v12, (a1)  */
    0x0a860457, /* vsub.vv v8,  v8, v12 */
    0x02060427, /* vse8.v  v8,  (a2)  */
};
static const uint32_t grid_vadd_count = sizeof(grid_vadd) / sizeof(grid_vadd[0]);
