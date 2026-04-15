// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2022 Jiuyang Liu <liu@jiuyang.me>

package org.chipsalliance.t1.rtl.vrf

import chisel3._
import chisel3.experimental.hierarchy.instantiable
import chisel3.experimental.{SerializableModule, SerializableModuleParameter}
import chisel3.util._
import org.chipsalliance.t1.rtl.{LSUWriteCheck, VRFReadRequest, VRFWriteReport, VRFWriteRequest}

sealed trait RamType
object RamType  {
  implicit val rwP: upickle.default.ReadWriter[RamType] = upickle.default.ReadWriter.merge(
    upickle.default.macroRW[p0rw.type],
    upickle.default.macroRW[p0rp1w.type],
    upickle.default.macroRW[p0rwp1rw.type]
  )

  case object p0rw extends RamType

  case object p0rp1w extends RamType

  case object p0rwp1rw extends RamType
}
object VRFParam {
  implicit val rwP: upickle.default.ReadWriter[VRFParam] = upickle.default.macroRW
}

/** Parameter for [[Lane]].
  * @param vLen
  *   VLEN
  * @param laneNumber
  *   how many lanes in the vector processor
  * @param datapathWidth
  *   ELEN
  * @param chainingSize
  *   how many instructions can be chained
  * @param portFactor
  *   How many ELEN(32 in current design) can be accessed for one memory port accessing.
  *
  * @note:
  *   if increasing portFactor:
  *   - we can have more memory ports.
  *   - a big VRF memory is split into small memories, the shell of memory contributes more area...
  *
  * TODO: add ECC cc @sharzyL 8bits -> 5bits 16bits -> 6bits 32bits -> 7bits
  */
case class VRFParam(
  vLen:          Int,
  laneNumber:    Int,
  datapathWidth: Int,
  chainingSize:  Int,
  portFactor:    Int,
  ramType:       RamType)
    extends SerializableModuleParameter {

  val chaining1HBits: Int = 2 << log2Ceil(chainingSize)

  /** See documentation for VRF. chainingSize * 3 + 1 + 1: 3 read /slot + maskedWrite + lsu read 0: maskedWrite last:
    * lsu read Each element represents a read port of vrf, The number inside is which of the above requests will share
    * this port.
    */
  val connectTree: Seq[Seq[Int]] = Seq.tabulate(chainingSize * 3 + 1 + 1) { i => Seq(i) }
  val vrfReadPort: Int           = connectTree.size

  /** VRF index number is 32, defined in spec. */
  val regNum: Int = 32

  /** The hardware width of [[regNum]] */
  val regNumBits:           Int = log2Ceil(regNum)
  // One more bit for sorting
  /** see [[VParameter.instructionIndexBits]] */
  val instructionIndexBits: Int = log2Ceil(chainingSize) + 1

  /** the width of VRF banked together. */
  val rowWidth: Int = datapathWidth * portFactor

  /** the depth of memory */
  val rfDepth: Int = vLen * regNum / rowWidth / laneNumber

  /** see [[LaneParameter.singleGroupSize]] */
  val singleGroupSize: Int = vLen / datapathWidth / laneNumber

  /** see [[LaneParameter.vrfOffsetBits]] */
  val vrfOffsetBits: Int = log2Ceil(singleGroupSize)

  // 用data path width 的 ram 应该是不会变了
  val ramWidth:  Int = datapathWidth
  val rfBankNum: Int = rowWidth / ramWidth

  /** used to instantiate VRF. */
  val VLMaxWidth: Int = log2Ceil(vLen) + 1

  /** bits of mask group counter */
  val maskGroupCounterBits: Int = log2Ceil(vLen / datapathWidth)

  val vlWidth: Int = log2Ceil(vLen)

  val elementSize: Int = vLen * 8 / datapathWidth / laneNumber

  // todo: 4 bit for ecc
  val memoryWidth: Int = ramWidth + 4

  // 1: pipe access request + 1: SyncReadMem
  val vrfReadLatency = 2
}

class VRFProbe(parameter: VRFParam) extends Bundle {
  val valid:              Bool = Bool()
  val requestVd:          UInt = UInt(parameter.regNumBits.W)
  val requestOffset:      UInt = UInt(parameter.vrfOffsetBits.W)
  val requestMask:        UInt = UInt((parameter.datapathWidth / 8).W)
  val requestData:        UInt = UInt(parameter.datapathWidth.W)
  val requestInstruction: UInt = UInt(parameter.instructionIndexBits.W)
}

class VRFInterface(parameter: VRFParam) extends Bundle {
  val clock = Input(Clock())
  val reset = Input(Bool())

  /** VRF read requests ready will couple from valid from [[readRequests]], ready is asserted when higher priority valid
    * is less than 2.
    */
  val readRequests: Vec[DecoupledIO[VRFReadRequest]] = Vec(
    parameter.vrfReadPort,
    Flipped(
      Decoupled(new VRFReadRequest(parameter.regNumBits, parameter.vrfOffsetBits, parameter.instructionIndexBits))
    )
  )

  // @todo @Clo91eaf should pull read&write check
  //                 performance checker, in the difftest, it can observe all previous events, and know should it be stalled?
  //                 then, we can know the accuracy read&write check hardware signal.
  // 3 * slot + 2 cross read
  val readCheck: Vec[VRFReadRequest] = Input(
    Vec(
      parameter.chainingSize * 3 + 2,
      new VRFReadRequest(parameter.regNumBits, parameter.vrfOffsetBits, parameter.instructionIndexBits)
    )
  )

  /** VRF read results. */
  val readCheckResult: Vec[Bool] = Output(Vec(parameter.chainingSize * 3 + 2, Bool()))
  val readResults:     Vec[UInt] = Output(Vec(parameter.vrfReadPort, UInt(parameter.datapathWidth.W)))

  /** VRF write requests ready will couple from valid from [[write]], ready is asserted when higher priority valid is
    * less than 2. TODO: rename to `vrfWriteRequests`
    */
  val write: DecoupledIO[VRFWriteRequest] = Flipped(
    Decoupled(
      new VRFWriteRequest(
        parameter.regNumBits,
        parameter.vrfOffsetBits,
        parameter.instructionIndexBits,
        parameter.datapathWidth
      )
    )
  )

  val writeCheck: Vec[LSUWriteCheck] = Input(
    Vec(
      parameter.chainingSize + 1,
      new LSUWriteCheck(
        parameter.regNumBits,
        parameter.vrfOffsetBits,
        parameter.instructionIndexBits
      )
    )
  )
  val writeAllow: Vec[Bool]          = Output(Vec(parameter.chainingSize + 1, Bool()))

  /** when instruction is fired, record it in the VRF for chaining. */
  val instructionWriteReport: ValidIO[VRFWriteReport] = Flipped(Valid(new VRFWriteReport(parameter)))

  /** similar to [[flush]]. */
  val instructionLastReport: UInt = Input(UInt(parameter.chaining1HBits.W))

  val lsuLastReport: UInt = Input(UInt(parameter.chaining1HBits.W))

  val vrfSlotRelease: UInt = Output(UInt(parameter.chaining1HBits.W))

  val instructionValid: UInt = Output(UInt(parameter.chaining1HBits.W))

  val dataInLane: UInt = Input(UInt(parameter.chaining1HBits.W))

  val writeReadyForLsu: Bool = Output(Bool())
  val vrfReadyToStore:  Bool = Output(Bool())

  /** we can only chain LSU instructions, after [[LSU.writeQueueVec]] is cleared. */
  val loadDataInLSUWriteQueue: UInt = Input(UInt(parameter.chaining1HBits.W))

  val vrfProbe = Output(new VRFProbe(parameter))
}

/** Vector Register File. contains logic:
  *   - RAM as VRF.
  *   - chaining detection
  *   - bank split
  *   - out of order chaining hazard detection: TODO: move to Top.
  *
  * TODO: probe each ports to benchmark the bandwidth.
  */
@instantiable
class VRF(val parameter: VRFParam)
    extends FixedIOExtModule(new VRFInterface(parameter))
    with SerializableModule[VRFParam] {
  val paramDir = java.nio.file.Paths.get("zaozi-params")
  java.nio.file.Files.createDirectories(paramDir)
  java.nio.file.Files.write(
    paramDir.resolve("VRF.json"),
    upickle.default.write(parameter).getBytes(java.nio.charset.StandardCharsets.UTF_8)
  )
}
