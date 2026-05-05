# 2D-Fabric Programming Handoff

**Audience:** future programmers opening this repo
without prior context. Read this *before* writing or debugging any vector
kernel. This SoC uses the RISC-V V extension, but it is **not** a stock RVV
implementation and several textbook-RVV intuitions are wrong here.

This doc is a stable snapshot of what we know works, what we know breaks, and
where in the codebase to look when something behaves unexpectedly. It is
deliberately concrete and example-driven rather than spec-pure.

---

## 1. The big idea (and why "stock RVV" intuitions break)

Stock RVV is a one-dimensional vector machine: a single instruction sees
`vlmax` element lanes, the runtime decides how many of those `vl` lanes
participate, and the result is one `vlmax`-element register update. The
number of "hardware rows" is a back-end micro-architectural concern that
is invisible to the software contract.

This SoC adds a second dimension on top of that: **every vector instruction
is broadcast to a 128-row time-multiplexed grid**. Conceptually:

```
    v8 in hw-row 0    | v8 in hw-row 1    | v8 in hw-row 2    | ...  | v8 in hw-row 127
    [128 elements]   | [128 elements]    | [128 elements]    |       | [128 elements]
```

A single `vle8.v v8, (a0)` does **128 separate row-strided memory accesses**,
not one. Hardware row r reads from `&a0[r * COLS]`. So one `vle8` consumes a
full ROWS x COLS image in row-major layout. Same for `vse8`.

Per-element compute (`vadd.vv`, `vmul.vv`, `vrgather.vv`, `vslideup.vi`,
masking, etc.) **is replicated independently across all 128 hardware rows**.
That is the source of the "free 128x parallelism" the architecture is selling.

Reductions and other scalar-output ops **are NOT replicated** - see § 4.

The parameter set we routinely use is:

  * `--dLen 128 --extensions zvl256b --laneScale 2 --rowNumber 1`
    (mudkip2d128small1bram1chain2lanescale)
  * VLEN = 256 bits = 32 bytes per LMUL=1 register
  * SEW = 8 (i8), LMUL = 4, vl = 128 → exactly one image row per register group
  * 128 hardware rows = one image's worth in the time-multiplexed dimension

Whenever a kernel uses `vsetvli zero, COLS, e8, m4, ta, ma`, you are saying
"give me one whole image-row at SEW=8, with the 128 hw-rows handling the 128
image-rows in parallel." That is the canonical setup for everything in
`tests/vision_task/`.

---

## 2. The two compute modes

CSR `0x7c0` toggles between **horizontal** (= 0, default) and **vertical**
(= 1) compute mode. The toggle is *per-instruction*, latched on issue, and
takes effect for the next vector instruction.

  * **Horizontal compute mode.** Vector ops execute "left-to-right" on each
    hw-row's register independently. `vslideup.vi v_dst, v_src, 1` shifts
    elements within a row toward higher indices, i.e. shifts the image one
    column to the right. `vadd.vv` adds row r's v_a to row r's v_b. Etc.

  * **Vertical compute mode.** The VRF read/write paths transpose data on
    the fly (using a diagonal-skewed bank scatter, see
    `t1/src/vrf/SharedVRF.scala`). When you read v8 in vertical mode you
    see "v8[i] in vertical-lane c = original v8 of hw-row i, element c".
    `vslideup.vi` therefore moves between *vertically-adjacent* image
    rows, i.e. shifts the image one row down.

Slides and the related mask-pipe ops (`vrgather`, etc.) are the main reason
vertical mode exists. Everything else (`vadd`, `vmul`) gives the same final
data layout under either mode, *as long as the operands and the destination
are read/written through the same mode*. Mixing modes within a single kernel
works (see `simple_instruction_vert_hori.c` for the canonical example).

**LSU honours CSR 0x7c0.** As of 2026-05-04, `vle8.v` and `vse8.v` are
mode-aware. CSR `0x7c0` is captured at issue time in a per-instruction
snapshot that travels with the instruction through the chaining record;
SharedVRF's gate selects the vertical scatter/gather path for LSU
instructions whose snapshot is 1, independent of any subsequent CSR
flip during drain. Memory addressing stays row-major in both modes
(hw-row r touches `&base[r * COLS]`); only the VRF-side layout flips.
The LSU row pitch is a logical image-row pitch, currently fixed in RTL as
128 e8 elements for the 128x128 grid tests, and is intentionally independent
of the current instruction `vl`. Therefore a `vl=1` store writes element 0
from every hardware row to `grid[row][0]`, not a packed vector into
`grid[0][0..127]`:

  * `vle8.v` (vert): row-major load from memory, transposed layout in
    VRF. A subsequent **horizontal** read of vd sees the transpose Mᵀ
    of the loaded matrix.
  * `vse8.v` (vert): vertical gather from VRF, row-major store to
    memory. If vd held M, memory receives Mᵀ.
  * vle-vert + vse-vert is a no-op transpose-of-transpose (round-trips
    data unchanged). This used to be enforced by hardware; it is now
    just programmer arithmetic.

The full design rationale, gate formula, and difftest fence are
documented in `fyp_doc/LSU_vertical_mode_handoff.md`. Read that
before debugging any vert-LSU correctness issue.

---

## 3. Programmer rules (what works)

These are the practical conventions we landed on after `simple_vadd.c`'s
hang and the benchmark exploration. Follow them and your kernels will at
least run. Skip them and you will hit one of the failure modes in § 4.

### 3.1 Defeat auto-vectorisation in non-kernel code

The Nix builder enforces `-O2` with no per-file flag override. At `-O2` the
compiler will happily turn an init loop into LMUL=8 `vid.v + vse8.v`. That
generates wrong data because `vid.v` produces 0..vl-1 *per hardware row*,
and `vse8.v` then writes ROWS copies of that sequence into your grid.

Fix: dereference through `volatile int8_t *` everywhere you write data the
vector kernel will read. The `init_grids` pattern in
`tests/vision_task/simple_instruction_gather/simple_instruction_gather.c`
is the boilerplate.

### 3.2 Naked + no-call vector kernels

The compiler at `-O2` saves and restores live vector registers across
function calls using `vs1r.v` / `vl1r.v` (whole-register store/load). On
this fabric, `vs1r.v` fans out across all 128 hw-rows just like any other
vector store, **but the compiler reserved only `vlenb` bytes of stack per
spill**. That's 128x too small. The stack overflows, the saved RA gets
clobbered, and the function returns to garbage → illegal-instruction trap.
This is exactly what `simple_vadd.c` hit before we documented the
constraint.

The robust fix is to never let the compiler think it needs to spill:

  * Wrap every vector instruction sequence in a function decorated
    `__attribute__((naked, noinline))` with pure inline asm.
  * Inside the naked function, do not call any other C function -
    `printf`, `place_counter`, anything. Each call is a spill site.
  * If you must instrument inside the kernel, encode the MMIO write
    directly as `sw` in the same inline-asm block. `benchmark_vadd.c`'s
    `grid_vadd_per_iter` shows the pattern.

If you need to save vector state across a call, do it explicitly with
`vse8.v` / `vle8.v` into a buffer that you sized as `ROWS * COLS` bytes
per LMUL=1 register. Don't trust the compiler-generated spill code.
Make sure CSR `0x7c0` is 0 (horizontal mode) when you do the spill/restore
— otherwise the spill writes the transpose of the register's content, and
the restore reads back the wrong layout.

### 3.3 LMUL stays at 4

`zvl256b` plus our config gives 32-byte LMUL=1 registers. With SEW=8,
LMUL=4 hits exactly vl=128 = one image-row. The 2D plumbing in
`SharedVRF.scala` and the diagonal scatter logic assume LMUL ≤ 4; the
compiler at LMUL=8 in init code (R3.1) silently produces wrong behaviour
on the 2D path. Stick to LMUL=4 for kernels.

### 3.4 LMUL=4 register-group disjointness

`vrgather.vv` forbids overlap between source and destination groups. With
LMUL=4 the legal group-bases are `v0, v4, v8, v12, v16, v20, v24, v28`.
Pick three different ones for src/idx/dst. The standard layout in the
benchmark file is `v8` (input), `v12` (left/index), `v16` (right/dst),
`v20` (intermediate), `v24/v28` (cleaned/output). Don't drift outside
those - the SharedVRF banking is sensitive to vs/vd at non-LMUL-aligned
positions and you'll see hard-to-diagnose chaining hazards.

### 3.5 Toggle vertical mode via `csrw 0x7c0`

  * `csrw 0x7c0, t3` (where `t3` is loaded with 1) turns vertical mode
    on for subsequent vector instructions.
  * `csrw 0x7c0, zero` turns it off.
  * The CSR is sticky and architecturally programmer-visible
    (`csrr 0x7c0` returns the current value).

How the value reaches the vector unit:

  * **t1emu**: Spike maintains the CSR via
    `difftest/spike_rs/src/runner.rs` (`proc_register_basic_csr(0x7c0, 0)`,
    else `csrw 0x7c0` traps as illegal_instruction). The DPI driver
    `difftest/dpi_t1emu/src/drive.rs::update_vertical_mode_from_csr`
    mirrors Spike's CSR writes into a Rust-side `vertical_mode: bool`
    on `Driver`. When `Driver::issue_instruction` builds the next
    `IssueData` payload, it reads that mirror and populates
    `IssueData.vertical_mode`. RTL latches it as part of `T1Issue` at
    `io.issue.fire`.
  * **t1rocketemu**: Rocket's CSR file owns CSR 0x7c0 (`reg_verticalMode`
    in `rocketv/src/CSR.scala:674`). At vector issue time, RocketCore
    reads it and bundles into `T1Issue.verticalMode` alongside `vtype`,
    `vl`, `vstart`, `vcsr` (`rocketv/src/RocketCore.scala:1565+`).

In both cases, the CSR snapshot is captured **at issue time** and stable
for the entire instruction lifetime — including any LSU drain that
extends past the next CSR write. This is a behaviour-preserving change
from the older live-IO design (which had several timing races); the
programmer-visible contract is unchanged.

There is also a `+t1_vertical_mode` plusarg path (t1emu only) that
overrides `IssueData.vertical_mode` to 1 on every issue. Pass
`--vertical` to `run-test.sh` to use it. This is a debug aid; prefer
per-instruction `csrw` in real kernels.

---

## 4. Things that bend or break (open issues)

### 4.1 Reductions and `vl=1` stores

`vredsum.vs` emits its scalar result to element 0 of the destination
register group for each hardware row in the 2D replay. Earlier versions of
this note claimed only hw-row 0 materialised; that was a false diagnosis.
The observed symptom came from LSU row addressing: the store after the
reduction changed to `vl=1`, and the old LSU row pitch used
`rowCounter * vl`, so rows 0..127 were packed into `grid[0][0..127]`
instead of landing at `grid[row][0]`.

As of 2026-05-04, the LSU row pitch is fixed to the logical 128-element
image row, so the canonical reduction store works:

```
vmv.v.i v12, 0
vredsum.vs v12, v8, v12         # v8 holds image rows 0..127
vsetvli zero, one, e8, m4, ta, ma
vse8.v v12, (a1)
# afterwards:
#   grid_c[r][0] = sum of grid_a[r][:] = -64 (i8 wrap of 8128)
#   for every hw-row r = 0..127
```

Implications for ports of stock-RVV algorithms:

  * **Per-row reduction** can use `vredsum.vs` when the result shape is
    "one scalar per hardware row." Store it with `vl=1` if you only want
    `grid[row][0]`; the LSU row pitch keeps rows separated in memory.

  * **Grid-wide reduction** needs cross-hw-row collapsing first. Do 7
    steps of *vertical-mode* `vslidedown` + `vadd`. Vertical-mode slide
    DOES move data between hw-rows (the diagonal scatter in
    SharedVRF.scala makes this work). After the collapse, hw-row 0 holds
    the elementwise sum across all 128 image rows. A single horizontal
    `vredsum.vs` then gives the total in hw-row 0's vd[0], which
    `vmv.x.s` extracts to a scalar register.

  * **Don't trust** any algorithm that assumes "vredsum gives me the sum
    over the entire register, replicated to all lanes." That's not how
    this fabric implements it.

Still worth testing before relying on the full family: `vredmax.vs`,
`vredmin.vs`, `vredand.vs`, etc. should each get the same per-row scalar
store coverage as `vredsum.vs`. The current evidence is from
`benchmark_vadd.c` TEST 6 and TEST 11-15.

### 4.2 Mask v0 in vertical mode is mode-dependent

We warn about this in `benchmark_vadd.c` R7, but the exact behaviour is not
fully characterised yet. In TEST 10 we built v0 with the same `vid.v +
vand.vi + vmseq.vi` recipe in horizontal vs vertical mode and applied it
to a `vadd.vx` under each mode. The horizontal pass produced the expected
column-stripe; the vertical pass produced the input image unchanged - the
mask seemingly evaluated to "no element selected" or "all elements
selected with inverted polarity, then nothing changed because we used .t".

The hardware does treat v0 as a 2D entity (the same diagonal-scatter logic
applies), so a mask written in one mode and read in the other goes through
a transpose-like permutation. Until we dump v0 bytes at each step under
each mode, the safe rule is: **build the mask under the mode you intend to
use it in**, and don't expect a "horizontal even-columns" mask and a
"vertical even-rows" mask to come from the same source code.

**No auto-transpose on mode flip.** Switching CSR `0x7c0` does *not*
rewrite v0. The bytes that landed in v0 under one mode stay in the same
physical bank slots after the `csrw`, but the read path now applies the
other mode's scatter to them — which is a transpose, not a noop. So
when you switch from H to V (or V to H), **rebuild the mask under the
target mode** with a fresh `vid.v + vand.vi + vmseq.vi` (or whichever
recipe applies) before applying it. Same in reverse. There is no shadow
register or cache that gives you a "matched H/V pair" of v0 for free —
the H-mask and V-mask coexist in v0 only in the sense that the same
bit-pattern can be re-interpreted, and that re-interpretation almost
never gives you the mask you wanted. Programmer-side discipline is the
only contract until § 4.2 gets a hardware investigation.

To investigate: emit `vse8.v v0, (buf)` after each mask-construction step
and printf the bytes from C. Also re-read `t1/src/mask/MaskUnit.scala`
near every `verticalMode` reference for the mask read path.

### 4.3 ~~LSU is horizontal-only~~ — RESOLVED 2026-05-04

This was an open issue until PR-1/2/3 of the vertical-LSU work; see
`fyp_doc/LSU_vertical_mode_handoff.md` for the full design and
verification trail. Image transpose via H-load + V-store (or V-load +
H-store) now works as expected. The "RESERVED" tests at the bottom of
`benchmark_vadd.c` should be ported to runnable tests; the canonical
working example is
`tests/vision_task/simple_instruction_vert_lsu/simple_instruction_vert_lsu.c`.

Two practical caveats remain after the resolution:

  * **Difftest fence on vert-LSU events.** Spike's reference model
    treats vle/vse as plain row-major and cannot represent the
    diagonal scatter. The offline checker therefore skips byte-equality
    comparison for any LSU instruction whose IssueData snapshot was
    `vertical_mode=1`, plus any subsequent vstore that reads
    vertically-tainted VRF state. Horizontal LSU stays strictly
    checked. See § 5.4 of the LSU handoff.
  * **Spike's `shadow_mem` diverges in vert-store regions.** After a
    vse-vert, real memory holds Mᵀ but `shadow_mem` holds M. A
    *subsequent* Spike-checked scalar load from that region will
    diverge. Either clear/overwrite the buffer before the C-side
    validator reads it, or arrange for the validator's reads to be
    skip-flagged. The C drivers in `simple_instruction_vert_lsu` show
    one workable pattern.

---

## 5. Where to look when something is wrong

### 5.1 Hardware (Scala / Chisel)

  * `t1/src/T1.scala` - top-level wiring. Search for `verticalMode` to
    trace the CSR snapshot path:
    `requestRegCSR.verticalMode := requestReg.bits.issue.verticalMode`
    (the per-instruction snapshot from the issue payload, latched on
    `io.issue.fire`). The non-LSU consumers (`SharedVRF.verticalMode`
    IO, `MaskUnit.io.verticalMode`) read this directly. The LSU
    consumers read the chaining-record snapshot via
    `instVerticalMode(instIdx)` in SharedVRF, populated when
    `instructionWriteReport` fires from Lane (see `Lane.scala:~1310`).
    There is no live-IO `io.verticalMode` any more — it was deleted
    in PR-3 of the vert-LSU work along with the
    `t1_cosim_get_vertical_mode` DPI side-channel and the Rocket-side
    `t1.verticalMode` wire.
  * `t1/src/T1.scala::issueWritebackDrained` - the gate that holds
    `io.issue.ready` low until ALL writeback paths for the current
    instruction have drained, not just `replayFSM.lastRowFire`.
    Without this, slow non-LSU writebacks (e.g. vrgather/MaskUnit)
    can extend past the next instruction's issue, leaking the next
    instruction's CSR snapshot into the still-draining current one.
    Don't loosen this without re-running `simple_instruction_gather_scalar`
    and `simple_instruction_vert_lsu`.
  * `t1/src/vrf/SharedVRF.scala` - the bank scatter and the per-source
    selector that decides scatter vs no-scatter for each VRF
    transaction. The selector is
    `Mux(isLSUInst(idx), instVerticalMode(idx), verticalMode)` at five
    sites; LSU branch reads the snapshot, non-LSU branch reads the
    live (per-instruction-stable) verticalMode IO. Search
    `verticalMode &&` and `instVerticalMode` for every site.
  * `t1/src/laneStage/MaskExchangeUnit.scala` - mask gather/scatter,
    reductions, vrgather. The `gatherVerticalMode`, `reduceState`, and
    `narrowVertical` machinery is what makes vertical-mode compute work
    on the lane side.
  * `t1/src/decoder/attribute/*.scala` - per-instruction attributes that
    the decoder uses. If you suspect an instruction takes the wrong
    pipeline, find it here first.
  * `rocketv/src/RocketCore.scala:1565+` - Rocket-side bundling of
    `verticalMode` (and `vtype`/`vl`/`vstart`/`vcsr`) into `T1Issue`
    via `csr.io.csrToVector.get.verticalMode`. If t1rocketemu sees a
    vert-mode bug that t1emu does not, it is most likely here.
  * `difftest/dpi_t1emu/src/drive.rs::Driver::issue_instruction` -
    where t1emu populates `IssueData.vertical_mode` from
    `self.vertical_mode` (the Rust-side mirror updated by
    `update_vertical_mode_from_csr` whenever Spike commits a `csrw 0x7c0`).

### 5.2 Tests that work (good starting points)

  * `tests/vision_task/simple_instruction_gather/simple_instruction_gather.c`
    - canonical 1-shot vle + vrgather + vse with a naked kernel and a
      volatile-pointer init. The cleanest "this is how you write a
      kernel" example.
  * `tests/vision_task/simple_instruction_gather_scalar/simple_instruction_gather_scalar.c`
    - vrgather.vx (scalar-broadcast index) under H and V modes,
      masked and unmasked. The test that motivated the original
      `verticalModeReg := io.verticalMode` live-IO wiring; now it
      passes against the IssueData snapshot path. Run this as the
      regression for any change touching CSR 0x7c0 plumbing.
  * `tests/vision_task/simple_instruction_vert_hori/simple_instruction_vert_hori.c`
    - mid-kernel CSR toggle + vertical-mode vslideup. The right place
      to see a working H/V compute interleave.
  * `tests/vision_task/simple_instruction_vert_lsu/simple_instruction_vert_lsu.c`
    - the canonical vertical-LSU example: V-load + H-store transpose,
      H-load + V-store transpose, V-load + V-store round-trip, and a
      CSR-flip-during-LSU-drain stress test. All four scalar-C checks
      pass on both t1emu and t1rocketemu. Run this as the regression
      for any change to the SharedVRF gate logic, the chaining-record
      snapshot path, or the IssueData/T1Issue wiring.
  * `tests/vision_task/simple_instruction_asm/simple_instruction_asm.c`
    - naked vadd kernel; useful for confirming the
      stack-spill-avoidance pattern.
  * `tests/vision_task/benchmark_vadd/benchmark_vadd.c` - this is the
    document's reference implementation. Tests 1-5, 8 work end-to-end;
    tests 6, 7, 9, 10 hit the open issues in § 4 (kept on purpose as
    documented examples of the constraints). The "RESERVED" tests at
    the bottom (R-TEST 7/8/10) are no longer blocked — port them to
    runnable tests when convenient.

### 5.3 Tests that hang/trap (study to learn what NOT to do)

  * `tests/vision_task/simple_vadd/simple_vadd.c` - hangs because
    `place_counter()` is called inside the vector loop, forcing the
    compiler to spill v8/v12 across the call. Fixed by moving
    `place_counter` out and rewriting the kernel naked. Don't
    "fix" by enlarging the stack; see § 3.2.
  * `tests/vision_task/simple_vadd_big/simple_vadd_big.c` - has the
    auto-vectorised init from § 3.1. Look at the disassembled
    `initialise_grids` to see what `-O2` will do to a scalar nested
    loop that the compiler thinks is a candidate.

### 5.4 Logs and disassembly

After every `bash run-test.sh ...` invocation, the output directory under
`test_output/<config>/<test>-<timestamp>/` contains:

  * `run.log` - full simulator stdout. `[PERF] counter N START at cycle X`
    plus `[PERF] counter STOP at cycle X` lines come from the perf
    counters; the rest is `printf` output (with `T1_MIRROR_RTL_WRITES=1`
    in the env).
  * `<test>.s` - llvm-objdump of the test ELF. Great for verifying that
    your naked kernel really is the assembly you wrote, and that
    init/check loops were not auto-vectorised.
  * `rtl_event.jsonl` - DPI event stream. Useful for watching
    `LsuEnq` / commit cadence when the test is hung; tail this if
    `run.log` is silent.

Use `--max-cycles 50000000` for full-grid kernels - smaller caps abort
the run mid-test and look like genuine hangs. Stack trace pattern:
"trap_illegal_instruction (tval=0)" almost always = stack-spill-clobbered
RA, not a Spike-side bug.

---

## 6. Programmer's "first kernel" checklist

If you're about to write a new vector test, walk through this:

  1. Globals are `int8_t arr[ROWS][COLS]` with no initialiser. Init in C
     with `volatile int8_t *p = ...` boilerplate (§ 3.1).

  2. Kernel is `__attribute__((naked, noinline))` with one `__asm__
     volatile(...)` block. No C statements in the function body.

  3. First two instructions of the asm: `csrw 0x7c0, zero` (or `t3` for
     vertical) and `vsetvli zero, COLS, e8, m4, ta, ma`.

  4. Memory ops use `vle8.v` / `vse8.v` only, with the address coming
     from an `aN` argument register. They honour CSR `0x7c0` — set it
     to 0 for a normal row-major load/store, set it to 1 to get a
     transpose-on-the-way-in (vle-vert) or transpose-on-the-way-out
     (vse-vert). Memory addressing stays row-major in both modes;
     only the VRF-side layout flips. The row pitch is the logical image
     width, currently fixed at 128 e8 elements in RTL, not the current
     instruction `vl`; `vl=1` stores therefore write one element per
     hardware row to `grid[row][0]`.

  5. Compute ops AND memory ops can switch mode mid-block via
     `csrw 0x7c0, ...`. Restore to 0 at the end of the kernel so
     subsequent kernels see a clean state. The CSR snapshot is
     captured at issue time and stable for the entire instruction
     drain, so a `csrw 0x7c0, zero; vse8.v` sequence does NOT
     leak the new CSR value into the still-draining vse — the
     vse uses whatever value was set when it was issued.

  6. Counter-tagged perf measurement: encode `place_counter(tag)` as
     `sw <tag>, 0(<perf_reg>)` in the asm so it does not become a
     function call and force a spill.

  7. After building, look at `<test>.s` and grep for `vs1r.v`,
     `vl1r.v`, `csrr.*vlenb`. If any appear inside your kernel, you
     have a spill - rewrite to remove the call/intrinsic that caused
     it.

  8. Run with `--max-cycles 50000000`. If the run hangs, tail
     `rtl_event.jsonl` to see whether cycles are still advancing
     (live) or really stuck (dead).

  9. If a reduction looks wrong, suspect § 4.1 first.

  10. If a mask under vertical mode looks wrong, suspect § 4.2 first.

That's the whole programming environment. Welcome.
