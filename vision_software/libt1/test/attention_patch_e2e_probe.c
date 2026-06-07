/*
 * attention_patch_e2e_probe.c -- verify the FULL on-fabric patch-attention
 * pipeline (im2col -> blocked attention -> un-patchify) end-to-end on hardware,
 * exactly as the camera path runs it, but on a deterministic synthetic frame.
 * Calls attention_patch_run() (the production select-path entry) and compares
 * the un-patchified display frame to the C reference's un-patchified output.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_patch_e2e_probe
 *   run:   sudo ./test/attention_patch_e2e_probe
 */
#include "libt1.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_patch_select.h"   /* attention_patch_run + staging */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    fprintf(stderr, "[e2e] t1_init ok\n");

    static uint8_t frame[AP_IMG * AP_IMG];
    static uint8_t got[AP_IMG * AP_IMG];
    static uint8_t ref[AP_IMG * AP_IMG];
    ap_build_frame(frame);
    memset(got, 0, sizeof got);

    /* reference: full attention -> O tile -> un-patchify */
    static uint8_t rO[AP_TOKENS * AP_FEAT];
    uint8_t etab[128], stab[128];
    ap_exp_table(etab); ap_seed_table8(stab);
    ap_reference(frame, etab, stab, NULL, NULL, NULL, NULL, NULL, rO);
    ap_unpatchify(rO, ref);
    fprintf(stderr, "[e2e] reference computed\n");

    /* warm-up (stages operands) + timed run */
    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run"); return 1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run(2)"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[e2e] attention_patch_run: %.1f ms/frame (~%.2f fps)\n", ms, 1000.0 / ms);

    int errs = 0, fi = -1, fg = 0, fe = 0;
    for (int i = 0; i < (int)(AP_IMG * AP_IMG); i++) {
        int d = got[i] - ref[i]; if (d < 0) d = -d;
        if (d > 2) { if (!errs) { fi = i; fg = got[i]; fe = ref[i]; } errs++; }
    }
    if (errs == 0)
        printf("[PASS] un-patchified display frame matches reference (%u px, tol 2)\n",
               AP_IMG * AP_IMG);
    else
        printf("[FAIL] display frame: %d/%u px; first px %d (r%d c%d) got %d exp %d\n",
               errs, AP_IMG * AP_IMG, fi, fi / 128, fi % 128, fg, fe);

    printf("\n==== attention_patch e2e probe: %s ====\n", errs ? "FAIL" : "PASS");
    t1_close();
    return errs ? 1 : 0;
}
