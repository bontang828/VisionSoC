/*
 * attention_self_e2e_probe.c -- verify the full on-fabric SELF-attention pipeline
 * (im2col -> transpose(K=Q^T) -> attention(Q=K=V=frame patches) -> un-patchify)
 * end-to-end on hardware, exactly as the camera path runs it, on a deterministic
 * synthetic frame. Calls attention_patch_run() (the self select-path entry) and
 * compares the un-patchified display frame to ap_self_reference's output.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_self_e2e_probe
 *   run:   sudo ./test/attention_self_e2e_probe
 */
#include "libt1.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_self_select.h"    /* attention_patch_run (self) + staging */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    fprintf(stderr, "[self] t1_init ok\n");

    static uint8_t frame[AP_IMG * AP_IMG], got[AP_IMG * AP_IMG], ref[AP_IMG * AP_IMG];
    ap_build_frame(frame);
    memset(got, 0, sizeof got);

    /* reference: self-attention (K=V=patchify(frame)) -> O -> un-patchify */
    static uint8_t rO[AP_TOKENS * AP_FEAT];
    uint8_t etab[128], stab[128];
    ap_self_exp_table(etab);              /* self decay (180) */
    ap_seed_table8(stab);
    ap_self_reference(frame, etab, stab, rO);
    ap_unpatchify(rO, ref);
    fprintf(stderr, "[self] reference computed\n");

    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run"); return 1; }  /* warm-up */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (attention_patch_run(frame, got) < 0) { perror("attention_patch_run(2)"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fprintf(stderr, "[self] attention_patch_run(self): %.1f ms/frame (~%.2f fps)\n", ms, 1000.0 / ms);

    int errs = 0, fi = -1, fg = 0, fe = 0;
    for (int i = 0; i < (int)(AP_IMG * AP_IMG); i++) {
        int d = got[i] - ref[i]; if (d < 0) d = -d;
        if (d > 2) { if (!errs) { fi = i; fg = got[i]; fe = ref[i]; } errs++; }
    }
    if (errs == 0)
        printf("[PASS] self-attention display frame matches reference (%u px, tol 2)\n",
               AP_IMG * AP_IMG);
    else
        printf("[FAIL] self frame: %d/%u px; first px %d (r%d c%d) got %d exp %d\n",
               errs, AP_IMG * AP_IMG, fi, fi / 128, fi % 128, fg, fe);

    printf("\n==== attention_self e2e probe: %s ====\n", errs ? "FAIL" : "PASS");
    t1_close();
    return errs ? 1 : 0;
}
