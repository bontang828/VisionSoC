# Driver Implementation Status

Last updated: 2026-05-07

## Scope

This document records the current Task B implementation state for the
ARM-side VisionSoC driver. Primary instructions came from:

* `fyp_doc/implementation_tasks_index.md` Task B
* `fyp_doc/driver_implementation_handoff.md`
* `fyp_doc/driver_function_spec.md`

## Current tree

New files live under `vision_software/`:

* `vision_software/libt1/`
  * `libt1_regs.h`
  * `libt1.h`
  * `libt1.c`
  * `weights_format.h`
  * `build_kernel.sh`
  * `Makefile`
  * `test/smoke.c`
  * `test/ddr_roundtrip.c`
  * `test/port_grid_vadd.c`
  * `test/dma_loopback.c`
  * `test/vert_lsu.c`
* `vision_software/visionsoc_main/`
  * `main.c`
  * `camera.c`, `camera.h`
  * `display.c`, `display.h`
  * `kernels/grid_vadd.S`
  * `kernels/grid_vadd.h`
  * `Makefile`

## Implemented

`libt1_regs.h` mirrors the current wrapper register map in
`fpga/wrapper/t1_axi_lite_wrapper.sv`, including `VERTICAL_MODE` at
`0x44` and perf registers through `0x54`.

`libt1.c` implements:

* UIO lifecycle for `/dev/uio0` and `/dev/uio1`
* MMIO helpers with memory fences
* `t1_issue` and `t1_issue_kernel`
* RD/CSR FIFO drains
* MEM retire wait via UIO IRQ and `MEM_COUNT` W1C decrement
* perf counter helpers
* `u-dma-buf` allocation via `/dev/udmabufN` and sysfs `phys_addr`
* AXI DMA simple-mode helpers
* `/proc/self/pagemap` VA-to-PA fallback
* raw vertical-mode register access

Bringup tests are present for smoke, DDR LSU roundtrip, grid kernel
port, DMA loopback, and vertical-LSU transpose.

`visionsoc_main` has a first-pass single-threaded camera -> DMA -> T1
kernel -> DMA -> DRM loop, plus V4L2 and DRM wrappers.

## Local verification

Passed on this host:

```sh
cd vision_software/libt1
make
```

That built `libt1.a`, `libt1.so`, and all five test binaries with
`-Wall -Wextra -Werror`.

```sh
cd vision_software/visionsoc_main
make
```

This now compiles and links after installing `libdrm-dev`. The
`visionsoc_main` Makefile also has a rule to build `../libt1/libt1.a`
if needed, so a clean build from `vision_software/visionsoc_main` does
not require manually building libt1 first.

Not run locally:

* `build_kernel.sh`, because `riscv64-linux-gnu-as` is not installed on
  this host.
* Any hardware tests, because `/dev/uio*`, `udmabuf`, V4L2 camera, DMA,
  and DRM HDMI need the Kria with the Task A bitstream and overlay.

## Important caveats

`t1_issue` consumes one `MEM_COUNT` event for LSU instructions. Do not
call `t1_wait_mem(1)` immediately after `t1_issue()` for the same
`vle`/`vse`; that would wait for a second event.

`grid_vadd` keeps the historical name from the source test, but the
kernel instruction is `vsub.vv v8, v8, v12`, so the expected output is
`a - b`.

`vision_software/visionsoc_main/kernels/grid_vadd.h` is checked in so
the C code can compile without RISC-V binutils. Regenerate it on a
machine with `binutils-riscv64-linux-gnu`:

```sh
cd vision_software/visionsoc_main
../libt1/build_kernel.sh kernels/grid_vadd.S kernels/grid_vadd.h grid_vadd
```

The camera and display wrappers currently get physical addresses via
`t1_va_to_pa`. That may require root/CAP_SYS_ADMIN and may not work for
all V4L2/DRM buffer mappings. If it fails on the Kria, replace that path
with explicit dma-buf/udmabuf-backed buffers or a DRM PRIME import flow.

The AXI DMA helper is PG021 simple-mode register programming. For true
memory-to-memory loopback, the FPGA block design must wire the stream
path correctly. In the `fpga/system/system_top.tcl` snapshot read during
this implementation pass, the DMA stream/scratchpad wiring still needed
Task A confirmation.

`visionsoc_main` is a hardware-facing scaffold. It matches the intended
control flow, but final camera pixel format, display color conversion,
and any PL upscaler/frmbuf integration still need to be validated on the
actual KV260 design.

## Quality review against driver_function_spec.md (2026-05-07)

Cross-checked every public function and test against
`fyp_doc/driver_function_spec.md` § 1–14. Implementation is
spec-compliant and ready for Task C bringup. Notes below capture
deliberate deviations and follow-ups so the next session does not
re-investigate them.

### Spec compliance

* All 22 wrapper register offsets and bit fields in `libt1_regs.h`
  match `fpga/wrapper/t1_axi_lite_wrapper.sv` lines 11–36 verbatim.
  `T1_VTYPE_E8_M4_TA_MA = 0xC2` is correct for vsew=0, vlmul=2, vta=1,
  vma=1.
* All 22 declared functions in `libt1.h` are implemented in `libt1.c`.
* All 5 required tests (`smoke`, `ddr_roundtrip`, `dma_loopback`,
  `port_grid_vadd`, `vert_lsu`) compile under `-Wall -Wextra -Werror`.
* The four kernel encodings checked into
  `visionsoc_main/kernels/grid_vadd.h` were verified bit-by-bit
  against the RVV spec (vle8 v8/v12, vsub.vv v8,v8,v12, vse8 v8).

### Deliberate deviations from the spec (acceptable)

* `t1_issue` LSU path clears all pending mem events *before* issue and
  then waits for exactly 1, instead of "snapshot pre-mem, wait for
  pre-mem+1". Functionally equivalent, avoids the saturating-counter
  race the spec itself flags, and is the reason the existing caveat
  about not calling `t1_wait_mem(1)` after `t1_issue()` exists.
* MMIO fences use `__atomic_thread_fence(SEQ_CST)` rather than `dmb sy`.
  Portable equivalent on aarch64.
* `dma_loopback.c` kicks `t1_dma_s2mm_async` *before*
  `t1_dma_mm2s_async` so the stream sink is ready before the source
  produces. The spec did not prescribe an order; this is the right
  one for a stream-loopback BD.
* `udmabuf` PA parsing uses `fscanf(f, "%llx", ...)`. `%x` accepts an
  optional `0x` prefix, so this is more flexible than the spec's
  `0x%llx`.
* `t1_cycles` uses a do-while retry loop instead of "two-read,
  retry-once". Strictly more robust on a slow MMIO path.

### Genuine follow-ups (track during bringup)

1. `weights_format.h` declares `weights_load_manifest` without
   implementing it. Harmless (link-time only if called) but stray.
   Either implement when the main program needs weights, or drop the
   prototype.
2. `port_grid_vadd.c` reaches into the sibling tree via
   `#include "../../visionsoc_main/kernels/grid_vadd.h"`. Works via
   `-I../visionsoc_main` in the Makefile but couples the two trees.
   If it gets in the way, copy or symlink the kernel header into
   `libt1/test/`.
3. `grid_vadd.h` is checked in pre-generated. Before relying on it on
   the Kria, run `build_kernel.sh` on a host with
   `binutils-riscv64-linux-gnu` and diff against the committed file.
4. No systemd unit (`visionsoc.service` from spec § 9.5) yet. That is
   Task C deployment territory and not blocking Task B.

### Definition-of-done status

All five tests are written and compile; none have been run because
the host has no UIO / udmabuf / V4L2 / DMA / DRM. Hardware execution
is deferred to Task C per `implementation_tasks_index.md` § 4.

## Bringup-driven driver fixes (2026-05-07, after Task C started)

While `camera_bringup_status.md` § 6.2 was being investigated, three
driver issues were identified and fixed in this branch. Build
verified clean against `libt1.a` + `visionsoc_main`.

### Fix A — UIO node lookup by binding name (resolves blocker 6.2)

`libt1.c` previously hard-coded `T1_UIO_PATH "/dev/uio0"` and
`DMA_UIO_PATH "/dev/uio1"`. After our overlay loads, the four
PS-base `axi-pmon` UIOs occupy `uio0..3` and our T1/DMA/BRAM are at
`uio4..6`. The driver was opening the wrong nodes.

**Change:** added `find_uio_by_name()` that walks
`/sys/class/uio/uio*/name` and matches `"t1"` and `"dma"` (the DT
node names from `system_top_wrapper.dts`). `t1_init` now resolves
paths via `resolve_uio_path()`, which also honours `T1_UIO_PATH` and
`DMA_UIO_PATH` env-var overrides for testing and falls back to
`/dev/uio0` / `/dev/uio1` if both lookup and env are empty.

**Consequence:** robust against overlay enumeration order; one extra
sysfs read per init (negligible). Fails with `ENODEV` if the overlay
isn't loaded — louder than the previous "open the wrong device and
poke axi-pmon registers".

### Fix B — `t1_dma_wait` polls `DMA_SR.IDLE` instead of blocking on IRQ

The BD wires `axi_dma/mm2s_introut` and `axi_dma/s2mm_introut` to
*separate* GIC SPIs (89/90/91 via `irq_concat` in `system_top.tcl`
lines 264–267 and 533–539). `generic-uio` only registers IRQ index 0
(`mm2s`); s2mm completion is invisible at the `/dev/uio_dma` read
interface. The previous `t1_dma_wait` blocked on `read(uio_dma)` and
would therefore:

* Hang the full `T1_DMA_TIMEOUT_MS` for any s2mm-only sync.
* In `dma_loopback.c`, return on the first (mm2s) IRQ even if s2mm
  hadn't finished — silent corruption of `dst.va`.

**Change:** `t1_dma_wait` now polls `DMA_REG_MM2S_SR` and
`DMA_REG_S2MM_SR` until both have `DMA_SR_IDLE` set (or any error
bit is set). It still W1Cs IRQ status bits and re-arms the UIO fd
for completeness, but it no longer requires an IRQ to wake. A
channel that was never kicked since `dma_reset()` is already idle,
so the mm2s-only / s2mm-only sync paths still terminate quickly.

**Consequence:** active polling with `sched_yield()` between reads.
For the 16 KB / 64 KB DMAs we run today this completes in <50 µs of
wall time. CPU usage on the polling thread is non-zero; mitigated by
yield. The longer-term cleaner fix is a BD change to OR the two
DMA introut lines before `irq_concat` (or a custom UIO driver that
registers both IRQs); deferred until after the Task C blocker list
is cleared.

### Fix C — `t1_va_to_pa_range` for multi-page DMA buffers

`t1_va_to_pa(va)` returns the PA of the *first page* only. V4L2 MMAP
camera buffers (multi-page) and DRM dumb framebuffers (megabytes)
are not guaranteed physically contiguous on systems without
CMA-backed allocators. AXI DMA reading from a non-contiguous PA
silently corrupts pages 2..N — the camera path would produce
visually-plausible-but-wrong frames.

**Change:** new helper `uint32_t t1_va_to_pa_range(const void *va,
size_t size)` walks every page in the range via `/proc/self/pagemap`
and verifies contiguity. Returns the starting PA on success, or 0
with `errno = EXDEV` (non-contiguous), `EFAULT` (page not present),
`EOVERFLOW` (PA > 32 bits), or `EINVAL` (bad input). `camera.c` and
`display.c` now use this with the buffer length, so an open() on
non-contiguous mappings fails loudly instead of producing scrambled
DMA. `t1_va_to_pa(va)` is kept as-is for legitimate single-page
callers.

**Consequence:** `camera_open` / `display_open` will now *fail* on
hosts where V4L2/DRM hand out non-contiguous MMAP buffers — but
"failing" is correct, because the previous "success" was silently
broken. The actual fix once that triggers is to switch the camera
to `V4L2_MEMORY_DMABUF` backed by udmabuf, or import udmabuf into
DRM via PRIME. That refactor stays deferred until a clean run is
needed.

### Tests / build status after fixes

* `make -C vision_software/libt1` clean under `-Werror`.
* `make -C vision_software/visionsoc_main` clean under `-Werror`.
* **Hardware run 2026-05-09 against bitstream 5h
  (`mudkip2d128small1bram1chain2lanescale_fpga-20260509-121320`):**
  | Test | Result |
  |---|---|
  | `triage_t1` (control plane) | ALL PASS |
  | `smoke` | PASS |
  | `ddr_roundtrip` | PASS (after Fix D below) |
  | `port_grid_vadd` | PASS (after Fix D) |
  | `vert_lsu` | PASS (after Fix D) |
  | `dma_loopback` | FAIL — `t1_dma_wait: Connection timed out` |
  The DMA loopback failure is a longstanding BD design issue
  (`axi_dma/S_AXIS_S2MM` was never wired in the BD; visible as a
  CRITICAL WARNING since 5e). Not LSU-related, not driver-related.

### Fix D — udmabuf cache coherency for `t1_buf` regions (2026-05-09)

**Symptom that surfaced this:** every libt1 LSU roundtrip test
(`ddr_roundtrip`, `port_grid_vadd`, `vert_lsu`) failed with the
output udmabuf still at memset-zero, even though `t1_issue(vse8)`
returned success and our raw-mmio probe `lsu_store_probe` showed
T1 actually wrote correct data to DRAM. The CPU was reading stale
cached zeros while T1 had written the real data behind the cache.

**Root cause:** `udmabuf` mmap is **cached** by default. CPU writes
land in L1/L2 and don't reach DRAM until explicitly flushed. CPU
reads return cached values even after T1 writes to DRAM via
`m_axi_hb`. Linux's `msync()` on udmabuf does **not** reliably
perform the right cache operation — it's specific to filesystem
mappings, not bidirectional DMA buffers.

**The fix in libt1:**

  * `struct t1_buf` gains a private `_udmabuf_idx` field.
  * Two new public helpers:
    `int t1_buf_sync_for_cpu(struct t1_buf *buf);`
    `int t1_buf_sync_for_device(struct t1_buf *buf);`
    They write `"1"` to
    `/sys/class/u-dma-buf/udmabufN/sync_for_{cpu,device}` —
    the documented udmabuf cache-management interface that
    actually invokes the L1/L2 cache op.
  * All four LSU-using hardware tests (`ddr_roundtrip`,
    `port_grid_vadd`, `vert_lsu`, `dma_loopback`) replace
    `msync(MS_SYNC)` before T1 reads with
    `t1_buf_sync_for_device(&buf)`, and `msync(MS_INVALIDATE)`
    after T1 writes with `t1_buf_sync_for_cpu(&buf)`.

**Programming guide — required pattern around any T1 LSU op:**

```c
struct t1_buf in = {0}, out = {0};
t1_buf_alloc(&in, FRAME_BYTES);
t1_buf_alloc(&out, FRAME_BYTES);

cpu_init_input_buffer(in.va);            // CPU writes to in
memset(out.va, 0, out.size);             // CPU writes to out

/* MUST flush CPU cache to DRAM before T1's m_axi_hb reads */
t1_buf_sync_for_device(&in);
t1_buf_sync_for_device(&out);

t1_issue(/* vle8 from in.pa */);
/* ... compute ops ... */
t1_issue(/* vse8 to out.pa */);

/* MUST invalidate CPU cache before reading what T1 wrote */
t1_buf_sync_for_cpu(&out);

read_output_from(out.va);                // CPU now sees fresh DRAM
```

If you skip `sync_for_device` before T1 reads:
   T1 will read STALE DRAM (your CPU writes are still in cache).
If you skip `sync_for_cpu` after T1 writes:
   The CPU will see STALE CACHE (memset-zero or whatever was there
   before T1 wrote). Test will fail with confusing "T1 didn't
   write" symptoms; both LSU and AXI fabric are actually fine.

**Things NOT to do (tried and rejected):**

  1. **`msync(addr, size, MS_SYNC | MS_INVALIDATE)`** — does not
     reliably perform the cache op on udmabuf. Older versions of
     this code path used msync; ALL of those calls have been
     replaced. Removing the sync helpers and re-introducing
     `msync` will silently break LSU tests on hardware while
     passing in simulation (where no caches exist).
  2. **`open("/dev/udmabufN", O_RDWR | O_SYNC)`** — produces
     uncached mmap, which avoids cache management entirely BUT
     causes SIGBUS in any test that uses SIMD-optimised libc
     routines on the buffer (`memset`, `memcmp`, `memcpy` on
     16+ KB regions; the ARMv8 NEON ldp/stp paths are illegal on
     this kernel's uncached mappings). The proper path is the
     sysfs sync interface, not `O_SYNC`.
  3. **`__builtin___clear_cache(start, end)`** — only operates on
     the I-cache for self-modifying code. Not the D-cache, doesn't
     help here.
  4. **CPU dsb/isb barriers (`__asm__ __volatile__("dsb sy")`)** —
     orders memory accesses but doesn't flush or invalidate. Useful
     for ordering w.r.t. the sync calls, not as a replacement.

**Future API extension idea (not yet implemented):** wrap the
sync calls into `t1_issue_with_buffers(op, ins[], outs[])` so the
caller doesn't have to do it manually. Until that exists, follow
the guide above.

## Next pickup steps

1. Install Task B prerequisites on the Kria:

   ```sh
   sudo apt install -y build-essential binutils-riscv64-linux-gnu libdrm-dev v4l-utils udmabuf-utils
   ```

2. Rebuild:

   ```sh
   cd vision_software/libt1
   make clean
   make
   cd ../visionsoc_main
   make kernels
   make
   ```

3. Verify Task A first: `/dev/uio0`, `/dev/uio1`, cycle counter, and
   `VERTICAL_MODE` register round-trip.

4. Run hardware tests in order:

   ```sh
   cd vision_software/libt1
   sudo ./test/smoke
   sudo ./test/ddr_roundtrip
   sudo ./test/dma_loopback
   sudo ./test/port_grid_vadd
   sudo ./test/vert_lsu
   ```

5. Only after those pass, debug `visionsoc_main` camera/display behavior.
