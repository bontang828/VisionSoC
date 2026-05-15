/* Auto-generated from kernels/sobel.S by build_kernel.sh. */
#pragma once

#include <stdint.h>

static const uint32_t sobel[] = {
    0x02050407, 0x5e003657, 0x5e003857, 0x3e80b657, 
    0x3a80b857, 0x0ac80a57, 0x7c00d073, 0x5e003657, 
    0x5e003857, 0x3e80b657, 0x3a80b857, 0x0ac80c57, 
    0x7c005073, 0x0f403657, 0x1f460a57, 0x0f803857, 
    0x1f880c57, 0x874c0857, 0x02058827, 
};
static const uint32_t sobel_count = sizeof(sobel) / sizeof(sobel[0]);
