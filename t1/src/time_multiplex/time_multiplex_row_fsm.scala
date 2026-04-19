package org.chipsalliance.t1.rtl
import chisel3._
import chisel3.util._

//assumption used in this design: the time multiplexed instruction's meta data in the instructionRegQueue should not be dequeued/clear until the last row has consumed it
//this implies chaining is not supported for time multiplexing instructions, as the next instruction is not consumed by T1 before the current one finished
class time_multiplex_row_fsm(multiplex_num: Int) extends Module {
    val requestRegDequeueFire: Bool = IO(Input(Bool()))
    val rowDone: Bool = IO(Input(Bool()))
    val executionReady: Bool = IO(Input(Bool()))
    val firstRowFire: Bool = IO(Output(Bool()))
    val rowFire: Bool = IO(Output(Bool()))
    val lastRowFire: Bool = IO(Output(Bool())) //controls the instruction to dequeue in the instruction register
    val busy: Bool = IO(Output(Bool()))
    val rowCounter: UInt = IO(Output(UInt(log2Ceil(multiplex_num).max(1).W)))

    val sIdle :: sRow :: sWaitReady :: Nil = Enum(3)
    val state = RegInit(sIdle)
    val counter = RegInit(0.U(log2Ceil(multiplex_num).max(1).W))

    //fires to kick start a row execution
    val rowStartPulse = WireDefault(false.B)

    //core logic
    if (multiplex_num == 1) {
        rowStartPulse := requestRegDequeueFire
        firstRowFire := requestRegDequeueFire
        rowFire := rowStartPulse
        lastRowFire := requestRegDequeueFire
        busy := false.B
        rowCounter := 0.U
    } else {
        switch(state) {
            is(sIdle) {
                when(requestRegDequeueFire) {
                    state := sRow
                    counter := 0.U
                    rowStartPulse := true.B
                }
            }
            is(sRow) {
                when(rowDone) {
                    when(counter === (multiplex_num - 1).U) {
                        //last row finished
                        state := sIdle
                        counter := 0.U
                    } .otherwise {
                        //advance to next row, wait for lanes/LSU to be ready
                        counter := counter + 1.U
                        state := sWaitReady
                    }
                }
            }
            is(sWaitReady) {
                when(executionReady) { //this makes sure all lanes, LSU, VRF are idle and ready for the next row execution
                    state := sRow
                    rowStartPulse := true.B
                }
            }
        }

        firstRowFire := requestRegDequeueFire
        rowFire := rowStartPulse
        lastRowFire := rowFire && (counter === (multiplex_num - 1).U)
        busy := (state === sRow) || (state === sWaitReady)
        rowCounter := counter
    }
}