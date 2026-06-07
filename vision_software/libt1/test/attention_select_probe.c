/*
 * attention_select_probe.c -- verify the PRODUCTION camera-pipeline path
 * (kernels/attention_select.h's issue_active_kernel) on hardware, without the
 * camera. Exercises exactly what main.c does per frame in the DDR-I/O path:
 *   - operands Kt/V/expLUT/seedLUT staged in one DDR udmabuf by the select.h
 *   - Q read from a DDR udmabuf, O written to a DDR udmabuf
 * feeds a deterministic synthetic frame, and checks O against the bit-accurate
 * C reference.
 *
 * fprintf checkpoints (stderr, unbuffered) pinpoint any hang location.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_select_probe
 *   run:   sudo ./test/attention_select_probe
 */
#include "libt1.h"
#include "kernels/attention_weights.h"
#include "kernels/attention_select.h"   /* issue_active_kernel + DDR staging */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 128
#define GRID (N * N)

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    fprintf(stderr, "[sp] t1_init ok\n");

    struct t1_buf q = {0}, o = {0};
    if (t1_buf_alloc(&q, GRID) < 0 || t1_buf_alloc(&o, GRID) < 0) {
        perror("t1_buf_alloc"); return 1;
    }
    fprintf(stderr, "[sp] Q/O bufs ok (q=0x%08x o=0x%08x)\n", q.pa, o.pa);

    attn_build_Q((uint8_t *)q.va);          /* synthetic peaked-query frame */
    memset(o.va, 0, GRID);
    if (t1_buf_sync_for_device(&q) < 0 || t1_buf_sync_for_device(&o) < 0) {
        perror("sync_for_device"); return 1;
    }
    fprintf(stderr, "[sp] Q built+synced; calling issue_active_kernel...\n");

    /* warm-up call stages operands (one-time); measured call times the kernel */
    if (issue_active_kernel(q.pa, o.pa) < 0) { perror("issue_active_kernel"); return 1; }
    (void)t1_perf_start(1);
    if (issue_active_kernel(q.pa, o.pa) < 0) { perror("issue_active_kernel(2)"); return 1; }
    uint32_t cyc = t1_perf_stop();
    fprintf(stderr, "[sp] issue_active_kernel returned (%u kernel cycles, DDR operands)\n", cyc);

    if (t1_buf_sync_for_cpu(&o) < 0) { perror("sync_for_cpu"); return 1; }

    /* reference: attention(Q, Kt, V) with the same deterministic operands */
    static uint8_t Kt[GRID], V[GRID], rO[GRID];
    attn_build_Kt(Kt);
    attn_build_V(V);
    uint8_t etab[128]; attn_exp_table(etab);
    uint8_t stab[128]; attn_seed_table8(stab);
    attn_reference((const uint8_t *)q.va, Kt, V, etab, stab,
                   NULL, NULL, NULL, NULL, NULL, rO);

    const uint8_t *hw = (const uint8_t *)o.va;
    int errs = 0, fr = -1, fc = -1, fg = 0, fe = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int g = hw[r * N + c], e = rO[r * N + c], d = g - e;
            if (d < 0) d = -d;
            if (d > 2) { if (!errs) { fr = r; fc = c; fg = g; fe = e; } errs++; }
        }

    if (errs == 0)
        printf("[PASS] production select-path O matches reference (%d cells, tol 2)\n", GRID);
    else
        printf("[FAIL] select-path O: %d/%d cells; first [%d][%d] got %d exp %d\n",
               errs, GRID, fr, fc, fg, fe);

    printf("\n==== attention select-path probe: %s ====\n", errs ? "FAIL" : "PASS");
    t1_close();
    return errs ? 1 : 0;
}
