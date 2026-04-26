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
  timeMultiplexBatch:  Int
) {
  require(laneNumber == 2, "SharedVRF requires exactly 2 lanes per row due to FPGA BRAM dual port optimisation")

  val numBanks:             Int = 8 //based on diagonal skewing in memory mapping and how many banks needed for parallel access to a register
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

  // VRFParam for reuse with ChainingCheck, WriteCheck, VRFWriteReport 
  def toVRFParam: VRFParam = VRFParam(vLen, laneNumber, datapathWidth, chainingSize, 1, RamType.p0rw)
}

  // Keeping the VRF port connection the same as the old VRF to integrate with the lane ports. The bundle direction is from the VRF, lane needs the Flipped
class LaneVRFPorts(param: VRFParam) extends Bundle {
  val readRequests: Vec[DecoupledIO[VRFReadRequest]] = Vec(
    param.vrfReadPort,
    Flipped(
      Decoupled(new VRFReadRequest(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits))
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
      new VRFWriteRequest(param.regNumBits, param.vrfOffsetBits, param.instructionIndexBits, param.datapathWidth)
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

// Shared VRF consisted of 8 SRAM instances, due to 8 banks for diagonal skewing in memory mapping. Each SRAM has 2 ports, one for each lane.
// Memory addressing looks like this below:
// | rowCounter (time-multiplex) | vs (reg index) | offset (group index) | laneIndex |
//   7 bits for 128 rows          5 bits for 32 reg   1 bit for 2 groups    1bit for 2 lanes
// 
// logicalAddr = vs ## offset ## laneIndex   (7 bits, 0..127)
// flatAddr    = rowCounter ## logicalAddr    (14 bits, 0..16383) <- full address map of each individual entries
// bankAddr    = flatAddr >> 3               (11 bits, 0..2047) <- address used within a bank
// bank        = (rowCounter + logicalAddr)[2:0]

class SharedVRF(val parameter: SharedVRFParam) extends Module {
  val vrfParam: VRFParam = parameter.toVRFParam

  val lanePorts:    Vec[LaneVRFPorts] = IO(Vec(parameter.laneNumber, new LaneVRFPorts(vrfParam)))
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
  val byteCount: Int = parameter.ramWidth / 8
  val vrfSRAM: Seq[SRAMInterface[Vec[UInt]]] = Seq.fill(parameter.numBanks)(
    SRAM.masked(
      size = parameter.bankDepth,
      tpe = Vec(byteCount, UInt(8.W)),
      numReadPorts = 0,
      numWritePorts = 0,
      numReadwritePorts = 2
    )
  )

  // ----- Compute bank index and bank-internal address ----
  // Dynamic rc + dynamic laneIdx so vertical mode can feed per-byte rowCounter_i and byte-lane.
  def bankSelectRC(vs: UInt, offset: UInt, laneIdx: UInt, rc: UInt): UInt = {
    val logicalAddr = vs ## offset ## laneIdx(0)
    UIntToOH((rc +& logicalAddr)(2, 0), parameter.numBanks)
  }

  def bankInternalAddrRC(vs: UInt, offset: UInt, laneIdx: UInt, rc: UInt): UInt = {
    val logicalAddr = vs ## offset ## laneIdx(0)
    val flatAddr    = rc ## logicalAddr
    val shifted     = (flatAddr >> 3).asUInt
    shifted(parameter.bankAddrBits - 1, 0)
  }

  // Horizontal-mode convenience wrappers - use module's global rowCounter and compile-time laneIdx.
  def bankSelect(vs: UInt, offset: UInt, laneIdx: Int): UInt =
    bankSelectRC(vs, offset, laneIdx.U(1.W), rowCounter)

  def bankInternalAddr(vs: UInt, offset: UInt, laneIdx: Int): UInt =
    bankInternalAddrRC(vs, offset, laneIdx.U(1.W), rowCounter)

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

    // Bon2D: lookup whether an in-flight instruction is an LSU op (vle/vse).
    // Used to keep LSU traffic on the horizontal bank path even when verticalMode
    // is asserted, so the vertical permutation only applies to lane-side compute.
    // Without this, vle scatters into vertical banks and vse gathers them back -
    // the two cancel and the slideup result becomes byte-identical to horizontal.
    def isLSUInst(instIdx: UInt): Bool =
      chainingRecord
        .map(r => r.valid && (r.bits.instIndex === instIdx) && (r.bits.ls || r.bits.st))
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
        val bank       = bankSelect(v.bits.vs, v.bits.offset, laneIdx)
        val pipeBank   = Pipe(true.B, bank, parameter.vrfReadLatency).bits
        val bankCorrect = Mux(validCorrect, bank, 0.U(parameter.numBanks.W))

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

        (o | bankCorrect, t) //update the occupied banks with the current request for the next request in the slot
    }

    // ==========================================================================
    // Bon2D: Vertical-mode read-gather and write-scatter overlay.
    //
    // Assumes SEW=8, LMUL=4, vl=128 (128x128 image grid). rowCounter reinterpreted
    // as column index c in vertical mode. For a lane read request (vs,offset,L):
    //   rc_i     = 8*(4*vs(1,0) + 2*offset(0) + L) + i  for i in 0..7
    //   vs_s     = vs(4,2) ## cVsOff                     (absolute vs at column c)
    //   off_s    = cGroup, laneIdx_s = cLane, byte_s = cByte
    // Diagonal skew (bank = (rc+logicalAddr)(2,0)) guarantees the 8 rc_i hit 8
    // distinct banks, so a single lane-cycle fires all 8 bank ports in parallel.
    // ==========================================================================
    val cVsOff: UInt = rowCounter(parameter.rowCounterBits - 1, parameter.rowCounterBits - 2)
    val cGroup: UInt = rowCounter(parameter.rowCounterBits - 3)
    val cLane:  UInt = rowCounter(parameter.rowCounterBits - 4)
    val cByte:  UInt = rowCounter(2, 0)

    // Priority-select first valid read request - only one vertical read per lane per cycle.
    val readValidVec:     Seq[Bool] = readRequests.map(_.valid)
    val anyReadValid:     Bool      = readValidVec.reduce(_ || _)
    val firstReadOH:      UInt      = PriorityEncoderOH(VecInit(readValidVec).asUInt)
    val firstReadReq:     VRFReadRequest = Mux1H(firstReadOH, readRequests.map(_.bits))
    val firstReadIdxPipe: UInt = Pipe(true.B, firstReadOH, parameter.vrfReadLatency).bits

    // Bon2D: only fire the vertical read path for lane-side requests. LSU vse
    // reads must take the horizontal bank path so they undo the same horizontal
    // layout used by vle.
    val firstReadFromLSU: Bool = isLSUInst(firstReadReq.instructionIndex)
    val useVerticalRead:  Bool = verticalMode && !firstReadFromLSU
    val vReadFire: Bool = useVerticalRead && anyReadValid && sramReady
    val vsStore:   UInt = Cat(firstReadReq.vs(4, 2), cVsOff)(4, 0)

    val rcBase: UInt = Cat(
      firstReadReq.vs(1, 0),
      firstReadReq.offset(0),
      laneIdx.U(1.W),
      0.U(3.W)
    )
    val rcI:    Seq[UInt] = Seq.tabulate(parameter.numBanks) { i =>
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
    // In vertical mode, a firing vertical read consumes all 8 banks.
    val verticalOccupy: UInt = Mux(vReadFire, ((BigInt(1) << parameter.numBanks) - 1).U, 0.U(parameter.numBanks.W))
    val effectiveOccupied: UInt = Mux(useVerticalRead, verticalOccupy, firstOccupied)
    write.ready := sramReady && (writeBank & (~effectiveOccupied)).orR

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
    // Bon2D: only fire the vertical write path for lane-side writebacks. LSU vle
    // writes must take the horizontal bank path so vse can undo them later.
    val writeFromLSU:     Bool = isLSUInst(writePipe.bits.instructionIndex)
    val useVerticalWrite: Bool = verticalMode && !writeFromLSU
    val vWriteFire:  Bool = useVerticalWrite && writePipe.valid
    val vsStoreW:    UInt = Cat(writePipe.bits.vd(4, 2), cVsOff)(4, 0)
    val rcBaseW:     UInt = Cat(
      writePipe.bits.vd(1, 0),
      writePipe.bits.offset(0),
      laneIdx.U(1.W),
      0.U(3.W)
    )
    val rcIW: Seq[UInt] = Seq.tabulate(parameter.numBanks) { i =>
      (rcBaseW + i.U)(parameter.rowCounterBits - 1, 0)
    }
    val vWriteBankOHPerI: Seq[UInt] = rcIW.map(rc => bankSelectRC(vsStoreW, cGroup, cLane, rc))
    val vWriteAddrPerI:   Seq[UInt] = rcIW.map(rc => bankInternalAddrRC(vsStoreW, cGroup, cLane, rc))
    val writeByteOfI:     Seq[UInt] = Seq.tabulate(parameter.numBanks) { i =>
      writePipe.bits.data(8 * i + 7, 8 * i)
    }

    // Per-bank vertical write: one i maps to each bank (diagonal skew).
    val vWritePerBankValid: Vec[Bool] = VecInit(Seq.tabulate(parameter.numBanks) { b =>
      VecInit(vWriteBankOHPerI.map(_(b))).asUInt.orR && vWriteFire
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
    vrfSRAM.zipWithIndex.foreach { case (rf, bank) =>
      val hWriteValid: Bool = writePipe.valid && writeBankPipe(bank) && !useVerticalWrite
      val vWriteValid: Bool = vWritePerBankValid(bank)
      val ramWriteValid: Bool = hWriteValid || vWriteValid || laneResetValid

      val hReadAddr: UInt = Mux1H(
        bankReadF.map(_(bank)),
        readRequests.map(r => bankInternalAddr(r.bits.vs, r.bits.offset, laneIdx))
      )
      val hReadValid: Bool = bankReadF.map(_(bank)).reduce(_ || _)

      val readAddr:  UInt = Mux(useVerticalRead, vBankAddr(bank), hReadAddr)
      val readValid: Bool = Mux(useVerticalRead, vBankValid(bank), hReadValid)

      firstReadPipe(bank).bits.address := readAddr
      firstReadPipe(bank).valid        := readValid

      // Vertical scatter: broadcast the 1-byte update to byte lane cByte of the 8-byte slot.
      val vWriteByteVec: Vec[UInt] = VecInit(Seq.fill(byteCount)(vWritePerBankByte(bank)))
      val vWriteMask:    Vec[Bool] = VecInit(Seq.tabulate(byteCount)(i => cByte === i.U))

      val portWriteData: Vec[UInt] = Mux(useVerticalWrite && !laneResetValid, vWriteByteVec, writeByteVec)
      val portWriteMask: Vec[Bool] = Mux(useVerticalWrite && !laneResetValid, vWriteMask, writeByteMaskAll)
      val portWriteAddr: UInt      = Mux(useVerticalWrite && !laneResetValid, vWritePerBankAddr(bank), writeAddress)

      rf.readwritePorts(laneIdx).address   := Mux(ramWriteValid, portWriteAddr, firstReadPipe(bank).bits.address)
      rf.readwritePorts(laneIdx).enable    := ramWriteValid || firstReadPipe(bank).valid
      rf.readwritePorts(laneIdx).isWrite   := ramWriteValid
      rf.readwritePorts(laneIdx).writeData := portWriteData
      rf.readwritePorts(laneIdx).mask.get  := portWriteMask
      readResultF(bank) := rf.readwritePorts(laneIdx).readData.asUInt
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
      r.ready := Mux(useVerticalRead, vReady(i), hReady(i))
    }
    readResults.zipWithIndex.foreach { case (rr, i) =>
      rr := Mux(useVerticalReadPipe, vResult(i), hResult(i))
    }

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
