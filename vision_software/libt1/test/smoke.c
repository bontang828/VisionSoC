#include "libt1.h"

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    if (t1_init() < 0) {
        perror("t1_init");
        return 1;
    }

    uint64_t c0 = t1_cycles();
    usleep(10000);
    uint64_t c1 = t1_cycles();
    uint64_t delta = c1 - c0;
    printf("cycles advanced by %" PRIu64 " in 10 ms\n", delta);
    if (delta < 100000) {
        fprintf(stderr, "FAIL: PERF_CYCLES did not advance enough\n");
        t1_close();
        return 1;
    }

    t1_set_vertical_mode_raw(1);
    if (t1_get_vertical_mode_raw() != 1) {
        fprintf(stderr, "FAIL: VERTICAL_MODE did not round-trip as 1\n");
        t1_close();
        return 1;
    }

    t1_set_vertical_mode_raw(0);
    if (t1_get_vertical_mode_raw() != 0) {
        fprintf(stderr, "FAIL: VERTICAL_MODE did not round-trip as 0\n");
        t1_close();
        return 1;
    }

    printf("PASS: smoke\n");
    t1_close();
    return 0;
}
