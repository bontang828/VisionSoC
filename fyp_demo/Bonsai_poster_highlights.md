# BONSAI — A Vision Processor built on the open RISC-V architecture

**Authors:** Bon Tang, Nicholas Fry, Shinjeong Kim, Andrew J. Davison, Paul H. J. Kelly
**Institution:** Imperial College London
**Contact:** cbt22@ic.ac.uk / tangcbon@gmail.com

## One-line summary
BONSAI is a 2D RISC-V Vector (RVV) processor that treats an entire image as a single vector register, enabling on-sensor vision processing with standard RVV instructions, a square 2D register file with free transpose, and live FPGA demos at 30 FPS.

## Core concept
RISC-V is an open, royalty-free instruction set; its vector extension (RVV) implements a standard SIMD programming model. BONSAI's on-sensor prototype treats a vector register as an image (or bit plane) which is processed as a single instruction. The goal is to inherit the open tooling ecosystem of RISC-V while providing vision-specific hardware acceleration.

---

## Software Highlights

### 1. An image is one vector register
- 2D register fabric: every instruction is broadcast across 128 hardware rows at once.
- The image's natural 2D shape maps directly onto the register's shape — no flattening to 1D required.

### 2. Easy programming via standard assembly
- One instruction performs one operation, horizontally or vertically, across the whole register.
- Example code:
  ```
  vle8.v v8, (frame)   // pull in the entire image — 16,384 px
  vadd.vv v8, v8, v8    // one op brightens every pixel
  ```

### 3. Open source, standard Vector ISA
- Plain RISC-V Vector (RVV) extension, compiled with stock LLVM / GCC.
- Code is portable to any RVV-compliant chip — no proprietary toolchain.

### 4. A whole open ecosystem
- Compile with LLVM/GCC.
- Simulate with Verilator.
- Inherits the broader RISC-V open-source toolchain rather than requiring bespoke tools.

---

## Hardware Highlights

### 1. On-sensor processing, one silicon die
- The RVV 2D Engine sits on the same die as the image sensor.
- The image stays resident in the Vector Register File (VRF) throughout processing — only the final answer leaves the chip (privacy/bandwidth benefit).

### 2. Square 2D register file
- Register file is organized as a square grid (matching image dimensions) rather than a 1D array.
- Diagonal banking means a column read = a row read — transpose is effectively free (no separate transpose hardware/instructions needed).

### 3. Compute across row and column
- A single Config Register flip switches the sweep direction (row-wise or column-wise).
- This makes it easy to apply 2D filters in either orientation without restructuring data.

### 4. Scalable vector engine
- Parallel execution lanes are scalable depending on available silicon area.
- Operations are time-multiplexed across the 128×128 pixel grid (demonstrated with 2 lanes, replayed ×128).

---

## Live Demo Highlights

### 1. Real-time camera → display FPGA pipeline
- Hardware: AMD Kria KV260.
- 128×128 pixel grid.
- 30 FPS HDMI output.
- Programmed entirely in plain C on Linux.

### 2. MatMul → Attention, MLP, etc.
- int8 128×128 matrix multiplication, accelerated via the free transpose and dot-product structure of the square register file.
- Demonstrated as the building block for attention mechanisms (A × Bᵀ = C).

### 3. Dense optical flow
- Per-pixel motion shown as colour-coded direction.
- Computed via per-row Sum of Absolute Differences (SAD) + arg-min, every pixel processed in parallel.

---

## Key differentiators (for narrative/marketing use)
- **2D-native, not 1D-unrolled:** unlike conventional vector processors that flatten images into 1D streams, BONSAI's register shape matches the image shape directly.
- **Free transpose:** diagonal banking eliminates the need for explicit transpose instructions or hardware, useful for both vision kernels and matrix math (e.g., attention).
- **Data stays on-chip:** only computed results leave the silicon, relevant for privacy-sensitive or bandwidth-constrained sensor applications.
- **Standards-based:** built entirely on open RISC-V RVV instructions — no proprietary ISA extensions — so it inherits existing compiler/toolchain support.
- **Proven on real hardware:** not just simulated — running live at 30 FPS on an AMD Kria KV260 FPGA with real camera input.