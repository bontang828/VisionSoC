# VisionSoC perf harness

This folder holds the cycle/timing breakdown harness for the VisionSoC
pipeline and standalone kernels. The main entrypoints are the perf
binaries (`visionsoc_main_perf`, `sobel_perf`, `optical_flow_perf`,
`matmul_8bitraw_short_perf`), three shell wrappers (`sync_perf.sh`,
`run_perf.sh`, `gather_perf.sh`), one Python plotter (`plot_perf.py`),
and a per-run output directory layout under `perf/<UTC-timestamp>-<mode>/`.

## What the binaries measure

| Binary | Source | Drives | CSV |
|---|---|---|---|
| `visionsoc_main_perf` | `../main_perf.c` | full camera→DDR→DMA→URAM→T1→URAM→DDR→PS-edit→RGB-convert→DRM pipeline | one row per frame, columns = stage wall-µs + T1 cycles |
| `sobel_perf` | `../sobel_perf.c` | standalone T1-only path with a synthetic gradient frame; instruments each issued T1 instruction | one row per (iteration, instruction), columns = wall-µs + T1 cycles per instruction |
| `optical_flow_perf` | `../optical_flow_perf.c` | standalone T1-only path for the 5-direction block-matching optical-flow kernel | one row per (iteration, instruction), columns = wall-µs + T1 cycles per instruction |
| `matmul_8bitraw_short_perf` | `../matmul_8bitraw_short_perf.c` | standalone T1-only path for the short raw-8 matmul kernel | one row per logical kernel row; the seven body rows aggregate 128 repeats, including CSR markers |

Both share `libt1`'s `t1_perf_start(tag)` / `t1_perf_stop()` hardware
cycle counter for T1 work, and `clock_gettime(CLOCK_MONOTONIC)` for wall
time. The wall-µs minus (cycles / T1 clock freq) is the libt1/Linux
"sliver" — the cost of MMIO writes + IRQ wait that wouldn't exist on a
Rocket+T1 ASIC.

## Typical workflow

```sh
cd vision_software/visionsoc_main

# 1. After editing any of the *_perf.c sources, run sync to scp them
#    onto the board, native-build on aarch64, stage in the NOPASSWD-
#    eligible dir ~/vision_software/libt1/test/.
./sync_perf.sh

# 2. Run a measurement. gather_perf.sh = run_perf.sh on the board +
#    scp CSV back + plot. One command, output lands in a fresh
#    perf/<UTC-timestamp>-<mode>/ folder.
./gather_perf.sh sobel    100     # 100 sobel iterations
./gather_perf.sh optical_flow 100 # 100 optical-flow iterations
./gather_perf.sh matmul_8bitraw_short 100
./gather_perf.sh pipeline  30     # 30 frames pipeline run
./gather_perf.sh both             # both, default 100 / 30
```

Each `gather_perf.sh` invocation writes (under
`perf/<UTC-timestamp>-<mode>/`):

```
sobel_perf.csv               # if mode in {sobel, both}
sobel_breakdown.per_instr.png
sobel_breakdown.grouped.png
optical_flow_perf.csv        # if mode=optical_flow
matmul_8bitraw_short_perf.csv # if mode=matmul_8bitraw_short
pipeline_perf.csv            # if mode in {pipeline, both}
pipeline_breakdown.png
run.log                      # stdout+stderr from run_perf.sh
meta.txt                     # mode, N, t1-hz, board host, bitstream
                             # sha256, git head -- enough to reproduce
```

For ad-hoc runs (no per-run dir, no plotting), use `run_perf.sh`
directly:

```sh
ssh kv260 ~/vision_software/visionsoc_main/run_perf.sh matmul_8bitraw_short 100
scp 'kv260:/tmp/matmul_8bitraw_short_perf.csv' .
python3 perf/plot_perf.py matmul_8bitraw_short matmul_8bitraw_short_perf.csv --both
```

## Why three shell wrappers

`sync_perf.sh` (host) builds the binaries on the board and stages them
in the NOPASSWD-eligible test directory. No measurement, just build.

`run_perf.sh` (board) does the camera/DRM dance and runs the binaries.
For the **pipeline** mode this means:

1. `sudo pkill visionsoc_main` to free the camera + DRM
2. `systemctl mask gdm; systemctl stop gdm` to keep Xorg from grabbing
   the DRM master in the gap
3. `fpgautil -b ... -o ...` to reapply the overlay (cures any wedged
   CSI2RX state from a prior interrupted run; cheap, ~500 ms)
4. `media-ctl` setup of the AP1302→CSI2RX subdev format chain
5. Run `visionsoc_main_perf` with full display
6. Relaunch production `visionsoc_main`

For standalone-kernel modes (`sobel`, `optical_flow`,
`matmul_8bitraw_short`), only step 1 (kill production) and step 6
(relaunch) are needed; these binaries don't touch the camera or display.

`gather_perf.sh` (host) is the convenience wrapper that ssh-invokes
`run_perf.sh`, pulls the CSV, runs `plot_perf.py`, and writes the
timestamped output dir + `meta.txt`.

## Adding a perf test for a new kernel

To replicate `sobel_perf.c` for a different T1 vector kernel — say
`my_kernel.S` — the steps are:

1. **Write the assembly + select header**
   - `../kernels/my_kernel.S` — vector ops, with `csrwi 0x7c0, {0,1}` for
     H/V mode toggles. Follow the conventions in
     `../kernels/sobel.S` and `fyp_doc/2d_fabric_handoff.md`.
   - `../kernels/my_kernel_select.h` — define `ACTIVE_KERNEL` and
     `ACTIVE_KERNEL_NEUTRAL_UV`. Look at `sobel_select.h` as the
     template.

2. **Generate the C header from the assembly** (one-time on the board):
   ```sh
   ssh kv260 'cd ~/vision_software/visionsoc_main && \
              ../libt1/build_kernel.sh kernels/my_kernel.S \
                  kernels/my_kernel.h my_kernel'
   ```
   This produces `kernels/my_kernel.h` with the `my_kernel[]` `uint32_t`
   array. Sync it back to the host if you want it tracked in git.

3. **Clone `../sobel_perf.c` → `../my_kernel_perf.c`**
   - Replace the `#include "kernels/sobel.h"` with your kernel header.
   - Replace `sobel`, `sobel_count`, and the mnemonic lookup in the
     `mnemonic()` function as needed. The decoder switch covers the
     common SEW=8 opcodes used in `sobel.S`; extend it if your kernel
     uses anything outside (vle16, vrgather, vredsum, etc.).
   - If your kernel has a different LSU calling convention (e.g.
     multiple input buffers like `grid_vadd.S`), follow the dispatcher
     logic from `../kernels/active_kernel_dispatcher.h` instead of the
     `a0`/`a1` shortcut sobel uses.

4. **Wire it into the build**
   - In `../Makefile`, add a new target after the existing `sobel_perf`:
     ```make
     my_kernel_perf: my_kernel_perf.o ../libt1/libt1.a
     	$(CC) $(CFLAGS) -o $@ my_kernel_perf.o $(LDLIBS)

     my_kernel_perf.o: kernels/my_kernel.h
     ```
   - Update `clean:` to remove the new artifacts.

5. **Wire it into `sync_perf.sh`**
   - Append `my_kernel_perf.c` to the scp source list.
   - Append `my_kernel_perf` to the `make` invocation.
   - Append the staging copy of `my_kernel_perf` into
     `~/vision_software/libt1/test/`.

6. **Wire it into `run_perf.sh`**
   - Add a `my_kernel` mode in the case statement that calls a new
     `run_my_kernel` function. Model it on `run_sobel` if your kernel
     doesn't need the camera/display, or on `run_pipeline` if it does.
   - The function should: stop visionsoc_main, run the binary, relaunch
     visionsoc_main.

7. **Wire it into `plot_perf.py`** (optional)
   - If your kernel uses a different logical-group decomposition,
     mirror `SOBEL_GROUPS` and `SOBEL_GROUP_SHORT` for the new kernel
     and add a `plot_my_kernel_*` pair of functions. The existing
     `_draw_single_bar_dual_axis` helper is kernel-agnostic and can be
     reused — just supply the segments, palette, and logical-group
     mapping.

8. **Wire it into `gather_perf.sh`**
   - Add `my_kernel` to the `case "$MODE"` block. CSV path follows the
     `/tmp/<kernel>_perf.csv` convention.

A good sanity check is the cross-binary one already mentioned in the
plan file: sum of per-instruction T1 cycles from `<kernel>_perf` should
match `visionsoc_main_perf`'s `t1_kernel_cycles` column to within a few
percent (main_perf's `wait_ctrl_ready` polling adds a small overhead
inside its perf window).

## Compiler-optimisation gotcha

Both `*_perf.c` binaries are built at `-O2`. The C compiler is forbidden
from reordering `clock_gettime` / `t1_perf_start` / `t1_issue` /
`t1_perf_stop` with respect to each other because they're external
function calls that touch volatile MMIO or syscalls. We additionally:

- Store the perf-counter return values into `volatile` locals before
  copying into the per-instruction array.
- Insert `__asm__ __volatile__("" ::: "memory")` barriers around the
  timing window.

After the first build of a new perf binary, verify with `objdump -d`
that the four calls are still distinct `call`/`bl` sites and nothing
got inlined into the timing loop:

```sh
objdump -d my_kernel_perf | \
  awk '/<main>/{flag=1} flag{print} /^$/{if(flag)exit}' | \
  grep -E 'call.*(t1_perf_start|t1_perf_stop|t1_issue|clock_gettime)' | \
  head -20
```

## Sudoers / NOPASSWD constraints

`run_perf.sh` uses `sudo -n` to invoke commands that need root:
- `/usr/bin/pkill` (kill production `visionsoc_main`)
- `/usr/sbin/fpgautil` (reapply overlay)
- `/usr/bin/media-ctl` (subdev format setup)
- `/usr/bin/systemctl` (`stop` / `mask` gdm)
- `/home/ubuntu/vision_software/libt1/test/*` (the perf binaries)
- `/home/ubuntu/vision_software/visionsoc_main/visionsoc_main` (relaunch)
- `/usr/bin/rmdir` (overlay teardown via configfs)

The binaries are staged in `~/vision_software/libt1/test/` rather than
the build directory because that path is on the existing NOPASSWD
allow-list. If you stage them elsewhere you'll get a "password
required" failure.

## FPGA vs ASIC: what the numbers mean

| Metric | What it measures on KV260 | What survives to an ASIC port |
|---|---|---|
| T1 hardware cycles | Same T1 RTL as any future ASIC | Yes — cycle counts transfer 1:1 |
| Wall-µs per instruction | dominated by libt1's 9× AXI-Lite MMIO + IRQ wait | No — Rocket inline-issuing into `T1Issue` collapses the libt1 sliver to zero |
| Camera dq | Sensor-bound (~33 ms @ 30 fps) | Same |
| DMA µs | Descriptor setup + IRQ poll; ~78 µs for 16 KB | ASIC scratchpad/queue interface would be sub-µs |
| RGB convert | Single-threaded A53 scalar | Could move to T1 or be eliminated by an NV12-capable DRM plane |
| disp_qb | 60 Hz vsync | Same |

The pipeline chart's shape is FPGA-specific, but the **T1 cycles** axis
on the sobel chart is what would carry forward to any future ASIC
implementation of T1. That's the comparable number when judging T1's
intrinsic compute cost.

## Files at a glance

```
perf/
├── README.md             this file
├── plot_perf.py          matplotlib plotter (pipeline + kernel subcommands)
├── <UTC>-sobel/          one per gather_perf.sh sobel run
│   ├── sobel_perf.csv
│   ├── sobel_breakdown.per_instr.png
│   ├── sobel_breakdown.grouped.png
│   ├── run.log
│   └── meta.txt
├── <UTC>-matmul_8bitraw_short/
│   ├── matmul_8bitraw_short_perf.csv
│   ├── matmul_8bitraw_short_breakdown.per_instr.png
│   ├── matmul_8bitraw_short_breakdown.grouped.png
│   ├── run.log
│   └── meta.txt
└── <UTC>-pipeline/       one per gather_perf.sh pipeline run
    ├── pipeline_perf.csv
    ├── pipeline_breakdown.png
    ├── run.log
    └── meta.txt
```

The script entrypoints live one directory up:

```
vision_software/visionsoc_main/
├── sync_perf.sh        host-side: scp sources + native build on board
├── run_perf.sh         board-side: camera/DRM dance + run the binaries
├── gather_perf.sh      host-side: ssh-run + scp-back + plot, into a
│                       fresh perf/<UTC>-<mode>/ directory
├── main_perf.c         pipeline-instrumented twin of main.c
├── sobel_perf.c        standalone T1-instrumented sobel driver
├── optical_flow_perf.c standalone T1-instrumented optical-flow driver
└── matmul_8bitraw_short_perf.c
                        standalone T1-instrumented short matmul driver
```
