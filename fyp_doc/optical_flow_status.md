# Optical Flow Status

Date: 2026-05-19

## Current Status

The live all-black optical-flow output was traced to the host-side T1 dispatcher, not to the SAD/argmin kernel body. After that fix, the live output showed nonzero direction-map values. A small static/noise threshold has now been added to reduce speckle in static regions. The display path also has a luma-only fast path for optical flow, so it no longer pays for full NV12 chroma conversion.

Root cause:

- `vmul.vx v24, v24, a3` assembles as an OPMVX vector-scalar instruction: opcode `0x57`, `funct3 = 0x6`.
- `kernels/optical_flow_select.h` only injected `op.rs1 = 50` for OPIVX (`funct3 = 0x4`).
- Therefore the live `visionsoc_main` dispatcher issued the final scale multiply with `op.rs1 = 0`.
- Any nonzero direction class was multiplied by zero before `vse8.v v24, (a2)`, so `outY` stayed `0/0/0.0`.

Fix applied:

- `optical_flow_select.h` now routes scalar `a3 -> 50` for both OPIVX and OPMVX vector-scalar op families.
- This matches `optical_flow_perf.c`, which already handled both `AKD_FUNCT3_OPIVX` and `AKD_FUNCT3_OPMVX`.

Noise threshold applied:

- The kernel recomputes `SAD_static = abs(curr - prev)` after the 5-way argmin.
- If `SAD_static <= 6`, the pixel's direction class is forced back to `0` before the final `argmin * 50` visualization multiply.
- This is a temporal deadband: small frame-to-frame luma changes are treated as static even if a directional candidate happened to win by noise.
- The threshold adds five vector instructions: `vsub.vv`, `vrsub.vi`, `vmax.vv`, `vmsleu.vi`, and `vmerge.vim`.

Display path update:

- The DRM scanout buffers are `DRM_FORMAT_RGB565`, so the optical-flow `uint8_t` direction map cannot be displayed directly as raw luma.
- `optical_flow_select.h` defines `ACTIVE_KERNEL_FLOW_COLOR=1`, so `main.c` and `main_perf.c` map direction codes to distinct RGB565 colours:
  - `0` static: black
  - `50` right: red
  - `100` left: cyan
  - `150` down: green
  - `200` up: magenta
- Other `ACTIVE_KERNEL_NEUTRAL_UV=1` kernels still use a grayscale RGB565 pack path instead of the full NV12 YUV-to-RGB path.
- Full-colour kernels such as `frame_passthrough` still use the original NV12 conversion.
- In the pipeline CSV the column is still named `rgb_conv_us`, but for optical flow it now measures false-colour scale/pack into RGB565, not chroma conversion.
- A separate T1 helper kernel, `flow_color_rgb565`, exists as an architecture demo for direction-code to RGB565 colour mapping into two 128x128 byte planes.
- The active production/perf display path is back on the PS false-colour function because it measured faster than issuing the second T1 helper pass. The pipeline still needs the PS to scale/letterbox into the full HDMI framebuffer, so the extra T1 issue/cache-sync overhead was visible.
- `display_qbuf()` no longer waits for vblank by default. Set `VISIONSOC_WAIT_VBLANK=1` to restore the old `drmWaitVBlank()` pacing. Skipping that wait avoids the 3-vblank `~20 fps` bucket, but the steady pipeline is still near `30 fps` because the camera/display path then becomes the limiter.

## LMUL / Config Confirmation

The active assumption is LMUL=1 for the big configuration.

Confirmed in both:

- `designs/org.chipsalliance.t1.elaborator.t1.T1.toml`
- `designs/org.chipsalliance.t1.elaborator.t1emu.TestBench.toml`

Relevant config:

```toml
[mudkip2d128big1bram1chain2lanescale]
cmdopt = "--dLen 128 --extensions zvl1024b --extensions zve32x --laneScale 2 --chainingSize 1 --vrfBankSize 1 --vrfRamType p0rw --vfuInstantiateParameter minimal --rowNumber 1 --baseLMUL 1"
```

At SEW=8, `zvl1024b` means one LMUL=1 vector register holds 128 elements, i.e. one full 128-pixel image row. The live dispatcher uses `T1_VTYPE_E8_M1_TA_MA`, and the simulator test uses `vsetvli ..., e8, m1, ta, ma`.

## Simulator Regression

Command run:

```sh
T1_MIRROR_RTL_WRITES=1 bash run-test.sh vision_program.optical_flow \
  -c mudkip2d128big1bram1chain2lanescale -i t1emu -e verilator-emu \
  --max-cycles 50000000
```

Result:

```text
[CHECK] PASS optical_flow: all 16384 cells correct
meta_vlen: 1024
meta_isa: rv32gc_zvl1024b_zve32x
SIMULATION PASSED
```

Notes:

- The simulator dump prints `150` as `-106` because `grid_out` is `int8_t`.
- Interpreted as unsigned luma, `-106` is `150`, which is the expected down-motion display code (`argmin = 3`, scale `50`).
- The verifier was also fixed to preserve a `grid_prev_ref` copy, because the kernel intentionally overwrites `prev <- curr` at the end.
- After adding the threshold, the synthetic test pattern uses `curr[r][c] = (16*r + c) & 127` so the intended one-row motion remains above the `SAD_static <= 6` deadband.

## Board Sync

The board was still running `run_after_power_cycle.sh` / `visionsoc_main`, so it was stopped before syncing.

Command run from the dev host:

```sh
cd vision_software/visionsoc_main
./sync_kernel.sh optical_flow
```

Result:

```text
Wrote kernels/optical_flow.h
cc ... -o visionsoc_main ...
== done -- active kernel is now: optical_flow ==
```

The fixed `visionsoc_main` binary is now built on the KV260 under:

```text
/home/ubuntu/vision_software/visionsoc_main/visionsoc_main
```

## Perf / Pipeline Status

`gather_perf.sh pipeline 30` works with the optical-flow kernel when `kernels/active_kernel.h` includes `optical_flow_select.h`. It measures the full camera/DMA/T1/display loop using `visionsoc_main_perf`; it is not the same as `gather_perf.sh optical_flow`, which measures the standalone per-instruction `optical_flow_perf` binary.

Two perf-runner issues were fixed:

- `run_perf.sh` no longer rejects valid sudoers setups by checking `sudo -n true`; it now lets the actual NOPASSWD commands run.
- `start_vsm()` now runs `nohup sudo -n "$VSM" ...` instead of `sudo -n nohup "$VSM" ...`, so sudo matches the allowed `visionsoc_main` binary rather than `/usr/bin/nohup`.
- `gather_perf.sh` exits immediately if the board-side run fails, instead of trying to `scp` a missing CSV.

Recent command:

```sh
cd vision_software/visionsoc_main
./gather_perf.sh pipeline 30
```

Recent active result with the PS false-colour path:

```text
wrote perf/20260520T002807Z-pipeline/pipeline_breakdown.png: 159 frames, total=37,286us, ~26.8 fps
```

Average over frames 1..159:

```text
t1_kernel_us     ~=  6,189 us
rgb_conv_us      ~=  9,346 us  (PS false-colour RGB565 scale/pack)
disp_qb_us       ~=  7,205 us  (no explicit vblank wait)
frame_total_us   ~= 37,286 us
```

After the initial camera/display transients, the final steady frames in that run are about `33.3 ms` each (`~30 fps`). The remaining long frame time is no longer T1; it is the camera/display cadence and buffer handoff.

For comparison, the T1 colour-helper experiment was about:

```text
rgb_conv_us      ~= 12,683 us
frame_total_us   ~= 49,973 us
```

Build/sync note:

- `sync_kernel.sh` and `sync_perf.sh` now use `make -B` on the board. The KV260 clock can drift relative to the dev host, and plain `make` may otherwise reuse stale objects after `scp`.

Before the luma/flow display fast path, the original full NV12 conversion path was about:

```text
rgb_conv_us      ~= 27,705 us
frame_total_us   ~= 66,637 us
```

## Next HDMI Check

Run this on the board terminal:

```sh
cd ~/vision_software/visionsoc_main
sudo ./run_after_power_cycle.sh
```

Expected log behavior after the first frame:

- `camY` remains nonzero from the camera.
- `outY` should no longer remain `0/0/0.0` during motion.
- Static areas should have less speckle than the first non-thresholded optical-flow version.
- Static areas can still be black because static class is direction code `0`.
- Moving edges should show gray levels near `50`, `100`, `150`, or `200` depending on direction.
