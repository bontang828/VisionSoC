# T1 Instruction Failure Log

This note records T1 instruction-level failures seen during kernel bring-up so
they can be rechecked later without reconstructing the whole debug session.

## Context

- Date: 2026-05-12
- Board: KV260
- Bitstream: 5o loaded from
  `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5o-backup`
- Software path: `vision_software/libt1` one-instruction UIO issue path
- Test under development:
  `vision_software/libt1/test/cnn2d_decoder_probe.c`
- Primitive pack:
  `vision_software/visionsoc_main/kernels/cnn2d_decoder_primitives.S`

Important vertical-compute test rule:

- To test a vertical compute op such as vertical `vslideup`, use a horizontal
  load, then the vertical compute instruction, then a horizontal store.
- A vertical load followed by a vertical store double-transposes the data. If a
  vertical compute op is inserted between those two operations, the memory
  result can look like the horizontal case and hide the bug being tested.

## 1. `vmv.x.s a0, v20` Previously Did Not Produce A Host-Visible RD FIFO Entry

Primitive:

```asm
vmv.x.s a0, v20   /* primitive 49, scalar extract for debug/bringup */
```

Sequence that exposed it:

```asm
vle32.v     v4, (a0)        /* load 10 tagged logits */
vmv.v.i     v20, 0
vredmax.vs  v20, v4, v20    /* expected tagged winner in v20[0] */
vmv.x.s     a0, v20         /* expected scalar writeback */
```

Host-side expectation:

- `t1_drain_rd()` should return one scalar result packet after the
  `vmv.x.s`.
- For the current probe logits, the expected full tagged value is
  `0x000005b5` because class 5 has logit 91 and the tag format is
  `(logit << 4) | class`.

Observed result:

```text
cnn2d_issue_tagged_argmax: Resource temporarily unavailable
```

Meaning:

- `t1_issue()` completed the instruction.
- `t1_drain_rd()` returned 0, meaning `RD_FIFO_STS` reported no scalar result
  packet.
- The helper converted that empty FIFO into `errno = EAGAIN`, hence the
  `Resource temporarily unavailable` message.

This was not a wrong-value failure. It was a missing host-visible
scalar-retire packet caused by checking the non-blocking RD FIFO drain too
early.

Resolution, 2026-05-13:

- `fyp_doc/vision_kernel_programming_guide.md` section 4.4 documents the
  correct IRQ/retire-pipe handling.
- `cnn2d_issue_tagged_argmax()` now uses `t1_wait_rd()` after issuing
  `vmv.x.s`, instead of immediately calling non-blocking `t1_drain_rd()`.
- Verified on 5o with `cnn2d_decoder_probe`:

```text
PASS: tagged argmax class=5 tagged=0x000005b5
PASS: cnn2d_decoder_probe
```

Previous workaround, still available as a fallback:

- Keep the argmax reduction on the vector unit.
- Store `v20[0]` to DDR with an e32 `vse32.v` and read the single word from the
  CPU after `t1_buf_sync_for_cpu()`.
- Verified on 5o with `cnn2d_decoder_probe`: the tagged winner was
  `0x000005b5`, class 5.

Follow-up checks:

- Keep `t1_wait_rd()` in any vector-to-scalar test path. Use `t1_drain_rd()`
  only for deliberate polling.

## 2. Vertical `vslide*.vx` Dynamic Slides Were Previously Suspect

Concrete failing primitive:

```asm
vslideup.vx v12, v8, a0     /* primitive 26, dynamic positive shift */
```

Related primitive to retest:

```asm
vslidedown.vx v16, v8, a0   /* primitive 27, dynamic negative shift */
```

Test pattern:

- Load feature map with a horizontal `vle8.v`.
- Issue primitive 26 with `vertical_mode = 1` and offset `2`.
- Store `v12` with a horizontal `vse8.v`.

Expected:

- A vertical shift by 2 rows.
- For row 0, all values should be 0 because the shift fills the top boundary.

Observed:

```text
FAIL: vertical shift mismatch at [0][3]: got -32 expected 0
```

The value `-32` matched the horizontal-shift source from the same row, so the
dynamic `vslideup.vx` path appeared to ignore the vertical compute view at the
time.

Related result:

- The immediate-slide path does work. After adding fixed immediate slide
  primitives for the planned CNN dilation radii, the board probe reported:

```text
PASS: edge/LUT stem and threshold store
PASS: horizontal shifted tap
PASS: vertical shifted tap
PASS: row max pooling
cnn2d_issue_tagged_argmax: Resource temporarily unavailable
```

Retest result, 2026-05-13:

- Added `vision_software/libt1/test/vslide_vx_probe.c`.
- Reloaded `/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin`,
  whose checksum matches `system_top_wrapper.bit.bin.5o-backup`.
- The probe uses the required horizontal-load / vertical-compute /
  horizontal-store structure and compares dynamic `vx` slides against
  immediate `vi` slides at the same offset.
- It also tests the old decoder-style path:
  `load -> vslide*.vx under vertical_mode=1 -> vsadd under vertical_mode=1
  -> horizontal store`.

Observed:

```text
PASS: vslideup.vx vertical matches vertical expectation
PASS: vslideup.vi vertical matches vertical expectation
PASS: vslidedown.vx vertical matches vertical expectation
PASS: vslidedown.vi vertical matches vertical expectation
PASS: vslideup.vx vertical via acc matches vertical expectation
PASS: vslidedown.vx vertical via acc matches vertical expectation
```

This means the original vertical `vslide*.vx` failure is no longer
reproducible on the current 5o FPGA image and current `libt1` driver path.
The dynamic scalar-source slide path is not currently failing in isolation.

Current status:

- `vslideup.vx` and `vslidedown.vx` are usable for vertical compute on 5o.
- Immediate slides for fixed radii `1, 2, 4, 8, 16, 31` remain available in
  the primitive pack because they are convenient for fixed CNN schedules.

Follow-up checks:

- If the old failure reappears, capture the exact software revision and cache
  sync sequence around the input/output buffers; the standalone FPGA probe now
  exercises both `vslideup.vx` and `vslidedown.vx` successfully.
- If changing RTL in the future, keep `vslide_vx_probe` in the regression set
  because it covers the vertical dynamic-slide path that was previously
  suspected.

## Confirmed Non-Failures From The Same Session

- Vertical LSU transpose still passes:

```text
PASS: vertical LSU transpose
```

- The edge/LUT stem using immediate vertical slides passed in the CNN probe.
- Row max pooling with `vredmax.vs` and `vl = 1` store passed in the CNN probe.
