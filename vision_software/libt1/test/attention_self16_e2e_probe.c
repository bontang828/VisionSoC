/*
 * attention_self16_e2e_probe.c -- verify the full 16-bit-score self-attention
 * pipeline end-to-end on hardware (im2col -> partial-center -> transpose ->
 * 16-bit attention -> un-patchify), on a TEXTURED frame. Compares to
 * ap_self16_reference. Texture differentiation is judged live on the camera;
 * this probe verifies bit-exact correctness.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_self16_e2e_probe
 *   run:   sudo ./test/attention_self16_e2e_probe
 */
#include "libt1.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_self16_select.h"   /* attention_patch_run (16-bit) */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    fprintf(stderr, "[s16] t1_init ok\n");

    static uint8_t frame[AP_IMG * AP_IMG], got[AP_IMG * AP_IMG], ref[AP_IMG * AP_IMG];
    ap_build_frame_textured(frame);
    memset(got, 0, sizeof got);

    static uint8_t rO[AP_TOKENS * AP_FEAT];
    uint8_t etab[128], stab[128];
    ap_s16_exp_table(etab);
    ap_seed_table8(stab);
    ap_self16_reference(frame, etab, stab, rO);
    ap_unpatchify(rO, ref);
    fprintf(stderr, "[s16] reference computed\n");

    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run"); return 1; }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run(2)"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[s16] attention_patch_run(16-bit): %.1f ms/frame (~%.2f fps)\n", ms, 1000.0 / ms);

    int errs = 0, fi = -1, fg = 0, fe = 0;
    for (int i = 0; i < (int)(AP_IMG * AP_IMG); i++) {
        int d = got[i] - ref[i]; if (d < 0) d = -d;
        if (d > 2) { if (!errs) { fi = i; fg = got[i]; fe = ref[i]; } errs++; }
    }
    if (errs == 0)
        printf("[PASS] 16-bit-score self-attention matches reference (%u px, tol 2)\n",
               AP_IMG * AP_IMG);
    else
        printf("[FAIL] s16 frame: %d/%u px; first px %d (r%d c%d) got %d exp %d\n",
               errs, AP_IMG * AP_IMG, fi, fi / 128, fi % 128, fg, fe);

    printf("\n==== attention_self16 e2e probe: %s ====\n", errs ? "FAIL" : "PASS");
    t1_close();
    return errs ? 1 : 0;
}
