# Vision Kernel Programming Guide

**Audience:** an agent picking up the project to write new vision
kernels for the T1 2D-fabric vector processor on the KV260 board. The
camera pipeline has separate work in flight (see
`fyp_doc/camera_bringup_status.md`), so this guide is structured so
you can develop kernels against **dummy data on the host-allocated DDR
buffers today** and switch to **live camera frames in the same
udmabuf** once the camera path is finalised — the API surface and
buffer layout are identical, only the producer changes.

This is a programming guide, not a build-system guide. For build
status, BD edits, and Vivado iteration trail, see
`fyp_doc/fpga_build_status.md`.

---

## 1. The 60-second mental model

You have a Linux user-space program on the Kria PS (Cortex-A53 quad-core,
aarch64) that:

1. Allocates DDR buffers (via `t1_buf_alloc` / V4L2 / udmabuf).
2. Issues vector instructions to a T1 hardware vector unit in the PL,
   one at a time, through three AXI4-Lite registers (instruction,
   rs1, rs2). The T1 wrapper acks each issue.
3. T1 reads/writes data via two AXI4 ports: `m_axi_hb` (128-bit
   high-bandwidth, → DDR via HPC0 *and* → 32 KB BRAM scratchpad in
   the PL) and `m_axi_idx` (32-bit indexed lane → DDR via HP0).
4. Each T1 vector instruction is **time-multiplexed across 128
   hardware rows**. A single `vle8.v v8, (a0)` does 128 row-strided
   loads from `a0[0]`, `a0[128]`, `a0[256]`, …, `a0[127*128]`. So
   one instruction sweeps a full 128×128 image at SEW=8.
5. Per-element compute (`vadd.vv`, `vmul.vv`, `vslideup.vi`,
   `vrgather.vv`, …) replicates across all 128 hw-rows in parallel —
   that is where the "free" 128× throughput comes from.
6. CSR `0x7c0` flips between **horizontal compute mode** (work moves
   within a row) and **vertical compute mode** (work moves between
   rows — this is the architecture's primary way to do 2D
   neighbourhood ops without a dedicated transpose).

Stop and re-read item (4) and (6). They are the only two things that
make this hardware different from stock RISC-V V.

**Read next:** `fyp_doc/2d_fabric_handoff.md` (the canonical
programming reference). The rest of this guide assumes you have
internalised that doc.

### 1.1 Canonical kernel assumptions (memorise these)

Every kernel in this codebase — probes, vision programs, the real
runtime — uses the same fixed parameter set. Stay inside this box
unless you have a very specific reason to step out, and document the
reason if you do.

| Parameter | Value | Why |
|---|---|---|
| **SEW** | 8 bits | One element = one pixel (greyscale i8 / u8) |
| **LMUL** | 4 | Register-group bytes = 4 × `vlenb` = 4 × 32 B = 128 B → one full image row per register group at SEW=8 |
| **vl** | 128 | One image row across `vl` element lanes |
| **Grid** | 128 × 128 | 128 image rows mapped 1:1 onto the 128 hw-rows of the time-multiplexed dimension |
| **vtype encoding** | `0x000000C2` | `vsetvli zero, 128, e8, m4, ta, ma` — declared as `T1_VTYPE_E8_M4_TA_MA` in `libt1_regs.h` |
| **CSR 0x7c0** | 0 (default) / 1 (vertical mode) | Per-instruction snapshot; flip with `csrw 0x7c0, t3` |
| **Image layout in memory** | Row-major, contiguous, 1 byte per pixel | `&grid[r][c]` = `base + r*128 + c` |

That row-major layout is what makes `vle8.v v8, (base)` correctly fill
all 128 hw-rows: hw-row `r` strides by `r * 128` bytes from `base`.
The pitch (128) is currently fixed in the LSU RTL; you cannot
parameterise it from software in the current build.

**Why LMUL=4 specifically:** with `--dLen 128 --extensions zvl256b
--laneScale 2 --rowNumber 1`, `vlenb = 32` bytes. SEW=8 × LMUL=4 ×
vlenb = exactly 128 byte lanes = one row. LMUL=2 leaves half the row
in dead lanes; LMUL=8 misbehaves on the 2D plumbing (see
`2d_fabric_handoff.md` § 3.3).

---

## 2. Where to connect, what's already there

### 2.1 Board access

The dev host can reach the Kria over SSH:

```sh
ssh kv260
```

User: `ubuntu`. Sudo is configured passwordless for a scoped allowlist
(see `fyp_doc/camera_bringup_status.md` § 4 for the full list — fpgautil,
install, dmesg, devmem2, modprobe, the libt1 test binaries, etc.).

File transfer: **use `scp -r`**. The dev host does not have `rsync`.
The Kria's HTTPS git misbehaves ("could not read Username"), so any
external git clone you need should happen on the dev host first then
`scp -r` over.

### 2.2 What's already installed on the Kria

  * Ubuntu Server 22.04 (jammy), kernel 5.15-xilinx-zynqmp.
  * `udmabuf` kernel module auto-loads on boot (`/dev/udmabuf{0,1,2}`,
    each 4 MB, see `/etc/modules-load.d/u-dma-buf.conf`).
  * `riscv64-linux-gnu-as` / `objcopy` for kernel assembly.
  * `dtc` for device-tree overlay compilation.
  * `fpgautil` for bitstream loading.
  * `i2c-tools`, `media-ctl`, `v4l2-ctl` for camera bringup (camera-only,
    not needed for T1 work).
  * `libdrm-dev` for HDMI display via the Zynq DP IP (PS, not PL).
  * `~/vision_software/` is a working tree — the repo's user-space
    sources are scp'd here as they evolve.

### 2.3 The deployed FPGA artefact

The Kria has a bitstream pre-loaded at boot (the k26-starter-kits
overlay). To use VisionSoC, you remove that overlay and load
visionsoc:

```sh
ssh kv260 '
  sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null
  sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
                -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo
'
```

After this, `/dev/uio{4,5,6}` enumerate as `t1`, `dma`, `bram`. They
get cleared on reboot (k26 comes back as default) — re-fpgautil after
each boot.

---

## 3. Which bitstream to use **right now**

**For kernel-development-against-dummy-data work, deploy 5m:** it has
T1 fully working, the BRAM scratchpad routed, and `sp_v12_compute_probe`
+ `sp_4issue_with_verify_probe` validated. The deployment procedure
and current state are documented in `fyp_doc/fpga_build_status.md`.

The 5m artefact lives on the Kria at:

```
/lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5m-backup
```

To make 5m the active bit.bin (one-shot):

```sh
ssh kv260 '
  sudo install -m 644 /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5m-backup \
                      /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
  # dtbo currently on disk should already have bram@a0080000; verify with:
  #   strings /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo | grep bram
'
```

**Do NOT use 5o** for kernel-dev work yet. 5o adds the
`v_frmbuf_wr/s_axi_CTRL` wiring needed for live camera frames; it's in
flight (see `fyp_doc/fpga_build_status.md` § 0). When 5o is validated,
camera frames land in the same kind of DDR buffer your kernel already
takes as input — your code does not change.

### Sanity check after deploy

```sh
ssh kv260 'cd ~/vision_software/libt1 && sudo ./test/sp_v12_compute_probe'
# Expected: PASS: sp_v12_compute_probe (v12 from BRAM + vadd + store to DDR, sp.pa=0xa0080000)
```

If this fails, the bitstream is not 5m or the dtbo is mismatched. Stop
and re-check before writing any new kernel.

---

## 4. The memory system in plain terms

The PL fabric sees four address spaces; you mostly only care about the
first two from C code.

### 4.1 DDR — main system memory (~4 GB on KV260)

  * Owned by the PS. The OS allocates pages of it for user space.
  * **CPU view:** allocated via `mmap` of `/dev/udmabufN` (cache-coherent
    but *cached* — `t1_buf_sync_for_{device,cpu}` flushes/invalidates
    the L1/L2 lines). For V4L2 camera buffers, `VIDIOC_QUERYBUF` +
    `mmap(fd, ...)` gives you a VA; the libt1 helper
    `t1_va_to_pa_range(va, len)` returns the PA via `/proc/self/pagemap`.
  * **T1 view:** the same PA, reached through T1's `m_axi_hb` master
    (which goes via smartconnect_hb → HPC0 → PS internal NIC → DDR
    controller). T1 sees the full 2 GB DDR-low range starting at PA
    `0x00000000`.
  * Bandwidth: ~6.4 GB/s shared with PS reads, sufficient for ~30 fps
    of full-resolution camera frames *and* T1 streaming.

When your kernel reads/writes its `src`/`dst` buffers, those buffers are
in DDR. You allocate them via:

```c
struct t1_buf src = {0}, dst = {0};
t1_buf_alloc(&src, 128 * 128);    // pa = 0x38300000 or similar; va is your mmap
t1_buf_alloc(&dst, 128 * 128);

// fill src with dummy data
memset(src.va, 0, 128 * 128);
for (int i = 0; i < 128 * 128; ++i) ((uint8_t*)src.va)[i] = i & 0xFF;

t1_buf_sync_for_device(&src);     // CPU writes flushed to DDR
t1_buf_sync_for_device(&dst);     // dst pre-zeroed; flush too

// ... issue T1 kernel with op.rs1 = src.pa / dst.pa ...

t1_buf_sync_for_cpu(&dst);        // T1 writes invalidated from CPU cache
// now ((uint8_t*)dst.va)[i] is the T1 output
```

### 4.2 BRAM scratchpad — 32 KB at PA 0xA0080000

  * In the PL fabric. Tightly coupled to T1's `m_axi_hb` via
    `smartconnect_hb/M01_AXI` (so a vle from scratchpad is single-cycle
    128-bit on each beat — no DDR round-trip).
  * **CPU view:** *not* directly accessible from PS on 5m (the F6
    "PS→BRAM mmap" path is deferred). Treat it as a T1-only fast
    cache.
  * **T1 view:** lives at PA `0xA0080000` (32 KB = enough for
    two 128×128-byte images side by side). Hand a sub-range PA to T1
    as `op.rs1` exactly like DDR.

To allocate a scratchpad region from libt1:

```c
struct t1_buf sp = {0};
t1_scratchpad_alloc(&sp, /*offset=*/0, /*size=*/4096);  // sp.pa = 0xA0080000
// sp.va exists (uio6 mmap) but DO NOT dereference it from PS code —
// the F4 panic on the PS side is mechanical; T1 writes are fine.
```

Typical fast-loop pattern: load image to scratchpad once, run many
T1 kernel issues against the scratchpad, store result back to DDR
once. See `vision_software/libt1/test/sp_v12_compute_probe.c` for the
canonical example.

### 4.3 T1 control registers — UIO at 0xA0000000

These are the MMIO seat at the PL boundary that libt1 talks to. You
don't poke them directly in normal code — libt1 abstracts it. The
relevant subset (from `vision_software/libt1/libt1_regs.h`):

  * `0x04` = INSTRUCTION (the 32-bit RVV opcode you want issued)
  * `0x08` = RS1_DATA (scalar source 1, typically a base address)
  * `0x0C` = RS2_DATA (scalar source 2)
  * `0x10` = VTYPE
  * `0x14` = VL
  * `0x44` = VERTICAL_MODE (CSR 0x7c0; 0 = horizontal, 1 = vertical)
  * Reading `0x18` / `0x1C` etc gives per-issue counters (used by
    `triage_t1` to validate the wrapper is alive).

### 4.4 T1 IRQs and retire pipes

T1 returns three classes of asynchronous results through wrapper
queues. Normal code should use the `libt1` helpers instead of reading
these registers directly.

IRQ bits:

  * `T1_IRQ_RD`  / bit 0: scalar writeback pipe has data. This is used
    by vector-to-scalar instructions such as `vmv.x.s`.
  * `T1_IRQ_CSR` / bit 1: CSR side-effect pipe has data, for `vxsat`
    and `fflags` style results.
  * `T1_IRQ_MEM` / bit 2: memory-retire counter has at least one event.
    This is used by LSU instructions such as `vle*.v` and `vse*.v`.

`libt1` is single-threaded: serialize all `t1_*` calls in one host
thread. The UIO interrupt is shared by RD, CSR, and MEM, and Linux
disables a UIO interrupt after each wake until userspace rearms it.
`libt1` handles that rearm internally; do not mix direct `/dev/uioN`
reads with `libt1` waits in the same process.

Use the blocking helpers when you expect a result:

```c
uint32_t data = 0;
uint8_t rd_addr = 0;
bool is_fp = false;

/* After issuing vmv.x.s / vfmv.f.s / similar vector-to-scalar op. */
int rc = t1_wait_rd(&data, &rd_addr, &is_fp, 1000);
if (rc < 0) {
    perror("t1_wait_rd");
    return -1;
}
if (rc == 0) {
    fprintf(stderr, "timed out waiting for scalar retire\n");
    return -1;
}
printf("rd x%u = 0x%08x%s\n", rd_addr, data, is_fp ? " (fp)" : "");
```

For CSR retire data:

```c
uint32_t vxsat = 0, fflag = 0;
int rc = t1_wait_csr(&vxsat, &fflag, 1000);
if (rc <= 0) {
    /* rc < 0 is an error; rc == 0 is timeout. */
    return -1;
}
```

For memory operations, `t1_issue()` already waits for one MEM retire
event when the issued instruction is an LSU instruction. If you issue
a larger sequence and need to wait for extra memory events explicitly,
use:

```c
if (t1_wait_mem(1) < 0) {
    perror("t1_wait_mem");
    return -1;
}
```

The non-blocking drain helpers still exist:

  * `t1_drain_rd(...)` returns `1` if it popped a scalar packet, `0`
    if the RD FIFO was empty, and `-1` on error.
  * `t1_drain_csr(...)` has the same convention for the CSR FIFO.

Use `drain_*` only when polling is deliberate. For normal kernel
programming, prefer `wait_*`; otherwise a just-retired result can be
missed by checking the FIFO too early.

### 4.5 AXI DMA registers — UIO at 0xA0010000

PS-initiated DMA between DDR and BRAM. 5m has axi_dma's streaming
loopback broken (longstanding; that's the F5 work) — for now treat
PS-driven DMA as not-available and use T1 itself to move bytes
between DDR and scratchpad (which works perfectly).

---

## 5. Where camera frames will appear

### 5.1 Today (no camera): dummy DDR buffers

While the camera pipeline is being commissioned (see
`fyp_doc/camera_bringup_status.md`), write your kernel against
`t1_buf_alloc(&in, 128*128)` and fill `in.va` with dummy pixels. Your
code paths are exactly what real camera frames will use.

### 5.2 Tomorrow (5o or later): V4L2 buffers in DDR

The camera path is:

```
AR1335 sensor → AP1302 ISP (does crop + downsample to 128×128 UYVY)
              → MIPI CSI-2 RX (PL)
              → axis_data_fifo_cap
              → axis_subset_converter_cap   (UYVY → 24-bit padded)
              → v_frmbuf_wr  (PL DMA)
              → DDR via HP1
```

`v_frmbuf_wr` is a Xilinx Video Frame Buffer Write IP. The
`xilinx-frmbuf` kernel driver presents it through the V4L2 API. From
user space:

```c
int fd = open("/dev/video0", O_RDWR);
struct v4l2_format fmt = { /* UYVY 128×128 */ };
ioctl(fd, VIDIOC_S_FMT, &fmt);

struct v4l2_requestbuffers req = { .count = 4, .type = ..., .memory = V4L2_MEMORY_MMAP };
ioctl(fd, VIDIOC_REQBUFS, &req);

for (each i) {
    struct v4l2_buffer buf = { .index = i, ... };
    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    void *va = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    uint32_t pa = t1_va_to_pa_range(va, buf.length);
    // queue the buffer
    ioctl(fd, VIDIOC_QBUF, &buf);
}

ioctl(fd, VIDIOC_STREAMON, &type);
while (...) {
    struct v4l2_buffer buf = { ... };
    ioctl(fd, VIDIOC_DQBUF, &buf);            // wait for a captured frame
    // NOW: buf.index → va, pa points at 128×128 UYVY in DDR
    // give pa to t1_issue as op.rs1 for your vle source
    // ...
    ioctl(fd, VIDIOC_QBUF, &buf);             // give buffer back to driver
}
```

The reference user-space wrapper is in
`vision_software/visionsoc_main/camera.c`. **You do not need to write
this from scratch** — call `camera_open(&cam, "/dev/video0", 128, 128)`,
then `camera_dequeue(&cam, &cb)` in a loop, and `cb.pa` is your T1
input address. `cb.length` is `128*128*2` (UYVY = 2 bytes per pixel,
so the input is **256-byte-pitched 128 lines** — you typically take
only the Y bytes for greyscale operations).

The crucial point for kernel development today: **`cb.pa` will be
exactly the kind of DDR PA that `t1_buf_alloc` returns**. So a kernel
that consumes `in.pa` (`t1_buf_alloc`) and produces `out.pa`
(`t1_buf_alloc`) will, without modification, consume a V4L2 camera PA
when you plug it in.

### 5.3 The libt1 / camera C API in practice

The reference user-space wrapper lives in
`vision_software/visionsoc_main/{camera.h, camera.c}`. You include
`"camera.h"`, link against `libt1`, and the API is four entry points:

```c
struct camera {              /* opaque-ish state; treat as a handle */
    int fd; int width; int height;
    uint32_t pixfmt;         /* V4L2 fourcc as negotiated by the driver */
    unsigned nbufs;          /* number of mmap'd capture buffers */
    int streaming;
    struct camera_mmap_buf bufs[CAMERA_MAX_BUFS];  /* va/pa/length for each */
};

struct camera_buf {          /* per-dequeued-frame info */
    unsigned index;          /* identifier — pass back to camera_qbuf */
    void *va;                /* userspace VA of this frame's buffer */
    uint32_t pa;             /* physical address — give to t1_issue as op.rs1 */
    size_t length;           /* allocated bytes (whole buffer) */
    size_t bytesused;        /* bytes actually written by the IP this frame */
};

int  camera_open (struct camera *cam, const char *dev, int width, int height);
void camera_close(struct camera *cam);
int  camera_dqbuf(struct camera *cam, struct camera_buf *cb);    /* wait for a frame */
int  camera_qbuf (struct camera *cam, const struct camera_buf *cb); /* return buffer */
```

Canonical use pattern:

```c
#include "camera.h"
#include "libt1.h"

int main(void) {
    struct camera cam;
    if (t1_init() < 0)                          return 1;
    if (camera_open(&cam, "/dev/video0", 128, 128) < 0) return 1;

    /* Allocate an output buffer for T1 to write its kernel result. */
    struct t1_buf out = {0};
    t1_buf_alloc(&out, 128 * 128);

    while (!should_exit()) {
        struct camera_buf cb;
        if (camera_dqbuf(&cam, &cb) < 0)        break;     /* fatal */

        /* cb.pa is now the DDR PA of the captured frame. T1 reads
         * it via op.rs1 = cb.pa, exactly like t1_buf_alloc's .pa. */
        t1_buf_sync_for_device(&out);
        run_my_kernel(cb.pa, out.pa);           /* your code; see § 6.1 */
        t1_buf_sync_for_cpu(&out);

        camera_qbuf(&cam, &cb);                 /* give buffer back */
        consume_kernel_output(out.va);          /* display, save, etc. */
    }

    t1_buf_free(&out);
    camera_close(&cam);
    return 0;
}
```

The crucial guarantee: **`cb.pa` is interchangeable with
`t1_buf.pa`**. Anywhere your kernel takes a `t1_buf_alloc`'d PA, you
can pass `cb.pa` instead. The format on the wire (current state:
UYVY 16-bit; target state: NV12 Y-plane 8-bit, see § 5.4) is the only
difference.

**Cache coherency for camera buffers:** V4L2 handles
flush/invalidate around `camera_dqbuf` / `camera_qbuf` internally
through the videobuf2 framework. You do NOT need to call
`t1_buf_sync_for_{cpu,device}` on `cb.va` / `cb.pa`. You DO still
need to sync your own `t1_buf_alloc`'d output buffer.

`camera_open` does the full V4L2 negotiation: opens `/dev/video0`,
sets the format (UYVY at the dts-configured resolution per § 5.4),
requests `CAMERA_MAX_BUFS` mmap'd buffers, queries each buffer's VA
+ PA (via `t1_va_to_pa_range` against `/proc/self/pagemap`), queues
them, and starts streaming. `camera_dqbuf` blocks until a frame is
ready (timeout is wired internally via `poll`). The kernel handles
the producer-consumer queue.

### 5.4 UYVY vs NV12 — what's currently negotiated and what we want

**Current state (5o + dts `xlnx,csi-pxl-format = <0x1E>`):**
the pipeline negotiates UYVY 4:2:2 end-to-end. `cb.va` points at
`128*128*2 = 32768` bytes of interleaved `[U,Y0,V,Y1]` pairs.

For an 8-bit greyscale kernel, the Y bytes are at odd offsets
(`Y0` at `cb.pa + 1`, `Y1` at `cb.pa + 3`, …). To feed this to a
contiguous `vle8.v`, you need to extract Y first. Options:

  * **CPU pre-pass (cheap, what `visionsoc_main` does):**
    ```c
    static uint8_t y_buf[128 * 128];   /* or a t1_buf */
    const uint8_t *uyvy = cb.va;
    for (int i = 0; i < 128*128; ++i) y_buf[i] = uyvy[2*i + 1];
    ```
    ~16 µs per frame on the Cortex-A53.
  * **T1 stride-2 load:** `vlse8.v v8, (cb.pa+1), 2` — the strided
    LSU variant. **Untested on the 2D fabric** (flagged § 10.1). If
    you try this, file a probe test result.
  * **T1 vrgather extraction:** load 2 vregs full of UYVY bytes,
    then `vrgather.vv` with a fixed odd-index pattern to keep
    every second byte. Adds 1 extra issue and an index-vector
    setup.

**Target state (better fit, requires BD rebuild):** if the BD has
`HAS_Y_UV8_420 = 1` (NV12 family), frmbuf produces NV12 in memory:

```
NV12 buffer layout (128×128 frame):
  bytes [0      .. 16383]    : Y plane (128 rows × 128 cols, contiguous)
  bytes [16384  .. 24575]    : UV plane (64×64 interleaved, half-res chroma)
```

Then your kernel does `vle8.v v8, (cb.pa)` for the 128×128 greyscale
view — zero pre-pass, zero stride-load, zero vrgather. The chroma
bytes after offset 16384 are ignored if you only consume `vl * 128
= 16384` bytes per issue.

Until the NV12 rebuild lands, write your kernels assuming you have
a contiguous Y buffer at some PA (call it `y_pa`). For development
today, use `t1_buf_alloc` + dummy data populating that buffer. For
production once the rebuild lands, `y_pa = cb.pa`. For production on
the current UYVY pipeline, do the CPU pre-pass into a `t1_buf` and
use that buffer's `.pa`. Code path is the same in all three cases.

---

## 6. Programming model — the patterns that work

### 6.1 Issue one instruction at a time via libt1

The libt1 API is **one-instruction-at-a-time**. You build a
`struct t1_op`, populate `.instruction` (the 32-bit RISC-V V word),
`.rs1` (typically a PA), `.rs2`, `.vtype`, `.vl`, and call
`t1_issue(&op)`. The function blocks until the wrapper acks the
issue (FIFO-ready, not retire-complete). For LSU instructions you
typically follow up with `t1_lsu_wait()` (built into `t1_issue`
when applicable) to drain in-flight loads/stores before the next
issue.

The canonical pattern for a 4-issue kernel (load → store-to-sp →
load-from-sp → store-to-ddr):

```c
struct t1_op op = { .vtype = T1_VTYPE_E8_M4_TA_MA, .vl = 128 };

op.instruction = 0x02050407u;  op.rs1 = src.pa;     // vle8.v  v8, (a0)
t1_issue(&op);
op.instruction = 0x02050427u;  op.rs1 = sp.pa;      // vse8.v  v8, (a0) → BRAM
t1_issue(&op);
op.instruction = 0x02058607u;  op.rs1 = sp.pa;      // vle8.v  v12, (a1) ← BRAM
t1_issue(&op);
op.instruction = 0x02058627u;  op.rs1 = dst.pa;     // vse8.v  v12, (a1) → DDR
t1_issue(&op);
```

The 32-bit instruction words come from `riscv64-linux-gnu-as`.
There's a helper script `vision_software/libt1/build_kernel.sh` that
takes a `.S` file and emits a C header with the encoded words as
constants:

```sh
cd vision_software/libt1
./build_kernel.sh ../visionsoc_main/kernels/my_kernel.S \
                  ../visionsoc_main/kernels/my_kernel.h my_kernel
```

After that you can `#include "my_kernel.h"` and pull instructions from
the `my_kernel[]` array.

### 6.2 Setting up VTYPE and VL once

For the canonical 128×128 8-bit kernel you always want
`vsetvli zero, 128, e8, m4, ta, ma` — that gives `vl = 128` (one
image row per vector register group) at LMUL=4 over 8-bit elements.
The constant `T1_VTYPE_E8_M4_TA_MA = 0x000000C2` encodes that vtype.
**LMUL stays at 4** for any kernel touching the 2D fabric — see
`2d_fabric_handoff.md` § 3.3 for why other LMULs misbehave.

### 6.3 Vertical mode toggle in real kernels

Wrap CSR writes in a tiny inline-asm sequence and issue them as
part of the kernel stream:

```asm
li    t3, 1
csrw  0x7c0, t3        # vertical mode on
vle8.v v8, (a0)        # row-major load, transposed VRF layout
csrw  0x7c0, zero      # back to horizontal
vadd.vv v8, v8, v8     # per-row doubling under horizontal-mode read
```

The CSR snapshot is taken **at issue time** and travels with the
instruction. You can flip it freely between issues without worrying
about in-flight LSU drains.

### 6.4 Cache coherency around T1 ↔ DDR

ARM Cortex-A53 caches are coherent with HPC0 (the cache-coherent HP
port — that's why we wired T1's hb to HPC0 not HPM). But Linux's
udmabuf mmap is *cached* by default, and `msync()` does not reliably
flush udmabuf pages. **You must use `t1_buf_sync_for_{cpu,device}`**:

```c
// CPU just wrote to src.va:
t1_buf_sync_for_device(&src);   // L1/L2 flush — DDR now sees the writes
// T1 reads src.pa, writes dst.pa
t1_buf_sync_for_cpu(&dst);      // L1/L2 invalidate for dst — CPU now sees T1's writes
```

Skipping these is the #1 source of "T1 wrote zeros" symptoms that
turn out to be cache staleness. Read
`fyp_doc/camera_bringup_status.md` § 6.4 for the full diagnosis trail.

For V4L2 camera buffers, the V4L2 framework handles cache management
internally during `VIDIOC_DQBUF` / `VIDIOC_QBUF`. You don't need to
call `t1_buf_sync_*` on those.

For scratchpad (`t1_scratchpad_alloc`), there's no CPU cache in play
(the BRAM is in the PL, behind UIO mmap that's mapped uncached). The
`t1_buf_sync_*` calls are no-ops in that case but harmless.

### 6.5 v0 mask under horizontal vs vertical mode

This is subtle and a common footgun. The full treatment is in
`2d_fabric_handoff.md` § 4.2 (read it); the short version:

**The v0 bit map is mode-agnostic. The data it gates is not.**

  * v0 is a packed bit vector indexed by element/lane number. Bit
    `(c, r)` always gates lane c of hw-row r. There is no
    V-scatter on the mask-read path; mask consumers see the same bit
    layout in horizontal and vertical mode.
  * What changes between modes is which *image pixel* maps to lane
    `(c, r)`. With horizontal-mode load + horizontal-mode consumer,
    lane `(c, r)` = `grid[r][c]`. With horizontal-mode load + a
    vertical-mode consumer, lane `(c, r)` = `grid[c][r]` (the
    transpose-of-the-data, not of the mask).
  * **Same-mode is safe.** If your `vle8` and the masked op (e.g.
    `vadd.vv v8, v8, v9, v0.t`) both run in the same mode, the same
    v0 bit gates the same image pixel. Reuse the bitmap freely
    across instructions inside a same-mode block.
  * **Mode mismatch reveals the data transpose.** vle in H then mask
    in V (or any cross-mode combination) gates the transposed image,
    not the original. If that's what you want (the mask is a
    spatial pattern that should follow the data transpose),
    reuse it. If not, rebuild the mask in the new mode's
    orientation.

**Recipes that build mode-invariant masks (safe to share across modes):**

```asm
# Recipe A: "every Nth column" style — purely index-driven
vid.v   v20
vmseq.vi v0, v20, 5       # bit set on lane 5 of every hw-row → column-5 mask

# Recipe B: image-derived mask, kept entirely in one mode
li    t3, 0
csrw  0x7c0, t3           # horizontal
vle8.v v20, (mask_buf_pa) # load a byte-image of mask seeds
vmsne.vi v0, v20, 0       # v0 = (pixel != 0); bits encode H-orientation pixels
```

**Recipe that builds a mode-sensitive mask (its content embeds the build mode):**

```asm
li    t3, 1
csrw  0x7c0, t3           # vertical
vle8.v v8, (img_pa)       # v8's lane-(c,r) sees grid[c][r] (transpose)
vmsne.vv v0, v8, v9       # v0's bits encode the V-mode view
li    t3, 0
csrw  0x7c0, t3           # back to horizontal
# At this point v0 is still a bitmap, but its CONTENT was computed
# from the V-mode-view of v8/v9. If you now use v0 in a horizontal
# data-path op, the bits target the H-mode view of subsequent loads —
# the gated pixels will be the transpose of what you might "expect".
```

**The decision rule when sharing v0 across a mode flip:** ask *"is
the 2D orientation that the new mode will surface the orientation I
want?"* If yes, share. If no — for example, a face-shaped mask built
in H mode that you're about to use to gate data in V mode where it
would gate a rotated silhouette — rebuild a fresh v0 in the new
mode's data orientation. The mask gating itself is always correct;
it's the image layout under the mask that flips.

Cross-references: `t1/src/mask/MaskUnit.scala` (predicate read path,
no V-scatter) and `t1/src/vrf/SharedVRF.scala` (the data-path
V-scatter that creates the apparent transpose).

---

## 7. The example library — read these before writing new kernels

Three layers of "kernel-like" code exist in the tree:

### 7.1 Probe tests — `vision_software/libt1/test/*.c`

Native ARM64 user-space programs that issue a small T1 sequence and
verify the result. These are your **first** working examples — they
build native on the Kria and run as `./test/sp_*_probe`. The most
useful for kernel-development bootstrapping:

  * `triage_t1` — checks the wrapper register interface (no vector ops)
  * `smoke` — issues `vle8/vse8` round-trip on DDR
  * `ddr_roundtrip` — 4-issue kernel: DDR → vreg → DDR
  * `port_grid_vadd` — full `grid_vadd` kernel via the `.h` file
  * `port_grid_vadd_scratchpad` — same but with BRAM
  * `sp_v12_compute_probe` — F4-validated DDR ↔ scratchpad ↔ DDR
  * `vert_lsu` — verifies vertical-mode load + horizontal-mode store
    (the transpose-via-CSR pattern)

To build on the Kria: `cd ~/vision_software/libt1 && make`.

### 7.2 Real kernels — `vision_software/visionsoc_main/kernels/*.S`

The `.S` files are assembly kernels you assemble to instruction words
with `build_kernel.sh`. They're meant to be called from C in
`visionsoc_main` once per frame.

Current kernels:

  * `grid_vadd.S` — `vle/vle/vsub/vse`: the simplest "compute on two
    inputs, write one output" pattern. Read this first.
  * `scratchpad_self.S` — exercises the F4 scratchpad path
    (DDR → sp → sp → DDR).

### 7.3 Synthetic / simulator vision programs — `tests/vision_program/*`

These are RISC-V binaries built with the verilator/emurt toolchain
for simulator testing, **not** the libt1-based on-Kria path. They are
the right place to look for *kernel algorithm reference* — they have
high-fidelity C implementations of operations like `gaussian_blur`,
`sobel_edge`, `cnn_digit`, `matvec_fc_relu`, `matmul_via_vt`. Port
the algorithmic insight from these to native libt1 kernels for
hardware execution; see § 8 for examples.

---

## 8. What the 2D fabric makes easy (and what to invent)

This is the most useful section for someone exploring new kernel
styles. **Stop and read `2d_fabric_handoff.md` first if you have not.**

### 8.1 The "outer product" intuition

A 2D image is naturally an outer product of row and column indices.
The fabric's 128 hw-rows × 128 lanes-per-row gives you a literal
outer-product machine: **operations along the row index** run in
horizontal mode (per-row ops, "left-right"), and **operations along
the column index** run in vertical mode (cross-row ops, "up-down"
relative to the original image). Equivalently, vertical mode is a
*free, on-the-fly transpose* of the VRF view.

That gives you several things stock RVV would force you to either
serialise (do row pass, then transpose memory, then do column pass)
or precompute (build a separate transposed copy of the input):

  * **Separable convolutions** (gaussian_blur, sobel, box filter,
    1D LP/HP cascades): horizontal pass + CSR flip + vertical pass.
    Single LSU round-trip, two compute passes back-to-back in the
    VRF. The `gaussian_blur` reference in
    `tests/vision_program/gaussian_blur/gaussian_blur.c` shows the
    pattern.
  * **2D neighbourhood ops** (Sobel, Laplacian, structure tensor):
    each `vslideup`/`vslidedown` moves the entire image one column
    (horizontal mode) or one row (vertical mode) without touching
    DDR.
  * **2D reductions** (column sums, row sums, integral images):
    horizontal mode's reductions hit one element per row (see
    `2d_fabric_handoff.md` § 4.1 — the per-row scalar lands in
    `v[0]` of each hw-row). For column sums, vertical mode does the
    same thing transposed.

### 8.2 Patterns to look for in your kernel design

When you sketch a new kernel, ask:

  * **Does the data flow stay inside the VRF for >1 instructions?**
    If yes, you avoid DDR bandwidth and get N× speedup over a
    stock-RVV implementation that materialises every intermediate
    through L1/L2 caches. (The non-2D path doesn't have an LMUL>1
    register group efficient enough to hold a full intermediate.)
  * **Can you express your work as "shift + add + mask" in one mode,
    flip CSR, do the same in the other mode?** That is the
    separable-filter pattern.
  * **Does your kernel need a transpose?** On stock RVV that's a
    `tests/vision_task/transpose_*` kernel that's painful (multiple
    loads, strided stores). On the 2D fabric it's a `csrw 1`
    around a vle/vse pair, no extra LSU traffic.
  * **Are you doing per-row independent processing?** If yes, you
    are paying for 1× the work and getting 128×. This is the easiest
    win for ports of stock CPU vision code.

### 8.3 Unexplored / promising kernel ideas

These are the ones we have not yet implemented but are first-class
fits for the architecture. Pick one to prototype:

  1. **Bilateral filter** — separable + per-row range table. The
     range weight lookup is a `vrgather.vv` (with LMUL=4 disjoint-group
     rules from `2d_fabric_handoff.md` § 3.4) and the spatial weight
     is a separable Gaussian.
  2. **Census transform / BRIEF descriptor** — central pixel vs N
     offset pixels, encoded as a bit per offset. Vertical mode
     gives you offsets along the column axis for free; horizontal
     mode gives offsets along the row axis. Result: 8 or 16 bits
     per pixel image of descriptors after one LSU round-trip.
  3. **Local histogram / soft segmentation** — vertical-mode
     cumulative sum (CSR=1 + `vredsum.vs` per row gives per-column
     totals) makes integral image cheap; from there local box-sum
     queries are constant cost.
  4. **2D vrgather permutation kernels** — non-trivial spatial
     remapping (rotate, fisheye correction, lens distortion undistort)
     becomes a per-row index vector + `vrgather.vv` in horizontal
     mode, then a per-column index vector + `vrgather.vv` in
     vertical mode. The hard part is generating the index vectors
     compactly; vrgather supports 16-element-window granularity (see
     `fyp_doc/vrgather_vx_debug_handoff.md`).
  5. **Multi-frame fusion** — load frame N into v8 (horizontal), load
     frame N-1 from a second buffer into v12, vsub for temporal
     difference, threshold via `vmsgt`-style mask ops, write a
     binary motion mask. Whole pipeline in 4-6 issues per frame.
  6. **Sparse/skipped compute via masks** — the `v0`-mask path on
     `vadd.vv` etc. is per-element per-hw-row. You can compute a
     "interesting pixel" mask cheaply and have subsequent passes
     touch only those pixels. Useful for stereo matching candidate
     selection or feature extraction prefilters.

### 8.4 Anti-patterns

Some intuitions transfer poorly from stock RVV — re-derive these from
first principles, don't assume:

  * **"vl=1 stores"** are not "store one byte". They store byte 0
    from every hw-row — i.e. one *column* of the image. See § 4.1 of
    `2d_fabric_handoff.md` for the exact LSU pitch behaviour.
  * **Reductions across the full image** are not free. `vredsum.vs`
    is per-hw-row only (gives you a column of 128 row-sums). To get
    a single scalar over the whole 128×128 image you need a second
    reduction pass — typically a horizontal `vredsum.vs` on the
    `v[0]` column to fold the 128 row-sums.
  * **Auto-vectorisation in C init code** at -O2 emits LMUL=8 and
    writes 128 copies of `vid` per memory write. Use `volatile
    int8_t *` casts for any data the kernel will read; see
    `2d_fabric_handoff.md` § 3.1.

---

## 9. The host ↔ board iteration loop

The fast development cycle is:

1. **Edit C/asm on the dev host.** The repo is at
   `~/code/code_fyp/VisionSoC/` (this clone).
2. **`scp -r` the changed files to the Kria.** Typical:
   ```sh
   scp -r vision_software/libt1/test/my_new_probe.c \
          vision_software/libt1/Makefile \
          kv260:~/vision_software/libt1/
   ```
3. **Build natively on the Kria.** SSH in, `cd ~/vision_software/libt1
   && make`. The compile is fast (~5 s for one new probe).
4. **Run.** `sudo ./test/my_new_probe`. The test binary is on the
   sudoers allowlist, no password prompt.
5. **Inspect.** `dmesg` for any kernel events; the binary itself
   prints PASS/FAIL.

For complete vision applications (multi-stage pipelines):

  * Build `visionsoc_main`: `cd ~/vision_software/visionsoc_main && make`.
  * Run: `sudo ./visionsoc_main`. It opens `/dev/video0` for camera
    (or fail-graceful if camera isn't up), issues a kernel per frame,
    and pushes to the HDMI display via the PS DP. See § 5 for the
    camera details.

### 9.1 Useful inspection commands

```sh
# Wrapper alive + register-plane sanity:
sudo ./test/triage_t1

# What's currently loaded in PL:
ls /sys/kernel/config/device-tree/overlays/
cat /sys/class/uio/uio*/name
cat /sys/class/uio/uio6/maps/map0/addr   # scratchpad PA — sanity check

# kernel events (V4L2 probe, I2C, DMA errors):
sudo dmesg | tail -30
```

### 9.2 When the kernel panics

The Kria can SError-panic if T1 hits an unrouted address (rare for
well-formed kernels) or if a V4L2 driver writes to an unrouted CSR
(this is the 5o build's reason for existing — see
`fyp_doc/camera_bringup_status.md` § 6.3). After a panic, the board
silently reboots; SSH reconnects in 60-120 s; the visionsoc overlay
is GONE (k26-starter-kits comes back on boot); you re-run the
`fpgautil -b ... -o ...` from § 2.3.

---

## 10. Required reading, ranked

If you only read three docs:

  1. **`fyp_doc/2d_fabric_handoff.md`** — programmer's contract for
     the 2D fabric. Non-negotiable.
  2. **`fyp_doc/LSU_vertical_mode_handoff.md`** — exactly how
     CSR 0x7c0 interacts with vle/vse, the gate formula, fence
     semantics. Skim if you won't use vertical mode, read in full if
     you will.
  3. **`fyp_doc/driver_function_spec.md`** — libt1 API contract
     (`t1_init`, `t1_buf_alloc`, `t1_issue`, etc.). The reference for
     "how do I do X from C".

If you go deeper:

  * `fyp_doc/camera_bringup_status.md` — for camera-side issues, V4L2
     wiring, AP1302 quirks. Needed only if you're touching the
     capture path.
  * `fyp_doc/fpga_build_status.md` — for FPGA build status, BD-level
     iteration. Mostly hardware-engineer territory.
  * `fyp_doc/vrgather_vx_debug_handoff.md` — when you need permutation
     kernels, this is the canonical reference for vrgather quirks.
  * `fyp_doc/lmul_register_increase.md` — context on why LMUL=4 is the
     ceiling.

For the underlying T1 architecture (the Scala source of truth):

  * **Repo:** Chipyard-flavoured T1 in this monorepo under `t1/src/`.
  * **Start with `t1/src/T1.scala`** for the top-level wiring, then
     `t1/src/vrf/SharedVRF.scala` for the 2D banking + diagonal scatter
     (this is where the vertical-mode VRF transpose physically lives).
  * `t1/src/lsu/` has the LSU pipeline; the per-instruction
     vertical-mode capture machinery is documented in
     `LSU_vertical_mode_handoff.md` and lives in `t1/src/lsu/LSU.scala`.
  * **External:** RISC-V V extension 1.0 spec is the
     [riscv-v-spec PDF](https://github.com/riscv/riscv-v-spec/releases).
     Anything stock-RVV in this doc is in there. Anything 2D-fabric
     is *not* in there.

### 10.1 What's NOT in any doc yet

If you find yourself needing these and they aren't documented,
either ask the user or add to the doc when you've figured it out:

  * Stride-2 / strided-element vle/vse semantics on the 2D fabric.
    Stock RVV has `vlse8.v` (with stride in rs2) but the 2D
    interaction is untested. UYVY chroma split is the obvious use.
  * Mask-pipe LMUL=8 behaviour (forbidden per § 3.3 but the failure
    mode isn't pinned down to specific instruction families).
  * Vertical-mode reductions (`vredsum.vs` under CSR 0x7c0=1): does
    the per-row scalar end up at v[0] in the transposed layout? Not
    test-covered.

---

## 11. Quick-start checklist (paste-able)

```sh
# 1. Connect
ssh kv260

# 2. Deploy 5m (the kernel-dev bitstream)
sudo install -m 644 /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin.5m-backup \
                    /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin
sudo rmdir /sys/kernel/config/device-tree/overlays/k26-starter-kits_image_1 2>/dev/null
sudo fpgautil -b /lib/firmware/xilinx/visionsoc/system_top_wrapper.bit.bin \
              -o /lib/firmware/xilinx/visionsoc/system_top_wrapper.dtbo

# 3. Sanity check
cd ~/vision_software/libt1 && make
sudo ./test/sp_v12_compute_probe
# Expect: PASS: sp_v12_compute_probe (...)

# 4. Now write your kernel.
# Start by copying test/sp_v12_compute_probe.c, rewrite issue_*() with
# your new instruction sequence, recompile, run. Iterate.

# 5. When the camera path is ready (5o or later), wire camera_dequeue
# from vision_software/visionsoc_main/camera.c — your kernel takes a
# DDR PA which is exactly what cb.pa gives you.
```

That's the whole loop. Have at it.
