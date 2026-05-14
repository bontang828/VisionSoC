package org.chipsalliance.dwbb.stdlib.queue

import chisel3._

import chisel3.util._
import chisel3.util.addAttribute

class QueueIO[T <: Data](private val gen: T, entries: Int) extends Bundle {
  val enq = Flipped(EnqIO(gen))
  val deq = Flipped(DeqIO(gen))

  val empty       = Output(Bool())
  val full        = Output(Bool())
  val almostEmpty = if (entries >= 2) Some(Output(Bool())) else None
  val almostFull  = if (entries >= 2) Some(Output(Bool())) else None
}

object Queue {
  def apply[T <: Data](
    enq:      ReadyValidIO[T],
    entries:  Int,
    pipe:     Boolean = false,
    flow:     Boolean = false,
    resetMem: Boolean = false
  ): DecoupledIO[T] = {
    val io = this.io(chiselTypeOf(enq.bits), entries, pipe, flow, resetMem)
    io.enq <> enq

    io.deq
  }

  def io[T <: Data](
    gen:              T,
    entries:          Int = 2,
    pipe:             Boolean = false,
    flow:             Boolean = false,
    resetMem:         Boolean = false,
    almostEmptyLevel: Int = 1,
    almostFullLevel:  Int = 1
  ): QueueIO[T] = {
    require(
      Range.inclusive(1, 2048).contains(gen.getWidth),
      "Data width must be between 1 and 2048"
    )
    require(
      Range.inclusive(1, 1024).contains(entries),
      "Entries must be between 1 and 1024"
    )

    val io = Wire(new QueueIO(gen, entries))

    if (entries == 1) {
      val data  = if (resetMem) RegInit(0.U.asTypeOf(gen)) else Reg(gen)
      val empty = RegInit(true.B)
      val full  = !empty

      val push = io.enq.fire && (if (flow) !(empty && io.deq.ready) else true.B)
      io.enq.ready := empty || (if (pipe) io.deq.ready else false.B)
      data         := Mux(push, io.enq.bits, data)

      val pop = io.deq.ready && full
      io.deq.valid := full || (if (flow) io.enq.valid else false.B)
      io.deq.bits  := (if (flow) Mux(empty, io.enq.bits, data) else data)

      empty := Mux(push =/= pop, pop, empty)

      io.empty := empty
      io.full  := full
    } else {
      require(
        Range.inclusive(1, entries - 1).contains(almostEmptyLevel),
        "almost empty level must be between 1 and entries-1"
      )
      require(
        Range.inclusive(1, entries - 1).contains(almostFullLevel),
        "almost full level must be between 1 and entries-1"
      )
      //calculate the number of bits needed for the counter
      val ptrWidth = log2Ceil(entries) + 1

      //create the memory to store queue data
      val ram = if (resetMem) {
        RegInit(VecInit(Seq.fill(entries)(0.U.asTypeOf(gen))))
      } else {
        Reg(Vec(entries, gen))
      }
      // Hint Vivado to map the queue storage into LUTRAM (distributed RAM)
      // instead of dedicated flip-flops. The Reg(Vec(...)) shape with
      // head/tail-pointer access is exactly what RAM32M / RAM64M expect.
      // For tiny queues (≤4 deep * narrow data) Vivado may legitimately
      // ignore the hint; that is fine - no semantic change either way.
      // Emitted as SystemVerilog `(* ram_style = "distributed" *)` via
      // chisel3.util.addAttribute -> firrtl AttributeAnnotation -> firtool.
      addAttribute(ram, "ram_style = \"distributed\"")

      //write and read pointers
      val enqPtr = RegInit(0.U(ptrWidth.W))
      val deqPtr = RegInit(0.U(ptrWidth.W))

      //calculate queue status
      val ptrMatch = enqPtr(ptrWidth - 2, 0) === deqPtr(ptrWidth - 2, 0)
      val empty    = ptrMatch && (enqPtr(ptrWidth - 1) === deqPtr(ptrWidth - 1))
      val full     = ptrMatch && (enqPtr(ptrWidth - 1) =/= deqPtr(ptrWidth - 1))

      //calculate the number of elements in the queue
      val count = Mux(
        enqPtr >= deqPtr,
        enqPtr - deqPtr,
        entries.U + enqPtr - deqPtr
      )

      //almost empty/full signals
      val almostEmpty = count <= almostEmptyLevel.U
      val almostFull  = count >= (entries.U - almostFullLevel.U)

      //enqueue logic
      val doEnq = WireDefault(io.enq.fire && (if (flow) !(empty && io.deq.ready) else true.B))
      when(doEnq) {
        ram(enqPtr(ptrWidth - 2, 0)) := io.enq.bits
        enqPtr := Mux(enqPtr(ptrWidth - 2, 0) === (entries - 1).U, ~enqPtr(ptrWidth - 1) ## 0.U((ptrWidth - 1).W), enqPtr + 1.U)
      }

      //dequeue logic
      val doDeq = WireDefault(io.deq.ready && !empty)
      when(doDeq) {
        deqPtr := Mux(deqPtr(ptrWidth - 2, 0) === (entries - 1).U, ~deqPtr(ptrWidth - 1) ## 0.U((ptrWidth - 1).W), deqPtr + 1.U)
      }

      //connect outputs
      io.enq.ready := !full || (if (pipe) io.deq.ready else false.B)
      io.deq.valid := !empty || (if (flow) io.enq.valid else false.B)
      io.deq.bits  := (if (flow) Mux(empty, io.enq.bits, ram(deqPtr(ptrWidth - 2, 0))) else ram(deqPtr(ptrWidth - 2, 0)))
      io.empty           := empty
      io.full            := full
      io.almostEmpty.get := almostEmpty
      io.almostFull.get  := almostFull

    }

    io
  }
}