package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, Instance, Instantiate}
import chisel3.experimental.SerializableModule
import chisel3.properties.{Path, Property}
import chisel3.util._
import org.chipsalliance.stdlib.GeneralOM
import org.chipsalliance.t1.rtl.vfu.{Abs32, VectorAdder64, VectorMultiplier32UnsignedDSP}

class LaneMulDSPOM(parameter: LaneMulParam) extends GeneralOM[LaneMulParam, LaneMulDSP](parameter) {
  override def hasRetime: Boolean = true
}

@instantiable
class LaneMulDSP(val parameter: LaneMulParam) extends VFUModule with SerializableModule[LaneMulParam] {
  val omInstance: Instance[LaneMulDSPOM] = Instantiate(new LaneMulDSPOM(parameter))
  omInstance.retimeIn.foreach(_ := Property(Path(clock)))

  val response: LaneMulResponse = Wire(new LaneMulResponse(parameter))
  val request:  LaneMulReq      = connectIO(response).asTypeOf(parameter.inputBundle)

  val sew1H: UInt =
    RegEnable(UIntToOH(requestIO.bits.asTypeOf(request).vSew)(2, 0), 0.U(3.W), requestIO.fire)
  val vxrm1H:   UInt = UIntToOH(request.vxrm)
  val opcode1H: UInt = UIntToOH(request.opcode(1, 0))
  val ma:       Bool = opcode1H(1) || opcode1H(2)
  val asAddend: Bool = request.opcode(2)
  val negative: Bool = request.opcode(3)

  val responseVec: Seq[(UInt, Bool)] = Seq.tabulate(parameter.laneScale) { index =>
    val subRequest = Wire(new LaneMulReq(parameter.eLen))
    subRequest.elements.foreach { case (k, v) => v := request.elements(k) }
    subRequest.src.zip(request.src).foreach { case (sink, source) =>
      sink := cutUIntBySize(source, parameter.laneScale)(index)
    }

    val mul0:            UInt      = subRequest.src.head
    val mulAbs0:         UInt      = Abs32(mul0, sew1H)
    val mul0InputSelect: UInt      = Mux(subRequest.sign0, mulAbs0, mul0)
    val mul0Sign:        Seq[Bool] = cutUInt(mul0, 8).map(_(7) && subRequest.sign0)

    val mul1:            UInt      = Mux(asAddend || !ma, subRequest.src(1), subRequest.src.last)
    val mulAbs1:         UInt      = Abs32(mul1, sew1H)
    val mul1InputSelect: UInt      = Mux(subRequest.sign || (ma && !asAddend), mulAbs1, mul1)
    val mul1Sign:        Seq[Bool] = cutUInt(mul1, 8).map(_(7) && subRequest.sign)

    val addend: UInt = Mux1H(
      Seq(
        (ma && asAddend)  -> subRequest.src.last,
        (ma && !asAddend) -> subRequest.src(1)(parameter.eLen - 1, 0)
      )
    )

    val fusionMultiplier: Instance[VectorMultiplier32UnsignedDSP] = Instantiate(new VectorMultiplier32UnsignedDSP)
    fusionMultiplier.a   := mul0InputSelect
    fusionMultiplier.b   := mul1InputSelect
    fusionMultiplier.sew := sew1H

    val multiplierSum:   UInt = fusionMultiplier.multiplierSum
    val multiplierCarry: UInt = fusionMultiplier.multiplierCarry

    val sumVec   = cutUInt(multiplierSum, 16)
    val carryVec = cutUInt(multiplierCarry, 16)

    val MSBBlockVec: UInt = true.B ## sew1H(0) ## !sew1H(2) ## sew1H(0)
    val LSBBlockVec: UInt = sew1H(0) ## !sew1H(2) ## sew1H(0) ## true.B
    val negativeTag: Vec[Bool] = VecInit(mul0Sign.zip(mul1Sign).map { case (s0, s1) => s0 ^ s1 ^ negative })
    val negativeBlock: UInt = negativeTag(3) ##
      Mux(sew1H(0), negativeTag(2), negativeTag(3)) ##
      Mux(sew1H(2), negativeTag(3), negativeTag(1)) ##
      Mux1H(sew1H, Seq(negativeTag(0), negativeTag(1), negativeTag(3)))

    val addendDataVec: Vec[UInt] = cutUInt(addend, 8)
    val zeroByte:      UInt      = Fill(8, false.B)
    val addendExtend: UInt = zeroByte ##
      Mux(sew1H(0), addendDataVec(3), zeroByte) ##
      Mux(sew1H(1), addendDataVec(3), zeroByte) ##
      Mux(!sew1H(2), addendDataVec(2), zeroByte) ##
      Mux(sew1H(2), addendDataVec(3), zeroByte) ##
      Mux1H(sew1H, Seq(addendDataVec(1), zeroByte, addendDataVec(2))) ##
      Mux(sew1H(0), zeroByte, addendDataVec(1)) ## addendDataVec(0)
    val addendExtendVec = cutUInt(addendExtend, 16)

    val blockCsaCarry: Vec[Bool] = Wire(Vec(4, Bool()))
    val add2Carry:     Vec[Bool] = Wire(Vec(4, Bool()))
    val adderInput: Seq[(UInt, UInt)] = sumVec.zipWithIndex.map { case (sum, blockIndex) =>
      val carry:               UInt = carryVec(blockIndex)
      val isLSB:               Bool = LSBBlockVec(blockIndex)
      val negativeMul:         Bool = negativeBlock(blockIndex)
      val needAdd2:            Bool = negativeMul && isLSB
      val previousAdd2Carry:   Bool = if (blockIndex == 0) false.B else add2Carry(blockIndex - 1)
      val pickPreviousAdd2Carry     = !isLSB && previousAdd2Carry
      val addCorrection             = addendExtendVec(blockIndex) +& (needAdd2 ## pickPreviousAdd2Carry)
      val csaAddInput:        UInt = addCorrection(15, 0)
      add2Carry(blockIndex) := addCorrection(16)
      val sumSelect:   UInt = Mux(negativeMul, (~sum).asUInt, sum)
      val carrySelect: UInt = Mux(negativeMul, (~carry).asUInt, carry)
      val (csaS, csaC)      = csa32(sumSelect, carrySelect, csaAddInput)
      blockCsaCarry(blockIndex) := csaC(15)
      val previousCarry:     Bool = if (blockIndex == 0) false.B else blockCsaCarry(blockIndex - 1)
      val pickPreviousCarry: Bool = !isLSB && previousCarry
      (csaS, csaC(14, 0) ## pickPreviousCarry)
    }

    val adder64: Instance[VectorAdder64] = Instantiate(new VectorAdder64)
    adder64.a   := VecInit(adderInput.map(_._1)).asUInt
    adder64.b   := VecInit(adderInput.map(_._2)).asUInt
    adder64.cin := 0.U
    adder64.sew := sew1H ## false.B
    val adderResultVec: Vec[UInt] = cutUInt(adder64.z, 16)
    val notZeroVec:     UInt      = Wire(UInt(4.W))

    val expectedSignVec: Vec[Bool] = Wire(Vec(4, Bool()))
    val expectedSignForBlockVec: UInt = expectedSignVec(3) ##
      Mux(sew1H(0), expectedSignVec(2), expectedSignVec(3)) ##
      Mux(sew1H(2), expectedSignVec(3), expectedSignVec(1)) ##
      Mux1H(sew1H, Seq(expectedSignVec(0), expectedSignVec(1), expectedSignVec(3)))
    val resultSignVec: Vec[Bool] = Wire(Vec(4, Bool()))
    val attributeVec: Seq[(Bool, UInt)] = adderResultVec.zipWithIndex.map { case (data, blockIndex) =>
      val sourceSign0            = cutUInt(mul0, 8)(blockIndex)(7)
      val sourceSign1            = cutUInt(mul1, 8)(blockIndex)(7)
      val isMSB:            Bool = MSBBlockVec(blockIndex)
      val notZero:          Bool = notZeroVec(blockIndex)
      val operation0Sign:   Bool = (sourceSign0 && subRequest.sign) ^ negative
      val operation1Sign:   Bool = (sourceSign1 && subRequest.sign) ^ negative
      val resultSign:       Bool = resultSignVec(blockIndex)
      val expectedSigForMul      = operation0Sign ^ operation1Sign
      expectedSignVec(blockIndex) := expectedSigForMul
      val overflow             = (expectedSigForMul ^ resultSign) && notZero
      val expectedSignForBlock = expectedSignForBlockVec(blockIndex)
      val overflowSelection    = !(expectedSignForBlock ^ isMSB) ## Fill(7, !expectedSignForBlock)
      (overflow, overflowSelection)
    }

    val roundResultForSew8: UInt = VecInit(adderResultVec.map { data =>
      val vd1:      Bool = data(6)
      val vd:       Bool = data(7)
      val vd2OR:    Bool = data(5, 0).orR
      val roundBits0     = vd1
      val roundBits1     = vd1 && (vd2OR || vd)
      val roundBits2     = !vd && (vd2OR || vd1)
      val roundBits      = Mux1H(vxrm1H(3) ## vxrm1H(1, 0), Seq(roundBits0, roundBits1, roundBits2))
      val shiftResult    = (data >> 7).asUInt
      (shiftResult + roundBits)(7, 0)
    }).asUInt

    val roundResultForSew16: UInt = VecInit(cutUInt(adder64.z, 32).map { data =>
      val vd1:      Bool = data(14)
      val vd:       Bool = data(15)
      val vd2OR:    Bool = data(13, 0).orR
      val roundBits0     = vd1
      val roundBits1     = vd1 && (vd2OR || vd)
      val roundBits2     = !vd && (vd2OR || vd1)
      val roundBits      = Mux1H(vxrm1H(3) ## vxrm1H(1, 0), Seq(roundBits0, roundBits1, roundBits2))
      val shiftResult    = (data >> 15).asUInt
      (shiftResult + roundBits)(15, 0)
    }).asUInt

    val roundResultForSew32: UInt = {
      val vd1:      Bool = adder64.z(30)
      val vd:       Bool = adder64.z(31)
      val vd2OR:    Bool = adder64.z(29, 0).orR
      val roundBits0     = vd1
      val roundBits1     = vd1 && (vd2OR || vd)
      val roundBits2     = !vd && (vd2OR || vd1)
      val roundBits      = Mux1H(vxrm1H(3) ## vxrm1H(1, 0), Seq(roundBits0, roundBits1, roundBits2))
      val shiftResult    = (adder64.z >> 31).asUInt
      (shiftResult + roundBits)(31, 0)
    }

    val roundingResult:    UInt      = Mux1H(sew1H, Seq(roundResultForSew8, roundResultForSew16, roundResultForSew32))
    val roundingResultVec: Vec[UInt] = cutUInt(roundingResult, 8)
    resultSignVec.zip(roundingResultVec).foreach { case (s, d) => s := d(7) }
    val roundingResultOrR = VecInit(roundingResultVec.map(_.orR)).asUInt
    val orSew16: UInt =
      VecInit(Seq(roundingResultOrR(0) || roundingResultOrR(1), roundingResultOrR(2) || roundingResultOrR(3))).asUInt
    val orSew32: Bool = orSew16.orR
    notZeroVec := Mux1H(
      sew1H,
      Seq(
        roundingResultOrR,
        FillInterleaved(2, orSew16),
        Fill(4, orSew32)
      )
    )

    val overflowTag = attributeVec.map(_._1)
    val overflowSelect: Vec[Bool] = Mux1H(
      sew1H,
      Seq(
        VecInit(overflowTag),
        VecInit(overflowTag(1), overflowTag(1), overflowTag(3), overflowTag(3)),
        VecInit(overflowTag(3), overflowTag(3), overflowTag(3), overflowTag(3))
      )
    )
    val addResultCutByByte = cutUInt(adder64.z, 8)
    val mulMSB: UInt = addResultCutByByte(7) ##
      Mux(sew1H(0), addResultCutByByte(5), addResultCutByByte(6)) ##
      Mux(sew1H(2), addResultCutByByte(5), addResultCutByByte(3)) ##
      Mux1H(sew1H, Seq(addResultCutByByte(1), addResultCutByByte(2), addResultCutByByte(4)))
    val msbVec = cutUInt(mulMSB, 8)

    val mulLSB: UInt = Mux1H(sew1H, Seq(addResultCutByByte(6), addResultCutByByte(5), addResultCutByByte(3))) ##
      Mux(sew1H(2), addResultCutByByte(2), addResultCutByByte(4)) ##
      Mux(sew1H(0), addResultCutByByte(2), addResultCutByByte(1)) ##
      addResultCutByByte(0)
    val lsbVec = cutUInt(mulLSB, 8)

    val overflowData: Seq[UInt] = attributeVec.map(_._2)

    (
      VecInit(Seq.tabulate(4) { blockIndex =>
        val overflow = overflowSelect(blockIndex)
        Mux1H(
          Seq(
            (opcode1H(0) && !subRequest.saturate) || ma,
            opcode1H(3),
            subRequest.saturate && !overflow,
            subRequest.saturate && overflow
          ),
          Seq(lsbVec(blockIndex), msbVec(blockIndex), roundingResultVec(blockIndex), overflowData(blockIndex))
        )
      }).asUInt,
      overflowSelect.asUInt.orR && subRequest.saturate
    )
  }

  response.data  := VecInit(responseVec.map(_._1)).asUInt
  response.vxsat := VecInit(responseVec.map(_._2)).asUInt.orR
}
