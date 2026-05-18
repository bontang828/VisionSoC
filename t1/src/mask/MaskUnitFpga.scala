// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2022 Jiuyang Liu <liu@jiuyang.me>
//
// MaskUnitFpga: FPGA-optimised parallel to MaskUnit.scala.
//
// Drop-in replacement (identical MaskUnitInterface IO) selected at the T1
// instantiation site when T1Parameter.useFpgaMaskUnit is true. The original
// MaskUnit.scala is preserved unchanged as the simulator path.
//
// Current phase landed in this file:
//   Phase 3b -- Opt 3: BRAM-back v0Vec + active-row register cache.
//     Replaces the per-hardware-row FF replication of v0Vec
//     (MaskUnit.scala:194-198 -- 128 rows x vLen bits = 131,072 FFs at
//     vLen=1024) with a v0_bram blackbox + a single vLen-bit v0Cache
//     register kept in sync with gatherRowCounter via a 1-cycle refill
//     FSM. Lane v0Update writes go write-through to BRAM and, when they
//     target the cached row, also update v0Cache. A pendingWrite shadow
//     covers the race where a write fires during a refill for the same
//     row (BRAM port B doesn't observe port A in the same cycle).
//
// Phases NOT yet landed in this file (will be added in later edits):
//   Phase 3c -- Opt 1: On-demand per-lane mask slicer (replace the three
//     parallel SEW-specific cutUInt+grouped+transpose+Mux1H trees with a
//     single runtime-parameterised slicer).
//   Phase 3d -- Opt 2: Pipelined slide write-mask trees (segment-walk
//     the wide scanRightOr/scanLeftOr over vLen bits into 8x128-bit
//     cycles).
//   Phase 3e -- Opt 7: Pipelined v0 slide barrel shifters (reuse a
//     single 128-bit shifter over 8 cycles for each of slideUpV0 /
//     slideDownV0).
//
// See fyp_doc/maskunit_fpga_handoff.md for the full plan and verification
// matrix.

package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.experimental.SerializableModule
import chisel3.experimental.hierarchy.{instantiable, public, Instance, Instantiate}
import chisel3.properties.{AnyClassType, ClassType, Property}
import chisel3.util._
import org.chipsalliance.dwbb.stdlib.queue.{Queue, QueueIO}
import org.chipsalliance.stdlib.GeneralOM
import org.chipsalliance.t1.rtl.decoder.Decoder

/** Chisel BlackBox wrapping v0_bram.v.
  *
  * Single-clock, simple dual-port BRAM: port A is write-only with per-byte
  * strobe (lane v0Update path); port B is read-only with READ_LATENCY_B=1
  * (active-row refill path). MEMORY_INIT_PARAM="0" in the Verilog matches
  * the RegInit(0) semantics of the original FF-backed v0Vec.
  *
  * The Verilog source is shipped in two locations:
  *   - t1/resources/v0_bram.sv -- consumed via HasBlackBoxResource by
  *     Chisel for the t1emu / Verilator path.
  *   - fpga/wrapper/v0_bram.v  -- referenced by fpga/system/system_top.tcl
  *     for the Vivado FPGA synthesis path (nix does not include the fpga/
  *     tree in the Chisel build source, so a duplicate is needed).
  * Keep both copies in sync.
  */
class V0BramBlackBox(addrWidth: Int, dataWidth: Int)
    extends BlackBox(
      Map(
        "DATA_WIDTH" -> dataWidth,
        "ADDR_WIDTH" -> addrWidth,
        "MEM_DEPTH"  -> (1 << addrWidth)
      )
    )
    with chisel3.util.HasBlackBoxResource {
  override def desiredName: String = "v0_bram"
  val io = IO(new Bundle {
    val clk:     Clock = Input(Clock())
    val wr_en:   Bool  = Input(Bool())
    val wr_addr: UInt  = Input(UInt(addrWidth.W))
    val wr_data: UInt  = Input(UInt(dataWidth.W))
    val wr_strb: UInt  = Input(UInt((dataWidth / 8).W))
    val rd_en:   Bool  = Input(Bool())
    val rd_addr: UInt  = Input(UInt(addrWidth.W))
    val rd_data: UInt  = Output(UInt(dataWidth.W))
  })
  // `.sv` extension is required by the nix mlirbc-to-sv post-processing,
  // which globs `*.sv` to build filelist.f for Verilator. The
  // FPGA-synthesis copy in fpga/wrapper/v0_bram.v keeps `.v` for the
  // Vivado add_files path; both files have identical content.
  addResource("/v0_bram.sv")
}

@instantiable
class MaskUnitFpgaOM(parameter: T1Parameter) extends GeneralOM[T1Parameter, MaskUnitFpga](parameter) {
  val compress   = IO(Output(Property[AnyClassType]()))
  @public
  val compressIn = IO(Input(Property[AnyClassType]()))
  compress := compressIn
}

@instantiable
class MaskUnitFpga(val parameter: T1Parameter)
    extends FixedIORawModule(new MaskUnitInterface(parameter))
    with SerializableModule[T1Parameter]
    with ImplicitClock
    with ImplicitReset {

  val omInstance: Instance[MaskUnitFpgaOM] = Instantiate(new MaskUnitFpgaOM(parameter))
  io.om := omInstance.getPropertyReference

  override protected def implicitClock: Clock = io.clock
  override protected def implicitReset: Reset = io.reset

  val instReq          = io.instReq
  val exeReq           = io.exeReq
  val exeResp          = io.exeResp
  val tokenIO          = io.tokenIO
  val readChannel      = io.readChannel
  val readResult       = io.readResult
  val writeRD          = io.writeRD
  val lastReport       = io.lastReport
  val laneMaskInput    = io.laneMaskInput
  val askMaskVec       = io.askMaskVec
  val v0UpdateVec      = io.v0UpdateVec
  val writeRDData      = io.writeRDData
  val gatherData       = io.gatherData
  val gatherRowDone    = io.gatherRowDone
  val gatherRowCounter = io.gatherRowCounter
  val gatherRead       = io.gatherRead

  io.tokenIO.foreach { tk =>
    tk.maskRequestRelease := true.B
  }

  val readQueueSize:          Int = 4
  val readVRFLatency:         Int = 3
  val maskUnitWriteQueueSize: Int = 8

  val compressParam: CompressParam = CompressParam(
    parameter.datapathWidth,
    parameter.xLen,
    parameter.vLen,
    parameter.laneNumber,
    parameter.laneParam.groupNumberBits,
    2,
    parameter.eLen
  )

  // ===========================================================================
  // Opt 3 (revised v3): BRAM-backed v0 with narrow port +
  //                    active/prefetch/writeback row caches.
  // ===========================================================================
  //
  // GENERALIZED for arbitrary laneNumber. With chunkWidth = max(dpWidth *
  // laneNumber, 64), each BRAM chunk maps to one OFFSET across all lanes
  // (clean (lane, offset) -> dpIdx mapping). For the big config: dLen=128,
  // laneScale=2, datapathWidth=64, laneNumber=2 -> chunkWidth=128,
  // numChunks=8.
  //
  // Coherence model: activeCacheChunks is always the row visible through v0.
  // The next sequential row is prefetched while the current row executes, so a
  // rowCounter change swaps in a complete cache instead of starting a refill
  // on the critical row-transition path. The row just left is written back to
  // BRAM in the background through port A.

  val chunkWidth: Int = math.max(parameter.datapathWidth * parameter.laneNumber, 64)
  val numChunks:    Int = parameter.vLen / chunkWidth
  val chunkBytes:   Int = chunkWidth / 8
  val chunkIdxBits: Int = log2Ceil(numChunks).max(1)
  val bramAddrBits: Int = parameter.rowCounterBits + chunkIdxBits

  val v0BramInst = Module(new V0BramBlackBox(bramAddrBits, chunkWidth))
  v0BramInst.io.clk := implicitClock

  val dpChunksPerBramChunk: Int = chunkWidth / parameter.datapathWidth

  val chunkWriteData: Vec[UInt] = VecInit(Seq.tabulate(numChunks) { c =>
    val perDp = Seq.tabulate(dpChunksPerBramChunk) { d =>
      val dpIdx     = c * dpChunksPerBramChunk + d
      val laneIdx   = dpIdx % parameter.laneNumber
      val offsetInt = dpIdx / parameter.laneNumber
      val v0Write   = v0UpdateVec(laneIdx)
      val active: Bool = v0Write.valid && (v0Write.bits.offset === offsetInt.U)
      Mux(active, v0Write.bits.data, 0.U(parameter.datapathWidth.W))
    }
    VecInit(perDp).asUInt
  })
  val chunkWriteStrb: Vec[UInt] = VecInit(Seq.tabulate(numChunks) { c =>
    val perDp = Seq.tabulate(dpChunksPerBramChunk) { d =>
      val dpIdx     = c * dpChunksPerBramChunk + d
      val laneIdx   = dpIdx % parameter.laneNumber
      val offsetInt = dpIdx / parameter.laneNumber
      val v0Write   = v0UpdateVec(laneIdx)
      val active: Bool = v0Write.valid && (v0Write.bits.offset === offsetInt.U)
      Mux(active, v0Write.bits.mask, 0.U((parameter.datapathWidth / 8).W))
    }
    VecInit(perDp).asUInt
  })
  val chunkWriteEn:  Vec[Bool] = VecInit(chunkWriteStrb.map(_.orR))

  val activeCacheChunks: Vec[UInt] =
    RegInit(VecInit(Seq.fill(numChunks)(0.U(chunkWidth.W))))
  val activeCacheRow: UInt = RegInit(0.U(parameter.rowCounterBits.W))

  def nextRow(row: UInt): UInt = {
    if (parameter.timeMultiplexBatch == 1) {
      0.U(parameter.rowCounterBits.W)
    } else {
      val inc = row + 1.U
      Mux(
        row === (parameter.timeMultiplexBatch - 1).U,
        0.U(parameter.rowCounterBits.W),
        inc(parameter.rowCounterBits - 1, 0)
      )
    }
  }

  val prefetchCacheChunks: Vec[UInt] =
    RegInit(VecInit(Seq.fill(numChunks)(0.U(chunkWidth.W))))
  val prefetchRow:     UInt = RegInit(nextRow(0.U(parameter.rowCounterBits.W)))
  val prefetchValid:   Bool = RegInit(false.B)
  val prefetchCounter: UInt = RegInit(0.U((chunkIdxBits + 1).W))

  val writebackCacheChunks: Vec[UInt] =
    RegInit(VecInit(Seq.fill(numChunks)(0.U(chunkWidth.W))))
  val writebackRow:     UInt = RegInit(0.U(parameter.rowCounterBits.W))
  val writebackValid:   Bool = RegInit(false.B)
  val writebackCounter: UInt = RegInit(0.U((chunkIdxBits + 1).W))

  def bramAddr(row: UInt, chunk: UInt): UInt = Cat(row, chunk(chunkIdxBits - 1, 0))

  v0BramInst.io.wr_en   := false.B
  v0BramInst.io.wr_addr := 0.U
  v0BramInst.io.wr_data := 0.U
  v0BramInst.io.wr_strb := 0.U
  v0BramInst.io.rd_en   := false.B
  v0BramInst.io.rd_addr := 0.U

  def applyByteWrite(prev: UInt, wdata: UInt, wstrb: UInt): UInt = {
    val expandedStrb = FillInterleaved(8, wstrb)
    (prev & (~expandedStrb).asUInt) | (wdata & expandedStrb)
  }

  val rowChanged: Bool = gatherRowCounter =/= activeCacheRow

  activeCacheChunks.zipWithIndex.foreach { case (chunkReg, c) =>
    when(!rowChanged && chunkWriteEn(c)) {
      chunkReg := applyByteWrite(chunkReg, chunkWriteData(c), chunkWriteStrb(c))
    }
  }

  val writebackIdx: UInt = writebackCounter(chunkIdxBits - 1, 0)
  when(writebackValid) {
    v0BramInst.io.wr_en   := true.B
    v0BramInst.io.wr_addr := bramAddr(writebackRow, writebackIdx)
    v0BramInst.io.wr_data := writebackCacheChunks(writebackIdx)
    v0BramInst.io.wr_strb := Fill(chunkBytes, 1.U(1.W))
    when(writebackCounter === (numChunks - 1).U) {
      writebackValid   := false.B
      writebackCounter := 0.U
    }.otherwise {
      writebackCounter := writebackCounter + 1.U
    }
  }

  val prefetchWritebackConflict: Bool = writebackValid && (prefetchRow === writebackRow)
  val prefetchBusy:              Bool = !prefetchValid && !prefetchWritebackConflict && !rowChanged
  val prefetchIssueIdx:          UInt = prefetchCounter(chunkIdxBits - 1, 0)
  val prefetchCaptureIdx:        UInt = (prefetchCounter - 1.U)(chunkIdxBits - 1, 0)
  val prefetchCanIssue:          Bool = prefetchBusy && (prefetchCounter < numChunks.U)
  val prefetchCanCapture:        Bool = prefetchBusy && (prefetchCounter > 0.U)

  when(prefetchCanIssue) {
    v0BramInst.io.rd_en   := true.B
    v0BramInst.io.rd_addr := bramAddr(prefetchRow, prefetchIssueIdx)
  }
  when(prefetchCanCapture) {
    prefetchCacheChunks(prefetchCaptureIdx) := v0BramInst.io.rd_data
  }
  when(prefetchBusy) {
    when(prefetchCounter === numChunks.U) {
      prefetchValid   := true.B
      prefetchCounter := 0.U
    }.otherwise {
      prefetchCounter := prefetchCounter + 1.U
    }
  }

  when(rowChanged) {
    writebackCacheChunks := activeCacheChunks
    writebackRow         := activeCacheRow
    writebackValid       := true.B
    writebackCounter     := 0.U

    when(prefetchValid && (prefetchRow === gatherRowCounter)) {
      activeCacheChunks := prefetchCacheChunks
    }.otherwise {
      activeCacheChunks := VecInit(Seq.fill(numChunks)(0.U(chunkWidth.W)))
    }
    activeCacheRow := gatherRowCounter

    prefetchRow     := nextRow(gatherRowCounter)
    prefetchValid   := false.B
    prefetchCounter := 0.U
  }

  // v0 view consumed by the rest of the unit. cutUInt returns Vec[UInt] so
  // the signature matches the original `v0Vec(gatherRowCounter)`.
  val v0Cache: UInt = activeCacheChunks.asUInt
  val v0: Vec[UInt] = cutUInt(v0Cache, parameter.datapathWidth)

  // ===========================================================================
  // The remainder of this module is structurally identical to MaskUnit.scala.
  // Phases 3c/3d/3e will replace specific subsections in place.
  // ===========================================================================

  val slide            = io.maskPipeReq.bits.uop === BitPat("b001??")
  val gather           = io.maskPipeReq.bits.uop === BitPat("b0001?")
  val extend           = io.maskPipeReq.bits.uop === BitPat("b0000?")
  val slideScalar      = io.maskPipeReq.bits.uop(0)
  val slideUp          = io.maskPipeReq.bits.uop(1)
  val sew1HForMaskPipe = UIntToOH(instReq.bits.sew)(2, 0)
  val slideSize:     UInt = Mux(slideScalar, instReq.bits.readFromScala, 1.U)
  val dByte:         Int  = parameter.laneNumber * parameter.datapathWidth / 8
  val shifterUpSize: UInt = Mux1H(
    sew1HForMaskPipe,
    Seq(
      changeUIntSize(slideSize, log2Ceil(dByte)),
      changeUIntSize(slideSize, log2Ceil(dByte) - 1),
      changeUIntSize(slideSize, log2Ceil(dByte) - 2)
    )
  ) & Fill(log2Ceil(dByte), !(slideSize >> parameter.laneParam.vlMaxBits).asUInt.orR)
  // =========================================================================
  // Opt 7 v2: Chunked v0 slide barrel shifter (single 128-bit-wide shifter
  //          reused over slideNumChunks cycles instead of one combinational
  //          vLen-bit shifter).
  // =========================================================================
  //
  // BACKGROUND
  // Opt 7 v1 unified the two parallel barrel shifters (slideUpV0 with `>>`,
  // slideDownV0 via reverse-shift-reverse) into one vLen-bit `>>` shifter
  // (~10K LUTs). Opt 7 v2 takes this further: compute the 8 output chunks
  // serially, one per cycle, using a single 128-bit-wide shifter (~1.3K
  // LUTs).
  //
  // For slideUp:    output chunk c = (v0 >> (slideSize + c*chunkWidth))[127:0]
  // For slideDown:  use the reverse trick on the input + reverse each chunk
  //                 on output; walk c counter so chunks land at the right
  //                 storeIdx (see equivalence proof in
  //                 fyp_doc/maskunit_chunked_slide_shifter_debug.md § 5).
  //
  // slideV0Overlap is still computed combinationally from a dByte-wide
  // slice of v0 (small, ~64-bit shifter; cost negligible).
  //
  // TIMING
  // slideV0Reg is consumed only when `askMaskVec(i).slide` is high (i.e.
  // by a slide-instruction's lane mask read). Slide instructions take
  // 12K-66K cycles per benchmark_instructions; the 8-cycle setup latency
  // is hidden trivially.

  val isSlideDown:     Bool = !slideUp
  val slideShiftStart: Bool = io.maskPipeReq.valid && slide

  val slideChunkWidth:    Int = parameter.datapathWidth * parameter.laneNumber
  val slideNumChunks:     Int = parameter.vLen / slideChunkWidth
  val slideChunkIdxBits:  Int = log2Ceil(slideNumChunks)
  val slideCounterBits:   Int = log2Ceil(slideNumChunks + 1)

  // Captured state during the 8-cycle shift sequence
  val slideShifterActive: Bool = RegInit(false.B)
  val slideChunkCounter:  UInt = RegInit(0.U(slideCounterBits.W))
  val slideShiftAmtReg:   UInt = RegInit(0.U(parameter.laneParam.vlMaxBits.W))
  val slideShiftDirReg:   Bool = RegInit(false.B) // true = slideDown
  val slideShiftV0:       UInt = RegInit(0.U(parameter.vLen.W))
  val slideV0RegBanks:    Vec[UInt] =
    RegInit(VecInit(Seq.fill(slideNumChunks)(0.U(slideChunkWidth.W))))

  // Capture inputs at the maskPipeReq.valid && slide edge
  when(slideShiftStart) {
    slideShifterActive := true.B
    slideChunkCounter  := 0.U
    slideShiftDirReg   := isSlideDown
    slideShiftAmtReg   := Mux(
      isSlideDown,
      changeUIntSize(shifterUpSize, parameter.laneParam.vlMaxBits),
      changeUIntSize(slideSize,     parameter.laneParam.vlMaxBits)
    )
    slideShiftV0       := Mux(isSlideDown, Reverse(v0.asUInt), v0.asUInt)
  }

  // Per-cycle chunk compute + bank write
  when(slideShifterActive) {
    // For slideDown the input is reversed, so chunks need to be computed
    // in reversed order then `Reverse`-d before storing.
    val effectiveC: UInt = Mux(
      slideShiftDirReg,
      (slideNumChunks - 1).U - slideChunkCounter,
      slideChunkCounter
    )
    // srcStart = slideShiftAmtReg + effectiveC * slideChunkWidth
    // Implemented as a shift to avoid a multiplier.
    val srcStart: UInt = slideShiftAmtReg + (effectiveC << log2Ceil(slideChunkWidth).U).asUInt
    val shifted: UInt = (slideShiftV0 >> srcStart).asUInt
    val rawChunk: UInt = changeUIntSize(shifted, slideChunkWidth)
    val finalChunk: UInt = Mux(slideShiftDirReg, Reverse(rawChunk), rawChunk)
    slideV0RegBanks(slideChunkCounter(slideChunkIdxBits - 1, 0)) := finalChunk

    when(slideChunkCounter === (slideNumChunks - 1).U) {
      slideShifterActive := false.B
    }.otherwise {
      slideChunkCounter  := slideChunkCounter + 1.U
    }
  }

  val slideV0Reg: UInt = slideV0RegBanks.asUInt

  // Overlap: top `shifterUpSize` bits of v0, LSB-first. Same formula as
  // Opt 7 v1 - small dByte-wide barrel shifter, cost negligible.
  val v0TopDByte: UInt = v0.asUInt(parameter.vLen - 1, parameter.vLen - dByte)
  val overlapShiftAmt: UInt = (dByte.U - shifterUpSize)
  val slideV0Overlap: UInt =
    changeUIntSize((v0TopDByte >> overlapShiftAmt).asUInt, dByte)
  val slideV0OverReg: UInt = RegEnable(slideV0Overlap, 0.U(dByte.W), slideShiftStart)
  val slideV0: Vec[UInt] = cutUInt(slideV0Reg, parameter.datapathWidth)

  // =========================================================================
  // Opt 2 v2: Per-lane writeMaskForMaskPipe push-down. Compute per-lane
  // writeBitMask slices DIRECTLY from sources (v0Cache, vl,
  // shifterValidSize, ...) instead of building the full vLen-bit
  // writeMaskForMaskPipe intermediate signal + register. The Pipe stage
  // now stores per-lane 128-bit slices (256 FFs) instead of a 1024-bit
  // wide signal (1024 FFs), and the per-lane SEW Mux1H + regroup happens
  // pre-Pipe so the post-Pipe path is just a Mux on typeVec.
  // =========================================================================

  val shifterValidSize:   UInt = changeUIntSize(instReq.bits.readFromScala, parameter.laneParam.vlMaxBits)
  val shifterSizeOverlap: Bool = (instReq.bits.readFromScala >> parameter.laneParam.vlMaxBits).asUInt.orR
  val writeMaskActive:    Bool = slideScalar && slideUp && slide
  val sew1HForExtend:     UInt = (sew1HForMaskPipe << instReq.bits.decodeResult(Decoder.crossWrite)).asUInt

  // Per-bit compute: writeMaskForMaskPipe[srcPos]
  //   = baseV0[srcPos] & vlCorrection[srcPos] & upCorrection[srcPos]
  // where:
  //   baseV0[srcPos]      = !maskType || v0Cache(srcPos)
  //   vlCorrection[srcPos]= (srcPos.U < vl)
  //   upCorrection[srcPos]= !active || ((srcPos.U >= shifterValidSize) && !shifterSizeOverlap)
  def computeWriteMaskBit(srcPos: Int): Bool = {
    val baseBit: Bool = !instReq.bits.maskType || v0Cache(srcPos)
    val vlBit:   Bool = srcPos.U < instReq.bits.vl
    val upBit:   Bool = !writeMaskActive || ((srcPos.U >= shifterValidSize) && !shifterSizeOverlap)
    baseBit && vlBit && upBit
  }

  def computeWriteBitMaskForSlide(lane: Int, sew1H: UInt): UInt = {
    val laneBits = parameter.vLen / parameter.laneNumber
    val perSewSlice: Seq[UInt] = Seq(4, 2, 1).map { singleSize =>
      val groupSize  = singleSize * (parameter.datapathWidth / parameter.eLen)
      val outerCount = laneBits / groupSize
      Cat((0 until outerCount).map { outer =>
        Cat((0 until groupSize).reverse.map { bg =>
          val srcPos = outer * parameter.laneNumber * groupSize + lane * groupSize + bg
          computeWriteMaskBit(srcPos)
        })
      }.reverse)
    }
    Mux1H(sew1H, perSewSlice)
  }

  def computeWriteBitMaskForExtend(lane: Int, sew1H: UInt): UInt = {
    val laneBits = parameter.vLen / parameter.laneNumber
    val perSewSlice: Seq[UInt] = Seq(4, 2, 1).map { singleSize =>
      val groupSize  = singleSize * (parameter.datapathWidth / parameter.eLen)
      val outerCount = laneBits / groupSize
      val orredBits = Cat((0 until outerCount).map { outer =>
        val groupBits = (0 until groupSize).map { bg =>
          val srcPos = outer * parameter.laneNumber * groupSize + lane * groupSize + bg
          computeWriteMaskBit(srcPos)
        }
        VecInit(groupBits).asUInt.orR
      }.reverse)
      changeUIntSize(orredBits, laneBits)
    }
    Mux1H(sew1H, perSewSlice)
  }

  // Pre-Pipe per-lane writeBitMask computation. Both SEW Mux1H selections
  // happen pre-Pipe so the registered slice is the final per-lane bit
  // mask for the captured instruction.
  val writeBitMaskForSlide_pre: Vec[UInt] =
    VecInit(Seq.tabulate(parameter.laneNumber) { lane =>
      computeWriteBitMaskForSlide(lane, sew1HForMaskPipe)
    })
  val writeBitMaskForExtend_pre: Vec[UInt] =
    VecInit(Seq.tabulate(parameter.laneNumber) { lane =>
      computeWriteBitMaskForExtend(lane, sew1HForExtend)
    })

  // The new WriteCountPipe0 stores the per-lane slices instead of the wide
  // writeMaskForMaskPipe. Saves 1024 - (2 * 128 * 2) = 512 FFs on the Pipe,
  // and the post-Pipe per-lane slicer/Mux1H is eliminated.
  class WriteCountPipe0 extends Bundle {
    val instructionIndex:       UInt      = UInt(parameter.instructionIndexBits.W)
    val writeBitMaskForSlide:   Vec[UInt] = Vec(parameter.laneNumber,
                                                  UInt((parameter.vLen / parameter.laneNumber).W))
    val writeBitMaskForExtend:  Vec[UInt] = Vec(parameter.laneNumber,
                                                  UInt((parameter.vLen / parameter.laneNumber).W))
    val typeVec:                UInt      = UInt(2.W)
  }

  val writeCountPipeWire0: WriteCountPipe0 = Wire(new WriteCountPipe0)
  writeCountPipeWire0.instructionIndex      := io.instReq.bits.instructionIndex
  writeCountPipeWire0.writeBitMaskForSlide  := writeBitMaskForSlide_pre
  writeCountPipeWire0.writeBitMaskForExtend := writeBitMaskForExtend_pre
  writeCountPipeWire0.typeVec               := VecInit(
    Seq(
      slide || gather,
      extend
    )
  ).asUInt

  val writeCountPipe0: Valid[WriteCountPipe0] = Pipe(io.maskPipeReq.valid, writeCountPipeWire0, 1)

  // Post-Pipe: just expose the registered per-lane slices.
  val writeBitMaskForSlide:  Vec[UInt] = writeCountPipe0.bits.writeBitMaskForSlide
  val writeBitMaskForExtend: Vec[UInt] = writeCountPipe0.bits.writeBitMaskForExtend

  class WriteCountPipe1 extends Bundle {
    val writeBitMask:     Vec[UInt] = Vec(parameter.laneNumber, UInt((parameter.vLen / parameter.laneNumber).W))
    val instructionIndex: UInt      = UInt(parameter.instructionIndexBits.W)
  }

  val writeCountPipeWire1: WriteCountPipe1 = Wire(new WriteCountPipe1)
  writeCountPipeWire1.instructionIndex := writeCountPipe0.bits.instructionIndex
  writeCountPipeWire1.writeBitMask     := Mux1H(
    writeCountPipe0.bits.typeVec,
    Seq(
      writeBitMaskForSlide,
      writeBitMaskForExtend
    )
  )
  val writeCountPipe1: Valid[WriteCountPipe1] = Pipe(writeCountPipe0.valid, writeCountPipeWire1, 1)

  class WriteCountPipe2 extends Bundle {
    val writeCount:       Vec[UInt] = Vec(parameter.laneNumber, UInt(log2Ceil(parameter.vLen / parameter.laneNumber).W))
    val instructionIndex: UInt      = UInt(parameter.instructionIndexBits.W)
  }

  val writeCountPipeWire2: WriteCountPipe2 = Wire(new WriteCountPipe2)
  writeCountPipeWire2.instructionIndex := writeCountPipe1.bits.instructionIndex
  writeCountPipeWire2.writeCount       := VecInit(writeCountPipe1.bits.writeBitMask.map(PopCount(_)))
  val writeCountPipe2: Valid[WriteCountPipe2] = Pipe(writeCountPipe1.valid, writeCountPipeWire2, 1)

  io.writeCountVec.zipWithIndex.foreach { case (req, index) =>
    req.valid                 := writeCountPipe2.valid
    req.bits.count            := writeCountPipe2.bits.writeCount(index)
    req.bits.instructionIndex := writeCountPipe2.bits.instructionIndex
  }
  io.maskE0 := v0(0)(0)

  // ===========================================================================
  // Opt 1: On-demand per-lane mask slicer.
  // ===========================================================================
  //
  // Replaces the three parallel SEW-specific cutUInt+grouped+transpose+Mux1H
  // trees in MaskUnit.scala:341-360 (one Vec[UInt(vLen.W)] per SEW). Instead
  // of pre-computing all three regroup permutations of the full vLen-bit
  // v0 / slideV0 and then per-lane Mux1H-ing 128 bits down to 32, we
  // compute only the 32-bit datapath slice the lane actually needs.
  //
  // For lane L, SEW s, maskSelect M, the slice gathers exactly the
  // datapathWidth bits the lane will consume; the SEW Mux1H now operates
  // on 32-bit values instead of 128-bit values, and the per-SEW regroup
  // permutation is folded into a static (lane, SEW)-indexed wire pattern.
  // All three SEWs are still supported via the runtime sew mux.

  def maskSliceFor(lane: Int, sew: UInt, maskSelect: UInt, source: UInt): UInt = {
    val laneBits     = parameter.vLen / parameter.laneNumber
    val perSewSlice: Seq[UInt] = Seq(4, 2, 1).map { singleSize =>
      val groupSize  = singleSize * (parameter.datapathWidth / parameter.eLen)
      val outerCount = laneBits / groupSize
      // Per-lane reordering of `source`: for outer in [0, outerCount), pick
      // the `groupSize` source bits starting at base = outer*laneNumber*groupSize
      // + lane*groupSize. This matches the OLD `cutUInt(...).grouped(laneNumber)
      // .transpose` permutation but restricted to this lane.
      val laneBitsAssembled: UInt = Cat((0 until outerCount).map { outer =>
        val base = outer * parameter.laneNumber * groupSize + lane * groupSize
        source(base + groupSize - 1, base)
      }.reverse)
      // Cut to datapath-width chunks (4 per lane at vLen=1024,
      // datapathWidth=32, laneBits=128) and select by maskSelect.
      cutUInt(laneBitsAssembled, parameter.datapathWidth)(maskSelect)
    }
    Mux1H(UIntToOH(sew)(2, 0), perSewSlice)
  }

  laneMaskInput.zipWithIndex.foreach { case (input, index) =>
    val maskSelect = askMaskVec(index).maskSelect
    val maskSew    = askMaskVec(index).maskSelectSew

    val v0Slice:      UInt = maskSliceFor(index, maskSew, maskSelect, v0.asUInt)
    val slideV0Slice: UInt = maskSliceFor(index, maskSew, maskSelect, slideV0.asUInt)

    // slideV0OverReg holds the overflow bits produced by the slide barrel
    // shifter when the slide pushes data past the vLen boundary. The
    // per-SEW overlap select keeps the same width semantics as the OLD
    // regroup path.
    val overlapSelect: UInt = Mux1H(
      UIntToOH(maskSew)(2, 0),
      Seq(4, 2, 1).map { singleSize =>
        val groupSize = singleSize * (parameter.datapathWidth / parameter.eLen)
        cutUInt(slideV0OverReg, groupSize)(index)
      }
    )
    val overlap: Bool =
      ((parameter.vLen / parameter.datapathWidth / parameter.laneNumber).U & maskSelect).orR

    val slideResult:    UInt = Mux(overlap, overlapSelect, slideV0Slice)
    val nonSlideResult: UInt = Mux(overlap, overlapSelect, v0Slice)

    input := Mux(askMaskVec(index).slide, slideResult, nonSlideResult)
  }

  val maskedWrite: BitLevelMaskWrite = Module(new BitLevelMaskWrite(parameter))

  def gatherIndex(elementIndex: UInt, vlmul: UInt, sew: UInt): (UInt, UInt, UInt, UInt, Bool) = {
    val intLMULInput: UInt = (1.U << vlmul(1, 0)).asUInt
    val positionSize = parameter.laneParam.vlMaxBits - 1
    val dataPosition = (changeUIntSize(elementIndex, positionSize) << sew).asUInt(positionSize - 1, 0)
    val sewOHInput   = UIntToOH(sew)(2, 0)

    val dataPathBaseBits = log2Ceil(parameter.datapathWidth / 8)
    val dataOffset: UInt = dataPosition(dataPathBaseBits - 1, 0)
    val accessLane =
      if (parameter.laneNumber > 1)
        dataPosition(log2Ceil(parameter.laneNumber) + dataPathBaseBits - 1, dataPathBaseBits)
      else 0.U(1.W)

    val dataGroup = (dataPosition >> (log2Ceil(parameter.laneNumber) + dataPathBaseBits)).asUInt
    val offsetWidth: Int = parameter.laneParam.vrfParam.vrfOffsetBits
    val offset            = dataGroup(offsetWidth - 1, 0)
    val accessRegGrowth   = (dataGroup >> offsetWidth).asUInt
    val decimalProportion = offset ## accessLane
    val decimal           = decimalProportion(decimalProportion.getWidth - 1, 0.max(decimalProportion.getWidth - 3))

    val overlap     =
      (vlmul(2) && decimal >= intLMULInput(3, 1)) ||
        (!vlmul(2) && accessRegGrowth >= intLMULInput) ||
        (elementIndex >> log2Ceil(parameter.vLen)).asUInt.orR
    val notNeedRead = overlap
    val reallyGrowth: UInt = changeUIntSize(accessRegGrowth, 3)
    (dataOffset, accessLane, offset, reallyGrowth, notNeedRead)
  }
  val (dataOffset, accessLane, offset, reallyGrowth, notNeedRead) =
    gatherIndex(instReq.bits.readFromScala, instReq.bits.vlmul, instReq.bits.sew)
  val idle :: sRead :: wRead :: sResponse :: sWaitNextRow :: Nil = Enum(5)
  val gatherReadState:   UInt = RegInit(idle)
  val gatherRequestFire: Bool = Wire(Bool())
  val gatherSRead:       Bool = gatherReadState === sRead
  val gatherWaiteRead:   Bool = gatherReadState === wRead
  val gatherResponse:    Bool = gatherReadState === sResponse
  val gatherWaitNextRow: Bool = gatherReadState === sWaitNextRow
  val gatherDatOffset:   UInt = RegEnable(dataOffset, 0.U, gatherRequestFire)
  val gatherLane:        UInt = RegEnable(accessLane, 0.U, gatherRequestFire)
  val gatherOffset:      UInt = RegEnable(offset, 0.U, gatherRequestFire)
  val gatherGrowth:      UInt = RegEnable(reallyGrowth, 0.U, gatherRequestFire)

  val gatherReplayBusy = io.gatherReplayBusy
  val gatherInstActive: Bool = RegInit(false.B)

  val instReg:     MaskUnitInstReq = RegEnable(instReq.bits, 0.U.asTypeOf(instReq.bits), instReq.valid)
  val enqMvRD:     Bool            = instReq.bits.decodeResult(Decoder.topUop) === BitPat("b01011")
  val instVlValid: Bool            =
    RegEnable((instReq.bits.vl.orR || enqMvRD) && instReq.valid, false.B, instReq.valid || lastReport.orR)
  gatherRequestFire := gatherReadState === idle && gatherRead && !instVlValid && (gatherInstActive || !gatherReplayBusy)
  when(gatherRequestFire) { gatherInstActive := true.B }
  when(!gatherRead)       { gatherInstActive := false.B }
  val viotaReq:   Bool            = instReq.bits.decodeResult(Decoder.topUop) === "b01000".U
  when(instReq.valid && (viotaReq || enqMvRD) || gatherRequestFire) {
    instReg.vs1              := instReq.bits.vs2
    instReg.instructionIndex := instReq.bits.instructionIndex
  }
  val readVS1Reg: MaskUnitReadVs1 = RegInit(0.U.asTypeOf(new MaskUnitReadVs1(parameter)))
  val sew1H:      UInt            = UIntToOH(instReg.sew)(2, 0)
  val readVS1Req: MaskUnitReadReq = WireDefault(0.U.asTypeOf(new MaskUnitReadReq(parameter)))

  when(instReq.valid || gatherRequestFire) {
    readVS1Reg.requestSend     := false.B
    readVS1Reg.dataValid       := false.B
    readVS1Reg.sendToExecution := false.B
    readVS1Reg.readIndex       := 0.U
  }

  val unitType:            UInt = UIntToOH(instReg.decodeResult(Decoder.topUop)(4, 3))
  val subType:             UInt = UIntToOH(instReg.decodeResult(Decoder.topUop)(2, 1))
  val readType:            Bool = unitType(0)
  val maskDestinationType: Bool = instReg.decodeResult(Decoder.topUop) === "b11000".U
  val compress:            Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b0100?")
  val viota:               Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b01000")
  val mv:                  Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b0101?")
  val mvRd:                Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b01011")
  val mvVd:                Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b01010")
  val ffo:                 Bool = instReg.decodeResult(Decoder.topUop) === BitPat("b0111?")
  val readValid:           Bool = readType && instVlValid

  val noSource: Bool = mv || viota

  val allGroupExecute: Bool = maskDestinationType || unitType(2) || compress || ffo
  val useDefaultSew:   Bool = unitType(0)
  val dataSplitSew:    UInt = Mux1H(
    Seq(
      useDefaultSew               -> instReg.sew,
      (unitType(3) && subType(2)) -> (0 + log2Ceil(parameter.laneScale)).U,
      (unitType(3) && subType(1)) -> (1 + log2Ceil(parameter.laneScale)).U,
      allGroupExecute             -> 2.U
    )
  )

  val sourceDataUseDefaultSew: Bool = !unitType(3)
  val sourceDataEEW:           UInt = Mux1H(
    Seq(
      sourceDataUseDefaultSew -> instReg.sew,
      unitType(3)             -> (instReg.sew >> subType(2, 1)).asUInt
    )
  )

  val lastExecuteIndex: UInt = Mux1H(
    UIntToOH(dataSplitSew),
    Seq(8, 16, 32).map { dw =>
      ((parameter.datapathWidth / dw - 1) * dw / 8).U(parameter.dataPathByteBits.W)
    }
  )

  val sourceDataEEW1H:  UInt = UIntToOH(sourceDataEEW)(2, 0)
  val lastElementIndex: UInt = (instReg.vl - instReg.vl.orR)(parameter.laneParam.vlMaxBits - 2, 0)

  val maskFormatSource: Bool = ffo || maskDestinationType

  val prioritizeLane: Bool = ffo

  val processingVl: Seq[(UInt, UInt)] = Seq(1, 2, 4).map { eByte =>
    val eByteLog      = log2Ceil(eByte)
    val lastByteIndex = (lastElementIndex << eByteLog).asUInt
    val rowWidth      = parameter.datapathWidth * parameter.laneNumber / 8
    val rowWidthLog:        Int  = log2Ceil(rowWidth)
    val lastGroupRemaining: UInt = changeUIntSize(lastByteIndex, rowWidthLog)
    val lastRowIndex = (lastByteIndex >> rowWidthLog).asUInt

    val laneDatalog       = log2Ceil(parameter.datapathWidth / 8)
    val lastLaneIndex     = (lastGroupRemaining >> laneDatalog).asUInt
    val lastGroupDataNeed = scanRightOr(UIntToOH(lastLaneIndex))
    (lastRowIndex, lastGroupDataNeed)
  }

  val processingMaskVl: Seq[(UInt, UInt)] = Seq(1).map { eBit =>
    val lastBitIndex = lastElementIndex
    val rowWidth     = parameter.datapathWidth * parameter.laneNumber
    val rowWidthLog:        Int  = log2Ceil(rowWidth)
    val lastGroupRemaining: UInt = changeUIntSize(lastBitIndex, rowWidthLog)
    val lastGroupMisAlign:  Bool = lastGroupRemaining.orR
    val lastRowIndex = (lastBitIndex >> rowWidthLog).asUInt

    val laneDatalog   = log2Ceil(parameter.datapathWidth)
    val lastLaneIndex = (lastGroupRemaining >> laneDatalog).asUInt -
      !changeUIntSize(lastGroupRemaining, laneDatalog).orR
    val dataNeedForPL = scanRightOr(UIntToOH(lastLaneIndex))

    val dataNeedForNPL    = Mux1H(
      sew1H,
      Seq(4, 2, 1).map { sewSize =>
        val eSize        = sewSize * parameter.laneScale
        val eSizeLog     = log2Ceil(eSize)
        val misAlign     = if (eSizeLog > 0) changeUIntSize(lastGroupRemaining, eSizeLog).orR else false.B
        val datapathSize = (lastGroupRemaining >> eSizeLog).asUInt +& misAlign

        val laneNumLog    = log2Ceil(parameter.laneNumber)
        val allNeed       = (datapathSize >> laneNumLog).asUInt.orR
        val lastLaneIndex = changeUIntSize(datapathSize, laneNumLog)
        val dataNeed: UInt = (~scanLeftOr(UIntToOH(lastLaneIndex))).asUInt | Fill(parameter.laneNumber, allNeed)
        dataNeed
      }
    )
    val lastGroupDataNeed = Mux(prioritizeLane, dataNeedForPL, dataNeedForNPL)
    (lastRowIndex, lastGroupDataNeed)
  }

  val dataSourceSew:   UInt = Mux(
    unitType(3),
    instReg.sew - instReg.decodeResult(Decoder.topUop)(2, 1),
    instReg.sew
  )
  val dataSourceSew1H: UInt = UIntToOH(dataSourceSew)(2, 0)

  val normalFormat:            Bool = !maskFormatSource && !mv
  val lastGroupForInstruction: UInt = Mux1H(
    Seq(
      mv                                   -> 0.U,
      maskFormatSource                     -> processingMaskVl.head._1,
      (normalFormat && dataSourceSew1H(0)) -> processingVl.head._1,
      (normalFormat && dataSourceSew1H(1)) -> processingVl(1)._1,
      (normalFormat && dataSourceSew1H(2)) -> processingVl(2)._1
    )
  )

  val lastGroupDataNeed: UInt = Mux1H(
    Seq(
      maskFormatSource                     -> processingMaskVl.head._2,
      (normalFormat && dataSourceSew1H(0)) -> processingVl.head._2,
      (normalFormat && dataSourceSew1H(1)) -> processingVl(1)._2,
      (normalFormat && dataSourceSew1H(2)) -> processingVl(2)._2
    )
  )

  val groupSizeForMaskDestination:   Int  = parameter.laneNumber * parameter.datapathWidth
  val elementTailForMaskDestination: UInt = lastElementIndex(log2Ceil(groupSizeForMaskDestination) - 1, 0)

  val exeRequestQueue: Seq[QueueIO[MaskUnitExeReq]] = exeReq.zipWithIndex.map { case (req, index) =>
    val queue: QueueIO[MaskUnitExeReq] =
      Queue.io(chiselTypeOf(req.bits), parameter.laneParam.maskRequestQueueSize, flow = true)
    queue.enq <> req
    queue
  }

  val exeReqReg:           Seq[ValidIO[MaskUnitExeReq]] = Seq.tabulate(parameter.laneNumber) { _ =>
    RegInit(
      0.U.asTypeOf(
        Valid(
          new MaskUnitExeReq(
            parameter.eLen,
            parameter.datapathWidth,
            parameter.instructionIndexBits,
            parameter.fpuEnable
          )
        )
      )
    )
  }
  val requestCounter:      UInt                         = RegInit(0.U(parameter.laneParam.groupNumberBits.W))
  val executeGroupCounter: UInt                         = Wire(UInt(parameter.laneParam.groupNumberBits.W))

  val counterValid: Bool = requestCounter <= lastGroupForInstruction
  val lastGroup:    Bool =
    requestCounter === lastGroupForInstruction || mv

  val lastExecuteGroupDeq: Bool = Wire(Bool())
  val viotaCounterAdd:     Bool = Wire(Bool())
  val groupCounterAdd:     Bool = Mux(noSource, viotaCounterAdd, lastExecuteGroupDeq)
  when(instReq.valid || groupCounterAdd) {
    requestCounter := Mux(instReq.valid, 0.U, requestCounter + 1.U)
  }

  val groupDataNeed: UInt = Mux(lastGroup, lastGroupDataNeed, (-1.S(parameter.laneNumber.W)).asUInt)
  val executeIndex:  UInt = RegInit(0.U(parameter.dataPathByteBits.W))

  def indexAnalysis(sewInt: Int)(elementIndex: UInt, vlmul: UInt, valid: Option[Bool] = None): Seq[UInt] = {
    val intLMULInput: UInt = (1.U << vlmul(1, 0)).asUInt
    val positionSize = parameter.laneParam.vlMaxBits - 1
    val dataPosition = (changeUIntSize(elementIndex, positionSize) << sewInt).asUInt(positionSize - 1, 0)
    val accessMask: UInt = Seq(
      UIntToOH(dataPosition(1, 0)),
      FillInterleaved(2, UIntToOH(dataPosition(1))),
      15.U(4.W)
    )(sewInt)
    val dataPathBaseBits = log2Ceil(parameter.datapathWidth / 8)
    val dataOffset: UInt = dataPosition(dataPathBaseBits - 1, 0)
    val accessLane =
      if (parameter.laneNumber > 1)
        dataPosition(log2Ceil(parameter.laneNumber) + dataPathBaseBits - 1, dataPathBaseBits)
      else 0.U(1.W)
    val dataGroup  = (dataPosition >> (log2Ceil(parameter.laneNumber) + dataPathBaseBits)).asUInt
    val offsetWidth: Int = parameter.laneParam.vrfParam.vrfOffsetBits
    val offset            = dataGroup(offsetWidth - 1, 0)
    val accessRegGrowth   = (dataGroup >> offsetWidth).asUInt
    val decimalProportion = offset ## accessLane
    val decimal           = decimalProportion(decimalProportion.getWidth - 1, 0.max(decimalProportion.getWidth - 3))

    val overlap      =
      (vlmul(2) && decimal >= intLMULInput(3, 1)) ||
        (!vlmul(2) && accessRegGrowth >= intLMULInput) ||
        (elementIndex >> log2Ceil(parameter.vLen)).asUInt.orR
    val elementValid = valid.getOrElse(true.B)
    val notNeedRead  = overlap || !elementValid
    val reallyGrowth: UInt = changeUIntSize(accessRegGrowth, 3)
    Seq(accessMask, dataOffset, accessLane, offset, reallyGrowth, notNeedRead, elementValid)
  }

  val executeGroup: UInt = Mux1H(
    UIntToOH(dataSplitSew)(2, 0),
    Seq(
      requestCounter ## executeIndex,
      requestCounter ## (executeIndex >> 1),
      requestCounter ## (executeIndex >> 2)
    )
  )

  val executeSizeBit: Int = log2Ceil(parameter.laneNumber)
  val vlMisAlign = instReg.vl(executeSizeBit - 1, 0).orR
  val lastexecuteGroup:     UInt = (instReg.vl >> executeSizeBit).asUInt - !vlMisAlign
  val isVlBoundary:         Bool = executeGroup === lastexecuteGroup
  val validExecuteGroup:    Bool = executeGroup <= lastexecuteGroup
  val vlBoundaryCorrection: UInt = Mux(
    vlMisAlign && isVlBoundary,
    (~scanLeftOr(UIntToOH(instReg.vl(executeSizeBit - 1, 0)))).asUInt,
    -1.S(parameter.laneNumber.W).asUInt
  ) & Fill(parameter.laneNumber, validExecuteGroup)

  val selectReadStageMask: UInt = cutUInt(v0.asUInt, parameter.laneNumber)(executeGroup)
  val readMaskCorrection:  UInt =
    Mux(instReg.maskType, selectReadStageMask, -1.S(parameter.laneNumber.W).asUInt) &
      vlBoundaryCorrection

  val maskSplit = Seq(0, 1, 2).map { sewInt =>
    val dataByte = 1 << sewInt
    val rowElementSize: Int = parameter.laneNumber * parameter.datapathWidth / dataByte / 8
    val maskSelect = cutUInt(v0.asUInt, rowElementSize)(executeGroupCounter)

    val executeSizeBit: Int = log2Ceil(rowElementSize)
    val vlMisAlign = instReg.vl(executeSizeBit - 1, 0).orR
    val lastexecuteGroup:     UInt = (instReg.vl >> executeSizeBit).asUInt - !vlMisAlign
    val isVlBoundary:         Bool = executeGroupCounter === lastexecuteGroup
    val validExecuteGroup:    Bool = executeGroupCounter <= lastexecuteGroup
    val vlBoundaryCorrection: UInt = maskEnable(
      vlMisAlign && isVlBoundary,
      (~scanLeftOr(UIntToOH(instReg.vl(executeSizeBit - 1, 0)))).asUInt
    ) & Fill(rowElementSize, validExecuteGroup)
    val elementMask = maskEnable(instReg.maskType, maskSelect) & vlBoundaryCorrection
    val byteMask    = FillInterleaved(dataByte, elementMask)
    (byteMask, elementMask)
  }
  val executeByteMask: UInt = Mux1H(sew1H, maskSplit.map(_._1))
  val executeElementMask: UInt = Mux1H(sew1H, maskSplit.map(_._2))

  val maskForDestination:             UInt = cutUInt(v0.asUInt, groupSizeForMaskDestination)(requestCounter)
  val lastGroupMask:                  UInt = scanRightOr(UIntToOH(elementTailForMaskDestination))
  val currentMaskGroupForDestination: UInt = maskEnable(lastGroup, lastGroupMask) &
    maskEnable(instReg.maskType && !instReg.decodeResult(Decoder.maskSource), maskForDestination)

  val minSourceSize:    Int       = 8 * parameter.laneNumber
  val groupSourceData:  UInt      = VecInit(exeReqReg.map(_.bits.source1)).asUInt
  val groupSourceValid: UInt      = VecInit(exeReqReg.map(_.valid)).asUInt
  val shifterSource:    UInt      = Mux1H(
    UIntToOH(executeIndex),
    Seq.tabulate(parameter.datapathWidth / 8) { i =>
      (groupSourceData >> (minSourceSize * i)).asUInt
    }
  )
  val maxExecuteTimes:  Int       = parameter.datapathWidth / 8
  val selectValid:      UInt      = Mux1H(
    sourceDataEEW1H,
    Seq(
      cutUIntBySize(FillInterleaved(maxExecuteTimes, groupSourceValid), maxExecuteTimes)(executeIndex),
      cutUIntBySize(FillInterleaved(maxExecuteTimes / 2, groupSourceValid), maxExecuteTimes / 2)(
        executeIndex(parameter.dataPathByteBits - 1, 1)
      ),
      if (maxExecuteTimes > 4)
        cutUIntBySize(FillInterleaved(maxExecuteTimes / 4, groupSourceValid), maxExecuteTimes / 4)(
          executeIndex(parameter.dataPathByteBits - 1, 2)
        )
      else
        groupSourceValid
    )
  )
  val source:           Vec[UInt] = Wire(Vec(parameter.laneNumber, UInt(parameter.datapathWidth.W)))
  source.zipWithIndex.foreach { case (d, i) =>
    d := Mux1H(
      sourceDataEEW1H,
      Seq(
        cutUInt(shifterSource, 8)(i),
        cutUInt(shifterSource, 16)(i),
        cutUInt(shifterSource, 32)(i)
      )
    )
  }

  exeRequestQueue.zip(exeReqReg).foreach { case (req, reg) =>
    req.deq.ready := !reg.valid || lastExecuteGroupDeq || viota
    when(req.deq.fire) {
      reg.bits := req.deq.bits
    }
    when(req.deq.fire ^ lastExecuteGroupDeq) {
      reg.valid := req.deq.fire && !viota
    }
  }

  val isLastExecuteGroup: Bool = (executeIndex === lastExecuteIndex) || allGroupExecute
  val allDataValid:       Bool = exeReqReg.zipWithIndex.map { case (d, i) => d.valid || !groupDataNeed(i) }.reduce(_ && _)
  val anyDataValid:       Bool = exeReqReg.zipWithIndex.map { case (d, i) => d.valid }.reduce(_ || _)

  val readVs1Valid: Bool =
    (compress || mvRd) && !readVS1Reg.requestSend || gatherSRead
  readVS1Req.vs := instReg.vs1
  when(compress) {
    val logLaneNumber = log2Ceil(parameter.laneNumber)
    readVS1Req.vs       := instReg.vs1 + (readVS1Reg.readIndex >> (parameter.laneParam.vrfOffsetBits + logLaneNumber))
    readVS1Req.offset   := readVS1Reg.readIndex >> logLaneNumber
    readVS1Req.readLane := changeUIntSize(readVS1Reg.readIndex, logLaneNumber)
  }.elsewhen(gatherSRead || gatherWaiteRead) {
    readVS1Req.vs         := instReg.vs1 + gatherGrowth
    readVS1Req.offset     := gatherOffset
    readVS1Req.readLane   := gatherLane
    readVS1Req.dataOffset := gatherDatOffset
  }

  val compressUnitResultQueue: QueueIO[CompressOutput] = Queue.io(new CompressOutput(compressParam), 4, flow = true)

  val noSourceValid:       Bool = noSource && counterValid &&
    (instReg.vl.orR || (mvRd && !readVS1Reg.sendToExecution))
  val vs1DataValid:        Bool = readVS1Reg.dataValid || !(compress || mvRd)
  val executeReady:        Bool = Wire(Bool())
  val executeDeqReady:     Bool = VecInit(maskedWrite.in.map(_.ready)).asUInt.andR && compressUnitResultQueue.empty
  val otherTypeRequestDeq: Bool =
    Mux(noSource, noSourceValid, allDataValid) &&
      vs1DataValid && instVlValid && executeDeqReady
  val requestStageDeq:     Bool = otherTypeRequestDeq && executeReady

  val executeIndexGrowth: UInt = (1.U << dataSplitSew).asUInt
  when(requestStageDeq && anyDataValid) {
    executeIndex := executeIndex + executeIndexGrowth
  }

  lastExecuteGroupDeq := requestStageDeq && isLastExecuteGroup

  val readVs1Fire:    Vec[Bool] = Wire(Vec(parameter.laneNumber, Bool()))
  when(readVs1Fire.asUInt.orR) {
    readVS1Reg.requestSend := true.B

    when(gatherSRead) {
      gatherReadState := wRead
    }
  }
  val readVs1AckFire: Vec[Bool] = Wire(Vec(parameter.laneNumber, Bool()))
  val readVs1AckData: Vec[UInt] = Wire(Vec(parameter.laneNumber, UInt(parameter.datapathWidth.W)))
  when(readVs1AckFire.asUInt.orR) {
    readVS1Reg.data      := Mux1H(readVs1AckFire, readVs1AckData) >> (readVS1Req.dataOffset ## 0.U(3.W))
    readVS1Reg.dataValid := true.B
    when(gatherWaiteRead) {
      gatherReadState := sResponse
    }
  }
  readChannel.zipWithIndex.foreach { case (request, index) =>
    maskedWrite.readResult(index)            := readResult(index)
    maskedWrite.readChannel(index).ready     := readChannel(index).ready
    readChannel(index).valid                 := maskedWrite.readChannel(index).valid
    readChannel(index).bits.vs               := maskedWrite.readChannel(index).bits.vs
    readChannel(index).bits.offset           := maskedWrite.readChannel(index).bits.offset
    readChannel(index).bits.readSource       := 2.U
    readChannel(index).bits.instructionIndex := instReg.instructionIndex
    readChannel(index).bits.narrowVertical   := false.B
    readChannel(index).bits.rowOverride      := 0.U
    when(readVs1Valid && readVS1Req.readLane === index.U) {
      readChannel(index).valid       := true.B
      readChannel(index).bits.vs     := readVS1Req.vs
      readChannel(index).bits.offset := readVS1Req.offset
    }
    readVs1Fire(index)                       := request.fire && readVS1Req.readLane === index.U
    readVs1AckFire(index)                    := readResult(index).fire && readVS1Req.readLane === index.U
    readVs1AckData(index)                    := readResult(index).bits
  }

  val executeEnqValid: Bool = otherTypeRequestDeq

  val compressUnit = Instantiate(new MaskCompress(compressParam))
  omInstance.compressIn := compressUnit.io.om.asAnyClassType

  val extendUnit: MaskExtend = Module(new MaskExtend(parameter))

  val source2: UInt = VecInit(exeReqReg.map(_.bits.source2)).asUInt
  val source1: UInt = VecInit(exeReqReg.map(_.bits.source1)).asUInt

  val vs1Split: Seq[(UInt, Bool)] = Seq(0, 1, 2).map { sewInt =>
    val dataByte = 1 << sewInt
    val vs1Size  = (parameter.datapathWidth / 8) * parameter.laneNumber / dataByte
    val setSize  = parameter.datapathWidth / vs1Size
    val vs1SetIndex: UInt =
      if (parameter.datapathWidth <= vs1Size) true.B
      else {
        val needed = log2Ceil(setSize)
        if (needed <= parameter.laneParam.groupNumberBits)
          requestCounter(needed - 1, 0)
        else
          requestCounter.pad(needed)
      }
    val selectVS1:   UInt =
      if (parameter.datapathWidth <= vs1Size) readVS1Reg.data
      else
        cutUIntBySize(readVS1Reg.data, setSize)(vs1SetIndex)
    val willChangeVS1Index = vs1SetIndex.andR
    (selectVS1, willChangeVS1Index)
  }

  val compressSource1: UInt = Mux1H(sew1H, vs1Split.map(_._1))
  val source1Select:   UInt = Mux(mv, readVS1Reg.data, compressSource1)
  val source1Change:   Bool = Mux1H(sew1H, vs1Split.map(_._2))
  when(source1Change && compressUnit.io.in.fire) {
    readVS1Reg.dataValid   := false.B
    readVS1Reg.requestSend := false.B
    readVS1Reg.readIndex   := readVS1Reg.readIndex + 1.U
  }
  viotaCounterAdd := compressUnit.io.in.fire

  compressUnit.io.clock                  := implicitClock
  compressUnit.io.reset                  := implicitReset
  compressUnit.io.in.valid               := executeEnqValid && unitType(1)
  compressUnit.io.in.bits.maskType       := instReg.maskType
  compressUnit.io.in.bits.eew            := instReg.sew
  compressUnit.io.in.bits.uop            := instReg.decodeResult(Decoder.topUop)
  compressUnit.io.in.bits.readFromScalar := instReg.readFromScala
  compressUnit.io.in.bits.source1        := source1Select
  compressUnit.io.in.bits.mask           := executeElementMask
  compressUnit.io.in.bits.source2        := source2
  compressUnit.io.in.bits.pipeData       := source1
  compressUnit.io.in.bits.groupCounter   := requestCounter
  compressUnit.io.in.bits.lastCompress   := lastGroup
  compressUnit.io.in.bits.ffoInput       := VecInit(exeReqReg.map(_.bits.ffo)).asUInt
  compressUnit.io.in.bits.validInput     := VecInit(exeReqReg.map(_.valid)).asUInt
  compressUnit.io.newInstruction         := instReq.valid
  compressUnit.io.ffoInstruction         := instReq.bits.decodeResult(Decoder.topUop)(2, 0) === BitPat("b11?")

  compressUnitResultQueue.enq.valid := compressUnit.io.out.compressValid
  compressUnitResultQueue.enq.bits  := compressUnit.io.out

  when(compressUnit.io.in.fire) {
    readVS1Reg.sendToExecution := true.B
  }

  extendUnit.in.eew          := instReg.sew
  extendUnit.in.uop          := instReg.decodeResult(Decoder.topUop)
  extendUnit.in.source2      := source2
  extendUnit.in.groupCounter := requestCounter

  val executeResult: UInt = Mux1H(
    unitType(3, 1),
    Seq(
      compressUnitResultQueue.deq.bits.data,
      extendUnit.out,
      extendUnit.out
    )
  )

  executeReady := Mux1H(
    unitType,
    Seq(
      true.B,
      true.B,
      true.B,
      executeEnqValid
    )
  )

  compressUnitResultQueue.deq.ready := VecInit(maskedWrite.in.map(_.ready)).asUInt.andR
  val compressDeq:  Bool = compressUnitResultQueue.deq.fire
  val executeValid: Bool = Mux1H(
    unitType(3, 1),
    Seq(
      compressDeq,
      false.B,
      executeEnqValid
    )
  )

  executeGroupCounter := requestCounter

  val executeDeqGroupCounter: UInt = Mux1H(
    unitType(3, 1),
    Seq(
      compressUnitResultQueue.deq.bits.groupCounter,
      requestCounter,
      requestCounter
    )
  )

  val executeWriteByteMask: UInt = Mux(compress || ffo || mvVd, compressUnitResultQueue.deq.bits.mask, executeByteMask)
  maskedWrite.needWAR := maskDestinationType
  maskedWrite.vd      := instReg.vd
  maskedWrite.in.zipWithIndex.foreach { case (req, index) =>
    val bitMask    = cutUInt(currentMaskGroupForDestination, parameter.datapathWidth)(index)
    val maskFilter = !maskDestinationType || bitMask.orR
    req.valid             := executeValid && maskFilter
    req.bits.mask         := cutUIntBySize(executeWriteByteMask, parameter.laneNumber)(index)
    req.bits.data         := cutUInt(executeResult, parameter.datapathWidth)(index)
    req.bits.bitMask      := bitMask
    req.bits.groupCounter := executeDeqGroupCounter
    req.bits.ffoByOther   := compressUnitResultQueue.deq.bits.ffoOutput(index) && ffo
  }

  val writeQueue: Seq[QueueIO[MaskUnitExeResponse]] = Seq.tabulate(parameter.laneNumber) { _ =>
    Queue.io(new MaskUnitExeResponse(parameter.laneParam), maskUnitWriteQueueSize)
  }

  val dataNotInShifter: Bool = writeQueue.zipWithIndex.map { case (queue, index) =>
    queue.enq.valid              := maskedWrite.out(index).valid
    maskedWrite.out(index).ready := queue.enq.ready
    queue.enq.bits               := maskedWrite.out(index).bits
    queue.enq.bits.index         := instReg.instructionIndex

    val writePort = exeResp(index)
    queue.deq.ready                 := writePort.ready
    writePort.valid                 := queue.deq.valid
    writePort.bits.last             := DontCare
    writePort.bits.instructionIndex := instReg.instructionIndex
    writePort.bits.data             := queue.deq.bits.writeData.data
    writePort.bits.mask             := queue.deq.bits.writeData.mask
    writePort.bits.vd               := instReg.vd + queue.deq.bits.writeData.groupCounter(
      parameter.laneParam.groupNumberBits - 1,
      parameter.laneParam.vrfOffsetBits
    )
    writePort.bits.offset           := queue.deq.bits.writeData.groupCounter
    writePort.bits.narrowVertical   := false.B
    writePort.bits.rowOverride      := 0.U

    val writeTokenSize    = 8
    val writeTokenWidth   = log2Ceil(writeTokenSize)
    val writeTokenCounter = RegInit(0.U(writeTokenWidth.W))

    val writeTokenChange = Mux(writePort.fire, 1.U(writeTokenWidth.W), -1.S(writeTokenWidth.W).asUInt)
    when(writePort.fire ^ io.writeRelease(index)) {
      writeTokenCounter := writeTokenCounter + writeTokenChange
    }
    writeTokenCounter === 0.U
  }.reduce(_ && _)
  writeRD <> DontCare

  val waiteLastRequest: Bool = RegInit(false.B)
  val waitQueueClear:   Bool = RegInit(false.B)
  val lastReportValid = waitQueueClear && !writeQueue.map(_.deq.valid).reduce(_ || _) && dataNotInShifter
  when(lastReportValid) {
    waitQueueClear   := false.B
    waiteLastRequest := false.B
  }
  when(!readType && requestStageDeq && lastGroup) {
    waiteLastRequest := true.B
  }
  val executeStageInvalid: Bool = Mux1H(
    unitType(3, 1),
    Seq(
      !compressUnitResultQueue.deq.valid && !compressUnit.io.stageValid,
      true.B,
      true.B
    )
  )
  val executeStageClean: Bool = waiteLastRequest && maskedWrite.stageClear && executeStageInvalid
  val alwaysNeedExecute: Bool = enqMvRD
  val invalidEnq:        Bool = instReq.fire && !instReq.bits.vl && !alwaysNeedExecute
  when(executeStageClean || invalidEnq) {
    waitQueueClear := true.B
  }
  lastReport := maskAnd(
    lastReportValid,
    indexToOH(instReg.instructionIndex, parameter.chainingSize)
  )
  writeRDData := compressUnit.io.writeData

  when(gatherRequestFire) {
    when(notNeedRead) {
      gatherReadState := sResponse
    }.otherwise {
      gatherReadState := sRead
    }
  }

  gatherData.valid := gatherResponse
  gatherData.bits  := Mux(readVS1Reg.dataValid, readVS1Reg.data, 0.U)
  when(gatherData.fire) {
    gatherReadState := sWaitNextRow
  }
  when(gatherWaitNextRow && gatherRowDone) {
    gatherReadState := idle
  }
}
