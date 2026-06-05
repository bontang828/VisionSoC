/*
 * attention_probe.c -- board self-check for the 2D-fabric attention kernel.
 *
 * Stages Q/Kt/V + exp/seed LUTs into a DDR udmabuf, issues kernels/attention.S
 * over MMIO with per-phase checkpoint stores enabled, reads back, and compares
 * every stage (S8 -> e -> Z -> R -> pq -> O) to the bit-accurate C reference in
 * kernels/attention_weights.h. A failure is isolated to the first wrong stage.
 *
 * Stage 1 (S8) is the FIRST hardware exercise of vwmulu/vwredsumu (the matmul
 * widening path was never run on HW). See fyp_doc/attention_kernel_status.md.
 *
 *   build: (on board)  cd ~/vision_software/libt1 && make test/attention_probe
 *   run:               sudo ./test/attention_probe
 */
#include "libt1.h"
#include "libt1_regs.h"
#include "kernels/attention_weights.h"
#include "kernels/attention.h"        /* generated: attention[], attention_count */
#include "kernels/attention_issue.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N   128u
#define D   128u
#define GRID (N * D)            /* 16384 bytes per 128x128 u8 buffer */
#define ARENA_BYTES (4u * 1024u * 1024u)
/* SEW=32 vl=1 checkpoint stores: the LSU row pitch is 128 ELEMENTS of the
 * current SEW, i.e. 128*4 = 512 bytes for e32 (empirically confirmed below).
 * So a 128-row u32 checkpoint needs 128*512 = 64 KB, not 16 KB. */
#define PITCH32 512u
#define U32_DBG_BYTES (N * PITCH32)   /* 65536 */

/* ---- simple page-aligned bump sub-allocator over one udmabuf arena ---- */
struct sub { uint8_t *va; uint32_t pa; };
static size_t g_off;

static struct sub suballoc(const struct t1_buf *arena, size_t bytes)
{
    size_t off = g_off;
    size_t need = (bytes + 4095u) & ~((size_t)4095u);   /* page align */
    if (off + need > arena->size) {
        fprintf(stderr, "arena exhausted (off=%zu need=%zu size=%zu)\n",
                off, need, arena->size);
        exit(2);
    }
    g_off += need;
    struct sub s = { (uint8_t *)arena->va + off, arena->pa + (uint32_t)off };
    return s;
}

/* ---- stage checks ---- */
static int check_grid(const char *name, const uint8_t *hw, const uint8_t *ref, int tol)
{
    int errs = 0, fr = -1, fc = -1, fg = 0, fe = 0;
    for (unsigned r = 0; r < N; r++) {
        for (unsigned c = 0; c < D; c++) {
            int g = hw[r * D + c], e = ref[r * D + c];
            int d = g - e; if (d < 0) d = -d;
            if (d > tol) {
                if (errs == 0) { fr = (int)r; fc = (int)c; fg = g; fe = e; }
                errs++;
            }
        }
    }
    if (errs == 0)
        printf("  [PASS] %-3s  (%u cells, tol %d)\n", name, GRID, tol);
    else
        printf("  [FAIL] %-3s  %d/%u cells; first [%d][%d] got %d exp %d\n",
               name, errs, GRID, fr, fc, fg, fe);
    return errs ? 1 : 0;
}

/* per-row u32 stored at byte r*PITCH32 by a vl=1 SEW=32 checkpoint store. */
static int check_perrow_u32(const char *name, const uint8_t *base,
                            const uint32_t *ref, int tol)
{
    int errs = 0, fr = -1; uint32_t fg = 0, fe = 0;
    for (unsigned r = 0; r < N; r++) {
        uint32_t g = *(const uint32_t *)(base + r * PITCH32);
        long d = (long)g - (long)ref[r]; if (d < 0) d = -d;
        if (d > tol) { if (errs == 0) { fr = (int)r; fg = g; fe = ref[r]; } errs++; }
    }
    if (errs == 0)
        printf("  [PASS] %-3s  (%u rows, tol %d)\n", name, N, tol);
    else
        printf("  [FAIL] %-3s  %d/%u rows; first row %d got %u exp %u\n",
               name, errs, N, fr, fg, fe);
    return errs ? 1 : 0;
}

int main(void)
{
    struct t1_buf arena = {0};
    int fail = 0;

    if (t1_init() < 0) { perror("t1_init"); return 1; }
    if (t1_buf_alloc(&arena, ARENA_BYTES) < 0) { perror("t1_buf_alloc"); return 1; }

    /* ---- sub-allocate buffers ---- */
    struct sub Q   = suballoc(&arena, GRID);
    struct sub Kt  = suballoc(&arena, GRID);
    struct sub V   = suballoc(&arena, GRID);
    struct sub EXP = suballoc(&arena, GRID);   /* replicated expLUT  (u8) */
    struct sub SEED= suballoc(&arena, GRID);   /* replicated seedLUT (u8) */
    struct sub O   = suballoc(&arena, GRID);
    struct sub dS8 = suballoc(&arena, GRID);
    struct sub dE  = suballoc(&arena, GRID);
    struct sub dZ  = suballoc(&arena, U32_DBG_BYTES);   /* u32 per row @ r*512 */
    struct sub dR  = suballoc(&arena, U32_DBG_BYTES);
    struct sub dPQ = suballoc(&arena, GRID);

    /* ---- stage deterministic inputs + LUTs ---- */
    attn_build_Q(Q.va);
    attn_build_Kt(Kt.va);
    attn_build_V(V.va);
    attn_build_exp_lut(EXP.va);
    attn_build_seed_lut(SEED.va);
    memset(O.va,   0, GRID);
    memset(dS8.va, 0, GRID); memset(dE.va, 0, GRID); memset(dPQ.va, 0, GRID);
    memset(dZ.va,  0, U32_DBG_BYTES); memset(dR.va, 0, U32_DBG_BYTES);

    if (t1_buf_sync_for_device(&arena) < 0) { perror("sync_for_device"); return 1; }

    /* ---- issue the kernel with checkpoints ---- */
    struct attn_pa pa = {
        .q = Q.pa, .kt = Kt.pa, .v = V.pa,
        .exp_lut = EXP.pa, .seed_lut = SEED.pa, .out = O.pa,
        .dbg_s8 = dS8.pa, .dbg_e = dE.pa, .dbg_z = dZ.pa,
        .dbg_r = dR.pa, .dbg_pq = dPQ.pa,
    };
    printf("issuing attention (%u words)...\n", attention_count);
    if (attention_issue(attention, &pa, 1) < 0) { perror("attention_issue"); return 1; }

    if (t1_buf_sync_for_cpu(&arena) < 0) { perror("sync_for_cpu"); return 1; }

    /* ---- C reference (identical fixed-point) ---- */
    static uint8_t  rS8[GRID], rE[GRID], rPQ[GRID], rO[GRID];
    static uint32_t rZ[N], rR[N];
    uint8_t etab[128]; attn_exp_table(etab);
    uint8_t stab[128]; attn_seed_table8(stab);
    attn_reference(Q.va, Kt.va, V.va, etab, stab, rS8, rE, rZ, rR, rPQ, rO);

    /* ---- diagnostics ---- */
    uint32_t zmin = ~0u, zmax = 0;
    for (unsigned i = 0; i < N; i++) { if (rZ[i] < zmin) zmin = rZ[i]; if (rZ[i] > zmax) zmax = rZ[i]; }
    printf("ref: Z range [%u,%u]  R[0]=%u  S8[0][0]=%u e[0][0]=%u pq[0][0]=%u O[0][0]=%u\n",
           zmin, zmax, rR[0], rS8[0], rE[0], rPQ[0], rO[0]);

    /* empirical SEW=32 vl=1 store-pitch probe: ref Z[0..2] should appear at
     * byte 0, PITCH32, 2*PITCH32 in dbg_z. */
    printf("pitch probe (ref Z[0]=%u Z[1]=%u Z[2]=%u):", rZ[0], rZ[1], rZ[2]);
    for (unsigned off = 0; off <= 1152; off += 128)
        printf(" @%u=%u", off, *(const uint32_t *)((const uint8_t *)dZ.va + off));
    printf("\n");

    /* ---- stage-by-stage compare ---- */
    printf("stage checks (hw vs reference):\n");
    fail |= check_grid("S8", dS8.va, rS8, 0);
    fail |= check_grid("e",  dE.va,  rE,  0);
    fail |= check_perrow_u32("Z", dZ.va, rZ, 0);
    fail |= check_perrow_u32("R", dR.va, rR, 1);   /* allow 1 LSB Newton jitter */
    fail |= check_grid("pq", dPQ.va, rPQ, 1);
    fail |= check_grid("O",  O.va,  rO,  2);

    printf("\n==== attention probe: %s ====\n", fail ? "FAIL" : "PASS");
    t1_close();
    return fail ? 1 : 0;
}
