# VisionSoC Perf Status

Date: 2026-05-20

## Current Display Timing Status

The live optical-flow pipeline is not limited by the T1 optical-flow kernel.
The kernel is around `371k` cycles, which is about `6.2 ms`. The frame-rate
limit comes from the full serial camera-to-display loop.

The latest display change was a quick pacing change, not true parallel
display:

- `display_qbuf()` no longer waits for vblank by default.
- Set `VISIONSOC_WAIT_VBLANK=1` to restore the old `drmWaitVBlank()` pacing.
- The active optical-flow display path is the PS false-colour RGB565 path.
- The separate `flow_color_rgb565` T1 helper kernel exists as an experiment,
  but it is not the active display path because it measured slower.

This is still mostly serial:

```text
camera dequeue
prep/copy input
DMA input to URAM
T1 optical-flow kernel
DMA output from URAM
PS false-colour RGB565 scale/pack
DRM display queue
camera requeue
next frame
```

True parallel display has not been implemented. That would require submitting
a page flip and immediately processing the next frame while HDMI scans out the
previous frame, plus page-flip event handling and display-buffer state tracking.

## Why 20 fps Happened

The old display path called:

```c
drmModeSetCrtc(...);
drmWaitVBlank(...);
```

On a 60 Hz HDMI mode:

```text
1 vblank  ~= 16.67 ms
2 vblanks ~= 33.33 ms -> about 30 fps
3 vblanks ~= 50.00 ms -> about 20 fps
```

When the serial pipeline missed the 2-vblank timing bucket, it waited one more
refresh and looked capped at `20 fps`. Skipping the explicit vblank wait avoids
that hard 3-vblank bucket, but the steady pipeline is still close to `30 fps`
because the camera/display cadence becomes the remaining limiter.

## Stage Breakdown Interpretation

`cam_dq_us`: time waiting for and dequeuing a camera frame. If this grows, the
loop is waiting for camera cadence or camera-buffer availability.

`stats_in_us`: CPU min/max/average scan over the input Y plane for logging.

`prep_in_us`: CPU copy of camera Y into the T1 input buffer plus cache flush.

`dma_in_us`: DMA transfer from DDR input buffer to URAM half A.

`t1_kernel_us`: wall-clock time for issuing/running the active T1 kernel.

`t1_kernel_cycles`: hardware T1 cycle count for the active kernel. For optical
flow this is typically about `371k` cycles.

`dma_out_us`: DMA transfer from URAM half B back to the DDR output buffer.

`sync_out_us`: cache invalidate so the PS can read the DMA-written output.

`stats_out_us`: CPU min/max/average scan over output Y for logging.

`uv_fill_us`: fills or copies the UV plane. For optical-flow false-colour this
is not the key display cost.

`disp_dq_us`: gets the next display backbuffer. Usually small in this code.

`rgb_conv_us`: display conversion/scale stage. For optical flow this is PS
false-colour RGB565 scale/pack, not full camera YUV conversion.

`disp_qb_us`: queues the framebuffer to DRM. With old vblank pacing, this
included `drmWaitVBlank()` and could dominate at `16-30 ms`. With vblank wait
disabled, it should be lower, though `drmModeSetCrtc()` still costs time.

`cam_qb_us`: returns the camera buffer to V4L2.

`frame_total_us`: full serial loop time for one frame. This is the number that
maps to pipeline FPS; it is not just T1 time.

## Recent Reference Numbers

With PS false-colour and vblank wait disabled:

```text
t1_kernel_us    ~=  6.2 ms
rgb_conv_us     ~=  9.3 ms
disp_qb_us      ~=  7.2 ms average in the 160-frame perf run
steady frames   ~= 33.3 ms, about 30 fps
```

The important conclusion is that optimizing the optical-flow kernel alone will
not remove the current frame-rate limit. The next meaningful display-side
optimization is a nonblocking page-flip/event pipeline with proper buffer
recycling.

## Per-Instruction Perf CSV — the "off-by-one drain" artifact (FIXED 2026-05-20)

> **Status:** the off-by-one artifact described here was a *measurement* bug in
> the harness, now fixed (see "The fix" at the end of this section). CSVs
> captured **on or after 2026-05-20** read each instruction's own latency
> directly — no de-shifting needed. CSVs captured **before** that date
> (e.g. `20260518T203412Z-sobel`, `20260519T190638Z-optical_flow`) are still
> shifted by one and must be read per the rules below.

This section answers a recurring confusion in the instruction-level CSVs
produced by `sobel_perf.c` and `optical_flow_perf.c`
(`iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us`):

> Why is the **first** `vmv.v.i` in the Gx pass blazing fast (~305 cyc) while
> the **second** `vmv.v.i` is ~40× slower (~13.5k cyc)? Why does the Gy pass's
> first `vmv.v.i` cost ~4.5k instead of ~305? And in optical flow, why is the
> `vsub.vv` at instr 2 only ~305 cyc while the `vsub.vv` at instr 16 is ~12.4k?

**These are not real per-op cost differences. The `t1_cycles` column is shifted
by one instruction: the number on row N is (almost entirely) the *drain latency
of the instruction issued just before N*, not the cost of N itself.**

### Why — the measurement path

Tracing `sobel_perf.c` / `optical_flow_perf.c` → `libt1.c::t1_issue` →
`fpga/wrapper/t1_axi_lite_wrapper.sv`:

1. The harness brackets each issue with `t1_perf_start()` … `t1_issue()` …
   `t1_perf_stop()`. `perf_delta` is just `perf_cycles_at_stop − perf_cycles_at_start`
   of the wrapper's free-running aclk counter (wrapper lines 351–385). So
   `t1_cycles[N]` = wall duration of the `t1_issue(N)` call, in T1 aclk cycles.

2. `t1_issue()` (libt1.c:586) does, in order:
   * `wait_ctrl_ready()` — spins on `T1_CTRL_ISSUE_READY` (= the T1 core's
     `issue.ready`, wrapper bit[1]) **before** writing the instruction.
   * write instruction regs + `ISSUE_START` (sets `issue_pending`).
   * for compute ops: `wait_ctrl_not_busy()` — spins on `T1_CTRL_ISSUE_BUSY`,
     which the wrapper drives from `issue_pending` (bit[2], wrapper line 405).
     `issue_pending` clears the cycle the core *accepts* the op
     (`issue_pending && issue_ready`, wrapper line 342) — i.e. at `issue.fire`,
     **not** at writeback completion.
   * for LSU ops (`vle8`/`vse8`): `wait_mem_events(1)` instead — this *does*
     block until the memory transaction completes.

3. The decisive hardware fact (see `fyp_doc/2d_fabric_handoff.md` §5.1,
   `T1.scala::issueWritebackDrained`): **`issue.ready` is held LOW until ALL
   writeback paths of the current instruction have drained.** A 2D op replays
   across 128 hw-rows, so that drain is thousands of cycles.

Putting it together for a compute op N:
* `wait_ctrl_not_busy()` returns at *acceptance* of N (~1–2 cyc after fire), so
  `t1_issue(N)` returns while N is **still draining** in the background.
* The harness loops to `t1_issue(N+1)`, whose **`wait_ctrl_ready()` now stalls
  for the entire residual drain of N** (because `issue.ready` stays low until N
  finishes).

⇒ `t1_cycles[N+1] ≈ full execution/drain latency of N`. The window on a row
measures the *previous* issued op, plus a handful of handshake cycles for the
current op.

### Why the "fast" rows are exactly where they are

The only sub-1000-cycle compute rows in either CSV sit **immediately after a
blocking LSU**:
* sobel: `vmv.v.i` at instr 1 (~305) follows `vle8.v` at instr 0.
* optical flow: `vsub.vv` at instr 2 (~305) follows `vle8.v` at instr 1.

For an LSU op the driver calls `wait_mem_events`, which forces the load to
*fully complete* before `t1_issue` returns. So when the next op issues,
`issue.ready` is already high → `wait_ctrl_ready` returns immediately → the row
records only the ~305-cycle issue handshake. Every *other* compute op is
preceded by a non-blocking compute op that was still draining, so it inherits
that drain (~4.5k–13.5k).

`csrwi(vmode=…)` rows are `0` cycles and `is_csr_flip=1`: the harness only
updates a software `vmode` flag for these and never issues them to hardware
(the T1 wrapper has no scalar core), so they are transparent in the issue
chain — instruction N's row reflects the last *real* hardware issue before it,
skipping any intervening csrwi.

### De-shifted (true) per-op latencies

Reading each op's true cost from the **next** row (skipping csrwi/`is_csr_flip`
rows), both kernels agree:

| Op class                         | True latency (cyc) |
|----------------------------------|--------------------|
| `vle8.v` / `vse8.v` (128-row LSU)| ~10k–14k (measured directly; LSU rows are honest) |
| `vmv.v.i`                        | ~13.5k             |
| `vslideup.vi` / `vslidedown.vi`  | ~12k–13.5k         |
| `vsub.vv`                        | ~4.5k–5.5k         |
| `vrsub.vi`                       | ~4.4k              |
| `vmax.vv`                        | ~4.4k–4.5k         |
| `vsadd.vv` / `vminu.vv` / `vmsltu.vv` / `vmerge.vim` | ~4.5k–5.6k |
| `vmul.vx`                        | ~4.5k (its 14k row is the preceding op's drain) |

Worked answers to the original questions:
* **Sobel Gx first vs second `vmv.v.i`:** identical ops, both ~13.5k in
  hardware. Instr 1 reads ~305 because it follows the fully-drained `vle8`;
  instr 1's real ~13.5k cost lands on instr 2's row. Instr 2's real cost lands
  on instr 3's row, etc.
* **Sobel Gy first `vmv.v.i` (~4.5k, not ~305):** the op issued just before it
  is the Gx-pass `vsub.vv` (instr 5; the `csrwi` at instr 6 is not a hardware
  issue). `vsub` drains in ~4.5k, and that drain is charged to the Gy `vmv`
  row. It is larger than ~305 only because its predecessor was a compute op,
  not a blocking load.
* **Optical-flow `vsub.vv` instr 2 (~305) vs instr 16 (~12.4k):** instr 2
  follows the blocking `vle8` (instr 1) → handshake only. Instr 16 follows
  `vslidedown.vi` (instr 15), so its row shows the slide's ~12.4k drain. The
  true cost of the instr-16 `vsub` is on instr 17 (~4.5k) — same as every other
  `vsub`.

### Secondary (genuine) finding: slides and `vmv.v.i` cost ~3× the elementwise ALU ops

After de-shifting, there is a real ~3× split: `vmv.v.i` and the `vslide*`
family land at ~12–13.5k cyc, while pure elementwise ops (`vsub`, `vrsub`,
`vmax`, `vsadd`, `vminu`, `vmsltu`, `vmerge`, `vmul.vx`) land at ~4.5k. The
slides route through the cross-lane / mask-exchange path
(`MaskExchangeUnit.scala` / `MaskUnit(Fpga).scala`), which has a heavier
2D-replay writeback than the lane ALU. `vmv.v.i` sitting in the same ~13.5k
band as the slides is the one worth confirming against
`t1/src/decoder/Decoder.scala` attributes — if `vmv.v.i` is being routed onto
the mask/slide writeback path rather than a cheap lane broadcast, that is a
real optimization opportunity (it appears 4× in sobel and 6× in optical flow).

### Is this two instructions in flight, or a measurement bug? (it's the latter)

The shift is **purely a driver-measurement issue**, not evidence that the
fabric runs two instructions concurrently. The hardware is serialized at the
issue boundary. The confusion comes from the wrapper exposing *two different
"done" signals*, and the harness stopping on the early one:

| CTRL bit | Wrapper source | Means | Asserts |
|---|---|---|---|
| `ISSUE_BUSY` (bit 2) | `issue_pending` | op **accepted** into the pipe (`issue.fire`) | ~1–2 cyc after `ISSUE_START` |
| `ISSUE_READY` (bit 1) | core's `issue.ready` | core **done draining** current op, can take next | only after the full ~13.5k-cyc drain |

* `t1_issue()` for a compute op ends at `wait_ctrl_not_busy()`, which polls
  `ISSUE_BUSY` (= `issue_pending`). That clears at *acceptance*, so `t1_issue()`
  returns while the op is still draining in the background. The op's real cost
  was never timed in its own bracket — it lands in the next `t1_issue`'s
  `wait_ctrl_ready()` wait.
* The fabric does **not** overlap two compute ops. `issue.fire =
  issue_valid && issue_ready`, and `T1.scala::issueWritebackDrained` holds
  `issue.ready` LOW for the entire drain of the current op. So op N+1 cannot be
  accepted (and the driver does not even write N+1's instruction registers,
  because `wait_ctrl_ready()` blocks first) until op N has fully drained. At
  most one op is live at a time. The only thing that "overlaps" is *driver
  software prep* vs *hardware drain*, not hardware-vs-hardware.
* Note this is *more* serial than baseline chipsalliance T1, which is a
  chaining machine that normally keeps several vector instructions in flight.
  This 2D design deliberately switched chaining off with the
  `issueWritebackDrained` gate so the per-instruction CSR vertical-mode
  snapshot can't leak from a still-draining op into the next (handoff §5.1).
  There is real throughput left on the table here: if that CSR-snapshot
  constraint could be met without a full-drain gate, restoring native chaining
  would let these ~13.5k-cyc drains overlap instead of running strictly
  back-to-back.

### The fix (applied 2026-05-20)

The harness now waits on the *correct* signal inside the perf bracket:

```c
volatile uint32_t start = t1_perf_start(tag);
int rc = t1_issue(&op);
if (rc >= 0) rc = t1_wait_ready();   /* NEW: block on ISSUE_READY == full drain */
volatile uint32_t cycles = t1_perf_stop();
```

`t1_wait_ready()` (new public libt1 entry, `libt1.c`/`libt1.h`) polls
`ISSUE_READY` until the just-issued op has fully drained. Effects:

* Each CSV row now measures **its own** instruction's latency. The off-by-one
  shift is gone; no de-shifting needed.
* The ~305-cyc "fast" rows after a blocking LSU disappear — every compute op
  reports its true ~4.5k–13.5k cyc. (`is_csr_flip=1` rows are still `0`; they
  are never issued to hardware.)
* Because every op now waits for its own drain, the *next* op's
  `wait_ctrl_ready()` returns immediately, so the residual per-row reading is
  just that op's drain plus the fixed ~300-cyc AXI-Lite issue overhead (8
  register writes + `ISSUE_START`), which is uniform across ops.
* `t1_issue()` itself is **unchanged**, so production (`main.c` /
  `issue_active_kernel`) is unaffected — and would not have sped up anyway,
  since the hardware already serializes every issue on the same drain gate.
* The whole-kernel number in `main_perf.c` (`t1_kernel_cycles`, ~371k for
  optical flow) was always correct: it brackets the entire `issue_active_kernel`
  call, so every inter-op drain was already counted once, in order.

### Reading pre-fix CSVs (still shifted by one)

* **Do not read `t1_cycles` as the cost of the op named on its row.** Read it as
  the cost of the previous issued op. To get an op's own latency, take the value
  on the *next* hardware-issued row (skip `is_csr_flip=1` rows).
* The LSU rows (`vle8`/`vse8`) are the exception — they are measured honestly
  even in the old CSVs, because the driver blocked on `wait_mem_events`.
