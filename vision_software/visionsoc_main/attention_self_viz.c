/*
 * attention_self_viz -- stage-by-stage visualiser for the on-fabric ViT
 * 8x8-patch SELF-attention pipeline (attention_self).
 *
 * Grabs ONE live camera frame, runs the EXACT attention_self pipeline
 * (im2col -> transpose -> blocked 256-token attention), and dumps the
 * contents of the 128x128 data-plane at every important stage to P5
 * grayscale PPMs so a flow diagram can be assembled host-side. The intent
 * is to *see* what the "visualised" 2D vector processor is holding at each
 * step.
 *
 * IT DOES NOT MODIFY THE attention_self KERNEL. The patch / transpose /
 * output tiles already live in the DDR staging buffer; every in-VRF
 * intermediate (QK scores, exp, softmax sum Z, reciprocal R, normalised
 * probabilities) is captured through the kernel's OWN debug-checkpoint
 * stores -- attention_patch_issue(..., dbg=1) issues words 70..73 which
 * vse8/vse32 the live registers to caller-supplied DDR targets. These are
 * the same checkpoints attention_patch_probe.c verifies, so they are
 * hardware-proven.
 *
 * Stages dumped (query-block A = tokens 0..127 for the in-attention ones):
 *   00 input frame            128x128  [row=image row,  col=image col]
 *   01 patch tile Qa / Qb     128x128  [row=token,      col=feat 0..63]   <- im2col
 *   02 transpose Kt_a / Kt_b  128x128  [row=feat,       col=key  0..127]  <- on-fabric T
 *   03 scores Sa / Sb         128x128  [row=query 0..127, col=key]        <- QK
 *   04 exp ea / eb            128x128  [row=query,      col=key]          <- softmax num.
 *   05 Z (sum) / R (1/Z)      per-row scalar strips                       <- softmax den.
 *   06 prob pqa / pqb         128x128  [row=query,      col=key]          <- normalised
 *   07 output Oa / Ob         128x128  [row=token,      col=feat]         <- PV
 *   08 output frame           128x128  [row=image row,  col=image col]    <- un-patchify
 *
 *   build: make attention_self_viz
 *   run:   sudo ./attention_self_viz [--dev /dev/video0] [--warmup N] [--out-dir DIR]
 */
#include "camera.h"
#include "kernels/attention_patch_weights.h"
#include "kernels/attention_patch_im2col.h"
#include "kernels/attention_patch_im2col_issue.h"
#include "kernels/attention_self.h"
#include "kernels/attention_self_transpose.h"
#include "kernels/attention_patch_issue.h"
#include "libt1.h"

#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define IMG       128u
#define FRAME_BYTES (IMG * IMG)
#define TILE      16384u            /* one 128x128 u8 tile */
#define FRAMEPAD  (192u * 128u)     /* matches attention_self_select.h */
#define ZR_BYTES  (128u * 512u)     /* E32 per-row store: 128 rows x 512 B pitch */

/* staging layout (one t1_buf, ~460 KB << 4 MB udmabuf). */
#define O_FRAME 0u
#define O_IDXH1 (O_FRAME + FRAMEPAD)
#define O_IDXV  (O_IDXH1 + TILE)
#define O_IDXH2 (O_IDXV  + TILE)
#define O_EXP   (O_IDXH2 + TILE)
#define O_SEED  (O_EXP + TILE)
#define O_QA    (O_SEED + TILE)
#define O_QB    (O_QA + TILE)
#define O_KTA   (O_QB + TILE)
#define O_KTB   (O_KTA + TILE)
#define O_OA    (O_KTB + TILE)
#define O_OB    (O_OA + TILE)
/* dbg checkpoint targets (query-block A) */
#define O_SA    (O_OB + TILE)
#define O_SB    (O_SA + TILE)
#define O_EA    (O_SB + TILE)
#define O_EB    (O_EA + TILE)
#define O_Z     (O_EB + TILE)
#define O_R     (O_Z + ZR_BYTES)
#define O_PQA   (O_R + ZR_BYTES)
#define O_PQB   (O_PQA + TILE)
#define STAGE_BYTES (O_PQB + TILE)

static void die(const char *m) { perror(m); exit(1); }

/* write a (w x h) u8 tile as binary P5 grayscale; `pitch` is the row stride. */
static int write_p5(const char *dir, const char *name,
                    const uint8_t *tile, int w, int h, int pitch)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        if (fwrite(tile + (size_t)y * pitch, 1, (size_t)w, f) != (size_t)w) {
            fclose(f); return -1;
        }
    fclose(f);
    return 0;
}

/* render a per-row E32 array (128 values, 512-byte row pitch) as a
 * min-max-normalised 128-row x 16-col strip so it shows up in the diagram. */
static int write_rowscalar_p5(const char *dir, const char *name,
                              const uint8_t *base, int n)
{
    uint32_t mn = 0xffffffffu, mx = 0;
    for (int r = 0; r < n; r++) {
        uint32_t v;
        memcpy(&v, base + (size_t)r * 512u, sizeof(v));
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int W = 16;
    fprintf(f, "P5\n%d %d\n255\n", W, n);
    uint8_t row[16];
    for (int r = 0; r < n; r++) {
        uint32_t v;
        memcpy(&v, base + (size_t)r * 512u, sizeof(v));
        uint8_t b = (mx > mn)
                  ? (uint8_t)((uint64_t)(v - mn) * 255u / (mx - mn))
                  : 0u;
        memset(row, b, (size_t)W);
        fwrite(row, 1, (size_t)W, f);
    }
    fclose(f);
    return 0;
}

/* on-fabric transpose tile: dst = src^T (V-load + H-store), same as
 * attention_self_select.h's attn_self_transpose. */
static int transpose_tile(uint32_t src, uint32_t dst)
{
    struct t1_op v = { .instruction = attention_self_transpose[0], .rs1 = src,
                       .vtype = AP_E8_M1, .vl = 128, .vertical_mode = 1 };
    if (t1_issue(&v) < 0) return -1;
    struct t1_op h = { .instruction = attention_self_transpose[1], .rs1 = dst,
                       .vtype = AP_E8_M1, .vl = 128, .vertical_mode = 0 };
    return t1_issue(&h);
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "Usage: %s [--dev /dev/video0] [--warmup N] [--out-dir DIR]\n", a0);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/video0";
    const char *out_dir = "/tmp/attention_self_viz";
    unsigned warmup = 30;
    static const struct option opts[] = {
        {"dev",     required_argument, NULL, 'd'},
        {"warmup",  required_argument, NULL, 'w'},
        {"out-dir", required_argument, NULL, 'o'},
        {"help",    no_argument,       NULL, 'h'}, {0,0,0,0}};
    int c;
    while ((c = getopt_long(argc, argv, "d:w:o:h", opts, NULL)) != -1) {
        switch (c) {
            case 'd': dev = optarg; break;
            case 'w': warmup = (unsigned)strtoul(optarg, NULL, 0); break;
            case 'o': out_dir = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (mkdir(out_dir, 0777) < 0 && errno != EEXIST) die("mkdir out_dir");

    if (t1_init() < 0) die("t1_init");
    struct t1_buf stg = {0};
    if (t1_buf_alloc(&stg, STAGE_BYTES) < 0) die("t1_buf_alloc");
    uint8_t *b  = (uint8_t *)stg.va;
    uint32_t pa = stg.pa;
    memset(b, 0, STAGE_BYTES);
    ap_build_im2col_idxH1(b + O_IDXH1);
    ap_build_im2col_idxV (b + O_IDXV);
    ap_build_im2col_idxH2(b + O_IDXH2);
    ap_build_self_exp_lut(b + O_EXP);
    ap_build_seed_lut    (b + O_SEED);
    if (t1_buf_sync_for_device(&stg) < 0) die("sync staging");

    /* ---- grab one settled live camera frame (Y plane -> O_FRAME) ---- */
    struct camera cam;
    if (camera_open(&cam, dev, (int)IMG, (int)IMG) < 0) die("camera_open");
    struct camera_buf cb;
    for (unsigned i = 0; i <= warmup; i++) {
        if (camera_dqbuf(&cam, &cb) < 0) die("camera_dqbuf");
        if (cb.length < FRAME_BYTES) { errno = EINVAL; die("camera frame too small"); }
        if (i == warmup) memcpy(b + O_FRAME, cb.va, FRAME_BYTES);
        if (camera_qbuf(&cam, &cb) < 0) die("camera_qbuf");
    }
    camera_close(&cam);
    if (t1_buf_sync_for_device(&stg) < 0) die("sync frame");
    fprintf(stderr, "attention_self_viz: captured frame, staging=%uKB, out=%s\n",
            STAGE_BYTES / 1024u, out_dir);

    /* ---- patchify: frame -> Qa, Qb (im2col) ---- */
    for (int blk = 0; blk < 2; blk++) {
        struct ap_im2col_pa ip = {
            .idxh1 = pa + O_IDXH1, .idxv = pa + O_IDXV, .idxh2 = pa + O_IDXH2,
            .src   = pa + O_FRAME + (uint32_t)blk * 64u * 128u,
            .out   = pa + (blk ? O_QB : O_QA),
        };
        if (attention_patch_im2col_issue(attention_patch_im2col, &ip, 0) < 0)
            die("im2col");
    }

    /* ---- K = patches^T (V = patches directly) ---- */
    if (transpose_tile(pa + O_QA, pa + O_KTA) < 0) die("transpose A");
    if (transpose_tile(pa + O_QB, pa + O_KTB) < 0) die("transpose B");

    /* ---- attention query-block A (dbg=1: checkpoint every intermediate) ---- */
    struct ap_pa ap_a = {
        .q = pa + O_QA, .kt_a = pa + O_KTA, .kt_b = pa + O_KTB,
        .va = pa + O_QA, .vb = pa + O_QB,
        .exp_lut = pa + O_EXP, .seed_lut = pa + O_SEED, .out = pa + O_OA,
        .dbg_sa = pa + O_SA, .dbg_sb = pa + O_SB,
        .dbg_ea = pa + O_EA, .dbg_eb = pa + O_EB,
        .dbg_z  = pa + O_Z,  .dbg_r  = pa + O_R,
        .dbg_pqa = pa + O_PQA, .dbg_pqb = pa + O_PQB,
    };
    if (attention_patch_issue(attention_self, &ap_a, 1) < 0) die("attention A");

    /* ---- attention query-block B (plain, just to fill Ob for the frame) ---- */
    struct ap_pa ap_b = {
        .q = pa + O_QB, .kt_a = pa + O_KTA, .kt_b = pa + O_KTB,
        .va = pa + O_QA, .vb = pa + O_QB,
        .exp_lut = pa + O_EXP, .seed_lut = pa + O_SEED, .out = pa + O_OB,
    };
    if (attention_patch_issue(attention_self, &ap_b, 0) < 0) die("attention B");

    if (t1_buf_sync_for_cpu(&stg) < 0) die("sync_for_cpu");

    /* ---- un-patchify Oa, Ob -> output frame ---- */
    static uint8_t out_frame[FRAME_BYTES];
    for (int qb = 0; qb < 2; qb++) {
        const uint8_t *o = b + (qb ? O_OB : O_OA);
        for (int tb = 0; tb < 128; tb++) {
            int t = qb * 128 + tb, Py = t / 16, Px = t % 16;
            for (int f = 0; f < (int)AP_FEAT; f++) {
                int ry = f / 8, rx = f % 8;
                out_frame[(Py * 8 + ry) * (int)IMG + (Px * 8 + rx)] = o[tb * 128 + f];
            }
        }
    }

    /* ---- dump every stage ---- */
    int rc = 0;
    rc |= write_p5(out_dir, "00_input_frame.ppm",     b + O_FRAME, 128, 128, 128);
    rc |= write_p5(out_dir, "01_patch_Qa.ppm",        b + O_QA,    128, 128, 128);
    rc |= write_p5(out_dir, "01_patch_Qb.ppm",        b + O_QB,    128, 128, 128);
    rc |= write_p5(out_dir, "02_transpose_Kta.ppm",   b + O_KTA,   128, 128, 128);
    rc |= write_p5(out_dir, "02_transpose_Ktb.ppm",   b + O_KTB,   128, 128, 128);
    rc |= write_p5(out_dir, "03_scores_Sa.ppm",       b + O_SA,    128, 128, 128);
    rc |= write_p5(out_dir, "03_scores_Sb.ppm",       b + O_SB,    128, 128, 128);
    rc |= write_p5(out_dir, "04_exp_ea.ppm",          b + O_EA,    128, 128, 128);
    rc |= write_p5(out_dir, "04_exp_eb.ppm",          b + O_EB,    128, 128, 128);
    rc |= write_rowscalar_p5(out_dir, "05_softmax_sum_Z.ppm", b + O_Z, 128);
    rc |= write_rowscalar_p5(out_dir, "05_reciprocal_R.ppm",  b + O_R, 128);
    rc |= write_p5(out_dir, "06_prob_pqa.ppm",        b + O_PQA,   128, 128, 128);
    rc |= write_p5(out_dir, "06_prob_pqb.ppm",        b + O_PQB,   128, 128, 128);
    rc |= write_p5(out_dir, "07_output_Oa.ppm",       b + O_OA,    128, 128, 128);
    rc |= write_p5(out_dir, "07_output_Ob.ppm",       b + O_OB,    128, 128, 128);
    rc |= write_p5(out_dir, "08_output_frame.ppm",    out_frame,   128, 128, 128);
    if (rc) die("write_p5");

    fprintf(stderr, "attention_self_viz: wrote 16 stage PPMs to %s\n", out_dir);
    t1_buf_free(&stg);
    t1_close();
    return 0;
}
