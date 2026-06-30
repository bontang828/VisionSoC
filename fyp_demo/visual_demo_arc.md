# BONSAI — ECCV 2026 Demo Video Plan

**Purpose:** A ~3.5 min animated explainer for the ECCV 2026 conference, aimed at
vision researchers interested in *programmability* and *hardware architecture*.
**Spine:** "An image is one vector register" — we follow a single image on its
journey from the camera, into the 2D square register file, through one-instruction
whole-image compute, the free transpose, and out as a result.
**Style:** 3blue1brown-flavoured — clean pixel grids, integers overlaid on cells,
ops rippling/sweeping across the grid, smooth `Transform`s. Dark background,
restrained palette, on-screen captions (no audio).
**Tooling:** Manim Community v0.20.1 (installed). ffmpeg via imageio-ffmpeg.

---

## 0. Canonical numbers used on screen (keep consistent everywhere)

| Quantity                | Value shown                          |
|-------------------------|--------------------------------------|
| Image / register shape  | 128 × 128 px = **16,384 pixels**     |
| One image               | = **one logical 2D vector register** |
| Hardware rows (logical) | **128**, broadcast per instruction   |
| Physical lanes (silicon)| **2 lanes, replayed ×128**           |
| Element width           | int8 (SEW = 8)                       |
| Mode CSR                | `0x7c0` (0 = horizontal, 1 = vertical)|
| Live demo               | AMD Kria KV260, **30 FPS** HDMI      |

For pedagogy the on-screen grid is **rendered smaller** (e.g. 16×16) with a label
"128×128" so individual cells stay legible. The 16,384 number is the headline.

---

## 0b. On-die data path (from `fyp_diagram/selected/asic_die_v2.drawio`)

Two dies, with a hard chip boundary the story leans on:

```
NEAR-SENSOR PROCESSOR DIE                                   |  SoC DIE
[On-Die Image Sensor] -> [Frame Buffer] -> [DMA] -> [2D T1  |  [Off-Chip Scalar Core
                                            Vector Fabric]  |   (DDR / Host SoC)]
                                                 ^   |      |
                              Vector Instruction |   | Scalar Result Return
                                 (in)            |   v (out)
                                          [AMBA / AXI Interface] <-> High-Bandwidth + Indexed AXI
```

Key narrative facts this gives us:
- The **image sensor is on the same die** as the 2D fabric. Pixels go
  sensor → frame buffer → DMA → fabric **without leaving the chip**.
- Only **vector instructions come in** and **scalar results go out** across the
  die boundary to the off-chip scalar core. The frame itself stays resident.
- This is the literal picture behind "data stays on-die" (privacy/bandwidth).
  Use it directly in S2 (load) and S6 (why it matters).

---

## 1. Story arc (the highlights, in order)

The intellectual beats, each earning the next:

1. **Tension** — Vision data is 2D, but conventional vector/SIMD hardware flattens
   it into a 1D stream. Spatial neighbours get scattered; the programmer babysits
   strides. *What if the register kept the image's shape?*
2. **The idea** — In BONSAI an **image *is* one vector register.** One `vle8.v`
   pulls the whole 16,384-px frame on-chip; pixel (r,c) lands in register cell (r,c).
3. **The payoff of that idea** — **One instruction touches the entire image.**
   `vadd.vv` brightens all 16,384 pixels at once. 1 instruction vs 16,384 scalar ops.
4. **The 2D superpower** — A single config-register flip rotates the sweep 90°.
   Diagonal banking ⇒ **transpose is free** (column-read costs the same as row-read).
   This is what makes attention / matmul (A × Bᵀ) cheap.
5. **Honesty about the silicon** — The 128×128 square is *logical*; it is
   **time-multiplexed over just 2 physical lanes ×128**. Small, scalable silicon.
6. **Why it matters** — The image **stays resident on-die**; only the final answer
   leaves the chip (privacy + bandwidth). And it's **stock RISC-V RVV** — plain
   LLVM/GCC, portable, no proprietary ISA.
7. **Proof** — Live on real silicon: camera→display @30 FPS, dense optical flow,
   int8 matmul→attention. Kria KV260.

**The single sentence the viewer should leave with:**
> *BONSAI keeps an image's 2D shape all the way down to the register file, so one
> standard RISC-V instruction processes a whole frame — with transpose for free.*

---

## 2. Shot list (scene-by-scene, with Manim mapping)

Timings are targets; total ≈ 3:30. Each scene = one Manim `Scene` subclass so we
can render/iterate independently.

### S0 — Title sting  (0:00–0:08)  `class Title`
- **Show:** "BONSAI" grows from a single pixel that splits into a tiny grid;
  subtitle "A 2D RISC-V Vision Processor"; Imperial / ECCV 2026 line.
- **Manim:** `Square` grid reveal via `LaggedStart(FadeIn)`; `Text`.
- **Caption:** *An image is one vector register.*

### S1 — The tension: 2D flattened to 1D  (0:08–0:45)  `class FlattenProblem`
- **Show:** A 128×128 image (pixel grid). A "conventional vector register" appears
  as a long thin 1D strip. The image's rows peel off and concatenate into the 1D
  strip — neighbours visibly pulled apart. A roaming stride pointer hops awkwardly
  (`base + r*stride`).
- **Caption:** "Conventional SIMD flattens the image to 1D — neighbours drift
  apart, the programmer manages strides." End on "What if the register kept its shape?"
- **Manim:** `VGroup` of `Square`s (so cells animate individually); `Transform`
  rows → 1D row of cells; a `Dot`/arrow `add_updater` walking the strip.

### S2 — Load: image → square register (on-die)  (0:45–1:25)  `class LoadIntoRegister`
- **Show:** The die boundary from §0b. On-die sensor emits a frame → frame buffer →
  DMA. Code types in: `vle8.v v8, (frame)`. The whole image **flies as one block**
  into a **square 2D register file** labelled `v8`. One tracer pixel shows
  (r,c) → cell (r,c). Counter: "1 instruction · 16,384 pixels on-chip."
- **Caption:** "One load pulls the entire frame on-chip — and it never leaves."
- **Manim:** `Code`/typed `Text`; `image_grid.animate.move_to` + `Transform` into
  register frame; `Indicate` one cell; `Integer` counter; faint die-boundary line.

### S3 — One instruction, the whole image  (1:25–2:05)  `class OneInstrWholeImage`
- **Show:** Code: `vadd.vv v8, v8, v8`. A **horizontal sweep bar** travels across
  the register; every cell brightens as it passes (all rows in parallel). Tally:
  BONSAI = **1 instr** vs scalar CPU = **16,384 ops** (fast odometer). Brightened
  image results.
- **Caption:** "Every vector instruction is broadcast across 128 hardware rows —
  the whole image, one op."
- **Manim:** sweep = `Rectangle` + `ValueTracker` updater; cells recolour via
  `set_fill` keyed to sweep x; odometer = `Integer` updater.

### S4 — The 2D superpower: free transpose / vertical mode  (2:05–2:50)  `class FreeTranspose`
- **Show:** Two register copies. Caption "CSR `0x7c0` flips the sweep direction."
  Left = horizontal sweep (`vslideup` → image shifts right). Right = vertical sweep
  (`vslideup` → image shifts down). Then the headline: **diagonal banking** — the
  square's diagonal banks light up so a *column* read traces the same hardware path
  as a *row* read ⇒ the image **transposes with no extra instruction**. Tag:
  "free for attention: A × Bᵀ".
- **Caption:** "A column read costs the same as a row read — transpose is free."
- **Manim:** duplicate grid; diagonal highlight via `LaggedStart` over diagonal
  cells; transpose = `Transform` mapping cell (r,c)→(c,r) (rotate+flip composite or
  per-cell `TransformFromCopy`).

### S5 — How it really runs: time-multiplexing  (2:50–3:08)  `class TimeMultiplex`
- **Show:** Zoom out: the logical 128-row square dissolves to reveal **2 physical
  lanes**. A small hardware block replays ×128, row-counter ticking 0→127,
  "painting" the full square. Caption "The square is logical — 2 lanes, replayed
  ×128. Small, scalable silicon."
- **Manim:** 2 lane rectangles + `ValueTracker` row index; `LaggedStart` filling
  the grid row-by-row fast-forward.

### S6 — Why it matters + live proof  (3:08–3:28)  `class OnSensorAndDemos`
- **Show:** (a) "Data stays on-die" — reuse the die outline; image stays in the VRF,
  only a tiny result token crosses to the off-chip scalar core. (b) "Stock RISC-V
  RVV — plain LLVM/GCC, portable." (c) Montage of real results: camera→display,
  optical-flow colour field, matmul→attention. Label "Live @30 FPS · Kria KV260."
- **Manim:** chip = `RoundedRectangle`; montage = `ImageMobject`s / `FadeIn` panels
  (use real FPGA captures if available — see Assets).

### S7 — Closing card  (3:28–3:38)  `class Closing`
- **Show:** "BONSAI", one-line summary, authors (Bon Tang, Nicholas Fry, Shinjeong
  Kim, Andrew J. Davison, Paul H. J. Kelly), Imperial College London, ECCV 2026,
  contact. Pixel-grid logo motif echoes the open.

---

## 3. Visual system (consistency rules)

- **Background:** near-black (`#0e0e12`).
- **Palette:** pixel data = grayscale/natural; active sweep/highlight = teal
  (`#2dd4bf`); "results/output" accent = warm amber; code keywords = muted blue;
  transpose/negative paths = soft magenta.
- **Pixel cell:** `Square(side)` with thin `WHITE` stroke @ low opacity; value in
  `Integer` with `set_backstroke(BLACK, 3)` for legibility (3b1b trick).
- **Code:** monospace, typed-in reveal; keep to the two canonical poster lines so
  the "easy programming" claim lands.
- **Captions:** bottom third, one line, fade in/out per beat; consistent font/size.
- **Camera moves:** gentle zoom only when inspecting a cell or a bank.

---

## 4. Project structure (under fyp_demo/)

```
fyp_demo/
  visual_demo_arc.md          # this plan
  bonsai_video/
    manim.cfg                 # resolution, fps, ffmpeg path, output dir
    common/
      palette.py              # colours, fonts, caption helper
      pixel_grid.py           # PixelGrid mobject (reusable): cells, labels, sweep bar
      assets.py               # 128x128 sample frame generation / loading
    scenes/
      s0_title.py
      s1_flatten.py
      s2_load.py
      s3_one_instruction.py
      s4_free_transpose.py
      s5_time_multiplex.py
      s6_on_sensor_demos.py
      s7_closing.py
    assets/
      sample_frame.png        # 128x128 demo image (see Assets)
      demo_*.png/.mp4         # real FPGA captures if available
    render_all.py             # render every scene + concat to final mp4
```

Iterate one scene at a time:
`python -m manim -pql bonsai_video/scenes/s2_load.py LoadIntoRegister` (preview),
final pass `-qh` (1080p), then concatenate with ffmpeg in `render_all.py`.

---

## 5. Assets needed (decisions / TODO)

- [ ] **Sample 128×128 frame** for S1–S3. Default: generate a recognisable
      grayscale test image programmatically (so it's self-contained); OR drop in a
      real camera capture. *Need: pick one.*
- [ ] **Real FPGA result captures** for S6 (camera→display, optical flow, matmul).
      Strongly improves credibility. *Need: provide stills/short clips if available.*
- [ ] Imperial / ECCV logos for S7 (optional).

---

## 6. Open questions / risks

- Is S5 (time-multiplexing) too in-the-weeds for the vision audience? Can compress
  to ~10 s if pacing runs long.
- Show the actual transpose math (A×Bᵀ) or keep it a labelled gesture?
- Prefer `Text` over `Tex`/`MathTex` to avoid a hard LaTeX dependency unless
  equations are explicitly wanted.

---

## 7. Build status

- [x] Manim Community v0.20.1 installed; ffmpeg via imageio-ffmpeg located.
- [ ] Project scaffold (`common/`, `manim.cfg`).
- [ ] Scene S2 (Load) first vertical slice to lock the visual style.
- [ ] Remaining scenes.
- [ ] Final concat pass.
