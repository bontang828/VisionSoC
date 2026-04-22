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

  val lanePorts:  Vec[LaneVRFPorts] = IO(Vec(parameter.laneNumber, new LaneVRFPorts(vrfParam)))
  val rowCounter: UInt              = IO(Input(UInt(parameter.rowCounterBits.W)))

  // ------ SRAM banks: 8 banks, each dual-ported (2 RW ports) -----
  val sramReady:      Bool = RegInit(false.B)
  val sramResetCount: UInt = RegInit(0.U(parameter.bankAddrBits.W))
  val resetValid:     Bool = !sramReady
  when(resetValid) {
    sramResetCount := sramResetCount + 1.U
    when(sramResetCount.andR) { sramReady := true.B }
  }

  val vrfSRAM: Seq[SRAMInterface[UInt]] = Seq.fill(parameter.numBanks)(
    SRAM(
      size = parameter.bankDepth,
      tpe = UInt(parameter.memoryWidth.W),
      numReadPorts = 0,
      numWritePorts = 0,
      numReadwritePorts = 2
    )
  )

  // ----- Compute bank index and bank-internal address ----
  def bankSelect(vs: UInt, offset: UInt, laneIdx: Int): UInt = {
    val logicalAddr = vs ## offset ## laneIdx.U(1.W)
    UIntToOH((rowCounter +& logicalAddr)(2, 0), parameter.numBanks)
  }

  def bankInternalAddr(vs: UInt, offset: UInt, laneIdx: Int): UInt = {
    val logicalAddr = vs ## offset ## laneIdx.U(1.W)
    val flatAddr    = rowCounter ## logicalAddr
    (flatAddr >> 3).asUInt(parameter.bankAddrBits - 1, 0)
  }

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
        v.ready := portReady && sramReady

        bankReadF(i) := bankCorrect & (~o)

        val pipeFire = Pipe(true.B, v.fire, parameter.vrfReadLatency).bits
        readResults(i) := Mux(pipeFire, Mux1H(pipeBank, readResultF), 0.U)

        (o | bankCorrect, t) //update the occupied banks with the current request for the next request in the slot
    }

    // ----- Write ready: write can proceed if its bank is not occupied by reads ---
    write.ready := sramReady && (writeBank & (~firstOccupied)).orR // Checks that the target write bank are not occupied with OH AND

    val writeData: UInt = Mux(resetValid, 0.U(parameter.datapathWidth.W), writePipe.bits.data)
    val writeAddress: UInt = Mux(resetValid, sramResetCount, writeAddrPipe)

    // ----- Connect to SRAM banks (lane's port) ----
    // Only lane 0 drives reset (avoids same-address dual-port write during reset)
    val laneResetValid: Bool = if (laneIdx == 0) resetValid else false.B
    vrfSRAM.zipWithIndex.foreach { case (rf, bank) =>
      val writeValid:    Bool = writePipe.valid && writeBankPipe(bank)
      val ramWriteValid: Bool = writeValid || laneResetValid

      firstReadPipe(bank).bits.address :=
        Mux1H(
          bankReadF.map(_(bank)), // Extract the bit at index 'bank' to create a OH signal
          readRequests.map(r => bankInternalAddr(r.bits.vs, r.bits.offset, laneIdx)) // generate a vec that have a list of address for OH selection 
        )
      firstReadPipe(bank).valid := bankReadF.map(_(bank)).reduce(_ || _)

      rf.readwritePorts(laneIdx).address   := Mux(ramWriteValid, writeAddress, firstReadPipe(bank).bits.address)
      rf.readwritePorts(laneIdx).enable    := ramWriteValid || firstReadPipe(bank).valid
      rf.readwritePorts(laneIdx).isWrite   := ramWriteValid
      rf.readwritePorts(laneIdx).writeData := writeData
      readResultF(bank) := rf.readwritePorts(laneIdx).readData(parameter.ramWidth - 1, 0)
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
