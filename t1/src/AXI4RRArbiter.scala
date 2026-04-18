package org.chipsalliance.t1.rtl

import chisel3._
import chisel3.util._
import org.chipsalliance.amba.axi4.bundle.{AXI4BundleParameter, AXI4RWIrrevocable}

/**Round-robin N-to-1 AXI4 arbiter.
Assumptions:
 - Assumes single-beat transactions (len=0) on all channels, which holds for the T1 LSU (LoadUnit, StoreUnit, SimpleAccessUnit all set .len := 0.U).
 - Write transactions are serialised: no new AW is granted while a W beat is still outstanding, so W-channel ownership is always unambiguous.
 - Read transactions are arbitrated independently from writes.
*/
class AXI4RRArbiter(n: Int, param: AXI4BundleParameter) extends Module {
  require(n >= 1, "AXI4RRArbiter requires n >= 1")

  //Masters keep the original ID width; slave gains a log2(n)-bit prefix.
  val masters = IO(Vec(n, Flipped(new AXI4RWIrrevocable(param))))
  val slave   = IO(new AXI4RWIrrevocable(
    param.copy(idWidth = param.idWidth + log2Ceil(n))))

  if (n == 1) {
    slave <> masters(0)
  } else {
    val pfxBits = log2Ceil(n)

    //rrGrant: pure combinational round-robin priority encoder
    //Scans 'reqs' starting from the slot after 'last' (wrapping), and returns the index of the first asserted request
    //When no request is asserted the return value is 0 
    def rrGrant(reqs: UInt, last: UInt): UInt = {
      //Double the request bits so a single right-shift handles wrap-around.
      val doubled  = Cat(reqs, reqs)                        //2n bits
      val shifted  = (doubled >> (last +& 1.U))(n - 1, 0)  //n bits, slot (last+1) at LSB
      val relGrant = PriorityEncoder(shifted)               //relative offset from (last+1)
      val absGrant = relGrant +& last +& 1.U              //absolute index (may exceed n-1)
      val wrapped  = Mux(absGrant >= n.U,
                        (absGrant - n.U)(pfxBits - 1, 0),
                        absGrant(pfxBits - 1, 0))
      Mux(reqs.orR, wrapped, 0.U(pfxBits.W))
    }

    //Round-robin state (updated on each grant)
    val rrAW = RegInit(0.U(pfxBits.W))
    val rrAR = RegInit(0.U(pfxBits.W))

    //Write-side ownership tracking
    //Only one AW in-flight at a time, wOwner records which master it belongs to so that subsequent W data is steered correctly
    val wInflight = RegInit(false.B)
    val wOwner    = RegInit(0.U(pfxBits.W))



    //AW channel
    val awReqs  = VecInit(masters.map(_.aw.valid)).asUInt
    val awGrant = rrGrant(awReqs, rrAW)

    //Block new AW grants while a W beat is still outstanding.
    slave.aw.valid   := awReqs(awGrant) && !wInflight
    slave.aw.bits    := masters(awGrant).aw.bits
    slave.aw.bits.id := Cat(awGrant, masters(awGrant).aw.bits.id) //prepend master index

    masters.zipWithIndex.foreach { case (m, i) =>
      m.aw.ready := (i.U === awGrant) && slave.aw.ready && !wInflight
    }

    when(slave.aw.fire) {
      rrAW   := awGrant
      wOwner := awGrant
      //If W does not arrive in the same cycle, mark write as inflight.
      when(!slave.w.fire) { wInflight := true.B }
    }

    //W channel
    val wSel = Mux(wInflight, wOwner, awGrant)
    slave.w.valid := masters(wSel).w.valid
    slave.w.bits  := masters(wSel).w.bits
    masters.zipWithIndex.foreach { case (m, i) => m.w.ready := (i.U === wSel) && slave.w.ready
    }

    //Clear inflight once the W beat is accepted (unless AW fires again in the same cycle, which re-establishes ownership immediately)
    when(slave.w.fire && !slave.aw.fire) { wInflight := false.B }

    // B channel 
    val bOwner = slave.b.bits.id(pfxBits + param.idWidth - 1, param.idWidth) //demux using the upper pfxBits of the returned ID

    slave.b.ready := masters(bOwner).b.ready

    masters.zipWithIndex.foreach { case (m, i) =>
      m.b.valid   := slave.b.valid && (bOwner === i.U)
      m.b.bits    := slave.b.bits
      m.b.bits.id := slave.b.bits.id(param.idWidth - 1, 0) //strip prefix
    }

    // AR channel, independent round-robin, no ordering constraints with AW
    val arReqs  = VecInit(masters.map(_.ar.valid)).asUInt
    val arGrant = rrGrant(arReqs, rrAR)

    slave.ar.valid   := arReqs(arGrant)
    slave.ar.bits    := masters(arGrant).ar.bits
    slave.ar.bits.id := Cat(arGrant, masters(arGrant).ar.bits.id) //prepend master index

    masters.zipWithIndex.foreach { case (m, i) => m.ar.ready := (i.U === arGrant) && slave.ar.ready
    }

    when(slave.ar.fire) { rrAR := arGrant }

    //R channel
    val rOwner = slave.r.bits.id(pfxBits + param.idWidth - 1, param.idWidth) //demux using the upper pfxBits of the returned ID

    slave.r.ready := masters(rOwner).r.ready

    masters.zipWithIndex.foreach { case (m, i) =>
      m.r.valid   := slave.r.valid && (rOwner === i.U)
      m.r.bits    := slave.r.bits
      m.r.bits.id := slave.r.bits.id(param.idWidth - 1, 0) //strip prefix
    }
    }
}
