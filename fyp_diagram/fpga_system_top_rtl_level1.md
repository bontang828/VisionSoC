# VisionSoC FPGA — RTL Block Diagram, Level 1

Open `fpga_system_top_rtl_level1.drawio` in <https://app.diagrams.net> or
the desktop draw.io app. The previous `fpga_system_top_rtl.drawio` is
superseded.

## Three-level architecture (planned)

The system is split into three diagrams so each one stays readable. **Only
Level 1 is generated for now.** Levels 0 and 2 are described below for
context; let me know when to generate them.

| Level | Audience | Block count | Scope |
|---|---|---:|---|
| **0** — system overview | high-level (FYP intro slide) | 5–7 | KV260 board: AR1335 + AP1302 + FPGA SoC (PS + PL) + DDR4 + DisplayPort + UART |
| **1** — FPGA top-level | this diagram | 10 + 2 ext | First layer of the synth hierarchy, grouped into 3 PL subsystems (control / camera / compute) plus PS and external sensor |
| **2** — T1 microarchitecture | RTL-detail (FYP architecture chapter) | 8–12 | Inside `t1_top`: VRF, Lane × N, MaskUnitFpga, LSU, decoder, chaining/replay, AXI-Lite wrapper, m_axi_hb / m_axi_idx — drawn from `t1/src/T1.scala` |

## Level 1 layout rules followed

| Rule | How it's applied here |
|---|---|
| Left-to-right architecture | External sensor (left) → FPGA PL (center) → PS (right). Data flows L→R. |
| Grid-based, fixed columns | 4 inner PL columns at x = 360 / 620 / 880 / 1140 (180 wide, 80 gap). 3 horizontal subsystems at y = 110 / 300 / 520. |
| No overlapping boxes | All leaf blocks live in distinct grid cells; subsystem rectangles only contain their own leaf blocks. |
| Large boxes, short names inside | 180×80 (control), 180×100 (camera), up to 180×290 (T1, Data IC, URAM). Long-form names only in the body of each block. |
| Orthogonal elbow connectors | Every edge uses `edgeStyle=orthogonalEdgeStyle` and `rounded=0`. No diagonals. |
| Spacing ≥ 80 px horiz / 60 px vert | Horizontal gap 80 between leaf blocks and between containers; vertical gap 60 between subsystems and between DMA/skid stack. |
| Group into containers | `c_ext`, `c_pl` (with three nested `sub_*` containers), `c_ps`. |
| Avoid crossings | T1 → Data IC routed through the 70 px gap between DMA and DMA-skid; Data IC → PS HP routed through the 60 px gap above compute-band blocks; control fan-out routed through inter-band gaps. |
| Data path horizontal, control top | Control subsystem occupies the **top** band; camera + compute data planes are horizontal in the middle and bottom bands. |
| Memory on one side | PS / DDR4 is on the right edge; URAM scratchpad is the rightmost block in the compute band. All memory writes land on the right. |
| External outside FPGA container | `c_ext` (AR1335 + AP1302) and `c_ps` are outside `c_pl`. |
| Don't show every signal | Only 17 edges. CSR fan-out is shown as 2 arrows landing on subsystem boundaries, not 1 arrow per CSR. IRQ is bundled into one `pl_ps_irq0[5:0]` line. |

## Blocks (Level 1)

10 PL blocks + 2 external + 6 PS sub-blocks = 18 total leaf rectangles, organised inside 3 nested subsystem containers.

| Container | Block | Wraps synth-report cells |
|---|---|---|
| PL ▸ control | Sensor IIC | `sensor_iic` |
| PL ▸ control | Control IC | `smartconnect_ctrl` + `smartconnect_lpd` |
| PL ▸ camera | MIPI CSI-2 RX | `mipi_csi2_rx` |
| PL ▸ camera | AXIS Capture | `axis_data_fifo_cap` + collapsed `axis_subset_converter_cap` |
| PL ▸ camera | Frame Buffer WR | `v_frmbuf_wr` |
| PL ▸ camera | Video IC | `smartconnect_video` |
| PL ▸ compute | T1 Vector Core | `t1_top` (+ `axi_reg_slice_hb` absorbed) |
| PL ▸ compute | AXI DMA | `axi_dma` |
| PL ▸ compute | AXIS Skid | `axis_reg_slice_dma` |
| PL ▸ compute | Data IC | `smartconnect_hb` + `smartconnect_idx` |
| PL ▸ compute | URAM Scratchpad | `bram_ctrl` + `bram` (uram_scratchpad) |
| External | AR1335, AP1302 | (off-chip) |
| PS | ARM Cortex-A53, HPM port, DDR4, HP port, IRQ port, EMIO port | (Zynq hard logic) |

## Edges (Level 1)

Only the buses that cross subsystem boundaries are drawn — internal CSR wires
inside a subsystem are not shown.

**Data path (thick black, open arrow):**
1. AR1335 → AP1302 — Bayer (parallel)
2. AP1302 → MIPI CSI-2 RX — MIPI CSI-2, 4 lanes @ 896 Mbps
3. MIPI CSI-2 RX → AXIS Capture — AXIS 16 b @ 300 MHz
4. AXIS Capture → Frame Buffer WR — AXIS 24 b
5. Frame Buffer WR → Video IC — AXI4 128 b
6. Video IC → PS HP — AXI4 128 b @ 300 MHz → `S_AXI_HP1_FPD`
7. T1 → Data IC — `m_axi_hb` 128 b + `m_axi_idx` 32 b
8. AXI DMA → Data IC — `M_AXI_MM2S` / `S2MM` 128 b
9. AXI DMA ⇄ AXIS Skid — `M_AXIS_MM2S` / `S_AXIS_S2MM` (loopback)
10. Data IC ⇄ URAM Scratchpad — AXI4 128 b @ `0xA008_0000` (memory interface, double-line)
11. Data IC → PS HP — AXI4 128 b @ 60 MHz → `S_AXI_HPC0_FPD` / `HP0_FPD`

**Control path (thin grey, open arrow):**
12. PS HPM → Control IC — `M_AXI_HPM0_FPD/LPD`, AXI4-Lite @ 60/100 MHz
13. Control IC → Sensor IIC — AXI4-Lite @ `0xA005_0000`
14. Control IC → Camera subsystem — AXI4-Lite CSR fan-out (`csi2`, `frmbuf`)
15. Control IC → Compute subsystem — AXI4-Lite CSR fan-out (`t1`, `dma`)

**Sideband (dashed):**
16. Sensor IIC ⇄ AP1302 — I²C @ 400 kHz
17. PS EMIO → AP1302 — `ap1302_rst_b` (`emio_gpio_o[1]` via `xlslice`)
18. Compute subsystem → PS IRQ — `pl_ps_irq0[5:0]` (bundled)

## Notes for the FYP write-up

- The page is **1700 × 920 px**, designed for a 16:9 slide. For A4 landscape
  printing, export to PDF at *Fit page* and the diagram scales to ≈ 92 %.
- All boxes use a single sans-serif family; sizes are 16 pt (title), 14 pt
  (container titles), 12–13 pt (leaf blocks), 11 pt (edge labels). No
  emoji, no colour beyond white / grey / black.
- Buses are labelled on the edge, not in the box.
- `axi_reg_slice_hb` and `axis_subset_converter_cap` are absorbed into
  their neighbour for clarity — they're register slices / TDATA padders
  with no architectural meaning at this level.

## Next levels (on request)

- **Level 0 — system overview:** AR1335 + AP1302 + KV260 SoC (PS + PL one
  block each) + DDR4 + DisplayPort. Roughly 5 blocks, two arrows.
- **Level 2 — T1 microarchitecture:** open `t1_top` and show the chaining
  ring, the lane array, MaskUnitFpga + `v0_bram`, LSU, SharedVRF banks,
  decoder, AXI-Lite wrapper, `m_axi_hb` / `m_axi_idx` master ports.
  Source: `t1/src/T1.scala` and the `MaskUnitFpga` handoff doc.
