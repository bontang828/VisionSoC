package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, Instance, Instantiate}
import chisel3.experimental.{SerializableModule, SerializableModuleParameter}
import chisel3.util._
import org.chipsalliance.stdlib.GeneralOM
import org.chipsalliance.t1.rtl.decoder.{BoolField, Decoder}

object LaneDivStubParam {
  implicit def rw: upickle.default.ReadWriter[LaneDivStubParam] = upickle.default.macroRW
}

case class LaneDivStubParam(datapathWidth: Int, latency: Int)
    extends VFUParameter
    with SerializableModuleParameter {
  val decodeField:  BoolField       = Decoder.divider
  val inputBundle:  LaneDivRequest  = new LaneDivRequest(datapathWidth)
  val outputBundle: LaneDivResponse = new LaneDivResponse(datapathWidth)
  override val NeedSplit:   Boolean = true
  override val singleCycle: Boolean = false
}

class LaneDivStubOM(parameter: LaneDivStubParam) extends GeneralOM[LaneDivStubParam, LaneDivStub](parameter)

@instantiable
class LaneDivStub(val parameter: LaneDivStubParam) extends VFUModule with SerializableModule[LaneDivStubParam] {
  val omInstance: Instance[LaneDivStubOM] = Instantiate(new LaneDivStubOM(parameter))
  val response:      LaneDivResponse      = Wire(new LaneDivResponse(parameter.datapathWidth))
  val responseValid: Bool                 = Wire(Bool())
  val request:       LaneDivRequest       = connectIO(response, responseValid).asTypeOf(parameter.inputBundle)

  val responseValidReg: Bool = RegNext(vfuRequestFire, false.B)
  val executeIndexReg:  UInt = RegEnable(request.executeIndex, 0.U, vfuRequestFire)

  vfuRequestReady.foreach(_ := true.B)
  responseValid          := responseValidReg
  response.executeIndex  := executeIndexReg
  response.busy          := false.B
  response.data          := 0.U
}
