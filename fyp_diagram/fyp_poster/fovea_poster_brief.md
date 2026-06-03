# FOVEA / RVV² — Poster Design Brief

**For:** the next session (incl. the web Claude + Canva plugin) doing the final visual polish.
**Source of truth files in this folder:** `fovea_poster.html` (the reference layout + copy),
this brief, and the real diagram assets listed in §8.

This document is everything needed to recreate / polish the poster **without re-deriving the
architecture**. Treat the technical facts in §2 as ground truth — do **not** invent new claims.

---

## 1. What this is & who it's for

A one-page **conference poster** (CVPR-style) selling a new vision-processor architecture for
**on-/near-sensor processing**. The audience is mixed and we are deliberately courting two crowds:

- **Computer-vision researchers** — many won't know RISC-V. Sell *ease of programming* + *open standard*.
- **Hardware engineers** — sell the *architecture cleverness* + *it's real on FPGA*.

The poster must let each crowd **find their lane fast**: large `Software` / `Hardware` / `Today`
section headers so people catch the word for their field and dive in.

**Goal of the poster:** make people believe this is (a) a genuinely new, clever idea, (b) easy to use,
(c) already working on real hardware. The narrative arc is **Problem → Software → Hardware → Today**.

---

## 2. Ground-truth technical facts (do not contradict these)

The architecture is **FOVEA / RVV²** — a *2-D time-multiplexed RISC-V Vector fabric*, built on the
open-source **ChipsAlliance T1** vector core. Core facts:

| Fact | Value / detail |
|---|---|
| ISA | Standard **RISC-V "V" (RVV)** vector extension — *not* a custom ISA. One extra CSR (`0x7c0`) flips H/V mode. |
| The big idea | Every vector instruction is broadcast across a **128-row grid**. One `vle8.v` loads a whole **128×128** image = **16,384 pixels**; one op processes all of them. |
| Two directions | **Horizontal** mode = compute left↔right along an image row. **Vertical** mode = up↕down across image rows. Toggled by `csrw 0x7c0`. This gives separable filters, transpose, matmul "for free". |
| Square 2-D VRF | The 128×128 image lives in a **square register file** whose bytes are scattered on **diagonal banks** (`t1/src/vrf/SharedVRF.scala`). A full *column* access is as conflict-free as a *row* access → **transpose is a free read/write permutation**, no transpose engine. |
| Near-sensor dataflow | Camera pixels stream straight into the VRF and stay on-chip; the program emits **only the result** (a reduction, an arg-max coordinate, a motion vector), not the whole frame. *This is the architectural capability* — the HDMI display demo does stream frames back because it's a display demo. |
| Time-multiplex efficiency | The 128 logical rows are **time-multiplexed over just 2 physical lanes** (deployed config `--dLen 128 --laneScale 2` → laneNumber = 2). So it's **128× logical parallelism without 16,384 PEs** — fits a commodity FPGA, ASIC-ready. |
| Vector length | Deployed bitstreams: **vLen=256** and **vLen=1024** (4× register file). At vLen=1024, LMUL=1, SEW=8 → 128 elements = one image row. |
| Platform | **AMD Kria KV260** (Zynq UltraScale+). Synth/impl: ~105K LUT (89%), 144/144 BRAM, timing closed (WNS +0.24 ns). |
| Live performance | End-to-end camera→fabric→HDMI at **~20–30 fps** on 128×128 frames; T1 kernel ≈ 6 ms/frame. |
| Verified kernels (difftested bit-exact vs Spike) | Sobel, Gaussian blur, FC+ReLU, **128×128 int8 matmul** (via the free transpose), end-to-end **int8 CNN digit classifier** (10/10 correct), dense optical flow. |
| Toolchain | LLVM/GCC (stock RVV), Spike reference model, ChipsAlliance T1, Chisel RTL, Verilator, bare-metal Linux + `libt1`. |

**Accuracy guardrails (things NOT to claim):**
- No floating-point / `exp` / `div` primitives — inference is **int8**; matmul is int8; "attention" is the
  *building block reach*, not a full FP transformer.
- "Only the answer leaves" = architectural intent/capability, not "the display demo emits one pixel".
- fps is **~20–30**, not 60 (sensor/display-limited, not T1-limited).
- It's a **research FPGA prototype / FYP**, not a taped-out product.

---

## 3. The name (put a short version on the poster)

**FOVEA** = the fovea is the tiny central pit of the **retina** where vision is **sharpest** and where the
eye does its **first processing right at the sensor**. Perfect metaphor for **near-sensor vision**: dense,
sharp, on-sensor compute. It also backronyms to **F**ocal-plane **O**n-sensor **VE**ctor **A**rchitecture.
**RVV²** = "RVV, squared" — standard RISC-V Vector, plus a free **2nd (vertical) dimension**. It signals the
open-ISA pedigree the RISC-V crowd loves.

> Alternative if the team prefers the repo name: lead with **VisionSoC** and keep "FOVEA / RVV²" as the
> architecture name. One-line swap. (The repo/system is called VisionSoC.)

---

## 4. Format & layout (THIS poster is **landscape, 2:1**)

- **Aspect: 2 : 1 landscape.** Print page in the HTML is `1188 mm × 594 mm`. On-screen reference canvas is
  `1600 × 800 px`. In Canva, set a custom size with a **2:1 ratio** (e.g. 48 × 24 in, or 1188 × 594 mm).
- **Layout = left story rail + 3 big columns** (read left→right = the narrative):

```
┌───────────────────────────────────────────────────────────────────────────────────────┐
│ topline: FYP · near-sensor vision · Imperial      |    open RISC-V · FPGA · live camera │
├──────────────────────┬────────────────────────────────────────────────────────────────┤
│  STORY RAIL (~27%)    │  RISC-V plain-language hook (slim band, full width of right)     │
│                       ├──────────────────┬──────────────────┬──────────────────────────┤
│  FOVEA  ^RVV²         │   ███ SOFTWARE    │   ███ HARDWARE    │   ███ TODAY              │
│  tagline              │  for CV research  │  for HW engineers │  running on silicon      │
│  why "FOVEA" note     │                   │                   │                          │
│  THE PROBLEM (2 lines)│  • image = vreg   │  ┌──────┬──────┐  │  ▸ camera → display      │
│                       │    + code snippet │  │ ↔ ↕  │ □VRF │  │  ▸ matmul → attention    │
│  ┌ near-sensor ─────┐ │  • open ISA       │  ├──────┼──────┤  │  ▸ optical flow          │
│  │ today vs FOVEA   │ │  • open ecosystem │  │ near │ 128× │  │  (3 live cards w/ icons) │
│  │ dataflow diagram │ │                   │  └──────┴──────┘  │                          │
│  └──────────────────┘ │  (3 icon points)  │  (2×2 image tiles)│                          │
│  stat chips ▮▮▮▮▮▮▮    │                   │                   │                          │
├──────────────────────┴────────────────────────────────────────────────────────────────┤
│ footer: FOVEA · RVV² — near-sensor vision        built with ChipsAlliance T1 · Chisel …  │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

- **Section headers `Software` / `Hardware` / `Today` are intentionally LARGE** (serif, ~40 px on the 1600-px
  canvas, colour-coded). Keep them big in Canva — they are the wayfinding for the two audiences.
- **Design intent: more image, less text.** Each point = an icon/diagram + a bold headline + ≤1 short line.
  Prefer swapping my placeholder SVGs for the real renders in §8.

---

## 5. Exact copy (use verbatim or lightly edited)

**Title:** FOVEA  ^RVV²
**Tagline (serif italic):** *A camera that thinks in whole pictures — and answers in software you already know.*
**Why-FOVEA note:** The fovea is the eye's sharpest spot, where the retina senses *and* computes — right at
the sensor. So does this chip. (**F**ocal-plane **O**n-sensor **VE**ctor **A**rchitecture.)

**THE PROBLEM:** Cameras fire *millions of pixels* per frame. We ship them all to a distant GPU, burn power
moving data — then throw most away. FOVEA computes *where the image is born*; only the answer leaves.

**RISC-V hook (slim band):** **RISC-V is an open, royalty-free instruction set** — the open answer to Arm and
x86. Its **vector extension (RVV)** is the standard SIMD muscle. FOVEA **is** an RVV machine, so your code is
the same vector code every RISC-V chip understands — not a one-off DSL.

### SOFTWARE — *for vision researchers* (3 points)
1. **An image is one vector register** — Full kernels (edges, blur, matmul, CNN) in a few dozen RVV
   instructions. **No HDL.**  *(snippet: `vle8.v v8,(frame)` → `vmul.vv v8,v8,vK`)*
2. **Open, standard ISA** — Plain RISC-V Vector, built by stock **LLVM/GCC**. Portable to any RVV chip — no lock-in.
3. **A whole open ecosystem** — Inherit a toolchain, not a one-off. Every kernel **bit-exact** vs the
   reference model. *(chips: LLVM/GCC · Spike · T1 core · Verilator)*

### HARDWARE — *for hardware engineers* (4 tiles, 2×2)
1. **Compute ↔ and ↕** — One CSR flip sweeps a row or a column — 2-D filters, no data juggling.
2. **Square 2-D register file** — Diagonal banks: a column read = a row read. Transpose is **free**.
3. **Near-sensor by design** — Image stays in the VRF; **only the answer leaves** the chip.
4. **128× — not 128× the silicon** — Grid is **time-multiplexed over 2 lanes**: image-wide machine on a
   commodity FPGA.

### TODAY — *running on real silicon* (3 cards)
1. **Real-time camera → display** *(Live on FPGA)* — Full SoC on **AMD Kria KV260**: camera → FOVEA → HDMI at
   **~20–30 fps**, plain C on Linux.
2. **Matmul → attention** *(Verified kernel)* — int8 128×128 matmul via free transpose + dot-products — the
   **transformer attention** primitive (Q·Kᵀ · softmax·V building block).
3. **Dense optical flow** *(Live on FPGA)* — Per-pixel motion as **colour-coded direction** — per-row SAD +
   arg-min, every pixel in parallel.

**Stat chips (rail bottom):** 16,384 px / instruction · 128×128 2-D grid · 2 lanes → 128 rows · ~20–30 fps live ·
1024-bit vector · RISC-V V open ISA · KV260 FPGA.

**Footer:** FOVEA · RVV² — on-/near-sensor vision   |   built with ChipsAlliance T1 · Chisel RTL · RISC-V V ·
Verilator · bare-metal Linux + libt1.

---

## 6. Visual style tokens

- **Palette:** paper `#f7f5ef`, ink `#1b2138`, soft-ink `#4a5168`, muted `#8a8f9c`, hairline `#e2ded2`.
  Accents: **coral `#e3674f`** (problem / image / motion), **teal `#1f8a8a`** (hardware), **indigo `#3a4a8c`**
  (software / RISC-V), **green `#2f8f5b`** (today / near-sensor), gold `#d9a441`.
- **Type:** headings serif (Iowan/Palatino/Georgia family); labels & code mono; body sans. In Canva use a
  refined serif (e.g. *Source Serif / Lora / Spectral*) for headings, a clean sans for body, a mono for code/labels.
- **Colour-coding the 3 columns** (indigo Software / teal Hardware / green Today) is the wayfinding system —
  keep it.
- Rounded 11–12 px cards, thin 1 px borders, subtle radial background glows. Keep it calm and editorial,
  not "techy neon".

---

## 7. Export

- **From the HTML:** open in Chrome → Print → *Save as PDF* → custom paper **1188 × 594 mm**, **Landscape**,
  margins **None**, **Background graphics ON**.
- **For Canva:** create a 2:1 canvas; rebuild using §4 layout + §5 copy + §8 images. Or import the PDF as a
  background and overlay editable text. Keep the big section headers and the colour-coding.

---

## 8. Real assets to drop in (prefer these over my placeholder SVGs)

All under `../selected/` (i.e. `fyp_diagram/selected/`). "More image, less text" — use them.

| File | Shows | Put it in |
|---|---|---|
| `image_to_vector_fabric_v3.png` / `.svg` | Image → 128-row vector fabric (the big idea) | Story rail or Software point 1 |
| `rvv_vs_t1_v2.drawio` | Stock RVV (1-D) vs this 2-D fabric | Near a Software/Hardware divider — great "what's new" |
| `vrf_diagonal_banking_v2.png` / `…_v5_per_word.drawio` | The diagonal-banked square VRF | **Hardware tile 2** (square 2-D VRF) — replace my diag SVG |
| `fabric_instruction_basics.drawio` | H vs V instruction sweep | **Hardware tile 1** (↔ / ↕) |
| `T1_abstract_arch_fpga.drawio` / `fpga_system_top_t1style_v4.drawio` | System/architecture block diagram | Hardware column header or story rail |
| `t1_vs_scamp5_v3.drawio` | vs SCAMP-5 (a known near-sensor/focal-plane processor) | Optional "positioning" chip near Problem — strong for the CV crowd |
| `matmul_8bitraw_short_perf.png` | Matmul performance | **Today card 2** (matmul→attention) |
| `optical_flow_perf.png` | Optical-flow performance | **Today card 3** (optical flow) |
| `sobel_perf.png` / `sobel_kernel_steps_v2.drawio` | Sobel steps / perf | Optional extra Today/Software visual |
| `asic_die.drawio` / `asic_die_v2.drawio` | ASIC die plot | Optional — reinforces "ASIC-ready" in Hardware tile 4 |

**Best-bang swaps:** (1) `image_to_vector_fabric_v3` in the rail, (2) `vrf_diagonal_banking` in HW tile 2,
(3) the two `*_perf.png` charts in the Today cards, (4) a live camera/HDMI photo if one exists — a real
photo of the board running beats any diagram for the "Today" section.

---

## 9. Open choices for the editor

- **Name:** FOVEA / RVV² (current) vs lead-with-VisionSoC. — pick one, keep the other as subtitle.
- **Replace placeholder SVGs** with §8 real renders (recommended) and ideally **one photo of the live board**.
- **Authors / repo URL / QR code** — add to the footer (currently blank).
- **Logos:** Imperial College + RISC-V International marks could go top-right of the topline.
- If space feels tight at 2:1, the most compressible items are: the code snippet, the ecosystem chip row, and
  the Problem prose (the dataflow diagram carries it).
