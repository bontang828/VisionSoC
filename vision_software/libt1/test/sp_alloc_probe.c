/*
 * sp_alloc_probe - minimal "just mmap /dev/uio6" probe.
 *
 * Issues nothing to T1, never derefs the scratchpad VA, never touches
 * BRAM via the data path. If THIS crashes the kernel, the kernel UIO
 * subsystem's mmap of the bram_scratch dts node is doing something
 * bad - probably probing the PA range during the mmap call before
 * userspace can fault any pages.
 *
 * Expected behaviour on a healthy bitstream:
 *   prints "PASS: sp_alloc_probe pa=0xb0000000" and exits 0.
 */

#include "libt1.h"

#include <stdio.h>

int main(void)
{
    struct t1_buf sp = {0};

    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }

    if (t1_scratchpad_alloc(&sp, 0u, 4096u) < 0) {
        perror("t1_scratchpad_alloc");
        t1_close();
        return 1;
    }

    printf("PASS: sp_alloc_probe pa=0x%08x size=%zu\n", sp.pa, sp.size);
    t1_buf_free(&sp);
    t1_close();
    return 0;
}
