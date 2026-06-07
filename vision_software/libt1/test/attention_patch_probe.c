/*
 * attention_patch_probe.c -- verify the blocked ViT 8x8-patch attention math
 * (attention_patch.S) on hardware, standalone (Q tiles staged directly via
 * ap_build_Q_block; no im2col, no camera). For each query-block it stages the
 * tiles + fixed K/V + LUTs in DDR, issues with dbg=1, and compares every stage
 * (Sa,Sb,ea,eb,Z,R,pqa,pqb,O) to the bit-accurate ap_reference. t1_perf reports
 * the per-query-block kernel cost.
 *
 * Packed into two udmabuf arenas (only 3 /dev/udmabuf nodes, 4 MB each).
 * Z/R checkpoints are vse32 vl=1 -> SEW-scaled pitch = 128 e32 = 512 bytes/token.
 *
 *   build: cd ~/vision_software/libt1 && make test/attention_patch_probe
 *   run:   sudo ./test/attention_patch_probe
 */
#include "libt1.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_patch.h"          /* generated: attention_patch[] */
#include "kernels/attention_patch_issue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TILE   16384u          /* 128*128 bytes */
#define PITCH32 512u           /* vse32 vl=1 per-token byte pitch (128 e32)     */
#define ZR_BYTES (128u * PITCH32)

/* in-arena: the staged tiles */
#define I_QA 0u
#define I_QB (I_QA + TILE)
#define I_KTA (I_QB + TILE)
#define I_KTB (I_KTA + TILE)
#define I_VA (I_KTB + TILE)
#define I_VB (I_VA + TILE)
#define I_EXP (I_VB + TILE)
#define I_SEED (I_EXP + TILE)
#define IN_BYTES (I_SEED + TILE)
/* out-arena: one query-block's outputs/checkpoints */
#define O_O 0u
#define O_SA (O_O + TILE)
#define O_SB (O_SA + TILE)
#define O_EA (O_SB + TILE)
#define O_EB (O_EA + TILE)
#define O_PQA (O_EB + TILE)
#define O_PQB (O_PQA + TILE)
#define O_Z (O_PQB + TILE)
#define O_R (O_Z + ZR_BYTES)
#define OUT_BYTES (O_R + ZR_BYTES)

/* reference outputs (256 tokens) */
static uint8_t  rS8[AP_TOKENS * AP_TOKENS];
static uint8_t  rE [AP_TOKENS * AP_TOKENS];
static uint32_t rZ [AP_TOKENS];
static uint32_t rR [AP_TOKENS];
static uint8_t  rPQ[AP_TOKENS * AP_TOKENS];
static uint8_t  rO [AP_TOKENS * AP_FEAT];

static int g_fail = 0;

/* compare a 128x128-keys u8 checkpoint (hw-row tb, lane key) vs ref[t][keybase+key] */
static void cmp_keys(const char *name, const uint8_t *got, const uint8_t *ref,
                     int qb, int keybase, int tol)
{
    int errs = 0, ftb = -1, fk = -1, fg = 0, fe = 0;
    for (int tb = 0; tb < 128; tb++)
        for (int k = 0; k < 128; k++) {
            int g = got[tb * 128 + k];
            int e = ref[(qb * 128 + tb) * (int)AP_TOKENS + (keybase + k)];
            int d = g - e; if (d < 0) d = -d;
            if (d > tol) { if (!errs) { ftb = tb; fk = k; fg = g; fe = e; } errs++; }
        }
    if (errs) { printf("  [FAIL] %s: %d/16384; first tok %d key %d got %d exp %d\n",
                       name, errs, qb * 128 + ftb, keybase + fk, fg, fe); g_fail += errs; }
    else printf("  [PASS] %s\n", name);
}

int main(void)
{
    if (t1_init() < 0) { perror("t1_init"); return 1; }
    fprintf(stderr, "[patch] t1_init ok\n");

    /* reference (uses the same synthetic frame, internally patchified) */
    static uint8_t frame[AP_IMG * AP_IMG];
    ap_build_frame(frame);
    uint8_t etab[128], stab[128];
    ap_exp_table(etab); ap_seed_table8(stab);
    ap_reference(frame, etab, stab, rS8, rE, rZ, rR, rPQ, rO);
    fprintf(stderr, "[patch] reference computed\n");

    struct t1_buf in = {0}, out = {0};
    if (t1_buf_alloc(&in, IN_BYTES) < 0 || t1_buf_alloc(&out, OUT_BYTES) < 0) {
        perror("t1_buf_alloc"); return 1;
    }
    uint8_t *ib = (uint8_t *)in.va;
    ap_build_Q_block(frame, 0, ib + I_QA);
    ap_build_Q_block(frame, 1, ib + I_QB);
    ap_build_Kt_block(0, ib + I_KTA);
    ap_build_Kt_block(1, ib + I_KTB);
    ap_build_V_block(0, ib + I_VA);
    ap_build_V_block(1, ib + I_VB);
    ap_build_exp_lut(ib + I_EXP);
    ap_build_seed_lut(ib + I_SEED);
    if (t1_buf_sync_for_device(&in) < 0) { perror("sync in"); return 1; }
    fprintf(stderr, "[patch] tiles staged\n");

    for (int qb = 0; qb < (int)AP_NBLK; qb++) {
        memset(out.va, 0, OUT_BYTES);
        if (t1_buf_sync_for_device(&out) < 0) { perror("sync out"); return 1; }

        struct ap_pa pa = {
            .q    = in.pa + (qb ? I_QB : I_QA),
            .kt_a = in.pa + I_KTA, .kt_b = in.pa + I_KTB,
            .va   = in.pa + I_VA,  .vb   = in.pa + I_VB,
            .exp_lut = in.pa + I_EXP, .seed_lut = in.pa + I_SEED,
            .out  = out.pa + O_O,
            .dbg_sa = out.pa + O_SA, .dbg_sb = out.pa + O_SB,
            .dbg_ea = out.pa + O_EA, .dbg_eb = out.pa + O_EB,
            .dbg_z  = out.pa + O_Z,  .dbg_r  = out.pa + O_R,
            .dbg_pqa = out.pa + O_PQA, .dbg_pqb = out.pa + O_PQB,
        };

        fprintf(stderr, "[patch] qblock %d: issuing...\n", qb);
        if (attention_patch_issue(attention_patch, &pa, 0) < 0) { perror("issue"); return 1; } /* warm-up */
        (void)t1_perf_start(1);
        if (attention_patch_issue(attention_patch, &pa, 1) < 0) { perror("issue2"); return 1; }
        uint32_t cyc = t1_perf_stop();
        if (t1_buf_sync_for_cpu(&out) < 0) { perror("sync_for_cpu"); return 1; }
        fprintf(stderr, "[patch] qblock %d: %u kernel cycles\n", qb, cyc);

        const uint8_t *ob = (uint8_t *)out.va;
        printf("qblock %d checks:\n", qb);
        cmp_keys("Sa", ob + O_SA, rS8, qb, 0,   0);
        cmp_keys("Sb", ob + O_SB, rS8, qb, 128, 0);
        cmp_keys("ea", ob + O_EA, rE,  qb, 0,   0);
        cmp_keys("eb", ob + O_EB, rE,  qb, 128, 0);
        cmp_keys("pqa", ob + O_PQA, rPQ, qb, 0,   1);
        cmp_keys("pqb", ob + O_PQB, rPQ, qb, 128, 1);

        /* Z / R: vse32 vl=1, pitch 512 bytes per token */
        int ze = 0, re = 0;
        for (int tb = 0; tb < 128; tb++) {
            uint32_t z = *(const uint32_t *)(ob + O_Z + (uint32_t)tb * PITCH32);
            uint32_t r = *(const uint32_t *)(ob + O_R + (uint32_t)tb * PITCH32);
            uint32_t zr = rZ[qb * 128 + tb], rr = rR[qb * 128 + tb];
            if (z != zr) { if (!ze) printf("  [FAIL] Z: tok %d got %u exp %u\n", qb*128+tb, z, zr); ze++; }
            long dr = (long)r - (long)rr; if (dr < 0) dr = -dr;
            if (dr > 1) { if (!re) printf("  [FAIL] R: tok %d got %u exp %u\n", qb*128+tb, r, rr); re++; }
        }
        if (!ze) printf("  [PASS] Z\n"); else g_fail += ze;
        if (!re) printf("  [PASS] R\n"); else g_fail += re;

        /* O: hw-row tb pitch 128, feat 0..63 */
        int oe = 0, ftb = -1, ff = -1, fg = 0, fe = 0;
        for (int tb = 0; tb < 128; tb++)
            for (int f = 0; f < (int)AP_FEAT; f++) {
                int g = ob[O_O + tb * 128 + f];
                int e = rO[(qb * 128 + tb) * (int)AP_FEAT + f];
                int d = g - e; if (d < 0) d = -d;
                if (d > 2) { if (!oe) { ftb = tb; ff = f; fg = g; fe = e; } oe++; }
            }
        if (oe) { printf("  [FAIL] O: %d/8192; first tok %d feat %d got %d exp %d\n",
                         oe, qb*128+ftb, ff, fg, fe); g_fail += oe; }
        else printf("  [PASS] O\n");
    }

    printf("\n==== attention_patch probe: %s ====\n", g_fail ? "FAIL" : "PASS");
    t1_close();
    return g_fail ? 1 : 0;
}
