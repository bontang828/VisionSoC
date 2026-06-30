# On-fabric 8×8-patch ViT attention — principle-level breakdown

**Created:** 2026-06-06
**Companion to:** `attention_kernel_status.md` (status/results), the source in
`vision_software/visionsoc_main/kernels/attention_patch*`.

This document explains, from first principles, what the patch-attention kernel computes,
what every value means, how each stage maps onto the fabric (horizontal vs vertical mode),
how the whole pipeline stays on the vector unit, what you are looking at on the HDMI
screen, and a precision / performance evaluation of each stage.

---

## 0. Terminology (read this first — it fixes the "hardware row" ambiguity)

The earlier notes said "hardware row" and "lane" loosely. Here is the precise vocabulary
used throughout this document:

| Term used here | What it is | Count |
|---|---|---|
| **data-plane row** | a horizontal row of the 128×128 byte plane that one vector register occupies. **A *token* (one patch) lives on a data-plane row.** | 128 |
| **data-plane column** | a vertical column of that 128×128 plane — the per-row vector elements. **A *feature* or a *key* lives on a data-plane column.** | 128 (at SEW=8) |
| **physical hardware lane** | an actual compute lane *instantiated in the FPGA fabric* (`laneNumber = dLen/datapathWidth = 128/64 = 2`). The 128×128 plane is **time-multiplexed onto these 2 lanes.** | **2** |
| **vl** (vector length) | number of *active elements along the operating axis*. In **horizontal** mode that axis is the **columns**; in **vertical** mode it is the **rows**. It is NOT the physical-lane count. | ≤128 |

So when I previously wrote "hw-row" I meant a **data-plane row** (a token). When I wrote
"128 lanes" I meant the **128 data-plane columns** (the per-row elements), *not* the 2
physical hardware lanes. The 2 physical lanes are the silicon; the 128 columns are the
logical vector elements those 2 lanes chew through over several cycles.

### How the 128×128 plane sits on 2 physical lanes (the substrate)
Deployed config `…2lanescale_fpga_maskopt`: `vLen=1024`, `SEW=8` ⇒ **128 elements per
row**; `laneScale=2`, `eLen=32` ⇒ `datapathWidth=64 b = 8 bytes/lane/cycle`; `dLen=128` ⇒
**2 physical lanes**; `rowNumber=1` ⇒ the 128 data-plane rows are processed **one batch at
a time** (`rowCounterBits=7` → 128 row-batches).

For one full-plane instruction the hardware does, conceptually:
```
for each of 128 data-plane rows (serial, time-multiplexed):     # 128 batches
    2 physical lanes × 8 bytes = 16 columns processed per cycle  # 128 cols / 16 = 8 cycles
```
≈ 8 cycles × 128 rows ≈ 1024 cycles for a trivial element-wise op (measured `vadd.vv`
≈ 4357 cyc incl. pipeline/overhead). **Cross-column / cross-row ops (`vrgather`,
reductions, the slide-build) cost several times more** because they need communication
between columns or rows that the 2-lane datapath must serialize. This is the single most
important performance fact and it drives every design choice below (see §8, §9).

### Horizontal vs vertical compute mode (CSR `0x7c0`)
- **Horizontal mode (`0x7c0 = 0`):** each data-plane *row* is processed independently; ops
  act **along the columns** of that row. `vl` = active columns. Reductions
  (`vredmaxu`/`vwredsumu`) collapse a row's columns to a single value in column 0. This is
  the "easy", native direction.
- **Vertical mode (`0x7c0 = 1`):** the VRF read/write path transposes via a diagonal
  byte-scatter (`SharedVRF.scala`), so ops act **across rows, within a column**. `vl` now
  counts **rows**. Vertical mode is **SEW=8 only** (the scatter is byte-granular). We use it
  for exactly two things: SEW=8 `vle8` *V-loads* (transpose-on-load) and SEW=8 `vrgather`
  *row broadcasts / row gathers*. **Every arithmetic/widening op is horizontal.**

---

## 1. What the kernel computes (the principle)

Scaled-dot-product attention, ViT-style, with an **8×8 pixel patch as the token**:
```
O = softmax( Q · Kᵀ / √d ) · V
```
- The 128×128 camera frame is cut into a 16×16 grid of **8×8 patches** → **256 tokens**,
  each a **64-dimensional** vector (the 64 patch pixels).
- **Q** = the live patches (the query: "what does this patch look like?").
- **K, V** = a **fixed pseudo-random dictionary** of 256 patterns (no pretrained weights).
  K = "the things we can match against"; V = "what to emit when we match."
- For each query patch, attention computes a similarity to all 256 K patterns, turns those
  similarities into a probability distribution (softmax), and emits the weighted blend of
  the 256 V patterns. **Output patch = a soft dictionary lookup keyed by patch content.**

> **Important — this is *cross*-attention against a *fixed* dictionary, not self-attention.**
> Because K and V are fixed (not computed from the frame), each output patch depends **only
> on its own query content**. This is why, on screen, only the patches you physically change
> react (see §7). True ViT *self*-attention (K,V from the same frame) would make every patch
> depend on every other — that is a documented possible extension, not the current demo.

---

## 2. Staying entirely on the fabric, and what happens to the DDR camera image

The architectural goal is *near-sensor*: the camera image enters the vector unit and the
**entire** computation — including tokenization — happens there, with **no PS-core
arithmetic**.

Per frame (`attention_patch_select.h::attention_patch_run`):
1. The camera capture pipeline (AP1302 → CSI-2 → frame-buffer DMA) writes a 128×128 Y frame
   into a **DDR** buffer (`in`). This is the only "off-fabric" residence, and it is
   unavoidable — it is where the camera hardware deposits pixels.
2. The PS does **one `memcpy`** of those 128×128 bytes into a staging buffer (a *copy*, not
   a computation — the only thing the PS touches), then hands physical addresses to the
   fabric.
3. **Everything after that is vector instructions:** the frame is loaded into the VRF and
   (a) patchified on-fabric (Stage 1), (b) run through attention on-fabric (Stage 2). The
   only PS work at the end is the inverse index map for display (un-patchify — again a copy,
   no arithmetic).

So the DDR image is just the camera's drop-off point; the PS never does softmax, matmul, or
any pixel math. Operands (K, V, exp/seed LUTs, and the patchify index tiles) are built once
on the PS and live in DDR/staging; the fabric reads them.

---

## 3. Stage 1 — on-fabric patchify (im2col): `attention_patch_im2col.S`

### What it must do (principle)
The frame loads naturally as **data-plane row = image row, data-plane column = image
column**. But attention needs **data-plane row = token (patch), data-plane column =
feature**. An 8×8 patch (Py,Px) occupies image rows `Py·8…+7` and columns `Px·8…+7` — it
is spread over **8 different data-plane rows**. Tokenization must gather each patch's 64
pixels onto a **single** data-plane row. Formally it is the index reshuffle
```
token[Py·16+Px][ry·8+rx]  ←  image[Py·8+ry][Px·8+rx]
```
i.e. it **swaps the `ry` and `Px` index groups** — pixels must move *between* data-plane
rows and columns.

### Why it needs three passes (and why the LSU can't do it)
The load/store address generator is hardwired to `addr = base + row·128 + col·stride`
(`SimpleAccessUnit.scala:816`): every data-plane row is rigidly 128 bytes apart in memory.
So a load can never fold an 8-image-row-tall patch into one row — that requires moving bytes
**across data-plane rows**, which only the vertical path can do. A single transpose is a
*full* row↔column swap, not the partial `ry↔Px` swap we need. The clean solution is a
**3-pass gather** (a Beneš-style routing of the index bits):
```
S [row P][col Q]   = image[blockbase+P][Q]
M1[row P][col L1]  = S [row P][col idxH1[P][L1]]      Pass H1  (horizontal)
M2[row t][col L1]  = M1[row idxV[t][L1]][col L1]      Pass V   (vertical)
T [row t][col f ]  = M2[row t][col idxH2[t][f]]       Pass H2  (horizontal)
```
- **Pass H1 (horizontal `vrgather.vv`):** within each data-plane row, permute columns into
  an intermediate layout `L1 = rx·16 + ((Px+ry) mod 16)`. Horizontal = it shuffles columns,
  rows untouched. `vl=128` (all columns).
- **Pass V (vertical `vrgather.vv`):** for each destination token-row, pull the right
  source row (the `Px↔ry` swap happens here), column preserved. This is the **per-element
  cross-row router** — the key primitive. **`vl=128` (here vl counts *rows*; all 128 must be
  touched).**
- **Pass H2 (horizontal `vrgather.vv`):** within each token row, place the columns at their
  final feature positions `f = ry·8+rx`. `vl=128`.

The three index tiles are **constants computed once on the PS** and the whole decomposition
was proven bit-exact in plain C (`ap_im2col_simulate`) before any board run.

### Meaning of the result / values
After Stage 1, register `T` holds, on **data-plane row `t`**, the 64 pixels of patch `t` in
**columns 0–63** (columns 64–127 are unused padding). The values are just the original
pixel bytes, rearranged — no arithmetic, so **no precision question here**. Two source
loads (image rows 0–63 → tokens 0–127, image rows 64–127 → tokens 128–255) produce the two
query tiles `Qa`, `Qb`.

---

## 4. Stage 2 — blocked attention: `attention_patch.S`

256 tokens overflow the 128-row plane in *both* axes (256 queries > 128 rows; a score row
has only 128 columns but there are 256 keys). So the 256×256 score matrix is computed as
**2 query-blocks × 2 key-blocks**. One kernel call processes **one query-block (128 query
tokens) against both key-blocks**; the host calls it twice (`Qa→Oa`, `Qb→Ob`).

We chose **blocked, not flash**: both 128-key score halves fit in registers simultaneously,
so an ordinary softmax runs over all 256 keys — exact, no online running-state machinery,
and ≈ the same cost as flash here. Layout convention throughout: **row = query token,
column = key** (for scores/weights) or **column = feature** (for Q and O).

### 4a. QK — the score (similarity)
For each key `j` (looped 0…127 within a block, repeated for both blocks):
1. **`vrgather.vx` (vertical, `vl=128` rows):** broadcast key `j`'s feature vector `K[j]`
   to **every** data-plane row. (Vertical because we replicate one row across all rows.
   `vl=128` because vl counts rows in vertical mode — a `vl=64` here was the one bring-up
   bug: it left rows 64–127 stale.)
2. **`vwmulu.vv` (horizontal, `vl=64` columns):** multiply, per row, `Q[token][feature] ·
   K[j][feature]` across the 64 feature columns. Widening `u8×u8→u16`.
3. **`vwredsumu.vs` (horizontal, `vl=64`):** sum those 64 products along the columns →
   `raw_S[token][j]` in column 0. Widening `u16→u32`.
4. Narrow `raw_S >> 13`, saturate to u8 → **`S8[token][j]`**, slid into the score row so
   that after 128 keys the data-plane row holds `S8[token][0…127]` (key = column).

**Meaning:** `S8[i][j]` is the **similarity of query patch `i` to dictionary pattern `j`**
(a scaled dot product). Higher = more alike. `>>13` folds the `1/√d` scaling and brings the
64-pixel dot product into the u8 range. Repeating for the second key-block gives the full
256-key score on two registers (`Sa`, `Sb`).

### 4b. Softmax — turn scores into a probability distribution (division-free)
All horizontal (reductions over the key columns), all per query-token-row:
1. **Row-max** `m = max_j S8[i][j]` over **both** halves (`vredmaxu` on `Sa`, chained into
   `Sb`). Meaning: the best-matching key's score; subtracting it is the standard softmax
   numerical-stability trick.
2. **`e[i][j] = expLUT[min(m − S8, 127)]`** (horizontal `vrgather.vv` LUT). Meaning: the
   **unnormalized attention weight** in Q0.8 fixed point — `255 ≈ 1.0` for the best match,
   decaying toward 0 for poor matches. No `exp` hardware needed; the LUT *is* `exp`. The √d
   scale and temperature τ are baked into `S_SHIFT` + the LUT decay.
3. **`Z[i] = Σ_j e[i][j]`** over all 256 keys (`vwredsumu` `u8→u16`, chained across halves).
   Meaning: the **softmax normalizer** (partition function).
4. **Reciprocal `R ≈ 2¹⁶ / Z`** via **Newton–Raphson** (no divide on the fabric):
   seed from a small LUT, then 3 iterations `R ← R·(2¹⁷ − Z·R) >> 16`, all SEW=32. Meaning:
   `R` is `1/Z` in 2¹⁶ fixed point, so we can normalize by *multiplying*.
5. **`pq[i][j] = (e·R) >> 8`** (Q0.8). Meaning: the **final softmax weight** — how much key
   `j` contributes to output `i`. `Σ_j pq ≈ 256` (= 1.0). This is "attention" proper: a
   probability distribution over the 256 dictionary entries for each query patch.

### 4c. PV — the output (weighted blend of values)
For each output feature `d` (looped 0…63):
1. **`vrgather.vx` (vertical, `vl=128`):** broadcast value column `V[:][d]` to all rows.
2. **`vwmulu` + `vwredsumu` (horizontal, `vl=128` over keys):** `Σ_j pq[i][j]·V[j][d]`,
   accumulated across **both** value-blocks (chained reduction over 256 keys).
3. Narrow `>>8`, saturate → **`O[i][d]`**, slid into the output row.

**Meaning:** `O[i][d]` is the `d`-th pixel of the output patch for query `i` — a **convex
blend of the 256 dictionary value-patterns**, weighted by how strongly the query matched
each key. If the query strongly matches one dictionary entry, the output ≈ that entry's
value patch; if it matches several, the output is their blend.

---

## 5. Horizontal vs vertical mode — per-operation summary

| Stage | Operation | Mode | Axis / `vl` | Why |
|---|---|---|---|---|
| im2col H1/H2 | `vrgather.vv` column shuffle | **H** | columns, vl=128 | rearrange columns within each row |
| im2col V | `vrgather.vv` cross-row gather | **V** | rows, vl=128 | move pixels between data-plane rows |
| QK | `vrgather.vx` broadcast `K[j]` | **V** | rows, vl=128 | replicate one key to all token rows |
| QK | `vwmulu`, `vwredsumu` | **H** | columns, vl=64 | multiply+reduce over the 64 feature columns |
| softmax | `vredmaxu`, `vwredsumu`, `vrgather.vv` LUT | **H** | columns, vl=128 | reduce/look-up along the key columns |
| Newton | `vmul/vrsub/vsrl` (SEW=32) | **H** | vl=1 | per-token scalar reciprocal (column 0) |
| PV | `vrgather.vx` broadcast `V[:][d]` | **V** | rows, vl=128 | replicate one value column to all rows |
| PV | `vwmulu`, `vwredsumu` | **H** | columns, vl=128 | multiply+reduce over the 256 key columns |
| Kt/V loads | `vle8` (transpose-on-load) | **V** | rows, vl=128 | load Kᵀ/V so a horizontal read sees the wanted orientation |

Rule of thumb: **vertical = "broadcast/move across rows" (always SEW=8); horizontal =
"compute/reduce along columns" (where all the widening lives).**

### 5a. The 64-feature padding and `vl=64` — not computing on the zeros

A token is an 8×8 patch = **64 features**, but a data-plane row is **128 columns** wide
(vLen=1024 ÷ SEW=8). So in every token tile (`Qa/Qb`, `Oa/Ob`) the features sit in
**columns 0–63 and columns 64–127 are zero padding** — the padding exists only because the
plane geometry is fixed at 128×128; the patch genuinely needs just 64 lanes.

**Yes — where the operating axis *is* those 64 features, `vl` is set to 64 so the fabric
does half the element-work and skips the padding entirely.** But `vl` counts a *different*
axis in the two modes, so this is not "always 64 for anything touching the tile":

| Stage / op | axis `vl` counts | `vl` | reason |
|---|---|---|---|
| QK `vwmulu` (Q·K element-mult) | the 64 **features** | **64** | one product per feature; cols 64–127 skipped |
| QK `vwredsumu` (sum the products) | the 64 **features** | **64** | reduce over 64 features only |
| build + store the output token (`vslideup`/`vmerge`/`vmv`/`vse8`) | the 64 output **features** | **64** | the output patch also has only 64 features |
| QK broadcast `vrgather.vx` (vertical) | the 128 **rows** (query tokens) | **128** | vertical `vl` = rows; 64 would leave tokens 64–127 stale (the bring-up bug) |
| QK score-commit `vslideup`/`vmerge`/`vmv` | the 128 **keys** | **128** | these place a score in the key dimension |
| PV `vwmulu`/`vwredsumu` | the 128 **keys** | **128** | PV reduces over keys, not features |

So the **QK reduction is over features → `vl=64`; the PV reduction is over keys → `vl=128`**,
and the `vl=64` reappears in PV only when building the 64-feature *output* token.

**The padding being zero is a correctness backstop, separate from the `vl=64` speed-up.**
Some ops legitimately span all 128 columns (e.g. the `vle8 Q` load at word 0 loads the whole
row). Because the padding is 0, any accidental full-width multiply-accumulate adds `0 × x =
0` and cannot corrupt a dot product — `vl=64` is the performance lever, zero-padding is the
safety net.

**It is measurable** (from `attention_self_perf`, cost ∝ active elements — §9):
`vwmulu` at `vl=64` ≈ **5.3 k cyc/op**, versus the `vl=128` key-dimension ops
(`vmerge`/`vmv`/`vslideup`) at ≈ **14 k cyc/op** — roughly the ~2× you expect from halving
the active elements. The one spot left un-trimmed is the `vle8 Q` load (`vl=128`, loads the
padding too): loads are the cheapest op class, so trimming it would not move the needle.

---

## 6. Why softmax maps so cleanly here
Because we put **key = column**, softmax-over-keys is a **horizontal reduction** — the
native, cheap direction (`vredmaxu`, `vwredsumu` collapse a row's columns to column 0). We
never need a cross-row reduction. The only cross-row traffic is the cheap-ish broadcasts of
one K/V row to all rows. This is the structural reason attention fits this fabric.

---

## 7. What you see on the HDMI screen

You see a **128×128 grayscale image, visibly organized as a 16×16 grid of 8×8 patches**
(blocky, because each patch is recomputed as a unit). Each 8×8 block is the **output patch
`O` for that token** — a blend of the fixed V dictionary patterns, chosen by how that
patch's pixels match the fixed K dictionary. (Chroma is forced neutral gray, so it's
luma-only.)

**When you put an object in the center:** the center patches' pixel content (their `Q`)
changes → their similarity scores to the K dictionary change → their softmax weights shift →
their output blend of V changes → **those patches change shade.** That is exactly the
"content-addressed lookup" working: the patch found different dictionary matches.

**Do the surrounding patches change too?** **No — not in this demo, and that is correct.**
Because K and V are a *fixed* dictionary (not derived from the frame), each output patch is
a function of **its own query only**:
`O[i] = softmax(Q_i · K) · V` depends on `Q_i`, nothing else. So a patch only reacts when
*its own* pixels change. You are seeing **per-patch cross-attention into a fixed
dictionary**, which is independent across patches.

**What would make the surroundings react (true global context)?** Make it **self-attention**
— compute K and V from the *current frame's* patches too (`Q=K=V=` frame patches). Then
output patch `i` = `Σ_j softmax(Q_i·K_j)·V_j` over the frame's own patches, so changing the
center patch changes the keys/values every other patch attends to → global coupling. The
kernel structure already supports this (stage `K`,`V` from the live patch tiles instead of
the fixed dictionary); it's a natural follow-up if you want the ViT "everything attends to
everything" behavior on screen. **It is implemented** — see `attention_self*` and §10.

### Reading the output patches: noisy vs smooth, dark vs bright
Each output patch is `O[i] = Σ_j pq[i][j]·V[j]` — a weighted average of the 256 fixed V
dictionary patterns, each of which is a *random* 8×8 pattern. So a patch's appearance is a
direct readout of the **shape of its attention distribution `pq[i]`**:

- **Noisy / high-texture patch ⇒ peaked attention.** `pq[i]` is concentrated on one (or a
  few) keys, so `O[i] ≈ V[j*]` — essentially a single *raw random* pattern, full of
  high-frequency variation. **Meaning: this input patch found a confident, specific match in
  the dictionary.**
- **Smooth / flat-gray patch ⇒ diffuse attention.** `pq[i]` is spread thinly over many keys,
  so `O[i]` is the average of many independent random patterns. Averaging cancels their
  variation (intra-patch variance shrinks ∝ Σ_j pq²), washing the patch toward a near-
  constant mid-gray. **Meaning: this input patch had no confident match — it is "generic".**

So **the output noise level visualizes the *confidence/peakedness* of that patch's
attention.** What makes a patch peaked is the **energy of its input patch**:
`Var_j(Q_i·K_j) = σ²_K · Σ_d Q_i[d]²`, i.e. the score spread scales with `Σ Q_i[d]²` (the
squared L2 norm). Bright and/or high-contrast/textured input patches (an object, an edge,
detail) → wide score spread → peaked → **noisy output**; dim or flat input patches (uniform
wall, shadow) → all keys score alike → flat → **smooth gray output**. This is why moving an
object into frame makes those patches "light up" with texture.

**Dark vs bright** is separate from noisiness — it reflects *which* dictionary entries won,
not the input brightness: `brightness(O[i]) = Σ_j pq[i][j]·mean(V[j])`. Smooth patches
hover near mid-gray (~128, the mean of random V), tilting slightly darker/brighter depending
on whether the weakly-matched keys have lower-/higher-mean value patterns; a noisy patch
takes the brightness of the specific `V[j*]` it locked onto. So a *dark smooth* patch ≈ "no
match, diffuse blend leaned dark"; a *bright noisy* patch ≈ "confident match on a bright,
textured dictionary entry".

(In the fixed-dictionary kernel this is per-patch and independent. In the **self-attention**
variant of §10 the same texture map instead reads "how strongly this patch matched *other
patches in the scene*", and couples globally.)

---

## 8. Accuracy / precision evaluation (per stage)

**Short answer:** yes, the kernel uses **widened SEW=16 and SEW=32 internally** (all in
horizontal mode) so the accumulations **do not clip**; the only narrowing is the intended
fixed-point scaling, and the two saturating clamps never trigger with the tuned data. The
end result is **bit-exact to the integer reference**, and within ≈ ±2 LSB of an idealized
fixed-point softmax (probe tolerances: scores/exp exact, Newton ≤1 LSB, output ≤2 LSB).

| Stage | Internal width | Worst-case value | Clip? |
|---|---|---|---|
| QK product `Q·K` | u8×u8 → **u16** (`vwmulu`) | 255·255 = 65 025 | fits u16, **no clip** |
| QK sum over 64 feats | u16 → **u32** (`vwredsumu`) | 64·65 025 = 4.16e6 | fits u32, **no clip** |
| `S8 = raw_S>>13` | → u8, `vminu 255` | tuned peak ≈ 140 | clamp present but **never hits 255** |
| `e = expLUT[…]` | u8 (exact LUT) | 0…255 | exact (Q0.8 quantized) |
| `Z = Σ e` (256 keys) | u8 → **u16** | 256·255 = 65 280 | fits u16 (max 65 535 — **1 LSB headroom**) |
| Newton `R≈2¹⁶/Z` | **u32** throughout | `R·(2¹⁷−Z·R)` ≤ ~1.7e7 | ≪ 2³² (4.29e9), **no overflow**; ≤1 LSB after 3 iters |
| `pq = (e·R)>>8` | u8·u16 → u16 → u8 | 255·155 = 39 525 | fits u16, **no clip**; Σpq ≈ 256 |
| PV sum over 256 keys | u8×u8→u16 → **u32** | 256·65 025 = 1.66e7 | fits u32, **no clip** |
| `O = sum>>8` | → u8, `vminu 255` | convex blend → ≤255; tuned range [0,117] | clamp present but **never hits 255** |

**"If a wider SEW is used, does it get clipped?"** — The wide stages (`u16`/`u32`) are
precisely what *prevents* clipping; they hold the full-precision products and sums. Clipping
could only occur at the two deliberate u8 narrows (`S8`, `O`), and we **tuned `S_SHIFT=13`
and the exp decay so neither saturates** (host sweep: 0 % score saturation, output ≤117).
If you shrank `S_SHIFT`, `S8` would start clipping at 255 and you'd lose the ability to
distinguish strong matches — that is the failure mode to watch.

**Register pressure with widening.** Widening costs register *width*, not many registers:
- `u16` products occupy an LMUL=2 pair (e.g. `v12:v13`); the `u32` accumulators are vl=1
  single elements (column 0 only) so cost one register each.
- Total live registers ≈ **16 of 32** (mask, Q, Kt/Va, Vb, the score/exp/pq register that
  is *reused* through its lifecycle `Sa→ea→pqa` in `v3` and `Sb→eb→pqb` in `v6`, the
  broadcast/slide scratch, the `u16` product pair, the `u32` dot, two narrow temporaries,
  the max, the LUT, the max-broadcast, and the zero source).
- The blocked (two-key-block) design needs `Sa,Sb,ea,eb,pqa,pqb` "live" but they share
  registers by **lifecycle reuse** (a value is consumed into the next as it's produced), so
  there is **no register-pressure problem** — ~16 registers of headroom remain. Widening did
  not force any spills.

---

## 9. Dominating factor (what makes it ~1 fps)

Measured: **≈ 993 ms/frame ≈ 1.0 fps** = ~2× 181 k cycles (im2col) + ~2× 29.7 M cycles
(attention) ≈ 59.8 M fabric cycles + a small PS un-patchify copy.

The cost is **not** the PS, **not** MMIO issue overhead, and **not** arithmetic. It is
**per-instruction fabric execution time on the 2-physical-lane, 128-row-time-multiplexed
datapath**, dominated by the **cross-lane data-movement** instructions in the matmul bodies:

| per output column | op | ~cycles | why expensive |
|---|---|---|---|
| broadcast K/V | `vrgather.vx` | ~14 k | cross-row gather, serialized on 2 lanes |
| build the result row | `vslideup` + `vmv.v.v` | ~25 k | per-column slide-and-commit |
| reduce | `vwredsumu` | ~9 k | cross-column reduction tree |

QK runs 256 key-columns × 2 query-blocks (= 512 columns); PV runs 64 feature-columns × 2
query-blocks, each over 2 key-blocks. Each column costs ~60–90 k cycles, ~75 % of which is
the **slide-build and broadcast — pure data movement, not the multiply/add**. The
multiply/add (`vmacc`/`vwmulu`) is cheap (~1× baseline). im2col is comparatively tiny (3
`vrgather` passes per block).

**Implications / levers (in order of impact):**
1. **More physical lanes** (a larger fabric synth, e.g. raising `laneScale`/`dLen`) would
   cut the 128-row × 2-lane serialization directly — the biggest lever, but needs a
   bitstream rebuild.
2. **Fewer active elements** per op: we already issue the QK feature ops at `vl=64` (half
   the columns) — the active-element lever. Masking unused work helps similarly.
3. **Restructure to avoid the per-column slide-build** (the ~25 k/column `vslideup`+`vmv`),
   e.g. a primitive that writes a reduction result directly to its destination column — this
   would attack the dominant cost but is an RTL change.

In short: this kernel is **data-movement-bound on a deliberately small (2-lane) fabric**;
correctness and the full on-fabric architecture are achieved, and throughput scales with
fabric width rather than with any software change.

---

## 10. Self-attention variant (`attention_self*`) — true global context

The kernel above is *cross*-attention against a fixed dictionary (§1, §7): each output patch
depends only on its own pixels. The **self-attention** variant makes **Q = K = V = the live
frame's own patches**, so every output patch is a similarity-weighted blend of *all the
other patches in the scene* — change one patch and every patch that attends to it shifts.
This is the "everything attends to everything" behaviour. **HW-verified end-to-end on
2026-06-06** (`attention_self_e2e_probe`: 16384 px bit-exact, ~994 ms/frame ≈ 1.0 fps).

### What changed (almost nothing — mostly reuse)
The attention *math* (QK → softmax → PV), the im2col patchify, and the issue sequencer are
**reused unchanged**. Only the operands and one small step differ:

| operand | cross-attention (`attention_patch`) | self-attention (`attention_self`) |
|---|---|---|
| `V` (value block) | fixed `ap_V` dictionary | **the patch tiles `Qa,Qb` directly** (V[key]=patch[key]) |
| `Kt` (key block, V-loaded → K) | fixed `ap_K`ᵀ dictionary | **transpose of the patch tiles** (`Kt = Qᵀ`, K[key]=patch[key]) |
| score shift / decay | `S_SHIFT=13`, decay 200 | `S_SHIFT=14`, decay 180 (see below) |

The one **new on-fabric step** is a **128×128 transpose** (`attention_self_transpose.S`,
2 instructions: a vertical `vle8` + horizontal `vse8`, which by the §4.3 primitive deposits
`Mᵀ` in memory). Per frame: `im2col → Qa,Qb`; `transpose → Kt_a=Qaᵀ, Kt_b=Qbᵀ`; then the
*same* attention kernel with `Q=Qa/Qb, K=Kt_a/Kt_b, V=Qa/Qb`. The whole thing stays on the
fabric (the transpose is two vector ops; no PS math).

### Why `S_SHIFT=14` for self
Self scores are `Q·Q`. The largest possible score is the diagonal `Q_i·Q_i = Σ patch_i² ≤
64·255² = 4.16e6`, and by Cauchy–Schwarz every off-diagonal is ≤ that too. `>>14` maps
4.16e6 → 254, so **`S_SHIFT=14` guarantees no saturation for *any* frame** (a tighter bound
than cross-attention, where K is random). Decay 180 (sharper) gives more peakedness; on the
*smooth synthetic* test frame self-attention still looks flat (all patches alike), but real
textured frames produce sharper, more interesting distributions.

### What you see on screen (self vs cross)
Now the texture/shade map reads **"how strongly this patch matches *other patches in the
scene*"**, and it **couples globally**: place an object in the centre and not only do the
centre patches change — **surrounding patches that resemble (or contrast with) the new
content also shift**, because the object changed the keys/values they attend to. That global
reaction is the visible difference from the fixed-dictionary version.

### Partial-centering variant (`attention_self_centered`) — brightness-bias fix
Raw-pixel self-attention is **brightness-biased**: the dot product `Q·Q` is dominated by the
patch DC (mean brightness), so bright patches dominate and dark structure washes out
(sharper temperature makes it *worse*, concentrating weight on the brightest patches). The
standard fix is LayerNorm's mean-subtraction. **Full** centering, however, removes *all* DC,
and on 8-bit fixed point a flat/low-texture patch then has ~0 covariance with everything →
uniform softmax → weights underflow to 0 → **black patches** (verified). So this variant uses
**partial centering (α=0.5):** `c = clamp(raw − mean/2 + 128, 0, 255)` for Q and K (V stays
raw). This **halves** the brightness bias (host: out-vs-in brightness corr 0.64 → 0.03)
while keeping enough DC floor to avoid degeneracy (zero degenerate tokens). It reuses the
`attention_self` math unchanged (the unsigned `>>14` score path) plus a small on-fabric
centering kernel (`attention_self_centered.S`); the `+128` offset keeps the intermediate ≥1
so the clamp is a single unsigned `vminu` (no signed-clamp ambiguity). HW-verified
(`attention_self_centered_e2e_probe`, 16384 px bit-exact, ~1.0 fps).

### 16-bit-score variant (`attention_self16`) — texture differentiation
Even partial-centered, the 8-bit score path can't *differentiate textures*: the score is
narrowed to 8 bits **before** the softmax max-subtract, and the texture covariance is only
~3–4 levels out of 256 (the brightness DC eats the rest), so same-brightness textures look
alike. The fix is to keep the score at **16-bit through the max-subtract** — `S16 = dot>>6`,
`m16 = max(S16)`, then narrow only the *difference* `(m16−S16)>>K16` into the 8-bit exp-LUT
index. The DC cancels in `m−S`, so the texture survives. Q,K partial-centered, V raw.
HW-verified (`attention_self16_e2e_probe`, 16384 px bit-exact, ~1.0 fps). Tunables:
`AP_S16_DIFF` (texture shift, default 8) and the decay. Caveat: still 8-bit *features* +
partial centering, so it sharpens texture grouping but isn't a full learned-ViT.

### Switching between the variants
`./sync_kernel.sh attention_patch` (cross, fixed dictionary) ·
`./sync_kernel.sh attention_self` (self, brightness-biased) ·
`./sync_kernel.sh attention_self_centered` (self, partial-centered — less brightness bias) ·
`./sync_kernel.sh attention_self16` (self, 16-bit score — best texture differentiation).
All share im2col, the attention math structure, and the probes; the row-token `attention`/
`attention_uram` kernels remain untouched.
