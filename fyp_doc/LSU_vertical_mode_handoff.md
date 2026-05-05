# Vertical-mode LSU: Implementation handoff

**Audience:** an automated coding agent (Codex, Claude Code, ...) tasked with
landing vertical-mode load/store on this 2D-fabric design. Read this *and*
`fyp_doc/2d_fabric_handoff.md` end-to-end before touching any code. The
2D-fabric handoff is the prerequisite — it explains the row-counter
time-multiplexing, CSR `0x7c0` semantics, and the diagonal bank scatter that
this work depends on.

Goal: make `vle8.v` and `vse8.v` honour CSR `0x7c0`. When the CSR is 1 at the
time the LSU instruction is issued, the LSU should write to (or read from) VRF
through the same vertical scatter/gather path that lane-side compute already
uses. The memory address pattern stays row-major (hw-row `r` touches
`&base[r * COLS]`) — only the VRF-side layout flips. Net effect:

  * `vle8.v` (vert): row-major load from memory, transposed layout in VRF
    (subsequent **horizontal** reads of vd see Mᵀ).
  * `vse8.v` (vert): vertical gather from VRF, row-major store to memory
    (memory ends up holding Mᵀ if vd held M).

This is the missing primitive blocking R-TEST 7/8/10 in
`tests/vision_task/benchmark_vadd/benchmark_vadd.c:291-298` (full-grid sum
via H-store + V-load, image transpose via H-load + V-store, and 3x3 box
blur with mode-switched LSU).

---

## 1. Background: why the gate exists today

`SharedVRF.scala` *already* implements vertical-mode read-gather and
write-scatter for lane traffic. The plumbing is complete. There is exactly
one thing standing in the way of vertical LSU: an explicit gate that forces
LSU traffic onto the horizontal path even when `verticalMode` is asserted.

The gate is `isLSUInst(instIdx)` in `t1/src/vrf/SharedVRF.scala:227-230`:

```scala
def isLSUInst(instIdx: UInt): Bool =
  chainingRecord
    .map(r => r.valid && (r.bits.instIndex === instIdx) && (r.bits.ls || r.bits.st))
    .reduce(_ || _)
```

It is consulted in five places, all of the form
`verticalMode && !isLSUInst(...)`:

| Site                              | What it gates                                   |
|-----------------------------------|-------------------------------------------------|
| `SharedVRF.scala:362`             | `isWideVertReq_i` per-port read fold suppress   |
| `SharedVRF.scala:422`             | `useVerticalRead` (lane wide-vertical gather)   |
| `SharedVRF.scala:467-468`         | `writeWouldScatter` (bank-arb all-banks claim)  |
| `SharedVRF.scala:488-489`         | `useVerticalWrite` (per-bank scatter write)     |
| `SharedVRF.scala:600`             | `isWideVerticalReq` (per-port `r.ready` mux)    |

The reason the gate was put there is in the comment at `SharedVRF.scala:222-226`:
without it, vle scatters into vertical banks and vse gathers them back, so
both cancel and the slideup result becomes byte-identical to horizontal.
That cancellation is exactly the property we now want to make
*programmer-controlled* rather than enforced in hardware. Lifting the gate
means a vle-vert + vse-vert round-trip is again a no-op (transpose-of-transpose),
but a vle-vert + vse-horiz now produces an in-memory transpose, which is the
goal.

There is **no LSU-side change**. The LSU already sends row-stride
addresses. We only change how `SharedVRF` lays the bytes into the bank
array on the VRF side.

---

## 2. The per-instruction-latching subtlety

`verticalMode` arrives at `SharedVRF` as a **live** IO input
(`SharedVRF.scala:128`, driven from `T1.scala:792`). Lane-side compute
finishes within a single row-counter cycle, so the live value matches the
issue-time value. **LSU traffic does not.** A vle8 streams hundreds of
cycles; if the program does:

```
csrw 0x7c0, t3      # t3 = 1, vertical
vle8.v v8, (a0)
csrw 0x7c0, zero    # back to horizontal
vadd.vv v12, v8, v8 # this latches verticalMode=0
```

the compute instruction's `csrw` will flip `verticalMode` while the vle is
still draining. Half the vle's writes go to the scatter path, half go to
the horizontal path → corruption. So the gating must use the
*per-instruction* verticalMode that was active when this LSU was issued,
not the live IO.

### 2.1 Timing precondition — DO NOT sample any RTL-side edge

> **REVISION (2026-05-04).** The original § 2.1 claimed
> `laneRequest.bits.issueInst` was a safe sample edge. That was
> empirically *wrong* — see TEST2 of `simple_instruction_vert_lsu`
> (`test_output/.../-20260503-213420/`), where the chaining record's
> verticalMode bit was always 0 even though the vse was issued under
> verticalMode=1. The waveform showed `io.verticalMode` pulsing 0→1
> at cycle 27610 and 1→0 at 27612, but `instructionWriteReport`
> for the vse fired *after* 27612, capturing 0. The fix is to stop
> sampling any RTL-side edge of `io.verticalMode` and instead let
> t1emu hand the per-instruction value over inside the IssueData
> payload itself. Read on for why.

The CSR plumbing has two independent paths from t1emu to the RTL,
which is the root of every timing bug we have hit:

  * **`IssueData` payload** (`difftest/dpi_t1emu/src/dpi.rs:73-82`).
    Atomic struct delivered from t1emu to RTL when
    `t1_cosim_issue_vector_instruction` is called. Contains
    `instruction_bits`, `vtype`, `vl`, `vstart`, `vcsr`, etc.
    Synchronous with `io.issue.fire`.

  * **`io.verticalMode` IO** (`difftest/dpi_t1emu/src/dpi.rs:268`).
    Separate DPI call `t1_cosim_get_vertical_mode` returning the
    Rust-side `Driver::vertical_mode` mirror. Queried independently
    each cycle the RTL evaluates it, **not synchronised with
    issue.fire**.

This split is the bug. The mirror update for a `csrw 0x7c0, t3`
happens inside `step()` at `drive.rs:336`, which runs ahead of the
RTL — by the time the corresponding vector instruction's
`io.issue.fire` actually pulses in RTL, t1emu may already have
processed the *next* csrw and flipped the mirror back. For slow
instructions like vse the gap is many cycles wide. So **any RTL-side
sample of `io.verticalMode` is fundamentally racy**, regardless of
the edge:

  * `io.issue.fire` / `requestRegDequeueFire` — sometimes too early
    (mirror not yet updated for *this* instruction). Disproved by
    `simple_instruction_gather_scalar` TEST2.
  * Live IO read during LSU drain — sometimes too late (mirror
    already advanced past *this* instruction to the next csrw).
    Disproved by `simple_instruction_vert_lsu` TEST2.
  * `laneRequest.bits.issueInst` — same problem as live-IO read,
    one of N cycles in the LSU drain happens to be the report-fire
    cycle, and there's no reason it has to land in the brief mirror
    pulse for this instruction. Disproved by
    `simple_instruction_vert_lsu` TEST2 / TEST4.

There is no RTL-side edge that is correct in all cases. Stop looking
for one.

### 2.1a The fix: thread verticalMode through IssueData

Capture the snapshot **inside t1emu**, *before* the next csrw can
touch the mirror, and ship it as part of the atomic IssueData
payload. RTL never samples the live IO; it reads the value from the
already-latched issue bundle.

  1. **Rust side, `IssueData` struct** (`dpi.rs:73-82`). Add a field:
     ```rust
     #[repr(C, packed)]
     #[derive(Default)]
     pub(crate) struct IssueData {
       pub meta: u32,
       pub vcsr: u32,
       pub vstart: u32,
       pub vl: u32,
       pub vtype: u32,
       pub src2_bits: u32,
       pub src1_bits: u32,
       pub instruction_bits: u32,
       pub vertical_mode: u32,   // <-- new, 0 or 1
     }
     ```

  2. **Rust side, `Driver::issue_instruction`** (`drive.rs:344+`).
     When constructing IssueData for a vector instruction, populate
     the field from `self.vertical_mode`:
     ```rust
     IssueData {
       instruction_bits: se.inst_bits,
       // ... existing fields ...
       vertical_mode: if self.vertical_mode { 1 } else { 0 },
       ..Default::default()
     }
     ```
     This runs *after* `step()` has finished processing all preceding
     csrw's (the mirror update at `drive.rs:336` is inside the step
     loop, which has already returned by the time `issue_instruction`
     constructs the payload). The captured value is therefore the
     mirror state at the exact moment Spike committed *this*
     instruction.

  3. **Chisel side, `T1Issue` bundle.** Add a `verticalMode: Bool`
     field next to the other CSR/issue bits. Adjust the bundle width
     to match the new IssueData layout.

  4. **Chisel side, `T1.scala:568-573`.** Replace the live-IO
     wiring:
     ```scala
     // before:
     val verticalModeReg: Bool = io.verticalMode
     requestRegCSR.verticalMode := verticalModeReg

     // after:
     requestRegCSR.verticalMode := requestReg.bits.issue.verticalMode
     ```
     `requestReg` is enabled by `io.issue.fire` (`T1.scala:801`), so
     the field is latched once at issue and stable for the whole
     instruction. No race.

  5. **Chisel side, `T1.scala:792`.** Drop the live-IO wiring to
     SharedVRF and source it from the same per-instruction value:
     ```scala
     // before:
     sharedVRF2D.foreach(_.verticalMode := verticalModeReg)
     // after:
     sharedVRF2D.foreach(_.verticalMode := requestRegCSR.verticalMode)
     ```
     This collapses the two paths (LSU snapshot + non-LSU compute)
     into one stable per-instruction value. The `Mux` in § 2.4 still
     uses `instVerticalMode(idx)` for LSU and the now-stable
     `verticalMode` IO for non-LSU; both branches read the same
     underlying value but routed through their respective gating.

  6. **Chisel side, `Lane.scala` instructionWriteReport.** Step (5)
     of § 5.2 still applies — capture the bit into the chaining
     record from `csrInterface.verticalMode`. With the new wiring,
     `csrInterface.verticalMode` is the issue-bundle value
     propagated through `request.bits.csrInterface := requestRegCSR`
     (`T1.scala:1198`), so the chaining record gets the correct
     per-instruction snapshot.

  7. **Chisel side, `io.verticalMode` IO** (`T1.scala:441,453`).
     After (5) it has no remaining consumers. **Delete it** — keep
     the surface area small. The corresponding DPI call
     `t1_cosim_get_vertical_mode` (`dpi.rs:268`) can also be
     removed once nothing reads it. Codex: confirm by `grep -n
     verticalMode` across all RTL after the refactor — only
     `requestReg.bits.issue.verticalMode` and downstream uses
     should remain.

### 2.1c Mirror change for t1rocketemu

The same race exists on the Rocket+T1 backend, just with a real Rocket
CSR file in place of the Spike mirror. The current Rocket integration
in `rocketv/src/RocketCore.scala:1565-1573` is:

```scala
t1IssueQueue.enq.bits.vtype       := csr.io.csrToVector.get.vtype
t1IssueQueue.enq.bits.vl          := csr.io.csrToVector.get.vl
t1IssueQueue.enq.bits.vstart      := csr.io.csrToVector.get.vstart
t1IssueQueue.enq.bits.vcsr        := csr.io.csrToVector.get.vcsr
// ...
t1.verticalMode                   := csr.io.csrToVector.get.verticalMode
```

`vtype`/`vl`/`vstart`/`vcsr` are all standard RVV CSRs and they are
**bundled into `T1Issue`**, then queued through `t1IssueQueue` and
delivered atomically when T1 accepts the issue. `verticalMode` is the
outlier — it leaks out as a separate live wire, with the comment
("T1 latches it at its own io.issue.fire") describing the exact racy
mechanism § 2.1 retracted. The fix is symmetric to § 2.1a:

  1. **`rocketv/src/Bundle.scala:1590`** (`T1Issue` class): add
     ```scala
     val verticalMode: Bool = Bool()
     ```
     next to the existing `vtype`/`vl`/`vstart`/`vcsr` fields.

  2. **`rocketv/src/RocketCore.scala:1565+`**: in the same block that
     fills `t1IssueQueue.enq.bits.{vtype,vl,vstart,vcsr}`, add:
     ```scala
     t1IssueQueue.enq.bits.verticalMode := csr.io.csrToVector.get.verticalMode
     ```
     The `t1IssueQueue` (the `Queue` between Rocket's issue stage and
     T1's `io.issue` port) carries the snapshot through any
     back-pressure cycles. By the time the issue is dequeued, the
     value is frozen at enqueue time — immune to any later `csrw
     0x7c0` Rocket may execute while waiting for T1.ready.

  3. **`rocketv/src/RocketCore.scala:1573`**: delete the side-channel
     ```scala
     t1.verticalMode := csr.io.csrToVector.get.verticalMode
     ```

  4. **`rocketv/src/Bundle.scala:1587`**: delete the
     `verticalMode: Bool = Output(Bool())` on the Rocket→T1 IO
     bundle (whatever class it lives on — search for line 1587).

  5. **`t1rocket/src/T1RocketTile.scala:528`**: delete
     ```scala
     t1.io.verticalMode := rocket.io.t1.get.verticalMode
     ```

After (1)-(5), Rocket's plumbing matches t1emu's plumbing:
verticalMode is an issue-bundle field, indistinguishable from
`vtype`/`vl`. The previous side-channel was the only thing
preventing a clean per-instruction snapshot.

**Architectural note: this is not a deviation from RVV.** RVV does
not specify the scalar↔vector interface — only the programmer-visible
CSRs and instruction semantics. Bundling CSR snapshots into the issue
payload is the standard pattern this design already uses for
`vtype`/`vl`/`vstart`/`vcsr`; we are just extending it to one more
CSR. The architectural visibility of CSR 0x7c0 is preserved
(`reg_verticalMode` in `rocketv/src/CSR.scala:674`,
read-mux at `:844-845`, write decoder at `:1819-1820`). A program
still sees `csrw 0x7c0, X` as a sticky, programmer-visible state
update, and `csrr 0x7c0` returns the current value. The issue-bundle
move is purely microarchitectural.

If anything, the issue-bundle approach gives **stronger** ordering
guarantees than the side-channel:

  * `csrw 0x7c0, X; vop` → vop sees X (snapshot at issue).
  * `vop; csrw 0x7c0, Y` → vop is unaffected by Y (snapshot is
    frozen, csrw retires after vop's snapshot was taken).
  * `vop; csrw 0x7c0, Y` while vop is still draining in T1 →
    same: vop's chaining-record entry holds the issue-time
    snapshot; Y has no effect on the in-flight vop.

Those are the ordering rules a programmer naturally expects from
"per-instruction mode CSR" semantics, and they are not achievable
with a live-wire side-channel.

### 2.1b Why this doesn't regress simple_instruction_gather_scalar

The gather_scalar bug was: RTL latched `io.verticalMode` (the racy
mirror) at `io.issue.fire`, capturing one-issue-late. The fix above
*does not sample the live IO at all on the RTL side*. t1emu
populates the value inside the IssueData payload before RTL sees the
issue, so there is no edge to be late-on. As long as t1emu's
`issue_instruction()` reads `self.vertical_mode` **after**
`step()` has completed all CSR mirror updates for preceding
instructions (which is the case — the loop in `step()` exits only
after the next vector instruction is committed, and any preceding
csrw's have already flowed through `update_vertical_mode_from_csr`),
the captured value is right.

Run `simple_instruction_gather_scalar` after the change as a
regression check (still on the acceptance checklist in § 7).

### 2.2 What about `laneRequest.bits.csrInterface.verticalMode`?

`requestRegCSR.verticalMode := io.verticalMode` is a **wire** assignment,
not a register (`T1.scala:572-573`). The signal flows combinationally
through `request.bits.csrInterface := requestRegCSR` (line 1198), through
the LaneInterface virtual-channel passthrough (`t1/src/interface/LaneInterface.scala:170-178`,
which does not latch the request bits, just forwards them), and arrives
at the lane's `laneRequest.bits.csrInterface.verticalMode` tracking the
live IO at whatever cycle the data physically traverses.

That signal is therefore **not** a stable per-instruction snapshot in its
own right. It is only correct *as a sample value* at the instant
`laneRequest.bits.issueInst` fires. After that instant, do not read it
again for this instruction — read the chaining-record bit instead.

This is fine for our use because we sample *exactly once* at issueInst
fire and write into the chaining record. After that, every gate site in
SharedVRF reads the chaining record (per § 3 below), not
`laneRequest.bits.csrInterface.verticalMode`.

### 2.3 LSU side does NOT need its own plumbing

`lsuRequestTopWire.bits.csrInterface := requestRegCSR` (`T1.scala:1338`)
also carries the live verticalMode bit, and the LSU's own
`csrInterface.verticalMode` field is therefore similarly live-fed and
unstable across long drains. **This does not matter** because the LSU
itself does not consume verticalMode for its address generation — it
always emits row-stride addresses (`&base[r * COLS]` per hw-row), which
is the correct memory access pattern for both vertical and horizontal
LSU. Verify this by `grep -n verticalMode t1/src/lsu/*.scala` returning
no matches.

The verticalMode-aware logic lives entirely on the VRF side
(`SharedVRF.scala`) and the MaskUnit side (`MaskExchangeUnit.scala`).
For this work we touch only the VRF side; the LSU module proper is
untouched.

### 2.4 The fix: chaining-record snapshot, lookup-by-index gates

  1. Add a `verticalMode: Bool` field to `VRFWriteReport`
     (`t1/src/Bundles.scala:398`).
  2. Populate it at the report site in `Lane.scala:1310-1343` from
     `csrInterface.verticalMode` (CSRInterface already carries the field
     at `Bundles.scala:265`, and LaneRequest carries CSRInterface at
     `Bundles.scala:831`). The report's `valid` is already gated on
     `laneRequest.bits.issueInst`, which is the correct sample edge per
     § 2.1, so the field captures the right value naturally.
  3. In `SharedVRF.scala`, define a sibling helper
     `instVerticalMode(instIdx: UInt): Bool` that looks up the
     chainingRecord entry the same way `isLSUInst` does, and returns
     `r.bits.verticalMode`. Use it in the five gate sites.

Lane-side gates that currently use the live `verticalMode` IO are still
correct (because at issue time the live IO equals the per-instruction
latched bit, and lane-side compute is single-row-counter-cycle so it
cannot straddle a CSR flip). The change at each gate site is **not**
"add a clause to the existing `verticalMode && !isLSUInst(...)`"; it is
**"replace the LSU-vs-non-LSU branch wholesale with a per-source
selector"**. The right phrasing:

```
// today: verticalMode && !isLSUInst(idx)
//   - LSU always horizontal (regardless of snapshot)
//   - non-LSU follows live IO
// new:   Mux(isLSUInst(idx), instVerticalMode(idx), verticalMode)
//   - LSU follows its issue-time snapshot, full stop
//   - non-LSU follows live IO (unchanged)
val takeVerticalPath = Mux(isLSUInst(idx),
                           instVerticalMode(idx),
                           verticalMode)
```

**Do not write the formula as `verticalMode && (!isLSUInst(idx) ||
instVerticalMode(idx))`.** That keeps the outer `verticalMode &&` term
which goes false the moment a later `csrw 0x7c0, zero` flips live IO.
For an LSU that is still draining bytes in vertical mode, the
expression then collapses to 0 mid-drain and the remaining writes leak
to the horizontal path — which is the exact corruption § 2 and § 4.3a
exist to prevent. The whole point of latching into the chaining record
is so the LSU's gate decision becomes **independent** of live IO; an
outer live-IO conjunction destroys that property.

The non-LSU side is the only branch where live `verticalMode` belongs.
Lane-side compute is single-row-counter-cycle, so live IO at the cycle
of the lane request fire equals the CSR snapshot for that instruction
by construction, and there is no drain to drift across.

For each of the five sites, replace the relevant subexpression with the
`Mux` form above. See § 5 for exact textual edits.

### 2.5 Sanity assertion (structural, not value-equality)

The useful invariant is **structural**: "LSU routing decisions read the
snapshot, not live IO." That is a property of the *signal flow* in the
gate formula, not a runtime equality between values, and Codex
correctly observed that the obvious "live == snapshot" form would fire
on the deliberately-CSR-flipping § 4.3a test.

The closest *runtime* check that is still useful is the contrapositive
of the bug: if a vertical-mode write fires from an LSU instruction,
that LSU's chaining-record snapshot must be high. If somebody rewrites
the gate to lean on live IO again, the test will pass through the
moment live IO is high but fail the moment it drops mid-drain — and
this assertion fires precisely on the writes that were meant to be
vertical (per snapshot) but routed horizontal (per buggy gate):

```scala
// Bon2D vertical LSU: structural check that any vertical-path write
// from an LSU is grounded in that LSU's chaining-record snapshot,
// not in live verticalMode. Fires only when the gate has effectively
// read live IO instead of the snapshot. § 4.3a (CSR-flip-during-drain)
// must run cleanly under this assertion.
when(writePipe.valid && isLSUInst(writePipe.bits.instructionIndex)) {
  // If the gate selected the vertical write path, the snapshot must say so.
  when(useVerticalWrite) {
    assert(instVerticalMode(writePipe.bits.instructionIndex),
      "LSU on vertical write path but its snapshot is 0 - " +
      "gate is reading live verticalMode instead of snapshot")
  }
  // If the snapshot says vertical, the gate must have selected the vertical path.
  when(instVerticalMode(writePipe.bits.instructionIndex)) {
    assert(useVerticalWrite,
      "LSU snapshot is vertical but gate routed horizontal - " +
      "gate is reading live verticalMode instead of snapshot")
  }
}
```

The two `when`s together pin `useVerticalWrite === instVerticalMode(idx)`
for any LSU write (both directions), without saying anything about the
live `verticalMode` IO. That is exactly the structural invariant we
want: "LSU routing == snapshot." Live IO can flip freely around this.

A symmetric assertion can be added for the read path with `useVerticalRead`
and `firstReadFromLSU` / `firstReadReq.instructionIndex`. Whether to
gate it on `firstReadFromLSU && useVerticalRead` or fold it into the
per-port loop depends on whether the wide-vertical fire predicate ends
up using the snapshot directly (preferred) or via a per-port selector
— land § 5.3 first, then mirror the assertion to whichever LSU-aware
predicate the read path settles on.

---

## 3. Difftest strategy: fence off vert-LSU regions

Spike's reference model treats vle/vse as plain row-major. Once vertical
LSU is enabled, the RTL VRF writes for a vle-vert (and the AXI writes for
a vse-vert) will diverge from Spike. The cleanest tactic is to **make the
difftest checker stop comparing for instructions issued under
verticalMode**, on a per-instruction basis, rather than try to teach Spike
about the diagonal scatter.

The DPI driver already tracks `vertical_mode` as a Rust-side mirror of CSR
0x7c0 (`difftest/dpi_t1emu/src/drive.rs:200,208-232,234-236`). Reuse that
state.

### 3.1 Where the comparisons happen

  * **VRF write byte-by-byte check:** `peek_vrf_write` in
    `difftest/t1-sim-checker/src/t1emu/json_events.rs:187-252`. The hard
    assertion is at line 226-241 (`assert_eq!(record.byte, written_byte, ...)`).
  * **Memory writes:** the AXI write side. Search
    `difftest/dpi_t1emu/src/drive.rs` for `axi_write` and the
    shadow_mem / spike memory comparisons.

### 3.2 What "fence off" means concretely

For each LSU instruction issued under verticalMode=1, mark the
corresponding `SpikeEvent` (or its `vrf_access_record`) with a flag
`skip_diff: bool`. When `peek_vrf_write` (or the AXI checker) finds a
matching event with `skip_diff == true`, **consume the bookkeeping**
(decrement `unretired_writes`, etc.) but skip the byte-equality assertion
and any shadow-mem mismatch reporting.

The flag is set at the SpikeEvent construction / commit point — the
moment Spike sees a vle/vse and the Rust-side `vertical_mode` mirror is
true. Look at `difftest/spike_rs/src/runner.rs` for the
`SpikeEvent::new`/`commit` site (around the CSR registration at line 97);
that is the layer that knows both the instruction (vle/vse) and the
current `vertical_mode` flag from the DPI driver.

This is the smallest possible change to the comparator: it does not
require translating the diagonal scatter into Spike, and it keeps
horizontal-mode LSU difftest fully strict. The cost is that vert-LSU
correctness is verified only by the in-test C-side checks (§ 4.3), not by
difftest.

### 3.3 What NOT to do

  * Do **not** delete the assertions at
    `json_events.rs:226-241` — they catch real bugs in horizontal LSU.
    The fence must be a per-event opt-out, not a global disable.
  * Do **not** make the fence keyed on the live `vertical_mode` flag.
    By the time the VrfWrite event arrives the CSR may have flipped.
    Latch it on the SpikeEvent at issue time.
  * Do **not** silently swallow horizontal-mode mismatches. If
    `skip_diff` is false and there is a mismatch, panic as today.

---

## 4. Verification plan

### 4.1 Quick sanity (smoke)

Land the change with the difftest fence active and run the existing
`benchmark_vadd` regression:

```
bash tests/vision_task/benchmark_vadd/run-test.sh --max-cycles 50000000
```

Tests 1-5 and 8 should still pass. They do not touch verticalMode at the
LSU, so this is a pure regression check that the gate-replacement did not
break horizontal LSU. If any of these regress, you almost certainly broke
the live-IO vs latched-bit distinction (§ 2).

### 4.2 Vertical-LSU primitive test

Add a new test under `tests/vision_task/simple_instruction_vert_lsu/`
modelled on `tests/vision_task/simple_instruction_vert_hori/` (canonical
H/V interleave example) and `tests/vision_task/simple_instruction_gather/`
(canonical naked-kernel boilerplate).

Kernel sketch (call it `transpose_via_vert_load`):

```
vsetvli zero, COLS, e8, m4, ta, ma
csrw    0x7c0, t3            # t3 = 1, vertical mode ON
vle8.v  v8, (a0)             # vert load: scatters into VRF as transpose
csrw    0x7c0, zero          # vertical mode OFF
vse8.v  v8, (a1)             # horiz store: writes Mᵀ to memory
```

C-side check:

  * Initialise `int8_t in[ROWS][COLS]` with a non-symmetric pattern
    (e.g. `in[r][c] = (int8_t)(r * 31 + c)` so `in[r][c] != in[c][r]`).
  * After kernel: assert `out[r][c] == in[c][r]` for all `r,c`.
  * Print the first 4x4 of input and output for eyeball confirmation.

Then mirror the test the other way (`transpose_via_vert_store`):

```
csrw    0x7c0, zero
vle8.v  v8, (a0)             # horiz load: regular layout in VRF
csrw    0x7c0, t3
vse8.v  v8, (a1)             # vert store: gathers Mᵀ to memory
```

Both should produce the same in-memory transpose. If they disagree, the
scatter and gather paths are not inverses for the LSU operand layout —
this would be a bug in the fix, not in the existing scatter machinery.

### 4.3a CSR-flip-during-LSU-drain test (timing precondition)

This is the regression that proves the § 2.1 latch is correct. Without
this, a future refactor that re-introduces stale-sampling will silently
break vert-LSU on programs that touch the CSR around an LSU.

```
csrw    0x7c0, t3            # vertical
vle8.v  v8, (a0)             # vert load - long drain begins
csrw    0x7c0, zero          # flip back DURING drain (next instruction
                             # depends on architectural ordering, but the
                             # CSR write reaches the live IO promptly)
csrw    0x7c0, t3            # and flip again
csrw    0x7c0, zero
vse8.v  v8, (a1)             # H-store of what should be Mᵀ
```

Output memory must equal Mᵀ exactly. If the chaining-record bit is being
read as live IO somewhere, the mid-drain flips will scramble the result.

A more aggressive variant: place the `csrw` flips *after* the vle8 issue
in the asm, separated by a `nop` chain so they fire while the LSU is
still draining. The exact cycle alignment matters less than just having
the live IO toggle while LSU writes are in flight.

### 4.3 Round-trip cancellation test (negative control)

```
csrw 0x7c0, t3
vle8.v v8, (a0)
vse8.v v8, (a1)              # both vert; should round-trip data unchanged
csrw 0x7c0, zero
```

Output memory must equal input memory byte-for-byte. This confirms the
scatter and gather are still mutual inverses (the property the original
isLSUInst gate was protecting; we want it to hold by construction, not
by gating).

### 4.4 Re-enable RESERVED tests

Once 4.2 and 4.3 pass, port `R-TEST 7` (full-grid sum), `R-TEST 8`
(transpose), and `R-TEST 10` (3x3 blur with mode-switched LSU) from the
design notes at `tests/vision_task/benchmark_vadd/benchmark_vadd.c:291-298`
into runnable tests. The kernel sketches should be at the bottom of the
same file (search for "RESERVED tests" — the file is 1012 lines, the
sketches are after the `test()` driver).

### 4.5 Difftest acceptance

Run all of the above with difftest enabled (default for t1emu):

  * Horizontal-mode tests must still produce zero divergences.
  * Vertical-LSU tests must run to completion. The fence must mean
    "no false positives", not "tests are skipped". Watch the run log for
    the "skip_diff" decision being logged for every vert-LSU
    instruction; if the count is 0 the fence is not wired up.

---

## 5. Concrete edits

This is the order to make them in. Each step is independently
buildable; commit between steps so a regression is bisectable.

### 5.0 Staging strategy: t1emu first, Rocket second, deletions last

**Do not land all of § 5 in one PR.** The work touches three
codebases (Chisel `t1`, Rust `difftest`, Chisel `rocketv` +
`t1rocket`). Stage it so each step is independently verifiable and
the blast radius of any regression is small:

  **PR-1: t1emu correctness.**
    1. § 5.1 — add `verticalMode` to `VRFWriteReport`.
    2. § 5.1b — fix store report allocation in `Lane.scala`.
    3. § 2.1a steps 1-2 — add `vertical_mode` to `IssueData`
       (Rust) and populate from `self.vertical_mode` in
       `Driver::issue_instruction`.
    4. § 2.1a step 3 — add `verticalMode: Bool` to `T1Issue`
       (Chisel) with width matching IssueData.
    5. § 2.1a step 4 — change `T1.scala:572-573` to source
       `requestRegCSR.verticalMode` from
       `requestReg.bits.issue.verticalMode`.
    6. § 5.2 — populate `instructionWriteReport.bits.verticalMode`
       from `csrInterface.verticalMode` in `Lane.scala`.
    7. § 5.3 — replace the five gate sites in `SharedVRF.scala`
       with the `Mux(isLSUInst(idx), instVerticalMode(idx),
       verticalMode)` form.
    8. § 5.4 — difftest fence (skip_diff on SpikeEvent).
    9. § 5.5 — add `simple_instruction_vert_lsu` test.
    10. Run § 4.1 (horizontal regression: benchmark_vadd 1-5/8 must
        pass with zero diffs), § 4.2 (transpose-via-vert-load and
        -vert-store), § 4.3 (round-trip cancellation), § 4.3a
        (CSR-flip-during-drain), `simple_instruction_gather_scalar`
        (no regression).

    Do **not** touch § 2.1a step 5 (drop SharedVRF live-IO wiring),
    step 7 (delete `io.verticalMode` IO and DPI call), § 2.1c
    (Rocket parity), or any other deletions yet. The live IO stays
    until the issue-bundle path is proven on both backends.

    PR-1 acceptance: § 4.1, § 4.2, § 4.3, § 4.3a, gather_scalar
    all pass. `io.verticalMode` IO still exists but has at most
    one read site (the SharedVRF live-IO path); confirm by `grep
    -n io.verticalMode` in `T1.scala`.

  **PR-2: t1rocketemu parity.**
    1. § 2.1c steps 1-2 — add `verticalMode` to `T1Issue` in
       `rocketv/src/Bundle.scala:1590` and assign it from
       `csr.io.csrToVector.get.verticalMode` in
       `RocketCore.scala:1565+`.
    2. Rerun the same § 4.1-§ 4.3a + gather_scalar suite against
       t1rocketemu. (This catches whether the Rocket-side issue
       queue's depth changes the timing in a way the t1emu mirror
       didn't expose.)

    Do **not** delete the `t1.verticalMode` side-channel yet.
    Run with both paths active — the live IO carrying the same
    value as the bundle field. Confirm by waveform that the bundle
    field arrives at T1 *before* the live IO would drift, even
    under back-pressure on `t1IssueQueue`.

    PR-2 acceptance: t1rocketemu vert-LSU tests pass; horizontal
    regression unchanged.

  **PR-3: deletions.**
    1. § 2.1a step 5 — `T1.scala:792` sources SharedVRF's
       `verticalMode` IO from `requestRegCSR.verticalMode`
       (downstream of the issue-bundle latch) instead of from
       `verticalModeReg`.
    2. § 2.1a step 7 — delete `io.verticalMode` IO from
       `T1.scala:441,453` and the DPI call
       `t1_cosim_get_vertical_mode` at `dpi.rs:268`.
    3. § 2.1c steps 3-5 — delete the Rocket-side side-channel
       (`RocketCore.scala:1573`,
       `t1rocket/src/T1RocketTile.scala:528`, and the
       `verticalMode: Bool = Output(Bool())` field on the
       Rocket→T1 IO bundle at `Bundle.scala:1587`).
    4. Final `grep -n verticalMode` audit per § 6 — only the
       allowed names should remain.

    PR-3 acceptance: full suite passes on both t1emu and
    t1rocketemu; `grep` audit clean.

Why this order works:

  * PR-1 fixes the actual correctness bug. Smallest blast radius.
    If anything regresses, only `t1emu` is affected and only the
    new IssueData field is suspect. Bisect-friendly.
  * PR-2 brings t1rocketemu into parity *before* deletions, so the
    live-IO side-channel is still around as a fallback if the
    Rocket-side bundle path has a subtle timing issue we didn't
    anticipate (e.g., if `t1IssueQueue`'s depth interacts with
    Rocket's CSR-write retire timing).
  * PR-3 removes the deprecated paths only after both backends have
    proven the new path. The deletion is then mechanical and the
    final `grep` audit confirms no leftover wiring.

**Codex: implement PR-1 only for now.** Do not start PR-2 or PR-3
until PR-1 has landed and `simple_instruction_vert_lsu` passes
end-to-end. The implementation order you proposed (IssueData Rust
field → T1Issue Chisel field → T1.scala source change → existing
Lane/SharedVRF snapshot gates → rerun vertical LSU test) matches
PR-1 and is correct.

### 5.1 Bundles: thread verticalMode through VRFWriteReport

File: `t1/src/Bundles.scala`, around line 398-426. Add:

```scala
// Bon2D vertical LSU: per-instruction snapshot of CSR 0x7c0 at issue. Used
// in SharedVRF to decide whether an in-flight LSU should take the vertical
// scatter/gather path instead of the horizontal path. Live verticalMode IO
// is unsafe for LSU because vle/vse drain across many cycles and the CSR
// can flip mid-drain.
val verticalMode: Bool = Bool()
```

### 5.1b Lane: ensure stores allocate a chaining-record entry

**This step was discovered after the first integration attempt and is
load-bearing for store correctness. Do this before § 5.2.**

The existing report-allocation predicate at `Lane.scala:1310` was
written for compute and load semantics, not store semantics:

```scala
vrf.instructionWriteReport.valid := laneRequest.bits.issueInst &&
  (!instructionFinishAndNotReportByTop || needWaitCrossWrite)
```

with `instructionFinishAndNotReportByTop` (line 1306-1307) defined as
`entranceControl.instructionFinished && !readOnly && (writeCount === 0.U)`.

For a vse:

  * `Decoder.readOnly` is N (false) — vse is not in the y-list at
    `t1/src/decoder/attribute/isReadonly.scala:18-30`. So `!readOnly`
    is true.
  * `writeCount === 0.U` is true — vse writes zero bytes to VRF.
  * `entranceControl.instructionFinished` (`Lane.scala:1147-1153`) is
    high on lanes that don't participate at the current vl/SEW.

So `instructionFinishAndNotReportByTop` evaluates true on at least one
lane for any vse, suppressing the report on that lane. That lane's
`chainingRecord` then has no entry for the vse, `isLSUInst(idx)`
returns 0, and the SharedVRF gate's per-source `Mux` falls through to
**live `verticalMode`**. A trailing `csrw 0x7c0, zero` in the kernel
(the `R3.5` "restore to clean state" pattern from
`2d_fabric_handoff.md`) drops live IO mid-drain, the vertical read
path disengages on that lane, and the store gathers horizontal layout
instead of the transpose. Symptom: V-load + H-store passes,
H-load + V-store and V-load + V-store store the horizontal layout.
This is exactly the asymmetry seen in the first integration's
`simple_instruction_vert_lsu` run log.

**Fix:** force the report for any LSU instruction. The comment on
line 1309 already states this is the intent ("LSU instruction will be
report to VRF"); the code just didn't enforce it for stores. Add
`laneRequest.bits.loadStore` as an OR clause:

```scala
// before: (!instructionFinishAndNotReportByTop || needWaitCrossWrite)
// after:  (laneRequest.bits.loadStore ||
//          !instructionFinishAndNotReportByTop ||
//          needWaitCrossWrite)
vrf.instructionWriteReport.valid := laneRequest.bits.issueInst &&
  (laneRequest.bits.loadStore ||
   !instructionFinishAndNotReportByTop ||
   needWaitCrossWrite)
```

This makes the report fire on every lane for every LSU, giving
SharedVRF a chaining-record entry to recover the per-instruction
verticalMode snapshot from. Loads were not affected by the original
bug because their `writeCount > 0` already kept the report alive.

#### Post-fix verification (do not skip)

  1. **Release path holds.** A store-only chaining record is released
     when `stFinish` flips. `Lane.scala:1338-1340` wires
     `stFinish := !loadStore` (starts low for stores) and
     `wWriteQueueClear := !(loadStore && !store)` (true for stores —
     they don't write the LSU write queue). The release relies on
     `lsuLastReport` (or equivalent) flipping `stFinish` to true.
     Smoke test: run § 4.3a in a tight loop (e.g. ≥ 16 back-to-back
     vse-vert with CSR flips between each). If records leak, the
     loop stalls when chaining slots fill up. If you see this, the
     release path is the next bug — debug `lsuLastReport`'s
     instIndex routing, do not work around by widening
     `chainingSize`.

  2. **Horizontal regression.** The fix allocates chaining records
     for stores that previously had none. This means a subsequent
     instruction that reads the store's vd register now correctly
     waits on the store. Re-run benchmark_vadd 1-5/8 — they should
     still pass with **zero** difftest divergence and at most a
     small (≤ few cycles per iter) cycle-count delta on tests like
     `benchmark_vadd 2` (back-to-back vadd). If a horizontal test
     breaks correctness, the release path is leaking.

  3. **Slot capacity.** `chainingSize + 1` slots per lane. Watch the
     run log for any `assert(freeRecord.orR)` firing — that's the
     overflow indicator. If it fires only on vert-LSU stress tests
     (not on horizontal regressions), the fix exposes a pre-existing
     under-sized `chainingSize` rather than a new bug; raise
     `chainingSize` only after confirming via waveform that records
     are released promptly.

This fix aligns the code with its existing comment ("LSU instruction
will be report to VRF") and is arguably a latent-bug fix even outside
this work — without it, store-after-store WAW hazards on the same vd
across lanes could already race in horizontal mode, just less
visibly than the vert-LSU symptom that exposed it.

### 5.2 Lane: populate the new field

> **REVISED (2026-05-04).** The original § 5.2 claimed that
> `csrInterface.verticalMode` at the cycle of
> `instructionWriteReport.valid` rise was a correct issue-time
> snapshot. That was wrong — see the § 2.1 revision. Under the
> new wiring (§ 2.1a), `csrInterface.verticalMode` is now driven
> from `requestReg.bits.issue.verticalMode` via
> `requestRegCSR.verticalMode`, which is the t1emu-supplied
> per-instruction snapshot in IssueData. So the *line* of code is
> the same, but its meaning has shifted: it is no longer "live IO
> at this cycle" but "stable per-instruction issue-bundle value".

File: `t1/src/Lane.scala`, in the block at lines 1310-1343 that fills
`vrf.instructionWriteReport.bits.*`. Add (next to the `.ls`/`.st` lines
~1328-1329):

```scala
// Bon2D vertical LSU: per-instruction snapshot of CSR 0x7c0.
// csrInterface.verticalMode is sourced from requestReg.bits.issue.verticalMode
// (the IssueData payload field that t1emu populates from its CSR mirror at
// the moment Spike commits this instruction). The value is stable for the
// whole instruction's lifetime, so it survives any number of subsequent
// csrw flips while the LSU drains. See LSU_vertical_mode_handoff.md § 2.1a.
vrf.instructionWriteReport.bits.verticalMode := csrInterface.verticalMode
```

Note: read directly from `csrInterface` (the local val at
`Lane.scala:454`), not via `laneRequest.bits.csrInterface` — they
alias the same wire here. With the § 2.1a wiring, this value is
already a stable per-instruction snapshot regardless of the cycle
you read it on (within the instruction's window), so there is no
"sample edge" sensitivity any more.

### 5.3 SharedVRF: helper + five gate replacements

File: `t1/src/vrf/SharedVRF.scala`.

Add a sibling helper next to `isLSUInst` (~line 230):

```scala
// Per-instruction verticalMode: the CSR snapshot latched into the chaining
// record at issue time. Use this in LSU gating instead of the live
// verticalMode IO, because vle/vse drain across many cycles.
def instVerticalMode(instIdx: UInt): Bool =
  chainingRecord
    .map(r => r.valid && (r.bits.instIndex === instIdx) && r.bits.verticalMode)
    .reduce(_ || _)
```

Then replace the five sites. The pattern is the same at each: today the
gate is `verticalMode && !isLSUInst(idx)` — live IO for non-LSU, hard
zero for LSU. We change it to a per-source selector:

  * non-LSU: live `verticalMode` (unchanged).
  * LSU: `instVerticalMode(idx)` (snapshot only — no `verticalMode &&`).

The `Mux(isLSUInst(idx), instVerticalMode(idx), verticalMode)` form is
the canonical phrasing. Critically, when `isLSUInst(idx)` is true the
expression is *purely* `instVerticalMode(idx)` — there is no live-IO
factor in the LSU branch. § 2.4 explains why; § 4.3a is the regression
that fails if you re-introduce a live-IO factor.

  1. `SharedVRF.scala:362`:
     ```scala
     val isWideVertReq_i = !v.bits.narrowVertical &&
       Mux(isLSUInst(v.bits.instructionIndex),
           instVerticalMode(v.bits.instructionIndex),
           verticalMode)
     ```

  2. `SharedVRF.scala:416-422` (`firstReadFromLSU` / `useVerticalRead`):
     ```scala
     val firstReadFromLSU: Bool = isLSUInst(firstReadReq.instructionIndex)
     val firstReadVertEffective: Bool =
       Mux(firstReadFromLSU,
           instVerticalMode(firstReadReq.instructionIndex),
           verticalMode)
     val useVerticalRead: Bool = firstReadVertEffective && !anyNarrowReq
     ```

  3. `SharedVRF.scala:467-468` (`writeWouldScatter`):
     ```scala
     val writeWouldScatter: Bool =
       Mux(isLSUInst(write.bits.instructionIndex),
           instVerticalMode(write.bits.instructionIndex),
           verticalMode)
     ```

  4. `SharedVRF.scala:488-489` (`useVerticalWrite`):
     ```scala
     val writeFromLSU: Bool = isLSUInst(writePipe.bits.instructionIndex)
     val useVerticalWrite: Bool =
       Mux(writeFromLSU,
           instVerticalMode(writePipe.bits.instructionIndex),
           verticalMode)
     ```

  5. `SharedVRF.scala:600` (`isWideVerticalReq` in r.ready mux):
     ```scala
     val isWideVerticalReq = !r.bits.narrowVertical && firstReadVertEffective
     ```
     (Reuses `firstReadVertEffective` from site 2. The original code
     had `verticalMode && !firstReadFromLSU` here; both terms collapse
     into the same `Mux` so the per-port loop just consults the shared
     wire.)

After this step the design should re-elaborate cleanly. Run a horizontal
regression (§ 4.1) before moving on. Do not attempt vertical LSU yet —
the difftest fence is not in place.

#### Forbidden phrasings (Codex review checklist)

If `git diff` of `SharedVRF.scala` matches any of these patterns, the
edit is wrong even if individual tests pass:

  * `verticalMode && (... || instVerticalMode(...))` — outer live-IO
    AND term. Bug: collapses to 0 when live IO drops mid-LSU-drain.
  * `verticalMode && !isLSUInst(...)` left in place anywhere that also
    handles LSU. Bug: LSU branch is unreachable, snapshot is dead code.
  * `verticalMode && instVerticalMode(...)` — both factors present in
    the LSU branch. Bug: same as the first item, just spelled
    differently.
  * Any read of `verticalMode` (the IO) inside an `if (...lsu...)`
    Scala-level branch or a `when (isLSUInst(...))` Chisel branch.
    The LSU branch must be `verticalMode`-IO-free.

### 5.4 Difftest fence

Files (verified in current tree):

  * `difftest/spike_rs/src/spike_event.rs` — `pub struct SpikeEvent`
    at line 62, `impl SpikeEvent { pub fn new(spike: &Spike, ...) }`
    at lines 108-109. **The struct lives here, not in `runner.rs`**.
    `SpikeEvent::new` only sees a `Spike` reference; it has no access
    to the DPI driver's `vertical_mode` mirror. Do not try to set the
    skip flag here.
  * `difftest/dpi_t1emu/src/drive.rs` — `Driver` carries
    `vertical_mode: bool` (line 200), updated by
    `update_vertical_mode_from_csr` (line 208). The driver pushes
    SpikeEvents into `spike_runner.commit_queue` from `step()`
    (line 324), specifically at line 338
    (`self.spike_runner.commit_queue.push_front(se.clone())`).
    Line 336 (`self.update_vertical_mode_from_csr(&se)`) runs
    immediately before that push — so by line 338 the mirror is
    already up to date. **This is where the flag must be set.**
  * `difftest/t1-sim-checker/src/t1emu/json_events.rs` — both
    `peek_vrf_write` (line 187, byte-equality at lines 226-241) and
    `peek_memory_write` (line 269, byte-equality at line 296) need
    fences. **Both call sites do per-byte assertions**; do not skip
    just one.

Concrete steps:

  a. Add `pub skip_diff: bool` to `SpikeEvent` in `spike_event.rs`
     (struct at line 62). Default to `false` in `SpikeEvent::new`
     (around line 109) so existing code paths are unaffected.

  b. In `drive.rs::step()`, set the flag on the `se` between the CSR
     mirror update (line 336) and the push to the commit queue
     (line 338). Use the existing `is_vload()` / `is_vstore()` helpers
     (already used by `issue_instruction()` at line 390 to bump
     `vector_lsu_count`). Sketch:

     ```rust
     loop {
       let mut se = self.spike_runner.spike_step();
       self.update_vertical_mode_from_csr(&se);
       // Bon2D vertical LSU: latch the snapshot of self.vertical_mode at
       // the moment Spike commits the vle/vse, so that downstream byte-
       // equality checks know to skip this instruction. Mirrors the
       // RTL-side per-instruction snapshot in chainingRecord.
       if self.vertical_mode && (se.is_vload() || se.is_vstore()) {
         se.skip_diff = true;
       }
       if se.is_v() || se.is_vfence() || se.is_load() || se.is_store() {
         self.spike_runner.commit_queue.push_front(se.clone());
         return se;
       }
     }
     ```

     Note `let mut se = ...` (the existing code has `let se = ...` at
     line 335). The mutability is required because we set the flag
     before clone-and-push.

  c. **VRF-side fence** in `peek_vrf_write`
     (`json_events.rs:187-252`). After the `commit_queue.iter_mut().rev().find(...)`
     lookup at lines 192-193, before the byte-compare block at lines
     222-252, branch on `se.skip_diff`. Preserve the
     `unretired_writes` / `retired_writes` / `retire_issue` accounting
     at lines 206-220 — that drives the commit-queue retire — but skip
     the `mask.iter().enumerate().filter(...).for_each(...)`
     byte-equality block at 222-252:

     ```rust
     if se.skip_diff {
         debug!("[{cycle}] VrfWrite: skipping diff (vert-LSU): issue_idx={}", vrf_write.issue_idx);
         // still run lines 206-220 retirement accounting before this branch,
         // OR refactor so this branch returns before 222 but after 220.
         return Ok(());
     }
     ```

  d. **Memory-side fence** in `peek_memory_write`
     (`json_events.rs:269-302`). Same pattern: after the
     `commit_queue.iter_mut().find(|se| se.lsu_idx == lsu_idx)` at
     line 276, **preserve `mem_write.num_completed_writes += 1`**
     (line 295 — incrementing it is what advances Spike's per-byte
     write cursor), but skip the `assert_eq!` at line 296 when
     `se.skip_diff`:

     ```rust
     mask.iter().enumerate().filter(|&(_, &mask)| mask).for_each(|(offset, _)| {
         let byte_addr = base_addr + offset as u32;
         let data_byte = *data.get(offset).unwrap_or(&0);
         let mem_write = se.mem_access_record.all_writes.get_mut(&byte_addr)
             .unwrap_or_else(|| panic!("[{cycle}] MemoryWrite: cannot find mem write of byte_addr {byte_addr:#x}"));
         let single_mem_write_val = mem_write.writes[mem_write.num_completed_writes].val;
         mem_write.num_completed_writes += 1;
         if se.skip_diff {
             debug!("[{cycle}] MemoryWrite: skipping diff (vert-LSU): byte_addr={byte_addr:#x}");
         } else {
             assert_eq!(single_mem_write_val, data_byte,
                 "[{cycle}] expect mem write of byte {single_mem_write_val:#02x}, actual byte {data_byte:#02x} \
                  (byte_addr={byte_addr:#x}, pc = {:#x}, disasm = {})",
                 se.pc, se.disasm);
         }
     });
     ```

     **Do not** skip the `mem_write.num_completed_writes += 1` increment
     or the `mem_access_record.all_writes.get_mut(&byte_addr)` lookup.
     Skipping the increment causes a desync where the next legitimate
     horizontal write reads a stale write-cursor and fails the assert
     for the wrong reason.

  e. **Horizontal LSU stays strict.** `skip_diff` defaults false on
     SpikeEvent. The fence path is the *only* `if se.skip_diff` branch
     in steps (c) and (d); the fall-through retains the existing
     `assert_eq!` calls. Verify by running benchmark_vadd 1-5/8 (no
     CSR=1 around any LSU) and confirming zero divergence reports.

### 5.5 Tests

Add `tests/vision_task/simple_instruction_vert_lsu/simple_instruction_vert_lsu.c`
following § 4.2/§ 4.3 above. Use the `tests/vision_task/simple_instruction_gather/`
or `tests/vision_task/simple_instruction_vert_hori/` directory as the
template for `Cargo.toml`/`run-test.sh`/build glue — those work
end-to-end on the current config.

---

## 6. Pitfalls & things to be aware of

  * **Auto-vectorisation in init code.** Re-read § 3.1 of
    `2d_fabric_handoff.md`. Use the `volatile int8_t *p = ...` pattern
    in any C-side reference initialisation, otherwise -O2 produces
    LMUL=8 vid+vse that writes garbage on the 2D fabric. The
    `init_grids` helper in `benchmark_vadd.c:335-346` is the canonical
    template.

  * **Naked + no-call kernels.** Re-read § 3.2 of `2d_fabric_handoff.md`.
    Every vector kernel must be `__attribute__((naked, noinline))` with a
    single inline-asm block and no function calls inside. After build,
    `<test>.s` must contain *no* `vs1r.v` / `vl1r.v` / `csrr.*vlenb`
    inside the kernel body. If it does, the compiler decided to spill
    and you will get an illegal-instruction trap on a clobbered RA.

  * **LMUL stays at 4.** Vertical LSU does **not** unlock LMUL=8.
    `zvl256b` + this config means LMUL=4 already saturates one image-row
    per register group (vl=128 at SEW=8). Going wider breaks the
    diagonal scatter math in `SharedVRF.scala`.

  * **Mask v0 in vertical mode is unverified.** § 4.2 of the 2D handoff
    flags this as a known fuzziness. Do **not** add masked vle/vse to the
    initial test set. A masked vle-vert may interact with the
    scatter mask (`writePipe.bits.mask` at `SharedVRF.scala:518-522`) in
    ways nobody has characterised. Get unmasked vert-LSU working first;
    mask interaction is a follow-up.

  * **Round-trip cancellation is now programmer-controlled.** vle-vert
    followed by vse-vert is a no-op transpose-of-transpose. This is
    *expected* and verified by § 4.3, not a bug. Programmers reading
    only the `2d_fabric_handoff.md` may be surprised by this — once
    this work lands, update §§ 1, 2, and 4.3 of that doc to describe
    the new semantics.

  * **CSR timing across an LSU — the most subtle bug class in this
    work.** Re-read § 2.1 / § 2.1a in full before debugging any
    vert-LSU correctness issue. **§ 2.1 / § 2.1a / § 5.2 are
    authoritative — disregard any older guidance suggesting an
    RTL-side sample edge is safe.** The constraint is: do not sample
    `io.verticalMode` (the live IO) on any RTL-side edge. Both early
    edges (`io.issue.fire`, `requestRegDequeueFire`,
    `replayFSM.firstRowFire`) and late edges
    (`laneRequest.bits.issueInst`, the live IO at LSU drain time)
    are racy in different ways:

      * Early edges: stale by one vector issue — disproved by
        `simple_instruction_gather_scalar` TEST2.
      * Late edges: t1emu may have already advanced the mirror past
        this instruction to the next csrw — disproved by
        `simple_instruction_vert_lsu` TEST2 / TEST4
        (waveform 27610-27612).

    The only correct path is: t1emu populates `IssueData.vertical_mode`
    from its CSR mirror at the moment of issue construction (§ 2.1a
    step 2), Rocket populates `T1Issue.verticalMode` from
    `csr.io.csrToVector.get.verticalMode` at the same point it bundles
    `vtype`/`vl`/`vstart`/`vcsr` (§ 2.1c step 2), RTL latches the
    payload into `requestReg` at `io.issue.fire`, and every consumer
    reads `requestRegCSR.verticalMode`. The chaining record's
    `verticalMode` field is captured from `csrInterface.verticalMode`
    at `instructionWriteReport.valid` rise — but with the new wiring
    that local val is itself sourced from
    `requestReg.bits.issue.verticalMode`, so the value is stable
    regardless of which cycle within the instruction window the
    capture happens.

    The fastest debug if you suspect a timing regression: run § 4.3a
    (CSR-flip-during-drain) in waveform mode and check three things:
      1. `requestReg.bits.issue.verticalMode` is stable for the whole
         instruction (only changes at `io.issue.fire` for the *next*
         instruction). If it tracks anything at sub-instruction
         granularity, somebody is driving it from a live wire instead
         of from the IssueData payload.
      2. `chainingRecord(<slot>).bits.verticalMode` stays high for
         the whole drain. If it doesn't, the chaining-record
         `verticalMode` field is combinationally driven instead of
         registered through the `chainingRecord(*) := initRecord`
         assignment at `SharedVRF.scala:619-625`.
      3. `useVerticalWrite` stays high for the whole drain and
         tracks the snapshot, not any live signal. If it drops
         mid-drain, the gate formula has an outer live-IO factor —
         see § 2.4's "Forbidden phrasings" list and § 5.3.

  * **`io.verticalMode` IO and any live-fed copies of it must be
    deleted, not "kept around just in case."** § 2.1a step 7 calls
    for removing the IO from `T1.scala`, the corresponding DPI call
    `t1_cosim_get_vertical_mode` from `dpi.rs:268`, and the
    Rocket-side side-channel from `RocketCore.scala:1573` /
    `T1RocketTile.scala:528` / the Rocket→T1 IO bundle field at
    `Bundle.scala:1587`. The reason for outright deletion (rather
    than leave-and-deprecate) is that having both a live IO and an
    issue-bundle field invites future readers to "improve" the
    design by going back to the live IO. After deletion,
    `grep -n verticalMode` across the whole repo should show only
    `requestReg.bits.issue.verticalMode`, `requestRegCSR.verticalMode`,
    `T1Issue.verticalMode`, `IssueData.vertical_mode`, and the
    chaining-record / VRFWriteReport field. Anything else is a smell.

  * **Spike's shadow memory diverges in vert-store regions.** After a
    vse-vert, real memory holds Mᵀ but Spike's `shadow_mem` (and
    `mem_access_record`) holds M (because Spike modelled the vse as
    plain row-major). If a *later* Spike-checked instruction reads
    from that memory region — scalar load, vle-horizontal, AXI read
    by another agent — the read value will diverge from RTL even
    though both sides are now behaving correctly per their own model.
    Keep vert-LSU output buffers off-limits for any subsequent
    Spike-checked read until you've cleared/overwritten them.
    The C-side validation in § 4.2 reads memory with scalar loads,
    which means *those scalar loads are themselves checked by
    Spike* — and they will fault. Either:

      * make the C-side check happen after the test driver has
        explicitly memset'd the buffer (Spike sees the memset and
        overwrites its shadow); or
      * read the buffer once with a vle-horizontal so RTL produces a
        VrfWrite event marked `skip_diff` (the `vector_lsu_count`
        was bumped at vse-vert time, so the matching commit will
        also be skip-flagged if you make is_vload/is_vstore both
        skip-eligible).

    The path of least resistance: the C-side validator reads with
    plain scalar loads from a memory region the kernel just wrote to
    via vse-vert, *before* any other Spike-checked instruction
    touches it, and accepts that those scalar loads will diverge in
    Spike. Skipping scalar-load diff is broader than vector-LSU
    skip; an alternative is to land vse-vert tests in a separate
    binary that exits immediately after the kernel, so divergent
    scalar loads never run.

    **Cleanest workaround for the v1 test set:** after the kernel,
    do an explicit `volatile` re-write of the buffer from the C
    driver (which Spike sees and shadow-overwrites with the new
    pattern), then C-side compare the *previous* buffer values
    against expectations using values cached in registers. Or just
    accept the divergence on the validator's scalar reads, and run
    the vert-LSU tests with difftest disabled at the assertion
    layer (skip_diff is per-event; not setting it on scalar loads
    means scalar reads of a vert-touched buffer will still panic).
    Codex: pick whichever is cleanest given how the existing
    test harness in `tests/vision_task/` runs C drivers.

  * **Spike custom CSR registration.** Already wired
    (`difftest/spike_rs/src/runner.rs:97`,
    `proc_register_basic_csr(0x7c0, 0)`). Don't remove or re-register.

  * **Don't disable difftest globally.** It's tempting to add a
    `--no-difftest` flag and call it a day. The fence per § 3 is
    smaller, keeps horizontal LSU strictly checked, and isolates the
    "vert-LSU is on the honour system" exception to exactly the
    instructions where it must be.

---

## 7. Acceptance checklist

Before declaring the work done:

  * [ ] `t1/src/Lane.scala` `instructionWriteReport.valid` predicate
        includes `laneRequest.bits.loadStore` as an OR clause (§ 5.1b)
        so stores allocate a chaining-record entry on every lane.
        Without this, `isLSUInst(idx)` returns 0 for vse on
        non-participating lanes and the gate falls through to live IO.
  * [ ] `difftest/dpi_t1emu/src/dpi.rs` `IssueData` struct has a
        `vertical_mode: u32` field, populated by
        `Driver::issue_instruction` from `self.vertical_mode` at
        the moment of issue construction (§ 2.1a step 1-2).
  * [ ] `t1/src/Bundles.scala` `T1Issue` bundle has a matching
        `verticalMode: Bool` field with bit width that matches
        IssueData layout (§ 2.1a step 3).
  * [ ] `t1/src/T1.scala:572-573` sources
        `requestRegCSR.verticalMode` from
        `requestReg.bits.issue.verticalMode`, NOT from
        `io.verticalMode` (§ 2.1a step 4).
  * [ ] `t1/src/T1.scala:792` drives `sharedVRF2D.foreach(_.verticalMode := ...)`
        from `requestRegCSR.verticalMode` — single source for both
        LSU and non-LSU paths (§ 2.1a step 5).
  * [ ] `io.verticalMode` IO and `t1_cosim_get_vertical_mode` DPI
        call removed — no remaining consumers
        (`grep -n verticalMode` should show only
        `requestReg.bits.issue.verticalMode` and downstream uses)
        (§ 2.1a step 7).
  * [ ] **t1rocketemu side** (§ 2.1c): `T1Issue` in
        `rocketv/src/Bundle.scala:1590` has `verticalMode: Bool`,
        `RocketCore.scala:1565+` assigns it from
        `csr.io.csrToVector.get.verticalMode` next to vtype/vl,
        side-channel `t1.verticalMode := ...` at
        `RocketCore.scala:1573` is deleted, and
        `t1rocket/src/T1RocketTile.scala:528` is deleted. Required
        for parity — without it, t1rocketemu hits the same race as
        t1emu did (just with a real Rocket CSR file instead of a
        Spike mirror).
  * [ ] `t1/src/Bundles.scala` `VRFWriteReport` has a `verticalMode` field.
  * [ ] `t1/src/Lane.scala` writes `csrInterface.verticalMode` into
        `instructionWriteReport.bits.verticalMode`.
  * [ ] `t1/src/vrf/SharedVRF.scala` has an `instVerticalMode` helper and
        all five gate sites (lines ~362, ~422, ~467-468, ~488-489, ~600
        in the pre-edit numbering) consult it.
  * [ ] `skip_diff: bool` field added to `SpikeEvent` in
        `difftest/spike_rs/src/spike_event.rs:62` (default false in
        `new()`).
  * [ ] `skip_diff` set in `difftest/dpi_t1emu/src/drive.rs::step()`
        (between the `update_vertical_mode_from_csr` call and the
        `commit_queue.push_front` call) when
        `self.vertical_mode && (se.is_vload() || se.is_vstore())`.
  * [ ] Difftest fence active in **both** `peek_vrf_write` (line 187)
        and `peek_memory_write` (line 269) of
        `difftest/t1-sim-checker/src/t1emu/json_events.rs`. The memory
        fence preserves `mem_write.num_completed_writes += 1`
        bookkeeping while skipping the `assert_eq!`.
  * [ ] § 4.1 horizontal regression (`benchmark_vadd` 1-5, 8) passes
        with zero diffs.
  * [ ] `simple_instruction_gather_scalar` (the test that motivated the
        live-IO CSR wiring) still passes — proves the new chaining-record
        latch did not regress the lane-side verticalMode timing.
  * [ ] § 4.2 transpose-via-vert-load and transpose-via-vert-store
        produce in-memory Mᵀ.
  * [ ] § 4.3a CSR-flip-during-drain test passes — proves the chaining
        record holds a stable per-instruction snapshot while live IO
        toggles around the LSU.
  * [ ] § 4.3 round-trip vert-vert cancels (output == input).
  * [ ] § 4.4 R-TEST 7 / 8 / 10 ported and passing.
  * [ ] `<test>.s` for every new naked kernel is free of `vs1r.v` /
        `vl1r.v` / `csrr.*vlenb`.
  * [ ] Run log for each vert-LSU test contains at least one
        "skipping diff (vert-LSU)" debug line — proves the fence wired
        up rather than vacuously absent.
  * [ ] Update `fyp_doc/2d_fabric_handoff.md` § 2 ("Load/store always
        run in horizontal mode" warning) and § 4.3 ("LSU is
        horizontal-only" open issue) to reflect the new semantics.
        Move the resolved item out of § 4 into § 2/3 as documented
        behaviour. Move R-TEST 7/8/10 out of "RESERVED" in
        `benchmark_vadd.c:291-298`.

---

## 8. Out of scope (do not attempt as part of this work)

  * Strided / indexed loads under verticalMode. Stick to unit-stride
    `vle8.v` / `vse8.v`.
  * Segmented (`vlseg`/`vsseg`) loads under verticalMode.
  * SEW ≠ 8. The diagonal scatter math is parameterised by `cByteBits`
    and friends, but no test in the repo exercises a non-SEW=8
    configuration. Keep the same SEW=8/LMUL=4/vl=128 invariants the 2D
    handoff documents.
  * Mask-driven vle/vse under verticalMode (§ 6 above).
  * Teaching Spike about the diagonal scatter. The fence is the
    accepted strategy.

If any of those become necessary to make the primitive tests pass, stop
and flag it — it means an assumption in this handoff is wrong and we
need to revisit § 2 before continuing.
