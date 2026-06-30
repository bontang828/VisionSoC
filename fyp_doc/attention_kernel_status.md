# Attention kernel on the 2D fabric — strategy & status

**Created:** 2026-06-05
**Target bitstream:** `mudkip2d128big1bram1chain2lanescale_fpga_maskopt` (5t-maskopt,
vLen=1024, baseLMUL=1) — the active KV260 production bitstream.
**Verification vehicle:** board-side C probe `vision_software/libt1/test/attention_probe.c`
(self-checks against a C reference over `ssh kv260` — faster than t1emu).

This doc is the canonical reference for the scaled-dot-product **attention** kernel
(`softmax(Q·Kᵀ/√dₖ)·V`). Read it before touching `kernels/attention*.{S,h}` or the probe.

---

## 1. Goal

A self-contained attention operator that runs **entirely in the T1 vector unit** — both
matmuls, the softmax (max/exp/sum), the √dₖ scaling, and the division — with **no
scalar-core arithmetic** (the near-sensor intent). 128 tokens × 128 features; Q/K/V are
each a 128×128 u8 matrix (one `vle8` each, hw-row = token, element = feature).

K and V are **fixed pseudo-random** weights (no pretrained model); deterministic so the
probe's C reference can check the hardware bit-for-bit.

---

## 2. Why attention maps cleanly onto this fabric

Two observations make it work:

1. **Softmax reduces over the *element* axis.** Scores `S[i][j]` put query `i` on the
   hw-row and key `j` on the lane, so softmax-over-keys is a **horizontal** reduction
   (`vredmax.vs`, `vwredsumu.vs`) — the native, easy direction. No cross-row collapse.
2. **Both matmuls are the verified `matmul.S` idiom.** `Q·Kᵀ` and `P·V` are each "A
   H-loaded, B V-loaded, per-output-column V-mode `vrgather.vx` broadcast, horizontal
   widening MAC, narrow." Reused verbatim.

### Fabric constraints and how they're resolved

| Constraint | Resolution |
|---|---|
| No divide | One **Newton–Raphson reciprocal per token** `R≈2¹⁶/Z`, then `pq=e·R>>8`. |
| No exp | **`vrgather.vv` LUT** (the CNN "activation LUT" idiom): `e=expLUT[max−S]`. |
| No sqrt | `1/√dₖ` is a **constant** → folded into the score right-shift + exp-LUT decay. |
| Widening is horizontal-only | All `vwmulu`/`vwredsumu`/Newton math is issued `vmode=0`. |
| **Vertical mode is SEW=8-only** | Vertical is used *only* for SEW=8 V-loads + `vrgather.vx` row-broadcasts. See `project_vertical_mode_sew8_only` memory + `SharedVRF.scala:223` (byte-granular scatter). |

The last row is the critical safety invariant: the vertical diagonal-scatter in
`SharedVRF.scala` is **byte-granular**, so a SEW=16/32 element's bytes would scatter and
corrupt. Every SEW>8 op therefore stays horizontal — exactly how `matmul_select.h`
already schedules its widening.

---

## 3. Numeric / fixed-point spec (the C reference)

Tunable constants (in `kernels/attention_weights.h`): `S_SHIFT` (folds 1/√128),
exp-LUT decay (sets temperature τ), `F=16` (reciprocal fractional bits),
`NEWTON_ITERS=3`. The probe check is exact regardless of these values because the C
reference uses the same constants and the same staged LUTs.

```
# Phase 1 — scores
raw_S[i][j] = Σ_d Q[i][d]*K[j][d]                 # u32, exact (horizontal widening)
S8[i][j]    = sat_u8(raw_S[i][j] >> S_SHIFT)      # saturating narrow

# Phase 2 — softmax over keys j (horizontal)
m[i]        = max_j S8[i][j]                       # vredmax.vs (per-row, lane0)
idx[i][j]   = min(m[i] - S8[i][j], 127)            # vssubu + vminu
e[i][j]     = expLUT[idx[i][j]]                    # vrgather.vv; expLUT[0]=255
Z[i]        = Σ_j e[i][j]                          # vwredsumu (u32 ∈[255,32640])

# Phase 2.5 — Newton reciprocal R[i] ≈ 2^16/Z[i]  (SEW=32, once for all 128 tokens)
R           = seedLUT[min(Z>>8,127)]               # bucket-center seed (in-basin: Z·R₀<2)
repeat 3:   R = (R*((1<<17) - Z*R)) >> 16          # quadratic convergence

# Phase 2.6 — normalize to Q0.8
pq[i][j]    = (e[i][j]*R[i]) >> 8                   # Σ_j pq ≈ 256

# Phase 3 — weighted values
O[i][v]     = sat_u8((Σ_j pq[i][j]*V[j][v]) >> 8)   # convex combo of V
```

Peaked test inputs (triangular key/query bumps, per-query width varied to spread `Z`)
make the output ≈ a blurred permutation of `V` and exercise Newton across `Z` buckets.

---

## 4. Fabric mapping detail

| Phase | A (H-load) | B (V-load) | output-col loop | narrow |
|---|---|---|---|---|
| 1 (QK) | Q (`src_pa`/staged) | **Kᵀ** `Kt[d][j]=K[j][d]` | j=127..0 → `S8[:,j]` | `>>S_SHIFT`, sat |
| 3 (PV) | pq (in-register) | **V** `V[j][v]` | v=127..0 → `O[:,v]` | `>>8`, sat |

V-load Kᵀ ⇒ H-view = K ⇒ V-gather idx=j = row j of K (so `Q·Kᵀ`). V-load V ⇒ V-gather
idx=v = column v of V (so `P·V`). All vertical ops are SEW=8.

---

## 5. Files

| File | Role | Status |
|---|---|---|
| `kernels/attention_weights.h` | constants, deterministic Q/K/V gens, expLUT/seedLUT builders, C reference | ✅ done |
| `kernels/attention.S` (+ generated `attention.h`) | bare RVV stream: preamble · QK×128 · softmax+Newton · PV×128 · postamble · checkpoint stores | ✅ done, HW-verified |
| `kernels/attention_issue.h` | shared sequencer (per-word vtype/vl/vmode/rs1); vmode hard-coded so SEW>8 is always horizontal | ✅ done |
| `libt1/test/attention_probe.c` | board self-check: stage → issue per-phase with checkpoint readback → compare S8/e/Z/R/pq/O to reference | ✅ done, PASS |
| `kernels/attention_select.h` | Phase B: production select.h (Q=src, O=dst); stages Kt/V/LUTs in **one DDR udmabuf** | ✅ done |
| `main.c` `ACTIVE_KERNEL_DDR_IO` hook + `active_kernel.h` | Phase B: run attention all-DDR (in→out, skip URAM round-trip); activate | ✅ done |
| `libt1/test/attention_select_probe.c` | Phase B: verify the production `issue_active_kernel` path (DDR) without the camera; reports kernel cycles | ✅ done, PASS |
| `kernels/attention_uram.S` + `attention_uram_select.h` | URAM variant: DMA-staged operands in scratchpad, URAM round-trip — for DDR-vs-URAM comparison | ✅ done |
| `libt1/test/attention_uram_probe.c` | Phase B: verify the all-URAM path (DMA-populated); reports kernel cycles | ✅ done, PASS |

Also fixed `libt1/build_kernel.sh` (`od -v`) — `od` collapsed the 3 identical Newton
iterations into `*`, producing invalid `0x*,`. Needed for any kernel with ≥4 repeated words.

### Phase B integration findings (2026-06-05/06)
- **The hang was `t1_scratchpad_alloc` CPU-mmap populate, NOT URAM reads.** Original
  symptom: all-URAM select-probe (operands built in URAM via the uio6 mmap `.va`) wedged
  the whole board. Bisected: **DDR Q/O + DMA-populated URAM operands PASS, and full
  all-URAM (Q/O + operands, all DMA-populated) PASS** — the attention kernel reads/writes
  URAM fine. It is populating URAM by **CPU mmap** (the `t1_scratchpad_alloc` `.va` path)
  and then having T1 read it that wedges the fabric. **Fix: DMA-populate URAM** (matmul's
  proven pattern), never CPU-mmap for T1-read scratchpad buffers.
- **Two working data placements, both verified + the same output:**
  - `attention` (DDR): `ACTIVE_KERNEL_DDR_IO`, operands in one 64 KB DDR udmabuf,
    Q/O = DDR `in`/`out`. `attention_select_probe` PASS.
  - `attention_uram` (URAM): `attention_uram_select.h` + `attention_uram.S` (copy),
    operands DMA-staged into the scratchpad (0x8000+), Q/O via main.c's URAM round-trip.
    `attention_uram_probe` PASS. Switch with `./sync_kernel.sh attention{,_uram}`.
- **DDR vs URAM kernel cycles: 23,917,704 vs 23,900,455 (~0.07% — negligible).**
  Attention is bottlenecked by per-instruction MMIO issue overhead (~2068 `t1_issue`s,
  ~190 µs each ≈ 0.4 s/frame ≈ 2.5 fps), not operand memory — so URAM doesn't help.
  Speed would come from fewer issues / batching, a libt1/HW change.
- **Camera CSI2RX needs a clean power cycle.** Loading the VisionSoC overlay onto a board
  still running k26-starter-kits leaves the CSI2RX wedged: STREAMON → "Stream Line Buffer
  Full" → "soft reset timed out". After a fresh power cycle + overlay reload + the
  media-ctl recipe (`field:none`, VYYUYY8/128x128 on ap1302:2, csiss:0, csiss:1), the
  camera opens and attention stages + runs on live frames.

---

## 6. Build / deploy / test

```sh
# from dev host
scp kernels/attention.S kernels/attention_weights.h kernels/attention_issue.h \
    kv260:~/vision_software/visionsoc_main/kernels/
scp vision_software/libt1/test/attention_probe.c kv260:~/vision_software/libt1/test/
ssh kv260 'cd ~/vision_software/visionsoc_main && \
           ../libt1/build_kernel.sh kernels/attention.S kernels/attention.h attention'
ssh kv260 'cd ~/vision_software/libt1 && make test/attention_probe && sudo ./test/attention_probe'
```

**Incremental bring-up** — the probe checks each stage in order so a failure is isolated:

```
S8  → e  → Z  → R  → pq  → O
```

Stage 1 (`S8`) is the **first hardware exercise of `vwmulu`/`vwredsumu`** (matmul.S
widening was never run on HW). If it fails, the widening path — not attention — is the
culprit.

---

## 7. Implementation status

| Item | State | Notes |
|---|---|---|
| Design / plan | ✅ done | approved; `~/.claude/plans/reflective-snacking-lynx.md` |
| `attention_weights.h` | ✅ done | reference sanity-checked |
| `attention.S` + `attention.h` | ✅ done | assembles; HW-verified |
| `attention_issue.h` | ✅ done | — |
| `attention_probe.c` | ✅ done | — |
| Deploy + stage verify on KV260 | ✅ **PASS** | 5t-maskopt (vLen=1024) confirmed by bitstream hash |
| Phase B camera integration | ☐ todo | optional next step |

## 8. Testing status — 2026-06-05, KV260 5t-maskopt (vLen=1024)

All six pipeline stages verified against the bit-accurate C reference. **First hardware
validation of the widening path (`vwmulu`/`vwredsumu`) AND of divide-free softmax on
this fabric.**

| Stage | Result | Tol | Notes |
|---|---|---|---|
| S8 (scores, widening) | ✅ PASS | 0 (exact) | first HW widening test — works |
| e (exp LUT) | ✅ PASS | 0 (exact) | `vrgather.vv` LUT |
| Z (row sum) | ✅ PASS | 0 (exact) | `vwredsumu` e8→16 |
| R (Newton reciprocal) | ✅ PASS | ≤1 LSB | 3 iters; minor SEW=32 `vmul` rounding |
| pq (normalized weights) | ✅ PASS | ≤1 LSB | `(e*R)>>8` |
| O (output) | ✅ PASS | ≤2 LSB | accumulated fixed-point jitter |

### Bugs found & fixed during bring-up
1. **`vredmax.vs` → `vredmaxu.vs`**: scores exceed 127, so signed max returned the
   wrong peak (near-max elements wrongly got `e=255`). Unsigned reduction required.
2. **`build_kernel.sh` `od -v`**: `*` line-collapse broke the generated header.
3. **SEW-scaled LSU store pitch** (probe-side, not a kernel bug): the vl=1 row pitch is
   **128 elements of the current SEW** = 512 bytes for e32, not a fixed 128 bytes. The
   handoff §2 only documents the e8 (128-byte) case. Confirmed by the pitch probe
   (`Z[0]@0, Z[1]@512, Z[2]@1024`). See `project_lsu_row_pitch_scales_with_sew`.

## 9. Notes / future tuning
- Test inputs give `Z∈[803,1051]` (Newton buckets 3–4 only). To exercise the seed/Newton
  across more buckets (incl. small-Z sharp attention), sharpen `ATTN_EXP_DECAY_Q8` or
  widen the per-query bump-width span in `attention_weights.h` — kernel unchanged, only
  the staged data + reference move together.
- Newton seed is weak for `b≤1` (very sharp attention, `Z<512`); fine for the current
  range. Improve later if extreme-peak attention is needed.

---

## 10. DDR vs URAM operand placement — comparison

Two verified data placements, **identical numerical output**, measured per-frame kernel
cycles via `t1_perf` in the probes (warm-up call to stage, then one measured call):

| Operand placement | Kernel cycles / frame | Probe | Switch |
|---|---|---|---|
| **DDR** (`attention`) — operands in one 64 KB DDR udmabuf, Q/O = DDR `in`/`out` | **23,917,704** | `attention_select_probe` PASS | `./sync_kernel.sh attention` |
| **URAM** (`attention_uram`) — operands DMA-staged to scratchpad, Q/O via URAM round-trip | **23,900,455** | `attention_uram_probe` PASS | `./sync_kernel.sh attention_uram` |

Difference ≈ **17 k cycles (~0.07 %) — negligible.**

### Why they're the same (the observation explained)

> **Mechanism correction (2026-06-06).** An earlier draft of this section attributed the
> per-instruction cost to the MMIO issue/drain handshake ("MMIO-issue-bound"). That is
> **wrong** — the handshake was measured small. The real cost is per-instruction **fabric
> execution time** on a heavily **time-multiplexed datapath**.

The deployed config (`...2lanescale_fpga_maskopt`: `laneScale 2`, `dLen 128`, `vLen 1024`,
`rowNumber 1`) has `datapathWidth = 64 b` and **only `laneNumber = 2` physical lanes**, so
each 1024-bit vector op is serialized at ~16 B/cycle over its active elements / the 128
hw-rows. **Cost ∝ number of active elements**, and the cross-lane ops (`vrgather`,
reductions, vertical-mode VRF access) serialize worst — that is the ~11.5 k cycles/instr.
Per-op cycle table: `vision_task.benchmark_instructions_lmul1` run.log (also captured in the
`project_t1_instruction_cycle_costs` memory).

This *also* re-explains DDR ≈ URAM: the bottleneck is the serial datapath, not operand
memory bandwidth, so where the few one-shot operands live doesn't move the needle.

**Implication (corrected):** the throughput lever is **fewer / narrower expensive ops** —
minimise `vl` (active elements), mask down, avoid per-column slide-builds — *not* operand
placement and *not* "batched issue". This directly shaped the patch kernel (§11):
feature-dimension ops run at `vl = 64`. A bigger win would need more physical lanes (a
larger fabric synth). Both placements are kept: DDR is the default; `attention_uram` exists
for this comparison.

---

## 11. On-fabric 8×8-patch ViT attention — IMPLEMENTED + HW-verified (2026-06-06)

Token = an **8×8 pixel square patch** (ViT-style). A 128×128 frame = 16×16 = **256
patch-tokens × 64 features**, full 256×256 self-attention against a fixed pseudo-random
K/V dictionary. **Everything — including tokenisation — runs on the vector fabric** (no PS
arithmetic); **no bitstream rebuild** (SEW=8 vertical only). New `attention_patch*` kernel;
the row-token `attention`/`attention_uram` are untouched. Source of truth +
fixed-point + C reference: `kernels/attention_patch_weights.h`.

### Two on-fabric stages

**(A) Patchify / im2col — `attention_patch_im2col.S`.** The reshape `image[Py·8+ry]
[Px·8+rx] → token[Py·16+Px][ry·8+rx]` swaps the `ry`↔`Px` index groups. The LSU row pitch
is hardwired to 128 (`SimpleAccessUnit.scala:816`, `addr = base + hw_row·128 + elem·stride`),
so it can't fold an 8-image-row patch into one token row — that needs cross-hw-row movement.
Realised as a **3-pass gather H → V → H** using the **per-element vertical `vrgather.vv`**
(each dest element pulls from an arbitrary source hw-row, lane preserved):
```
S[P][Q] = image[blockbase+P][Q]
M1[P][L1] = S[P][idxH1[P][L1]]      # H, per-row lane gather
M2[t][L1] = M1[idxV[t][L1]][L1]     # V, per-element row gather (vmode=1)
T[t][f]   = M2[t][idxH2[t][f]]      # H
```
with `L1 = rx·16 + ((Px+ry) mod 16)`. Index tiles are PS-computed constants; the whole
decomposition is **verified in pure C** (`ap_im2col_simulate`) before board time. Two
source loads (image rows 0..63 / 64..127) → token tiles Qa, Qb. ~**181 k cyc/block**.

**(B) Blocked attention — `attention_patch.S`.** **Blocked, not flash** (chosen: both 128-
key score blocks fit in registers, so one ordinary softmax over 256 keys — exact, no online
m/l/Oacc state, ≈ same cost as flash but far simpler). One call = one query-block over both
key/value-blocks; called twice (Qa→Oa, Qb→Ob):
```
Sa = QK(Q,Ka)->v3 ; Sb = QK(Q,Kb)->v6           # reuse row-kernel matmul body
m  = max(rowmax Sa, rowmax Sb)                   # chained vredmaxu over 256 keys
ea = expLUT[m-Sa] ; eb = expLUT[m-Sb]            # rebased to the same global max
Z  = sum(ea)+sum(eb) ; R = 2^16/Z (Newton, seed shift 9)
pqa=(ea*R)>>8 ; pqb=(eb*R)>>8
O[d] = (sum_k pqa·Va[k][d] + sum_k pqb·Vb[k][d]) >> 8     # both V-blocks, d=0..63
```
Reuses QK/PV widening, exp-LUT, Newton. New vs the row kernel: two-register softmax
(chained max/sum across Sa,Sb), pqa/pqb, two resident V-blocks. ~**29.7 M cyc/query-block**.

### Critical implementation rules (learned on HW)
- **Vertical-mode `vl` counts HW-ROWS, not lanes.** Every vmode=1 op (vle8 V-load,
  `vrgather.vx/.vv`) must be `vl=128` to reach all 128 tokens. The `vl=64` active-element
  saving (the §10 lever) is valid ONLY for the **horizontal** feature ops (`vwmulu`,
  `vwredsumu`). A `vl=64` on the vertical QK broadcast left tokens 64..127 stale → caught
  immediately by the per-stage probe.
- **Vertical stays SEW=8** — patchify is pure byte rearrangement, so no SEW>8 vertical / no
  RTL change was needed.
- **Conditioning** (host-swept): `AP_S_SHIFT=13`, exp decay `200` → 0% score saturation,
  Z∈[422,1013], R∈[13,150] (no degenerate R=0), attention genuinely peaked (top key ≈44%).

### Data flow + files
`frame → im2col → Qa,Qb → attention → Oa,Ob → un-patchify → display`, all packed in one
4 MB staging udmabuf (camera `in`/`out` use the other two of three nodes).
`attention_patch_select.h::attention_patch_run()` is the per-frame entry; `main.c` calls it
under `ACTIVE_KERNEL_PATCH_IO`. Files: `kernels/attention_patch_im2col.{S,h}` + `_issue.h`,
`kernels/attention_patch.{S,h}` + `_issue.h`, `kernels/attention_patch_weights.h`,
`kernels/attention_patch_select.h`; probes `libt1/test/attention_patch_im2col_probe.c`,
`attention_patch_probe.c`, `attention_patch_e2e_probe.c`.

## 12. Patch-kernel testing status — 2026-06-06, KV260 5t-maskopt (vLen=1024)

| Probe | Scope | Result |
|---|---|---|
| `attention_patch_im2col_probe` | im2col: M1/M2/T vs C 3-pass replica + `ap_build_Q_block`, both blocks | ✅ **PASS** (first try) |
| `attention_patch_probe` | attention: Sa/Sb/ea/eb/Z/R/pqa/pqb/O vs `ap_reference`, both query-blocks | ✅ **PASS** |
| `attention_patch_e2e_probe` | full pipeline im2col→attention→un-patchify vs reference | ✅ **PASS** (16384 px, tol 2) |

- **First HW use of the vertical `vrgather.vv` cross-row gather** — works exactly as a
  per-element row router (index H-loaded). See `project_vertical_vrgather_crossrow` memory.
- **Measured throughput: ~993 ms/frame ≈ 1.0 fps** (`attention_patch_run`): ~2× 181 k
  (im2col) + ~2× 29.7 M (attention) ≈ 59.8 M fabric cyc + PS un-patchify. ~2.5× the row
  kernel, as predicted by the corrected §10 cost model (it's compute-bound, not issue-bound).
- **Bug found + fixed during bring-up:** vertical QK broadcast issued `vl=64` (lane count)
  but vertical `vl` = hw-row count → tokens 64..127 stale; fixed to `vl=128`.
- **Status:** `main.c` integration build-verified, active kernel switched to
  `attention_patch` (`./sync_kernel.sh attention_patch`). Live camera run pending a clean
  power cycle (camera CSI2RX wedge, see §5) + `./run_after_power_cycle.sh`.
