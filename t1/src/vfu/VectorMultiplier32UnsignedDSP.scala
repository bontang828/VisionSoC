package org.chipsalliance.t1.rtl.vfu

import chisel3._
import chisel3.experimental.hierarchy.{instantiable, public}
import chisel3.util._

@instantiable
class VectorMultiplier32UnsignedDSP extends Module {
  @public
  val a = IO(Input(UInt(32.W)))
  @public
  val b = IO(Input(UInt(32.W)))
  @public
  val z = IO(Output(UInt(64.W)))
  @public
  val sew = IO(Input(UInt(3.W)))
  @public
  val multiplierSum: UInt = IO(Output(UInt(64.W)))
  @public
  val multiplierCarry: UInt = IO(Output(UInt(64.W)))

  val p8: Vec[UInt] = VecInit(Seq.tabulate(4) { index =>
    a(8 * index + 7, 8 * index) * b(8 * index + 7, 8 * index)
  })
  val out8: UInt = Cat(p8(3), p8(2), p8(1), p8(0))

  val p16: Vec[UInt] = VecInit(Seq.tabulate(2) { index =>
    a(16 * index + 15, 16 * index) * b(16 * index + 15, 16 * index)
  })
  val out16: UInt = Cat(p16(1), p16(0))

  val out32: UInt = a * b
  val result: UInt = Mux1H(sew, Seq(out8, out16, out32))

  multiplierSum   := result
  multiplierCarry := 0.U
  z               := result
}
