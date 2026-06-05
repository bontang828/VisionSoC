package org.chipsalliance.t1.rtl.vrf

import chisel3._
import chisel3.experimental.hierarchy.{Instantiate}
import chisel3.util._
import org.chipsalliance.t1.rtl.{
  ffo,
  indexToOH,
  instIndexLE,
  maskAnd,
  ohCheck,
  LSUWriteCheck,
  VRFReadPipe,
  VRFReadRequest,
  VRFWriteReport,
  VRFWriteRequest
}

case class SharedVRFParam(
  vLen:                Int,
  laneNumber:          Int,
  datapathWidth:       Int,
  chainingSize:        Int,
  timeMultiplexBatch:  Int,
  // When true, back each bank with an explicit XPM block-RAM primitive
  // (fpga/wrapper/vrf_bank_bram.v) instead of the chisel-inferred SRAM. Forces
  // BRAM and dodges Vivado's 1 Mb behavioural-memory limit (large sweep bank =
  // 8192x128 = 1.05 Mb). Default false -> deployed/sim configs unchanged.
  bankBramPrimitive:   Boolean = false
) {
  // SharedVRF maps the 2 lanes of a row onto the 2 read-write ports of a dual-port
  // BRAM (lane 0 -> port 0, lane 1 -> port 1) -- the FPGA BRAM dual-port optimisation,
  // so laneNumber must be exactly 2. A wider dLen (more bits/cycle) is obtained by
  // KEEPING 2 lanes and widening the datapath (laneScale), which widens each bank's
  // word and scales numBanks below -- NOT by adding lanes. (laneNumber != 2 would have
  // to share or replicate the 2 BRAM ports and is not supported.)
  require(laneNumber == 2, "SharedVRF requires exactly 2 lanes (one per dual-port BRAM port)")

  // numBanks = byteCount = datapathWidth/8, the diagonal-banking invariant: the
  // byteCount column-neighbours of one datapath word must each land in a distinct
  // bank for a single-cycle vertical gather (replaces the old magic 8). With
  // laneNumber pinned at 2 (2 lanes -> 2 BRAM ports), the bank count scales with
  // dLen via laneScale (datapathWidth = laneScale*eLen):
  //   datapathWidth=128 (laneScale4, dLen256) -> 16 banks
  //   datapathWidth=64  (laneScale2, dLen128) -> 8  banks  (== original/deployed)
  //   datapathWidth=32  (laneScale1, dLen64)  -> 4  banks
  val numBanks:             Int = datapathWidth / 8
  val bankIdxBits:          Int = log2Ceil(numBanks)
  // Each bank is a dual-port BRAM: 2 RW ports, one per lane (laneNumber == 2). A
  // wider dLen adds more BANKS (numBanks above), not more ports per bank, for the
  // parallel vertical access. parallel byte-accesses/cycle = numBanks * 2 ports
  // = laneNumber * byteCount.
  val sramReadWritePorts:   Int = laneNumber
  val regNum:               Int = 32 //based on the RVV spec
  val regNumBits:           Int = log2Ceil(regNum)
  val instructionIndexBits: Int = log2Ceil(chainingSize) + 1
  val chaining1HBits:       Int = 2 << log2Ceil(chainingSize)
  val singleGroupSize:      Int = vLen / datapathWidth / laneNumber
  val vrfOffsetBits:        Int = log2Ceil(singleGroupSize)
  val ramWidth:             Int = datapathWidth
  val memoryWidth:          Int = ramWidth + 4
  val elementSize:          Int = vLen * 8 / datapathWidth / laneNumber

  val groupsPerRegister: Int = vLen / datapathWidth
  val entriesPerRow:     Int = regNum * groupsPerRegister
  val logicalAddrBits:   Int = log2Ceil(entriesPerRow)
  val totalEntries:      Int = timeMultiplexBatch * entriesPerRow
  val bankDepth:         Int = totalEntries / numBanks
  val bankAddrBits:      Int = log2Ceil(bankDepth)
  val rowCounterBits:    Int = log2Ceil(timeMultiplexBatch).max(1)
  val flatAddrBits:      Int = log2Ceil(totalEntries)

  val connectTree: Seq[Seq[Int]] = Seq.tabulate(chainingSize * 3 + 1 + 1) { i => Seq(i) }
  val vrfReadPort: Int           = connectTree.size
  val vrfReadLatency:            Int = 2

  // Vertical-mode rowCounter decomposition.
  // rowCounter is reinterpreted as the column index c in vertical mode and split into:
  //   [cVsOff | cGroup | cLane | cByte]
  // cByteBits  = log2(byteCount)         which byte within an SRAM word
  // cLaneBits  = log2(laneNumber)        which physical lane (lane 0 vs lane 1)
  // cGroupBits = vrfOffsetBits           which group offset within a register
  // cVsOffBits = remaining top bits      which register within the LMUL group
  val cByteBits:  Int = log2Ceil(ramWidth / 8)
  val cLaneBits:  Int = log2Ceil(laneNumber)
  val cGroupBits: Int = vrfOffsetBits
  val cVsOffBits: Int = rowCounterBits - cByteBits - cLaneBits - cGroupBits
  val cByteLo:    Int = 0
  val cLaneLo:    Int = cByteLo  + cByteBits
  val cGroupLo:   Int = cLaneLo  + cLaneBits
  val cVsOffLo:   Int = cGroupLo + cGroupBits
  require(cVsOffBits >= 0, s"rowCounter too narrow for vertical decomposition: rowCounterBits=$rowCounterBits, cByteBits=$cByteBits, cLaneBits=$cLaneBits, cGroupBits=$cGroupBits")

  // VRFParam for reuse with ChainingCheck, WriteCheck, VRFWriteReport
  def toVRFParam: VRFParam = VRFParam(vLen, laneNumber, datapathWidth, chainingSize, 1, RamType.p0rw)
}

  // Keeping the VRF port connection the same as the old VRF to integrate with the lane ports. The bundle direction is from the VRF, lane needs the Flipped
class LaneVRFPorts(param: VRFParam, rowCounterBits: Int = 1) extends Bundle {
  val readRequests: Vec[DecoupledIO[VRFReadRequest]] = Vec(
    param.vrfReadPort,
    Flipped(
      Decoupled(new VRFReadRequest(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits, rowCounterBits))
    )
  )
  val readResults: Vec[UInt] = Output(Vec(param.vrfReadPort, UInt(param.datapathWidth.W)))

  val readCheck:       Vec[VRFReadRequest] = Vec(
    param.chainingSize * 3 + 2,
    Input(new VRFReadRequest(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits))
  )
  val readCheckResult: Vec[Bool]           = Vec(param.chainingSize * 3 + 2, Output(Bool()))

  val write: DecoupledIO[VRFWriteRequest] = Flipped(
    Decoupled(
      new VRFWriteRequest(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits, param.datapathWidth, rowCounterBits)
    )
  )

  val writeCheck: Vec[LSUWriteCheck] = Vec(
    param.chainingSize + 1,
    Input(new LSUWriteCheck(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits))
  )
  val writeAllow: Vec[Bool]          = Vec(param.chainingSize + 1, Output(Bool()))

  val instructionWriteReport: ValidIO[VRFWriteReport] = Flipped(Valid(new VRFWriteReport(param)))
  val instructionLastReport:  UInt                    = Input(UInt(param.chaining1HBits.W))
  val lsuLastReport:          UInt                    = Input(UInt(param.chaining1HBits.W))
  val vrfSlotRelease:         UInt                    = Output(UInt(param.chaining1HBits.W))
  val instructionValid:       UInt                    = Output(UInt(param.chaining1HBits.W))
  val dataInLane:             UInt                    = Input(UInt(param.chaining1HBits.W))
  val writeReadyForLsu:       Bool                    = Output(Bool())
  val vrfReadyToStore:        Bool                    = Output(Bool())
  val loadDataInLSUWriteQueue: UInt                   = Input(UInt(param.chaining1HBits.W))
}

// Shared VRF = numBanks SRAM instances for diagonal skewing. numBanks is now
// derived from dLen (= dLen/(2*byteCount)): dLen=128 -> 8 banks (original),
// 256 -> 16, 64 -> 4. Each SRAM has laneNumber read-write ports (one per lane).
// Memory addressing (bankIdxBits = log2(numBanks)):
// | rowCounter (time-multiplex) | vs (reg index) | offset (group index) | laneIndex |
//   rowCounterBits                5 bits for 32 reg   vrfOffsetBits         log2(laneNumber)
//
// logicalAddr = vs ## offset ## laneIndex
// flatAddr    = rowCounter ## logicalAddr          <- full per-entry address map
// bankAddr    = flatAddr >> bankIdxBits            <- address used within a bank
// bank        = (rowCounter + logicalAddr)[bankIdxBits-1:0]

// XPM block-RAM BlackBox for a SharedVRF bank: true dual-port, byte-masked,
// 1-cycle read. Used only when bankBramPrimitive is set; forces BRAM and dodges
// Vivado's 1 Mb behavioural-memory limit. Synth-only (no Verilator resource) -
// Vivado resolves it from fpga/wrapper/vrf_bank_bram.v via system_top.tcl.
class VrfBankBram(dataWidth: Int, addrWidth: Int, memDepth: Int)
    extends BlackBox(
      Map("DATA_WIDTH" -> dataWidth, "ADDR_WIDTH" -> addrWidth, "MEM_DEPTH" -> memDepth)
    ) {
  override def desiredName: String = "vrf_bank_bram"
  val io = IO(new Bundle {
    val clk    = Input(Clock())
    val a_en   = Input(Bool())
    val a_we   = Input(UInt((dataWidth / 8).W))
    val a_addr = Input(UInt(addrWidth.W))
    val a_din  = Input(UInt(dataWidth.W))
    val a_dout = Output(UInt(dataWidth.W))
    val b_en   = Input(Bool())
    val b_we   = Input(UInt((dataWidth / 8).W))
    val b_addr = Input(UInt(addrWidth.W))
    val b_din  = Input(UInt(dataWidth.W))
    val b_dout = Output(UInt(dataWidth.W))
  })
}

// Uniform per-bank dual-port memory so the read/write fabric drives either the
// inferred chisel SRAM (default) or the XPM BRAM BlackBox through the same calls.
// The SRAM path produces RTL identical to before, so non-flag configs (including
// the deployed bitstream config) are byte-for-byte unchanged.
sealed trait BankMem {
  def drive(port: Int, address: UInt, enable: Bool, isWrite: Bool,
            writeData: Vec[UInt], mask: Vec[Bool]): Unit
  def readData(port: Int): UInt
}
final class SramBankMem(rf: SRAMInterface[Vec[UInt]]) extends BankMem {
  def drive(port: Int, address: UInt, enable: Bool, isWrite: Bool,
            writeData: Vec[UInt], mask: Vec[Bool]): Unit = {
    rf.readwritePorts(port).address   := address
    rf.readwritePorts(port).enable    := enable
    rf.readwritePorts(port).isWrite   := isWrite
    rf.readwritePorts(port).writeData := writeData
    rf.readwritePorts(port).mask.get  := mask
  }
  def readData(port: Int): UInt = rf.readwritePorts(port).readData.asUInt
}
final class BramBankMem(bb: VrfBankBram) extends BankMem {
  def drive(port: Int, address: UInt, enable: Bool, isWrite: Bool,
            writeData: Vec[UInt], mask: Vec[Bool]): Unit = {
    val we = Mux(isWrite, mask.asUInt, 0.U)
    if (port == 0) {
      bb.io.a_en := enable; bb.io.a_we := we; bb.io.a_addr := address; bb.io.a_din := writeData.asUInt
    } else {
      bb.io.b_en := enable; bb.io.b_we := we; bb.io.b_addr := address; bb.io.b_din := writeData.asUInt
    }
  }
  def readData(port: Int): UInt = if (port == 0) bb.io.a_dout else bb.io.b_dout
}

class SharedVRF(val parameter: SharedVRFParam) extends Module {
  val vrfParam: VRFParam = parameter.toVRFParam

  val lanePorts:    Vec[LaneVRFPorts] = IO(Vec(parameter.laneNumber, new LaneVRFPorts(vrfParam, parameter.rowCounterBits)))
  val rowCounter:   UInt              = IO(Input(UInt(parameter.rowCounterBits.W)))
  /** Bon2D: when high, read/write fabric switches to 8-way column gather/scatter across banks. */
  val verticalMode: Bool              = IO(Input(Bool()))

  // ------ SRAM banks: 8 banks, each dual-ported (2 RW ports) -----
  val sramReady:      Bool = RegInit(false.B)
  val sramResetCount: UInt = RegInit(0.U(parameter.bankAddrBits.W))
  val resetValid:     Bool = !sramReady
  when(resetValid) {
    sramResetCount := sramResetCount + 1.U
    when(sramResetCount.andR) { sramReady := true.B }
  }

  // Bon2D: byte-granular writes for vertical-mode scatter. Each SRAM element is a byte; mask selects which bytes update.
  // byteCount = bytes per datapath word (= ramWidth/8). This is the vertical-mode
  // gather/scatter width and is INDEPENDENT of numBanks (which now scales with dLen).
  val byteCount: Int = parameter.ramWidth / 8
  // Dual-port banks (sramReadWritePorts = min(laneNumber,2)). numBanks scales with
  // datapathWidth so wider datapaths get more banks for parallel vertical access.
  // bankBramPrimitive picks an explicit XPM block-RAM per bank; otherwise the
  // chisel-inferred SRAM (identical to the original design).
  val vrfBanks: Seq[BankMem] = Seq.tabulate(parameter.numBanks) { _ =>
    if (parameter.bankBramPrimitive) {
      val bb = Module(new VrfBankBram(parameter.ramWidth, parameter.bankAddrBits, parameter.bankDepth))
      bb.io.clk := clock
      new BramBankMem(bb)
    } else {
      new SramBankMem(SRAM.masked(
        size = parameter.bankDepth,
        tpe = Vec(byteCount, UInt(8.W)),
        numReadPorts = 0,
        numWritePorts = 0,
        numReadwritePorts = parameter.sramReadWritePorts
      ))
    }
  }

  // ----- Compute bank index and bank-internal address ----
  // Dynamic rc + dynamic laneIdx so vertical mode can feed per-byte rowCounter_i and byte-lane.
  // logicalAddr = vs ## offset ## laneField, where the lane field is cLaneBits wide
  // (= log2(laneNumber)); empty when laneNumber==1. Generalises the old hard-coded
  // `laneIdx(0)` which assumed exactly 2 lanes and made logicalAddr 1 bit too narrow
  // at laneNumber=4 (dLen=256). Width is now exactly logicalAddrBits for any laneNumber.
  def logicalAddrOf(vs: UInt, offset: UInt, laneIdx: UInt): UInt =
    if (parameter.cLaneBits == 0) vs ## offset
    else vs ## offset ## laneIdx.pad(parameter.cLaneBits)(parameter.cLaneBits - 1, 0)

  def bankSelectRC(vs: UInt, offset: UInt, laneIdx: UInt, rc: UInt): UInt = {
    val logicalAddr = logicalAddrOf(vs, offset, laneIdx)
    // bank = (rc + logicalAddr) mod numBanks  (diagonal skew). Index width = log2(numBanks).
    UIntToOH((rc +& logicalAddr)(parameter.bankIdxBits - 1, 0), parameter.numBanks)
  }

  def bankInternalAddrRC(vs: UInt, offset: UInt, laneIdx: UInt, rc: UInt): UInt = {
    val logicalAddr = logicalAddrOf(vs, offset, laneIdx)
    val flatAddr    = rc ## logicalAddr
    // within-bank address = flatAddr / numBanks  (>> log2(numBanks)).
    val shifted     = (flatAddr >> parameter.bankIdxBits).asUInt
    shifted(parameter.bankAddrBits - 1, 0)
  }

  // Horizontal-mode convenience wrappers - use module's global rowCounter and compile-time laneIdx.
  def bankSelect(vs: UInt, offset: UInt, laneIdx: Int): UInt =
    bankSelectRC(vs, offset, laneIdx.U(parameter.cLaneBits.max(1).W), rowCounter)

  def bankInternalAddr(vs: UInt, offset: UInt, laneIdx: Int): UInt =
    bankInternalAddrRC(vs, offset, laneIdx.U(parameter.cLaneBits.max(1).W), rowCounter)

  // ---- Per-lane logic ------
  // Some of the logics are similar to the old VRF.scala (chaining record maintenance, hazard detection, write check), but the read/write arbitration and bank selection are redesigned to fit the shared SRAM architecture.
  val perLaneReadPipe:    Seq[Seq[ValidIO[VRFReadPipe]]] = Seq.fill(parameter.laneNumber)(
    Seq.tabulate(parameter.numBanks) { _ =>
      RegInit(0.U.asTypeOf(Valid(new VRFReadPipe(parameter.bankDepth))))
    }
  )
  val perLaneBankReadF:   Seq[Vec[UInt]] = Seq.fill(parameter.laneNumber)(
    Wire(Vec(parameter.vrfReadPort, UInt(parameter.numBanks.W)))
  )
  val perLaneReadResultF: Seq[Vec[UInt]] = Seq.fill(parameter.laneNumber)(
    Wire(Vec(parameter.numBanks, UInt(parameter.ramWidth.W)))
  )
  val perLaneWritePipe:     Seq[ValidIO[VRFWriteRequest]] = Seq.tabulate(parameter.laneNumber) { _ =>
    RegInit(0.U.asTypeOf(Valid(new VRFWriteRequest(
      parameter.regNumBits, parameter.vrfOffsetBits, parameter.instructionIndexBits, parameter.datapathWidth
    ))))
  }
  // writeBankPipe and writeAddrPipe are created locally inside the per-lane loop via RegNext
  for (laneIdx <- 0 until parameter.laneNumber) {
    val ports          = lanePorts(laneIdx)
    val write          = ports.write
    val readRequests   = ports.readRequests
    val readResults    = ports.readResults

    val firstReadPipe  = perLaneReadPipe(laneIdx)
    val bankReadF      = perLaneBankReadF(laneIdx)
    val readResultF    = perLaneReadResultF(laneIdx)
    val writePipe      = perLaneWritePipe(laneIdx)

    // ----- Write pipe ----
    val writeBank = bankSelect(write.bits.vd, write.bits.offset, laneIdx)
    val writeAddr = bankInternalAddr(write.bits.vd, write.bits.offset, laneIdx)
    writePipe.valid := write.fire
    when(write.fire) { writePipe.bits := write.bits }
    val writeBankPipe: UInt = RegNext(writeBank)
    val writeAddrPipe: UInt = RegNext(writeAddr)

    // ---- Chaining records (same logic as VRF.scala) -----
    val chainingRecord:     Vec[ValidIO[VRFWriteReport]] = RegInit(
      VecInit(Seq.fill(parameter.chainingSize + 1)(0.U.asTypeOf(Valid(new VRFWriteReport(vrfParam)))))
    )
    val chainingRecordCopy: Vec[ValidIO[VRFWriteReport]] = RegInit(
      VecInit(Seq.fill(parameter.chainingSize + 1)(0.U.asTypeOf(Valid(new VRFWriteReport(vrfParam)))))
    )
    val recordRelease: Vec[UInt] = WireDefault(
      VecInit(Seq.fill(parameter.chainingSize + 1)(0.U.asTypeOf(UInt(parameter.chaining1HBits.W))))
    )
    val recordValidVec: Seq[Bool] = chainingRecord.map(r => !r.bits.elementMask.andR && r.valid)

    // Bon2D: lookup whether an in-flight instruction is an LSU op (vle/vse)
    // and the verticalMode snapshot captured when that instruction reached Lane.
    def isLSUInst(instIdx: UInt): Bool =
      chainingRecord
        .map(r => r.valid && (r.bits.instIndex === instIdx) && (r.bits.ls || r.bits.st))
        .reduce(_ || _)

    def instVerticalMode(instIdx: UInt): Bool =
      chainingRecord
        .map(r => r.valid && (r.bits.instIndex === instIdx) && r.bits.verticalMode)
        .reduce(_ || _)

    // ------ Read check (chaining hazard detection, same logic as VRF.scala) ----
    ports.readCheck.zip(ports.readCheckResult).foreach { case (req, res) =>
      val recordSelect = chainingRecord
      val readRecord   =
        Mux1H(recordSelect.map(_.bits.instIndex === req.instructionIndex), recordSelect.map(_.bits))
      res :=
        recordSelect
          .zip(recordValidVec)
          .zipWithIndex
          .map { case ((r, f), recordIndex) =>
            val checkModule = Instantiate(new ChainingCheck(vrfParam))
            checkModule.read        := req
            checkModule.readRecord  := readRecord
            checkModule.record      := r
            checkModule.recordValid := f
            checkModule.checkResult
          }
          .reduce(_ && _)
    }

    // --- Read arbitration across 8 banks (current config p0rw: 1 RW port per lane per bank) -----
    val checkSize: Int = readRequests.size

    // Horizontal-mode per-port outputs. The fold writes these Wires (not the IO directly);
    // a single explicit Mux below picks between horizontal and vertical outputs.
    val hReady:  Vec[Bool] = Wire(Vec(readRequests.size, Bool()))
    val hResult: Vec[UInt] = Wire(Vec(readResults.size, UInt(parameter.datapathWidth.W)))

    // Narrow-vertical per-port outputs (one byte returned in the low byte lane).
    val nResult: Vec[UInt] = Wire(Vec(readResults.size, UInt(parameter.datapathWidth.W)))
    // Per-port pipelined "this read fired as narrow vertical", used by the output mux.
    val nPipe:   Vec[Bool] = Wire(Vec(readResults.size, Bool()))
    // Per-port narrow bank/addr (for the SRAM-port loop further down).
    val nBankPerPort: Seq[UInt] = Seq.tabulate(readRequests.size)(_ => Wire(UInt(parameter.numBanks.W)))
    val nAddrPerPort: Seq[UInt] = Seq.tabulate(readRequests.size)(_ => Wire(UInt(parameter.bankAddrBits.W)))

    // Hoisted from the wide-vertical block below so the narrow-read path inside
    // the read fold can reuse them. Slice rowCounter into the vertical-mode
    // (cVsOff, cGroup, cLane, cByte) decomposition. Any field whose bit width is 0
    // collapses to 0.U(0.W) so configs with LMUL=1 or singleGroupSize=1 still elaborate.
    def sliceField(lo: Int, width: Int): UInt =
      if (width == 0) 0.U(0.W) else rowCounter(lo + width - 1, lo)
    val cVsOff: UInt = sliceField(parameter.cVsOffLo, parameter.cVsOffBits)
    val cGroup: UInt = sliceField(parameter.cGroupLo, parameter.cGroupBits)
    val cLane:  UInt = sliceField(parameter.cLaneLo,  parameter.cLaneBits)
    val cByte:  UInt = sliceField(parameter.cByteLo,  parameter.cByteBits)
    def vsLowField(vs: UInt): UInt =
      if (parameter.cVsOffBits == 0) 0.U(0.W) else vs(parameter.cVsOffBits - 1, 0)
    def offsetField(off: UInt): UInt =
      if (parameter.cGroupBits == 0) 0.U(0.W) else off(parameter.cGroupBits - 1, 0)

    // Bon2D Phase 3b: hoisted so the read fold below can suppress wide-vertical
    // bankCorrect when a narrow request preempts the same cycle. Wide-vert
    // would otherwise pollute bankReadF/hReadAddr with its bank_h address even
    // though its r.ready is held low.
    val anyNarrowReq: Bool = readRequests.map(r => r.valid && r.bits.narrowVertical).reduce(_ || _)

    val (firstOccupied, _) = readRequests.zipWithIndex.foldLeft(
      (0.U(parameter.numBanks.W), 0.U(parameter.numBanks.W))
    ) {
      case ((o, t), (v, i)) => // Inheretaing the old structure in VRF.scala, where it used t for second port per lane, but we only have 1 port per lane, hence t is discarded and not tracked
        val recordSelect      = if (i < (checkSize / 2)) chainingRecord else chainingRecordCopy
        val readRecord        =
          Mux1H(recordSelect.map(_.bits.instIndex === v.bits.instructionIndex), recordSelect.map(_.bits))
        val portConflictCheck = Wire(Bool())
        val checkResult: Option[Bool] = Option.when(i == (readRequests.size - 1)) {
          recordSelect
            .zip(recordValidVec)
            .zipWithIndex
            .map { case ((r, f), recordIndex) =>
              val checkModule = Instantiate(new ChainingCheck(vrfParam))
              checkModule.suggestName(s"ChainingCheck_lane${laneIdx}_readPort${i}_record${recordIndex}")
              checkModule.read        := v.bits
              checkModule.readRecord  := readRecord
              checkModule.record      := r
              checkModule.recordValid := f
              checkModule.checkResult
            }
            .reduce(_ && _) && portConflictCheck
        }
        val validCorrect: Bool = if (i == (readRequests.size - 1)) v.valid && checkResult.get else v.valid

        // Horizontal address compute (legacy).
        val bank_h = bankSelect(v.bits.vs, v.bits.offset, laneIdx)

        // Narrow-vertical per-port address (Phase 3c, layout-confirmed):
        // The wide-vert fold reads bank/addr derived from
        // (vsStore = upper(vs) ## cVsOff, cGroup, cLane, rcI = rcBase + i)
        // with rcBase = Cat(vsLow_request, offset_request, laneIdx, 0). The
        // returned byte at gathered_word[i] is byte cByte (= rowCounter[2:0])
        // of the bank holding (rcI=rcBase+i, vsStore_c, cGroup_c, cLane_c) =
        // grid_in[rcBase+i][c]. To return the same byte at offset dataOffset,
        // narrow uses rc_n = rcBase + dataOffset (= rowOverride), but the
        // logicalAddr fields (vsStore/cGroup/cLane) and the byte selector
        // (cByte) MUST come from the current rowCounter - not from the
        // request's vs/offset/laneIdx. Otherwise the narrow read hits a
        // different SRAM cell than the wide-fold's i-th byte.
        val rcBase_n: UInt = {
          val vsLowPart   = if (parameter.cVsOffBits == 0) 0.U(0.W)
                            else v.bits.vs(parameter.cVsOffBits - 1, 0)
          val offsetPart  = if (parameter.cGroupBits == 0) 0.U(0.W)
                            else v.bits.offset(parameter.cGroupBits - 1, 0)
          val laneIdxPart = if (parameter.cLaneBits == 0) 0.U(0.W)
                            else laneIdx.U(parameter.cLaneBits.W)
          val byteZero    = 0.U(parameter.cByteBits.W)
          Cat(vsLowPart, offsetPart, laneIdxPart, byteZero)
        }
        val rc_n      = (rcBase_n + v.bits.rowOverride)(parameter.rowCounterBits - 1, 0)
        // logicalAddr fields tracked from rowCounter (matching wide-vert path).
        val vsStoreUpper_n: UInt =
          if (parameter.cVsOffBits == parameter.regNumBits) 0.U(0.W)
          else v.bits.vs(parameter.regNumBits - 1, parameter.cVsOffBits)
        val vsStore_n = Cat(vsStoreUpper_n, cVsOff)
        val cGroup_n  = cGroup
        val cLane_n   = cLane
        val cByte_n   = cByte
        val bank_n    = bankSelectRC(vsStore_n, cGroup_n, cLane_n, rc_n)
        val addr_n    = bankInternalAddrRC(vsStore_n, cGroup_n, cLane_n, rc_n)
        nBankPerPort(i) := bank_n
        nAddrPerPort(i) := addr_n

        // Effective bank: narrow path uses 1 bank just like horizontal, so it joins
        // firstOccupied for unified arbitration with horizontal LSU traffic.
        val bank        = Mux(v.bits.narrowVertical, bank_n, bank_h)
        val pipeBank    = Pipe(true.B, bank, parameter.vrfReadLatency).bits
        // Bon2D Phase 3b: when a narrow request preempts wide-vert this cycle,
        // wide-vert reads stay valid but not ready (see r.ready logic below).
        // Their bank_h must NOT contribute to bankCorrect/bankReadF, otherwise
        // hReadAddr's Mux1H would steer the SRAM port to the wide-vert's bank_h
        // address even though that read isn't actually firing.
        val readTakeVertical_i =
          Mux(isLSUInst(v.bits.instructionIndex), instVerticalMode(v.bits.instructionIndex), verticalMode)
        val isWideVertReq_i  = !v.bits.narrowVertical && readTakeVertical_i
        val wideVertPreempt  = isWideVertReq_i && anyNarrowReq
        val bankCorrect = Mux(validCorrect && !wideVertPreempt, bank, 0.U(parameter.numBanks.W))

        //p0rw: check against firstOccupied
        portConflictCheck := true.B
        val portReady: Bool = if (i == (readRequests.size - 1)) { //this checks the bank wanted and occupied have no conflict, else set ready to low to stall the read request
          (bank & (~o)).orR && checkResult.get
        } else {
          (bank & (~o)).orR
        }
        hReady(i) := portReady && sramReady

        bankReadF(i) := bankCorrect & (~o)

        val pipeFire = Pipe(true.B, v.fire, parameter.vrfReadLatency).bits
        hResult(i) := Mux(pipeFire, Mux1H(pipeBank, readResultF), 0.U)

        // Narrow-vertical result extraction: select cByte byte of the bank word.
        // pipelined cByte and narrowVertical flag follow the SRAM read-latency.
        val cBytePipe_n  = Pipe(true.B, cByte_n, parameter.vrfReadLatency).bits
        val narrowPipe_i = Pipe(true.B, v.fire && v.bits.narrowVertical, parameter.vrfReadLatency).bits
        nPipe(i) := narrowPipe_i
        val bankWord_n   = Mux1H(pipeBank, readResultF)
        val byteVec_n    = VecInit(Seq.tabulate(byteCount)(b => bankWord_n(8 * b + 7, 8 * b)))
        nResult(i) := Mux(narrowPipe_i, byteVec_n(cBytePipe_n).pad(parameter.datapathWidth), 0.U)

        (o | bankCorrect, t) //update the occupied banks with the current request for the next request in the slot
    }

    // ==========================================================================
    // Bon2D: Vertical-mode read-gather and write-scatter overlay.
    //
    // Generic across baseLMUL: works for any (vLen, baseLMUL) combo so long as
    // the require(cVsOffBits >= 0) at line 67 holds. The 128x128 image grid
    // is determined by rowCounterBits=7 (timeMultiplexBatch=128 hw-rows) and
    // by the LSU's logicalRowElements=128, NOT by the LMUL choice.
    //
    // rowCounter reinterpreted as column index c in vertical mode. For a lane
    // read request (vs,offset,L), the address fields decompose as
    //   [cVsOff | cGroup | cLane | cByte]
    // and the wide-vert path fires numBanks (=8) rcI = rcBase+i in parallel
    // across distinct banks via the diagonal skew bank=(rc+logicalAddr)(2,0).
    //
    // Worked example, baseLMUL=4 / vLen=256 / SEW=8 / vl=128:
    //   singleGroupSize=2, vrfOffsetBits=1, cVsOffBits=2
    //   rc_i     = 8*(2*vs(1,0) + offset(0)*2 + L*2) + i  for i in 0..7
    //   vs_s     = vs(4,2) ## cVsOff
    //   off_s    = cGroup, laneIdx_s = cLane, byte_s = cByte
    //
    // Worked example, baseLMUL=1 / vLen=1024 / SEW=8 / vl=128:
    //   singleGroupSize=8, vrfOffsetBits=3, cVsOffBits=0
    //   rcBase fills all 7 bits of rowCounter from (offset | L | 0)
    //   vsStore = vs (no LMUL-group splitting; LMUL=1 has 1 reg/group)
    //   rest unchanged. Same 8-bank diagonal fanout.
    // ==========================================================================
    // (cVsOff/cGroup/cLane/cByte and the vsLowField/offsetField helpers are
    // hoisted above the read fold so the narrow-vertical read path can reuse them.)

    // Priority-select first valid read request - only one vertical read per lane per cycle.
    val readValidVec:     Seq[Bool] = readRequests.map(_.valid)
    val anyReadValid:     Bool      = readValidVec.reduce(_ || _)
    val firstReadOH:      UInt      = PriorityEncoderOH(VecInit(readValidVec).asUInt)
    val firstReadReq:     VRFReadRequest = Mux1H(firstReadOH, readRequests.map(_.bits))
    val firstReadIdxPipe: UInt = Pipe(true.B, firstReadOH, parameter.vrfReadLatency).bits

    val firstReadFromLSU: Bool = isLSUInst(firstReadReq.instructionIndex)
    val firstReadVertEffective: Bool =
      Mux(firstReadFromLSU, instVerticalMode(firstReadReq.instructionIndex), verticalMode)
    // Bon2D Phase 3b: narrow-vertical reads preempt wide-vertical reads in the
    // same cycle. When any port has a narrow request valid, fall through to the
    // horizontal SRAM-port path (which carries narrow's per-port nAddrPerPort
    // via the bankReadF Mux1H). Wide-vert reads concurrent with narrow are
    // stalled below by the per-port r.ready logic.
    val useVerticalRead:  Bool = firstReadVertEffective && !anyNarrowReq
    val vReadFire: Bool = useVerticalRead && anyReadValid && sramReady
    // vsStore replaces the bottom cVsOffBits of the requested vs with the column-derived cVsOff.
    val vsStoreUpper: UInt =
      if (parameter.cVsOffBits == parameter.regNumBits) 0.U(0.W)
      else firstReadReq.vs(parameter.regNumBits - 1, parameter.cVsOffBits)
    val vsStore: UInt = Cat(vsStoreUpper, cVsOff)
    // rcBase packs (vsLow | offsetLow | laneIdx) into the rowCounter slot positions
    // [cVsOffLo:cByteBits], leaving the bottom cByteBits zero so rcI = rcBase + i sweeps
    // 8 image rows differing only in the byte-lane subfield.
    // (vsLowField/offsetField helpers are hoisted above.)
    val rcBase: UInt = Cat(
      vsLowField(firstReadReq.vs),
      offsetField(firstReadReq.offset),
      laneIdx.U(parameter.cLaneBits.W),
      0.U(parameter.cByteBits.W)
    )
    // One row-neighbour per byte of the datapath word (byteCount), NOT per bank.
    // With numBanks decoupled from byteCount, these byteCount neighbours fan out
    // across (a subset of) the numBanks physical banks via the diagonal skew.
    val rcI:    Seq[UInt] = Seq.tabulate(byteCount) { i =>
      (rcBase + i.U)(parameter.rowCounterBits - 1, 0)
    }
    val vBankOHPerI: Seq[UInt] = rcI.map(rc => bankSelectRC(vsStore, cGroup, cLane, rc))
    val vAddrPerI:   Seq[UInt] = rcI.map(rc => bankInternalAddrRC(vsStore, cGroup, cLane, rc))

    // Per-bank: exactly one i maps here (diagonal skew); pick that i's address.
    val vBankValid: Vec[Bool] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      VecInit(vBankOHPerI.map(_(b))).asUInt.orR && vReadFire
    })
    val vBankAddr: Vec[UInt] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      Mux1H(VecInit(vBankOHPerI.map(_(b))), vAddrPerI)
    })
    // vBankOHPerI_pipe indexed by i (not by bank). Used to re-assemble 8 bytes in output.
    val vBankOHPerIPipe: Seq[UInt] = vBankOHPerI.map(oh => Pipe(true.B, oh, parameter.vrfReadLatency).bits)
    val cBytePipe:       UInt      = Pipe(true.B, cByte, parameter.vrfReadLatency).bits
    val vReadFirePipe:   Bool      = Pipe(true.B, vReadFire, parameter.vrfReadLatency).bits

    // ----- Write ready: write can proceed if its bank is not occupied by reads ---
    // Wide vertical read locks all 8 banks for that cycle; horizontal/LSU writes
    // touch a single bank.
    // Bon2D Phase 3c: a wide-vertical WRITE scatters across all 8 banks
    // (vWritePerBankValid is gated by writePipe.bits.mask per byte, so banks
    // whose corresponding byte is masked-out do not fire - see the per-bank
    // valid below). For arbitration we still expand to all-banks here, which
    // is a conservative no-overlap check: a scatter write stalls if any narrow
    // read is pending on any of the 8 potential banks, even if the mask would
    // gate that bank out. Tightening this is a perf opt, not a correctness fix.
    val writeWouldScatter: Bool =
      Mux(isLSUInst(write.bits.instructionIndex), instVerticalMode(write.bits.instructionIndex), verticalMode)
    val allBanksMask: UInt = ((BigInt(1) << parameter.numBanks) - 1).U(parameter.numBanks.W)
    val verticalOccupy: UInt = Mux(vReadFire, allBanksMask, 0.U(parameter.numBanks.W))
    val effectiveOccupied: UInt = Mux(useVerticalRead, verticalOccupy, firstOccupied)
    val effWriteBank: UInt = Mux(writeWouldScatter, allBanksMask, writeBank)
    write.ready := sramReady && (effWriteBank & effectiveOccupied) === 0.U

    val writeData:    UInt = Mux(resetValid, 0.U(parameter.datapathWidth.W), writePipe.bits.data)
    val writeAddress: UInt = Mux(resetValid, sramResetCount, writeAddrPipe)
    // Horizontal/reset writes update all bytes. Vertical scatter overrides per-bank below.
    val writeByteVec: Vec[UInt] = VecInit(
      Seq.tabulate(byteCount)(i => writeData(8 * i + 7, 8 * i))
    )
    val writeByteMaskAll: Vec[Bool] = VecInit(Seq.fill(byteCount)(true.B))

    // Vertical write scatter: latched write bits live in writePipe. Interpret write as
    // vertical column-c store when verticalMode. The 8 bytes of writePipe.data fan out to
    // 8 banks at distinct rc_i, each at the same byte slot cByte_s = cByte.
    val writeFromLSU:     Bool = isLSUInst(writePipe.bits.instructionIndex)
    val useVerticalWrite: Bool =
      Mux(writeFromLSU, instVerticalMode(writePipe.bits.instructionIndex), verticalMode)
    val vWriteFire:  Bool = useVerticalWrite && writePipe.valid
    when(writePipe.valid && writeFromLSU) {
      when(useVerticalWrite) {
        assert(instVerticalMode(writePipe.bits.instructionIndex),
          "LSU on vertical write path but its snapshot is 0 - " +
            "gate is reading live verticalMode instead of snapshot")
      }
      when(instVerticalMode(writePipe.bits.instructionIndex)) {
        assert(useVerticalWrite,
          "LSU snapshot is vertical but gate routed horizontal - " +
            "gate is reading live verticalMode instead of snapshot")
      }
    }
    // Mirror of vsStore/rcBase but for the write side.
    val vsStoreWUpper: UInt =
      if (parameter.cVsOffBits == parameter.regNumBits) 0.U(0.W)
      else writePipe.bits.vd(parameter.regNumBits - 1, parameter.cVsOffBits)
    val vsStoreW:    UInt = Cat(vsStoreWUpper, cVsOff)
    val rcBaseW: UInt = Cat(
      vsLowField(writePipe.bits.vd),
      offsetField(writePipe.bits.offset),
      laneIdx.U(parameter.cLaneBits.W),
      0.U(parameter.cByteBits.W)
    )
    // One scatter source per byte of the datapath word (byteCount), NOT per bank.
    val rcIW: Seq[UInt] = Seq.tabulate(byteCount) { i =>
      (rcBaseW + i.U)(parameter.rowCounterBits - 1, 0)
    }
    val vWriteBankOHPerI: Seq[UInt] = rcIW.map(rc => bankSelectRC(vsStoreW, cGroup, cLane, rc))
    val vWriteAddrPerI:   Seq[UInt] = rcIW.map(rc => bankInternalAddrRC(vsStoreW, cGroup, cLane, rc))
    val writeByteOfI:     Seq[UInt] = Seq.tabulate(byteCount) { i =>
      writePipe.bits.data(8 * i + 7, 8 * i)
    }

    // Per-bank vertical write: one i maps to each bank (diagonal skew).
    // Bon2D Phase 3c: gate by the upstream per-byte mask. Without this, a
    // gather-style write (mask=1<<writeOffset, only byte writeOffset valid)
    // would still scatter the other 7 zero bytes into 7 sibling banks,
    // clobbering neighbour vd[i+1..i+7] cells at the current cByte slot.
    // For full-mask wide-vert writes (e.g. vector compute) all 8 mask bits
    // are 1, so every bank still fires - preserves Phase 2 baseline.
    val vWritePerBankValid: Vec[Bool] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      val maskedHits: Seq[Bool] = (0 until byteCount).map(i =>
        vWriteBankOHPerI(i)(b) && writePipe.bits.mask(i))
      VecInit(maskedHits).asUInt.orR && vWriteFire
    })
    val vWritePerBankAddr: Vec[UInt] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      Mux1H(VecInit(vWriteBankOHPerI.map(_(b))), vWriteAddrPerI)
    })
    val vWritePerBankByte: Vec[UInt] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      Mux1H(VecInit(vWriteBankOHPerI.map(_(b))), writeByteOfI)
    })

    // ----- Connect to SRAM banks (lane's port) ----
    // Only lane 0 drives reset (avoids same-address dual-port write during reset)
    val laneResetValid: Bool = if (laneIdx == 0) resetValid else false.B
    // One RW port per lane (laneNumber == 2): lane 0 -> port 0, lane 1 -> port 1.
    val portIdx: Int = laneIdx % parameter.sramReadWritePorts
    vrfBanks.zipWithIndex.foreach { case (bankMem, bank) =>
      val hWriteValid: Bool = writePipe.valid && writeBankPipe(bank) && !useVerticalWrite
      val vWriteValid: Bool = vWritePerBankValid(bank)
      val ramWriteValid: Bool = hWriteValid || vWriteValid || laneResetValid

      // Per-port read address: legacy horizontal uses bankInternalAddr; narrow
      // overrides with the per-port narrow address (rowOverride-derived).
      val hReadAddr: UInt = Mux1H(
        bankReadF.map(_(bank)),
        readRequests.zipWithIndex.map { case (r, i) =>
          Mux(r.bits.narrowVertical, nAddrPerPort(i),
            bankInternalAddr(r.bits.vs, r.bits.offset, laneIdx))
        }
      )
      val hReadValid: Bool = bankReadF.map(_(bank)).reduce(_ || _)

      val readAddr:  UInt = Mux(useVerticalRead, vBankAddr(bank), hReadAddr)
      val readValid: Bool = Mux(useVerticalRead, vBankValid(bank), hReadValid)

      firstReadPipe(bank).bits.address := readAddr
      firstReadPipe(bank).valid        := readValid

      // Wide-vertical scatter: broadcast the 1-byte update to byte lane cByte of the 8-byte slot.
      val vWriteByteVec: Vec[UInt] = VecInit(Seq.fill(byteCount)(vWritePerBankByte(bank)))
      val vWriteMask:    Vec[Bool] = VecInit(Seq.tabulate(byteCount)(i => cByte === i.U))

      val portWriteData: Vec[UInt] =
        Mux(useVerticalWrite && !laneResetValid, vWriteByteVec, writeByteVec)
      val portWriteMask: Vec[Bool] =
        Mux(useVerticalWrite && !laneResetValid, vWriteMask, writeByteMaskAll)
      val portWriteAddr: UInt =
        Mux(useVerticalWrite && !laneResetValid, vWritePerBankAddr(bank), writeAddress)

      bankMem.drive(
        portIdx,
        Mux(ramWriteValid, portWriteAddr, firstReadPipe(bank).bits.address),
        ramWriteValid || firstReadPipe(bank).valid,
        ramWriteValid,
        portWriteData,
        portWriteMask
      )
      readResultF(bank) := bankMem.readData(portIdx)
    }

    // Vertical-mode per-port outputs. Computed unconditionally; selected via Mux below.
    // Gather 8 bytes: byte_i = cByte-th byte of readResultF(bank where rc_i landed).
    val gatheredBytes: Seq[UInt] = vBankOHPerIPipe.map { oh =>
      val bankResult: UInt     = Mux1H(oh, readResultF)
      val byteVec:    Vec[UInt] = VecInit(Seq.tabulate(byteCount)(b => bankResult(8 * b + 7, 8 * b)))
      byteVec(cBytePipe)
    }
    val gatheredWord: UInt = Cat(gatheredBytes.reverse)
    val vReady:  Vec[Bool] = VecInit(Seq.tabulate(readRequests.size)(i => firstReadOH(i) && sramReady))
    val vResult: Vec[UInt] = VecInit(Seq.tabulate(readResults.size)(i =>
      Mux(firstReadIdxPipe(i) && vReadFirePipe, gatheredWord, 0.U)
    ))

    // Single explicit mode-select per IO sink
    // readResults is pipelined by vrfReadLatency, so its mode select must use the
    // useVerticalRead value from the cycle the read fired, not the current cycle.
    // Otherwise the last-of-batch LSU response is stolen by a transient flip when
    // firstReadFromLSU drops in the gap between LSU read batches.
    val useVerticalReadPipe: Bool = Pipe(true.B, useVerticalRead, parameter.vrfReadLatency).bits
    readRequests.zipWithIndex.foreach { case (r, i) =>
      // Narrow-vertical reads use 1 bank; treat them like horizontal for ready
      // (they participate in the same firstOccupied accumulation already).
      // Wide-vertical reads need the wide gather path; when narrow preempts
      // (useVerticalRead=false because anyNarrowReq), they must stall until
      // the narrow clears. Routing them through hReady would be wrong because
      // hResult returns horizontal-layout data, not the vertical fold.
      val isWideVerticalReq = firstReadVertEffective && !r.bits.narrowVertical
      r.ready := Mux(isWideVerticalReq,
                     useVerticalRead && vReady(i),
                     hReady(i))
    }
    readResults.zipWithIndex.foreach { case (rr, i) =>
      // Three-way: narrow takes priority because nPipe(i) is per-port and only set
      // when this very port's request fired as narrow.
      rr := Mux(nPipe(i), nResult(i),
            Mux(useVerticalReadPipe, vResult(i), hResult(i)))
    }

    // Phase 3b invariant: narrow preempts wide-vertical at the useVerticalRead
    // gating (line ~395), so this assertion now holds by construction. Wide-vert
    // reads concurrent with narrow are stalled via r.ready; the SRAM port serves
    // narrow's hReadAddr that cycle.
    assert(!(useVerticalRead && anyNarrowReq),
      s"lane $laneIdx: useVerticalRead must be 0 when any narrow read is valid (Phase 3b invariant)")

    // ---- Chaining record update (same logic as VRF.scala) ----
    val initRecord: ValidIO[VRFWriteReport] = WireDefault(0.U.asTypeOf(Valid(new VRFWriteReport(vrfParam))))
    initRecord.valid := true.B
    initRecord.bits  := ports.instructionWriteReport.bits
    val freeRecord: UInt = VecInit(chainingRecord.map(!_.valid)).asUInt
    val recordFFO:  UInt = ffo(freeRecord)
    val recordEnq:  UInt = Mux(ports.instructionWriteReport.valid, recordFFO, 0.U((parameter.chainingSize + 1).W))

    val writePort:         Seq[ValidIO[VRFWriteRequest]]    = Seq(writePipe)
    val loadUnitReadPorts: Seq[DecoupledIO[VRFReadRequest]] = Seq(readRequests.last)
    Seq(chainingRecord, chainingRecordCopy).foreach { recordVec =>
      recordVec.zipWithIndex.foreach { case (record, i) =>
        val writeOH    = writePort.map(p => UIntToOH((p.bits.vd - record.bits.vd.bits)(2, 0) ## p.bits.offset))
        val loadReadOH = loadUnitReadPorts.map(p => UIntToOH((p.bits.vs - record.bits.vs2)(2, 0) ## p.bits.offset))
        val dataInLsuQueue = ohCheck(ports.loadDataInLSUWriteQueue, record.bits.instIndex, parameter.chainingSize)

        val writeUpdateValidVec: Seq[Bool] =
          writePort.map(p =>
            p.fire && p.bits.instructionIndex === record.bits.instIndex &&
              p.bits.mask(parameter.datapathWidth / 8 - 1) && !record.bits.oooWrite
          )
        val writeUpdate1HVec: Seq[UInt] = writeOH.zip(writeUpdateValidVec).map { case (oh, v) => Mux(v, oh, 0.U) }

        val loadUpdateValidVec =
          loadUnitReadPorts.map(p => p.fire && p.bits.instructionIndex === record.bits.instIndex && record.bits.st)
        val loadUpdate1HVec: Seq[UInt] = loadReadOH.zip(loadUpdateValidVec).map { case (oh, v) => Mux(v, oh, 0.U) }

        val elementUpdateValid: Bool = (writeUpdateValidVec ++ loadUpdateValidVec).reduce(_ || _)
        val elementUpdate1H:    UInt = (writeUpdate1HVec ++ loadUpdate1HVec).reduce(_ | _)
        val dataInLaneCheck = ohCheck(ports.dataInLane, record.bits.instIndex, parameter.chainingSize)
        val laneLastReport  = ohCheck(ports.instructionLastReport, record.bits.instIndex, parameter.chainingSize)
        val topLastReport   = ohCheck(ports.lsuLastReport, record.bits.instIndex, parameter.chainingSize)
        val waitLaneClear   =
          record.bits.state.stFinish && record.bits.state.wWriteQueueClear &&
            record.bits.state.wLaneLastReport && record.bits.state.wTopLastReport
        val stateClear: Bool = waitLaneClear && record.bits.state.wLaneClear ||
          record.bits.elementMask.andR && !record.bits.onlyRead

        when(topLastReport) {
          record.bits.state.stFinish       := true.B
          record.bits.state.wTopLastReport := true.B
        }
        when(laneLastReport) {
          record.bits.state.wLaneLastReport := true.B
        }
        when(record.bits.state.stFinish && !dataInLsuQueue) {
          record.bits.state.wWriteQueueClear := true.B
        }
        when(waitLaneClear && !dataInLaneCheck) {
          record.bits.state.wLaneClear := true.B
        }
        when(stateClear) {
          record.valid := false.B
          when(record.valid) {
            recordRelease(i) := indexToOH(record.bits.instIndex, parameter.chainingSize)
          }
        }
        when(recordEnq(i)) {
          record := initRecord
        }.elsewhen(elementUpdateValid) {
          record.bits.elementMask := record.bits.elementMask | elementUpdate1H
        }
      }
    }

    // ---- Hazard detection (same as VRF.scala) -----
    val hazardVec: Seq[IndexedSeq[(Bool, Bool)]] = chainingRecordCopy.init.zipWithIndex.map {
      case (sourceRecord, sourceIndex) =>
        chainingRecordCopy.drop(sourceIndex + 1).zipWithIndex.map { case (sinkRecord, _) =>
          // todo: full hazard check disabled in original VRF (returns (false.B, false.B))
          (false.B, false.B)
        }
    }
    ports.writeReadyForLsu := !hazardVec.map(_.map(_._1).reduce(_ || _)).reduce(_ || _)
    ports.vrfReadyToStore  := !hazardVec.map(_.map(_._2).reduce(_ || _)).reduce(_ || _)
    ports.vrfSlotRelease   := recordRelease.reduce(_ | _)
    ports.instructionValid := chainingRecord
      .map(r => maskAnd(r.valid, indexToOH(r.bits.instIndex, parameter.chainingSize)).asUInt)
      .reduce(_ | _)

    // ---- Write check (same as VRF.scala) ------
    ports.writeCheck.zip(ports.writeAllow).foreach { case (check, allow) =>
      allow := chainingRecordCopy
        .zip(recordValidVec)
        .map { case (record, rv) =>
          val checkModule = Instantiate(new WriteCheck(vrfParam))
          checkModule.check       := check
          checkModule.record      := record
          checkModule.recordValid := rv
          checkModule.checkResult
        }
        .reduce(_ && _)
    }
  }
}
