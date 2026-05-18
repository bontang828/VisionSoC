// sew16_32_masktest - exercises MaskUnitFpga's slide/gather/mask-producing
// paths at SEW=16 and SEW=32. The existing benchmark_instructions only runs
// SEW=8, so the Mux1H over the SEW=16/32 slices in writeBitMaskForSlide /
// writeBitMaskForExtend / laneMaskInput per-lane slicers is never exercised
// at runtime. LMUL=1 throughout to keep within the t1emu Spike+config's
// supported vlmax for SEW=16/32 on this build.

#include <stdio.h>
#include <stdint.h>
#include "emurt.h"

#define COLS 16

int16_t input_16[COLS];
int16_t output_16[COLS];
int32_t input_32[COLS];
int32_t output_32[COLS];

static int total_errors = 0;

static void init_16(void) {
    for (int i = 0; i < COLS; i++) {
        input_16[i]  = (int16_t)i;
        output_16[i] = (int16_t)0xa5a5;
    }
}
static void init_32(void) {
    for (int i = 0; i < COLS; i++) {
        input_32[i]  = (int32_t)i;
        output_32[i] = (int32_t)0xa5a5a5a5;
    }
}

// ---------- SEW=16 kernels ----------

__attribute__((naked, noinline))
static void k_slideup1_16(int16_t *a, int16_t *c, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e16, m1, ta, ma   \n\t"
        "vle16.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vslideup.vi v8, v9, 1               \n\t"
        "vse16.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

__attribute__((naked, noinline))
static void k_slidedown1_16(int16_t *a, int16_t *c, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e16, m1, ta, ma   \n\t"
        "vle16.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vslidedown.vi v8, v9, 1             \n\t"
        "vse16.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

// vrgather.vx idx in a3 - broadcast element idx to all positions
__attribute__((naked, noinline))
static void k_gather_16(int16_t *a, int16_t *c, size_t n, size_t idx) {
    __asm__ volatile (
        "vsetvli zero, a2, e16, m1, ta, ma   \n\t"
        "vle16.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vrgather.vx v8, v9, a3              \n\t"
        "vse16.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

// vmseq.vx val in a3 - output[i] = (input[i] == val) ? 1 : 0
__attribute__((naked, noinline))
static void k_mseq_16(int16_t *a, int16_t *c, size_t n, int16_t val) {
    __asm__ volatile (
        "vsetvli zero, a2, e16, m1, ta, ma   \n\t"
        "vle16.v v9,  (a0)                   \n\t"
        "vmseq.vx v0, v9, a3                 \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vmerge.vim v8, v8, 1, v0            \n\t"
        "vse16.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

// ---------- SEW=32 kernels ----------

__attribute__((naked, noinline))
static void k_slideup1_32(int32_t *a, int32_t *c, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e32, m1, ta, ma   \n\t"
        "vle32.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vslideup.vi v8, v9, 1               \n\t"
        "vse32.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

__attribute__((naked, noinline))
static void k_slidedown1_32(int32_t *a, int32_t *c, size_t n) {
    __asm__ volatile (
        "vsetvli zero, a2, e32, m1, ta, ma   \n\t"
        "vle32.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vslidedown.vi v8, v9, 1             \n\t"
        "vse32.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

__attribute__((naked, noinline))
static void k_gather_32(int32_t *a, int32_t *c, size_t n, size_t idx) {
    __asm__ volatile (
        "vsetvli zero, a2, e32, m1, ta, ma   \n\t"
        "vle32.v v9,  (a0)                   \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vrgather.vx v8, v9, a3              \n\t"
        "vse32.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

__attribute__((naked, noinline))
static void k_mseq_32(int32_t *a, int32_t *c, size_t n, int32_t val) {
    __asm__ volatile (
        "vsetvli zero, a2, e32, m1, ta, ma   \n\t"
        "vle32.v v9,  (a0)                   \n\t"
        "vmseq.vx v0, v9, a3                 \n\t"
        "vmv.v.i v8,  0                      \n\t"
        "vmerge.vim v8, v8, 1, v0            \n\t"
        "vse32.v v8,  (a1)                   \n\t"
        "ret                                 \n\t"
    );
}

// ---------- Runners ----------

static void check_16(const char *name, int16_t (*ref)(int)) {
    int errs = 0;
    for (int i = 0; i < COLS; i++) {
        int16_t expected = ref(i);
        if (output_16[i] != expected) {
            if (errs < 4)
                printf("  [%s] MISMATCH [%d]: got %d, expected %d\n",
                       name, i, output_16[i], expected);
            errs++;
        }
    }
    total_errors += errs;
    printf("[%s] %s (%d errors)\n", name, errs == 0 ? "PASS" : "FAIL", errs);
}

static void check_32(const char *name, int32_t (*ref)(int)) {
    int errs = 0;
    for (int i = 0; i < COLS; i++) {
        int32_t expected = ref(i);
        if (output_32[i] != expected) {
            if (errs < 4)
                printf("  [%s] MISMATCH [%d]: got %d, expected %d\n",
                       name, i, (int)output_32[i], (int)expected);
            errs++;
        }
    }
    total_errors += errs;
    printf("[%s] %s (%d errors)\n", name, errs == 0 ? "PASS" : "FAIL", errs);
}

static int16_t ref_slideup1_16(int i)   { return (int16_t)(i == 0 ? 0 : (i - 1)); }
static int16_t ref_slidedown1_16(int i) { return (int16_t)(i == COLS - 1 ? 0 : (i + 1)); }
static int16_t ref_gather5_16(int i)    { (void)i; return (int16_t)5; }
static int16_t ref_mseq5_16(int i)      { return (int16_t)(i == 5 ? 1 : 0); }

static int32_t ref_slideup1_32(int i)   { return (int32_t)(i == 0 ? 0 : (i - 1)); }
static int32_t ref_slidedown1_32(int i) { return (int32_t)(i == COLS - 1 ? 0 : (i + 1)); }
static int32_t ref_gather5_32(int i)    { (void)i; return (int32_t)5; }
static int32_t ref_mseq5_32(int i)      { return (int32_t)(i == 5 ? 1 : 0); }

void test() {
    printf("=== SEW=16 / SEW=32 MaskUnitFpga regression (LMUL=1) ===\n");

    // SEW=16
    init_16(); k_slideup1_16(input_16, output_16, COLS);
    check_16("SEW=16 vslideup.vi 1", ref_slideup1_16);

    init_16(); k_slidedown1_16(input_16, output_16, COLS);
    check_16("SEW=16 vslidedown.vi 1", ref_slidedown1_16);

    init_16(); k_gather_16(input_16, output_16, COLS, 5);
    check_16("SEW=16 vrgather.vx idx=5", ref_gather5_16);

    init_16(); k_mseq_16(input_16, output_16, COLS, 5);
    check_16("SEW=16 vmseq.vx val=5 -> 0/1", ref_mseq5_16);

    // SEW=32
    init_32(); k_slideup1_32(input_32, output_32, COLS);
    check_32("SEW=32 vslideup.vi 1", ref_slideup1_32);

    init_32(); k_slidedown1_32(input_32, output_32, COLS);
    check_32("SEW=32 vslidedown.vi 1", ref_slidedown1_32);

    init_32(); k_gather_32(input_32, output_32, COLS, 5);
    check_32("SEW=32 vrgather.vx idx=5", ref_gather5_32);

    init_32(); k_mseq_32(input_32, output_32, COLS, 5);
    check_32("SEW=32 vmseq.vx val=5 -> 0/1", ref_mseq5_32);

    printf("\n=== sew16_32_masktest summary: ");
    if (total_errors == 0) {
        printf("PASS (all 8 kernels correct) ===\n");
    } else {
        printf("FAIL (%d total errors) ===\n", total_errors);
    }
}
