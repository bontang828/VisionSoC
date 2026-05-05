# Driver Function Spec — what to implement, file by file

**Audience:** future programmer or AI agent (Codex, Claude) implementing
the ARM-side driver. This doc is the **per-function checklist** that
sits alongside `driver_implementation_handoff.md`. Use it to:

  * Track progress while implementing (check off `[x]` as functions
    land and pass their unit tests).
  * Avoid forgetting helpers (decoder, IRQ re-arm, sysfs PA reader)
    that the prose doc hides inside example code.
  * Decide implementation order so each step is testable on its own.

The handoff doc has the *prose* and *example code*; this doc has the
*list of functions*, their *signatures*, and what each one does. If
the two ever conflict, the handoff doc wins on semantics, this one on
naming/scope.

---

## 0. Suggested implementation order

Implement in this order. Each step is gated by the previous one
passing its smoke test on the Kria.

  1. § 1 `libt1_regs.h` — constants only. ~10 min.
  2. § 2 `libt1.h` — public API skeleton (declarations + structs).
     ~30 min. No code yet.
  3. § 3.1 `libt1.c` lifecycle (`t1_init`, `t1_close`) — get
     `/dev/uio0` open and mmap'd. Verify with a 5-line test
     program that just opens + reads `PERF_CYCLES_LO`.
  4. § 3.5 `libt1.c` perf helpers — `t1_perf_start/stop/t1_cycles`.
     Verify with `smoke.c` (§ 8.1).
  5. § 3.4 `libt1.c` retire-side helpers (`t1_drain_rd`,
     `t1_wait_mem`) — these come before `t1_issue` because issue
     uses them.
  6. § 3.3 `libt1.c` `t1_issue` + `is_lsu_instruction` decoder —
     the centrepiece. Verify by issuing a single `vsetvli` (no
     memory effect) and confirming `issue_busy` clears.
  7. § 3.6 `libt1.c` udmabuf allocator — depends on `u-dma-buf`
     module being loaded. Verify with `ddr_roundtrip.c` (§ 8.2),
     which exercises both allocator and `t1_issue`.
  8. § 3.7 `libt1.c` DMA helpers + `/dev/uio1` — verify with a
     standalone udmabuf-to-udmabuf copy test (§ 8.4).
  9. § 4 `build_kernel.sh` — bash, ~30 min. Verify by assembling
     `simple_instruction_asm.c`'s `_start` and dumping the
     `kernel[]` array.
 10. § 5 `weights_format.h` — types only. ~15 min.
 11. § 8.3 `port_grid_vadd.c` — first kernel-via-byte-array test.
 12. § 6 main program (`visionsoc_main/`) — depends on everything
     above + camera + display.

Steps 1–8 can ship without the FPGA changes from
`fpga_implementation_handoff.md` § 5 (the streaming pipeline). § 12
needs camera + HDMI hardware up.

---

## 1. `vision_software/libt1/libt1_regs.h`

Constants only — no functions. Mirror exactly
`fpga/wrapper/t1_axi_lite_wrapper.sv` § 1 register map (ranges
0x00–0x54 after the wrapper extensions land). Source-of-truth in
prose: `driver_implementation_handoff.md` § 2.

Define:

  * [ ] `T1_REG_*` — byte offsets for all 22 wrapper registers.
  * [ ] `T1_CTRL_*` — CTRL bits (ISSUE_START, ISSUE_READY, ISSUE_BUSY).
  * [ ] `T1_IRQ_*` — IRQ_EN/IRQ_STATUS bits (RD, CSR, MEM).
  * [ ] `DMA_REG_*` — Xilinx AXI DMA (PG021) subset.
  * [ ] `DMA_CR_*`, `DMA_SR_*` — DMA control/status bits.

**Sanity check on every change:** when this file is edited, also
confirm the matching offset in
`fpga/wrapper/t1_axi_lite_wrapper.sv` lines 11–32 (the comment
header). Drift = silent corruption.

---

## 2. `vision_software/libt1/libt1.h`

Public API. All identifiers prefixed `t1_`. Source-of-truth code
sketch in `driver_implementation_handoff.md` § 3.

### Types

  * [ ] `struct t1_op` — issue payload (instruction + rs1/rs2 +
        vtype/vl/vstart/vcsr + vertical_mode). Caller fills, libt1
        reads.
  * [ ] `struct t1_buf` — udmabuf result (va, pa, size, internal fd).
        Caller passes by pointer; libt1 fills.

### Lifecycle

  * [ ] `int  t1_init(void);`
        Opens `/dev/uio0` + `/dev/uio1`, mmaps both register pages,
        enables MEM IRQ. Returns 0 on success, -1 with errno set.
        Idempotent if called twice (returns 0 if already open).
  * [ ] `void t1_close(void);`
        Munmap, close fds, reset module state.

### Issue path

  * [ ] `int  t1_issue(const struct t1_op *op);`
        Single-instruction issue with blocking wait until retire.
        Returns 0 on success, -1 on error (issue_ready never asserted
        within timeout, or IRQ-wait failed).
  * [ ] `int  t1_issue_kernel(const uint32_t *kernel, size_t n_words,
                              const struct t1_op *op_template);`
        Batch issue from a `kernel[]` array (output of build_kernel.sh).
        Loops `t1_issue` over the array; rs1/rs2/vertical_mode taken
        from `op_template` (no per-instruction override).

### Retire path

  * [ ] `int  t1_drain_rd(uint32_t *data, uint8_t *rd_addr, bool *is_fp);`
        Pop one entry from the rd FIFO if non-empty. Returns 1 if
        popped, 0 if empty. `data`/`rd_addr`/`is_fp` filled on pop.
        For ops like `vmv.x.s` that produce scalar results.
  * [ ] `int  t1_drain_csr(uint32_t *vxsat, uint32_t *fflag);`
        Pop one entry from the csr FIFO if non-empty. Returns 1 if
        popped, 0 if empty. Most callers don't need this; provided
        for completeness.
  * [ ] `int  t1_wait_mem(unsigned n_events);`
        Block via `read(uio_t1_fd, ...)` until `MEM_COUNT` ≥ n_events
        cumulative since last reset, then W1C-decrement. Used by
        `t1_issue` for LSU ops.

### Perf

  * [ ] `uint32_t t1_perf_start(uint8_t tag);`
        Writes `PERF_TAG = tag` (must be nonzero, otherwise it's a
        STOP). Returns the LO half of the cycle counter at start
        (informational; the canonical delta comes from `t1_perf_stop`).
  * [ ] `uint32_t t1_perf_stop(void);`
        Writes `PERF_TAG = 0`, returns `PERF_DELTA`. Wrapper handles
        the start→stop subtraction.
  * [ ] `uint64_t t1_cycles(void);`
        Free-running 64-bit `pl_clk0` cycle counter. Two-read,
        retry-if-overflow pattern (HI/LO/HI). Used for fps measurement.

### DMA

  * [ ] `int t1_dma_mm2s_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);`
        `dst_pa` is unused by AXI DMA mm2s in simple mode — kept in
        the API for symmetry. Returns 0 on kick.
  * [ ] `int t1_dma_s2mm_async(uint32_t src_pa, uint32_t dst_pa, uint32_t len);`
        Symmetric — `src_pa` unused for s2mm.
  * [ ] `int t1_dma_mm2s_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);`
        `t1_dma_mm2s_async` + `t1_dma_wait`. Convenience.
  * [ ] `int t1_dma_s2mm_sync(uint32_t src_pa, uint32_t dst_pa, uint32_t len);`
        `t1_dma_s2mm_async` + `t1_dma_wait`. Convenience.
  * [ ] `int t1_dma_wait(void);`
        Block on `/dev/uio1` IRQ; clear status; re-arm. Returns 0 on
        clean wake, -1 on error.

### Allocation

  * [ ] `int  t1_buf_alloc(struct t1_buf *buf, size_t size);`
        Open the next free `/dev/udmabufN`, mmap, read PA from sysfs
        `/sys/class/u-dma-buf/udmabufN/phys_addr`. Caller must have
        loaded `u-dma-buf` module with the right pre-sized instances
        (see `driver_implementation_handoff.md` § 4.6 note).
  * [ ] `void t1_buf_free(struct t1_buf *buf);`
        Munmap, close fd, zero out the struct.

### Helpers

  * [ ] `uint32_t t1_va_to_pa(const void *va);`
        Walk `/proc/self/pagemap` to translate a VA to a PA. Falls
        back path — prefer `t1_buf_alloc` which gives PA directly.
        Useful for debug or for buffers obtained outside libt1.
  * [ ] `void     t1_set_vertical_mode_raw(int v);`
        Direct write to `T1_REG_VERTICAL_MODE`. For tests that probe
        the wrapper without running an instruction.
  * [ ] `int      t1_get_vertical_mode_raw(void);`
        Read of the same. Mirror of the above for round-trip tests.

---

## 3. `vision_software/libt1/libt1.c`

Implementation. Keep all module state at file scope, prefixed `g_`.
No public entry points beyond what `libt1.h` declares.

### 3.1 Module state (file-scope)

  * [ ] `static int g_uio_t1_fd, g_uio_dma_fd;`
  * [ ] `static volatile uint32_t *g_t1_regs, *g_dma_regs;`
  * [ ] `static int g_initialised;`
  * [ ] `static int g_next_udmabuf_idx;` — for round-robin alloc.

### 3.2 Private MMIO helpers

Pick one and use it everywhere — don't mix raw `g_t1_regs[X/4] = v`
with helper macros.

  * [ ] `static inline uint32_t mmio_rd(volatile uint32_t *base, uint32_t off);`
        Reads `base[off/4]` with a memory barrier (`__atomic_thread_fence`
        or `asm volatile("dmb sy" ::: "memory")` on aarch64).
  * [ ] `static inline void     mmio_wr(volatile uint32_t *base, uint32_t off, uint32_t v);`

### 3.3 `t1_init` / `t1_close`

  * [ ] `t1_init`:
        - early-return 0 if `g_initialised`.
        - `open("/dev/uio0", O_RDWR | O_CLOEXEC)` → `g_uio_t1_fd`.
        - `mmap(NULL, 4096, PROT_RW, MAP_SHARED, fd, 0)` → `g_t1_regs`.
        - same for `/dev/uio1` → `g_uio_dma_fd`, `g_dma_regs`.
        - `mmio_wr(g_t1_regs, T1_REG_IRQ_EN, T1_IRQ_MEM)` to arm mem IRQ.
        - On any failure, undo the partial init (close opened fds,
          munmap mapped regions) and return -1.
        - Set `g_initialised = 1`.
  * [ ] `t1_close`:
        - munmap both regions, close both fds, zero state.

### 3.4 Retire-side helpers

Implement these BEFORE `t1_issue` because issue calls into them.

  * [ ] `t1_drain_rd`:
        - read `T1_REG_RD_FIFO_STS`, return 0 if `count==0`.
        - read `T1_REG_RD_POP_DATA` → `*data`. The read pops the FIFO
          (wrapper side-effect).
        - read `T1_REG_RD_POP_META` → `*rd_addr` (low 5 bits),
          `*is_fp` (bit 5).
        - return 1.
  * [ ] `t1_drain_csr`:
        - mirror of the above against `T1_REG_CSR_FIFO_STS`,
          `T1_REG_CSR_POP`, `T1_REG_CSR_FFLAG`.
  * [ ] `t1_wait_mem`:
        - read `T1_REG_MEM_COUNT`, if already ≥ `n_events`:
          W1C-decrement `n_events` times via `mmio_wr(g_t1_regs,
          T1_REG_MEM_COUNT, 1)`, return 0.
        - else `read(g_uio_t1_fd, &buf, 4)` to block on IRQ; on wake,
          re-arm by `write(fd, &one, 4)`. Loop until count condition
          met.
        - timeout — pick a sane bound (e.g. 1 s) using `select` /
          `poll`; return -1 with `errno = ETIMEDOUT` on timeout.

### 3.5 Perf helpers

  * [ ] `t1_perf_start`:
        - `mmio_wr(g_t1_regs, T1_REG_PERF_TAG, tag)`.
        - return `mmio_rd(g_t1_regs, T1_REG_PERF_CYCLES_LO)`.
        - **Caller responsibility**: pass nonzero tag (else this is a
          STOP from the wrapper's perspective).
  * [ ] `t1_perf_stop`:
        - `mmio_wr(g_t1_regs, T1_REG_PERF_TAG, 0)`.
        - return `mmio_rd(g_t1_regs, T1_REG_PERF_DELTA)`.
  * [ ] `t1_cycles`:
        - read HI, LO, HI.
        - if HI changed between the two HI reads, re-read LO; the
          second HI is authoritative.
        - return `((uint64_t)hi << 32) | lo`.

### 3.6 Issue helpers

  * [ ] `static bool is_lsu_instruction(uint32_t instr);`
        Returns true if `instr & 0x7F` is `0x07` (FP/V load) or `0x27`
        (FP/V store). RVV memory ops use these opcodes.
  * [ ] `t1_issue`:
        - poll `T1_REG_CTRL` until `ISSUE_READY` set (sub-µs typical).
        - write `INSTRUCTION`, `RS1_DATA`, `RS2_DATA`, `VTYPE`, `VL`,
          `VSTART`, `VCSR`, `VERTICAL_MODE` from `op`.
        - snapshot pre-issue `MEM_COUNT`.
        - W1S `CTRL = ISSUE_START` (auto-clears).
        - if `is_lsu_instruction(op->instruction)`:
          `t1_wait_mem(pre_mem + 1)` — but the count is saturating, so
          the snapshot may already have been ≥ pre_mem; safer is "wait
          until count > pre_mem" and W1C the difference.
        - else: poll `CTRL.ISSUE_BUSY` until clear (compute ops have
          no retire IRQ). Bound the poll with a deadline.
        - return 0 on clean retire, -1 on error.
  * [ ] `t1_issue_kernel`:
        - copy `op_template` to a local; loop: set `op.instruction`
          to `kernel[i]`, `t1_issue(&op)`, return -1 on first error.

### 3.7 udmabuf allocator

  * [ ] `t1_buf_alloc`:
        - format `path = /dev/udmabuf<g_next_udmabuf_idx>`.
        - open path RW; mmap `size` bytes.
        - format `sysfs = /sys/class/u-dma-buf/udmabuf<idx>/phys_addr`.
        - `fopen` + `fscanf("0x%llx", ...)` to read PA.
        - fill `buf->{va, pa, size, _udmabuf_fd}`; bump `g_next_udmabuf_idx`.
        - on failure, clean up partial state.
  * [ ] `t1_buf_free`:
        - munmap + close + zero. Don't decrement `g_next_udmabuf_idx`
          (allocator is monotonic; tests are short-lived).

### 3.8 DMA helpers

  * [ ] `t1_dma_mm2s_async`:
        - `mmio_wr(g_dma_regs, DMA_REG_MM2S_CR, DMA_CR_RUN | DMA_CR_IOC_IRQ_EN)`.
        - `mmio_wr(g_dma_regs, DMA_REG_MM2S_SRC_ADDR, src_pa)`.
        - `mmio_wr(g_dma_regs, DMA_REG_MM2S_LENGTH, len)` (this kicks).
  * [ ] `t1_dma_s2mm_async`: mirror with S2MM regs.
  * [ ] `t1_dma_*_sync`: trivial wrappers — async + `t1_dma_wait`.
  * [ ] `t1_dma_wait`:
        - `read(g_uio_dma_fd, &buf, 4)` blocks on IRQ.
        - clear `MM2S_SR.IOC` and `S2MM_SR.IOC` by W1C if needed
          (PG021 — typically reading SR is enough).
        - re-arm by `write(fd, &one, 4)`.
  * [ ] `static int dma_reset(void);` (private)
        - Issue `DMA_CR_RESET` to both MM2S_CR and S2MM_CR; spin on
          reset clearing. Call from `t1_init` if you observe the DMA
          sometimes wedged at boot.

### 3.9 Helpers

  * [ ] `t1_va_to_pa`: open `/proc/self/pagemap`, seek to
        `(va >> 12) * 8`, read 8 bytes; PFN = bits[54:0]; PA =
        `(PFN << 12) | (va & 0xFFF)`. Return 0 on failure.
  * [ ] `t1_set_vertical_mode_raw` / `t1_get_vertical_mode_raw`:
        thin wrappers over `mmio_wr` / `mmio_rd` of
        `T1_REG_VERTICAL_MODE`.

---

## 4. `vision_software/libt1/build_kernel.sh`

Bash, no functions. Source-of-truth in
`driver_implementation_handoff.md` § 6.

  * [ ] Argument parse: `<src.S> <out.h> <kernel_name>`.
  * [ ] Assemble with `riscv64-linux-gnu-as -march=rv32imafc_zvl256b`.
  * [ ] Extract `.text` section to a flat binary with `objcopy`.
  * [ ] Emit a `.h` containing `static const uint32_t <name>[] = { ... };`
        and `static const uint32_t <name>_count = sizeof(<name>)/4;`.
  * [ ] Smoke-test by running on `tests/vision_task/simple_instruction_asm.c`'s
        kernel and visually checking the hex against
        `riscv64-linux-gnu-objdump -d` output.

---

## 5. `vision_software/libt1/weights_format.h`

Types + conventions only. Source-of-truth in
`driver_implementation_handoff.md` § 5.

  * [ ] `struct weights_layer_descriptor` — pa, out_ch, in_ch, kH,
        kW, scale, zero_pt.
  * [ ] Comment block explaining: row-major, contiguous, 128-byte
        aligned, one file per layer copied to a reserved DDR region
        at boot.
  * [ ] (Optional, future) declare a manifest loader prototype, e.g.
        `int weights_load_manifest(const char *path, struct weights_layer_descriptor **out, size_t *n);`
        Implement only when the main program needs it.

No `.c` file required for this header alone — the loader, if any,
goes in the main program.

---

## 6. `vision_software/visionsoc_main/main.c`

Source-of-truth in `driver_implementation_handoff.md` § 9.1. Wires
camera + libt1 + display into a frame loop.

### Constants (file scope)

  * [ ] `BRAM_BASE_PA = 0xB0000000u`
  * [ ] `BRAM_HALF_A = BRAM_BASE_PA + 0x0000`
  * [ ] `BRAM_HALF_B = BRAM_BASE_PA + 0x4000`  (16 KB per half)
  * [ ] `FRAME_BYTES = 128 * 128`

### Functions

  * [ ] `static void die(const char *msg);` — `perror` + `exit(1)`.
  * [ ] `static void usage(const char *argv0);` — stderr help text.
  * [ ] `int main(int argc, char **argv);`
        Initialises t1, camera, display; runs the frame loop:
          1. `camera_dqbuf` — block on next captured frame.
          2. `t1_dma_mm2s_sync` — copy frame to BRAM half.
          3. Build `t1_op` template (vtype/vl/rs1=BRAM half).
          4. `t1_perf_start(1)`, `t1_issue_kernel(grid_vadd, ...)`,
             `t1_perf_stop()`.
          5. `display_dq_for_filling`, `t1_dma_s2mm_sync` to display
             buffer, `display_qbuf`.
          6. `camera_qbuf` to recycle camera buffer.
          7. Flip half index; periodically print fps.
        Cleanup on signal (install `SIGINT` handler that flips a
        `volatile sig_atomic_t g_should_exit`).

### Optional refinements (do these only after baseline 30 fps proven)

  * [ ] Double-buffer the kernel issue: kick DMA for frame N+1
        while T1 processes frame N (move from `_sync` to `_async`).
  * [ ] Pin to a single core (`taskset 0x4`) — note in main's help.

---

## 7. `vision_software/visionsoc_main/camera.{c,h}`

V4L2 capture wrapper. Source-of-truth in
`driver_implementation_handoff.md` § 9.2. Reference implementation
in AMD's `kv260-smartcam` userspace.

### Types

  * [ ] `struct camera` — fd, format (w/h/pixfmt), buffer ring
        (count + per-buffer va/pa/length).
  * [ ] `struct camera_buf` — index, va, pa, length.

### Functions

  * [ ] `int  camera_open(struct camera *cam, const char *dev,
                         int width, int height);`
        - `open(dev, O_RDWR | O_NONBLOCK)`.
        - `VIDIOC_S_FMT` to set `width` × `height` YUYV (or YUV422
          per AP1302 negotiated format — check
          `media-ctl --print-topology`).
        - `VIDIOC_REQBUFS` × 4 with `V4L2_MEMORY_MMAP`.
        - Per buffer: `VIDIOC_QUERYBUF` + `mmap` to get va; record
          length. PA via `t1_va_to_pa` (or use `V4L2_MEMORY_DMABUF`
          + udmabuf for clean PA).
        - `VIDIOC_QBUF` all buffers.
        - `VIDIOC_STREAMON`.
        - Returns 0 on success, -1 with errno on any failure.
  * [ ] `void camera_close(struct camera *cam);`
        - `VIDIOC_STREAMOFF`, munmap each buffer, close fd.
  * [ ] `int  camera_dqbuf(struct camera *cam, struct camera_buf *cb);`
        - `poll` on cam->fd; `VIDIOC_DQBUF`; fill `cb` with the
          buffer's va/pa/length.
  * [ ] `int  camera_qbuf(struct camera *cam, struct camera_buf *cb);`
        - `VIDIOC_QBUF` to recycle.
  * [ ] (Optional) `int camera_set_format(struct camera *cam,
        uint32_t pixfmt);` — change pixfmt (YUYV ↔ NV12 ↔ ...).
        Only needed if the bringup discovers AP1302 outputs something
        other than what `camera_open` defaults to.

### Helpers (private)

  * [ ] `static int xioctl(int fd, unsigned long req, void *arg);`
        EINTR retry wrapper around `ioctl`.

---

## 8. `vision_software/visionsoc_main/display.{c,h}`

DRM HDMI display wrapper. Source-of-truth in
`driver_implementation_handoff.md` § 9.3.

### Types

  * [ ] `struct display` — drm_fd, connector_id, crtc_id, mode,
        framebuffer ring (each: handle, fb_id, va, pa, pitch).
  * [ ] `struct display_buf` — index, va, pa.

### Functions

  * [ ] `int  display_open(struct display *disp);`
        - `drmOpen` `/dev/dri/card0`.
        - Find the first connected connector; pick its preferred mode.
        - `drmModeCreateDumb` × 2 (double-buffered) with the chosen
          mode width × height.
        - `drmModeAddFB2` to register both as framebuffers.
        - mmap each via `DRM_IOCTL_MODE_MAP_DUMB`.
        - PA via `t1_va_to_pa` (or use a custom GEM helper if udmabuf
          can be imported into DRM).
        - Set initial mode with `drmModeSetCrtc` on framebuffer 0.
  * [ ] `void display_close(struct display *disp);`
        - `drmModeRmFB`, `drmModeDestroyDumb`, `drmClose`.
  * [ ] `int  display_dq_for_filling(struct display *disp,
                                     struct display_buf *db);`
        - Return the back buffer (the one not currently scanned out).
        - Block if both are queued (via `drmModePageFlip` callback).
  * [ ] `int  display_qbuf(struct display *disp, struct display_buf *db);`
        - `drmModePageFlip` to swap to this buffer at next vblank.

### Notes

  * If `drm` is too heavy for the bringup, gstreamer's `kmssink` is a
    drop-in replacement and used in
    `driver_implementation_handoff.md` § 10 step 6 for the camera
    standalone test. Use that to validate the camera path before
    writing `display.c`.
  * The KV260 carrier exposes HDMI through `card0`; if there's a DSI
    on `card1` etc, double-check with `ls /dev/dri/`.

---

## 9. `vision_software/visionsoc_main/dma.{c,h}` (optional shim)

Source-of-truth in `driver_implementation_handoff.md` § 9.4 — this
file is optional. The DMA helpers already live in libt1.

If you do create it, only put domain-specific helpers here:

  * [ ] (Optional) `int prefetch_next_frame(uint32_t cam_pa,
        uint32_t bram_dst);`
        Wraps `t1_dma_mm2s_async` + bookkeeping for which BRAM half
        is the "next" target. Useful once the loop moves to
        double-buffered mode (§ 6 main optional refinements).

If you don't need this, just `#include "libt1.h"` from `main.c` and
call the `t1_dma_*` helpers directly. Skip creating `dma.{c,h}`.

---

## 10. Tests under `vision_software/libt1/test/`

### 10.1 `smoke.c` — wrapper accessibility

Source: `driver_implementation_handoff.md` § 7.1.

  * [ ] `int main(void);`
        - `t1_init`.
        - `t1_cycles()` two reads with a `usleep(10000)` between.
          Assert delta > 100k cycles (i.e. counter actually ticking).
        - `t1_set_vertical_mode_raw(1)`; assert
          `t1_get_vertical_mode_raw() == 1`.
        - `t1_set_vertical_mode_raw(0)` to leave hardware quiet.
        - Print PASS/FAIL.

### 10.2 `ddr_roundtrip.c` — vle + vse over DDR

Source: § 7.2.

  * [ ] `int main(void);`
        - `t1_init`, two `t1_buf_alloc` (16 KB each).
        - Fill input with a known pattern; zero output.
        - Issue `vsetvli`, `vle8.v v8, (rs1=in.pa)`,
          `vse8.v v8, (rs1=out.pa)`.
        - `t1_wait_mem(1)` to confirm vse retired.
        - `memcmp` input vs output.
        - **Encoding warning:** verify the hex for `vsetvli`/`vle8`/
          `vse8` with `riscv64-linux-gnu-objdump -d` on a sample
          assembly. The handoff doc's example values are illustrative.

### 10.3 `port_grid_vadd.c` — Style B end-to-end

Source: § 7.3. Ports
`tests/vision_task/simple_instruction_asm.c`'s grid_vadd.

  * [ ] `int main(void);`
        - `t1_init`, three `t1_buf_alloc` (a, b, out — 16 KB each).
        - Initialise a + b with patterns.
        - Use the `kernels/grid_vadd.h` array (built via
          `build_kernel.sh`) as the kernel.
        - Loop the kernel issue with rs1 set per iteration to a/b/out
          PAs as the kernel expects.
        - Compare `out` against `a + b` per element.

### 10.4 `dma_loopback.c` — DMA standalone (NEW — not in handoff)

Add this even though the handoff doc only mentions it in passing
(§ 10 step 7). Validates DMA in isolation before main.c.

  * [ ] `int main(void);`
        - `t1_init`, two `t1_buf_alloc` (16 KB src + dst).
        - Fill src with pattern; zero dst.
        - `t1_dma_mm2s_async(src.pa, 0, 16384)` followed by
          `t1_dma_s2mm_sync(0, dst.pa, 16384)`. (Or whichever
          order matches the BD: most likely DMA mm2s reads from src
          and writes to a stream sink, then s2mm reads stream and
          writes to dst — depends on BD wiring; check
          `system_top.tcl`.)
        - `memcmp` src vs dst.

### 10.5 `vert_lsu.c` — vertical-mode regression (NEW — gate test)

Source: handoff doc § 10 step 5 mentions it but doesn't show code.
Port `tests/vision_task/simple_instruction_vert_lsu.c`. **This is
the gate** that proves `VERTICAL_MODE` plumbing works end-to-end —
do not skip even if the other tests pass.

  * [ ] `int main(void);`
        - allocate input + output udmabuf.
        - Build a `t1_op` with `vertical_mode = 1`.
        - Issue a `vle` + `vse` pair; expect output to be the
          *transpose* of input (vertical mode swaps row/column
          striding).
        - Compare output against expected transpose.

---

## 11. Cross-cutting: error handling convention

  * Every `int`-returning function in libt1: `0` on success, `-1` on
    error with `errno` set. Rationale: lets callers chain with
    `if (... < 0) goto fail;`.
  * `t1_drain_rd` / `t1_drain_csr` are exceptions — they return `1`
    on pop, `0` on empty, `-1` on hard error. Document this in the
    header comment.
  * `t1_init` does NOT take a string error param; rely on `errno`
    (and `perror` on the caller side). Keeps the API minimal.
  * Internal helpers (file-scope `static`) may use a simple boolean
    return; consistency only matters at the public boundary.

---

## 12. Cross-cutting: thread-safety

  * Initial implementation: **single-threaded only**. Document in
    `libt1.h` header that `t1_*` calls from multiple threads are not
    safe. The frame loop in `main.c` is single-threaded.
  * If you add threading later (e.g. a separate IRQ thread for DMA),
    the cleanest path is a single big mutex around all of libt1,
    held for the duration of `t1_issue` and `t1_dma_wait`. Don't
    try to make individual ops lock-free.

---

## 13. Cross-cutting: encoding / debugging tooling

Don't hand-encode RVV instructions. Two acceptable patterns:

  1. **Style B (preferred):** write a `.S` file, run
     `build_kernel.sh`, get a `kernel[]` C array. Used by
     `port_grid_vadd.c` and the main program.
  2. **Style A (debug only):** declare a string of instruction hex
     in C, with each value commented with the assembly mnemonic.
     Cross-check by extracting the bytes via
     `riscv64-linux-gnu-objdump -d` on a one-shot assembly file.

For driver bringup tests where you need to issue 1–3 specific
instructions (`smoke.c`, `ddr_roundtrip.c`), Style A is fine *if*
you verify with objdump.

---

## 14. Definition-of-done checklist

The driver is "done" when all of these pass on the Kria:

  * [ ] § 10.1 `smoke.c` PASS
  * [ ] § 10.2 `ddr_roundtrip.c` PASS
  * [ ] § 10.4 `dma_loopback.c` PASS
  * [ ] § 10.3 `port_grid_vadd.c` PASS
  * [ ] § 10.5 `vert_lsu.c` PASS — vertical mode actually transposes
  * [ ] Camera standalone — `gst-launch-1.0 v4l2src ! videoconvert !
        kmssink` shows live camera on HDMI.
  * [ ] `visionsoc_main` — full pipeline at ≥ 30 fps with
        `t1_perf_start/stop` printing kernel cycles.

When all 7 pass, the driver side is complete.
`fyp_doc/implementation_tasks_index.md` § 3.5 has the canonical
"definition of done" — if it says something different, that wins.
