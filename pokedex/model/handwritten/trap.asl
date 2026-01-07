func TrapException(cause : integer{0..63}, trap_value : bits(32))
begin
  // Try delegate trap to supervisor mode. If none of the condition met, we
  // handle the trap in M mode.
  let delegated : boolean = TryDelegateTrap(cause, trap_value);
  if delegated then
    return;
  end

  // mepc
  MEPC = PC;

  // mcause
  MCAUSE = ['0', cause[30:0]];

  // mstatus
  MSTATUS_MPIE = MSTATUS_MIE;
  MSTATUS_MIE = '0';
  MSTATUS_MPP = CURRENT_PRIVILEGE;

  // mtval
  MTVAL = trap_value;

  PC = [ MTVEC_BASE, '00' ];
end

// we only support limited machien mode interrupt now
func TrapInterrupt(interrupt_code : integer{1,3,5,7,9,11,13})
begin

  // save current context
  MSTATUS_MPIE = MSTATUS_MIE;
  MSTATUS_MIE = '0';
  MSTATUS_MPP = CURRENT_PRIVILEGE;
  MEPC = PC;

  CURRENT_PRIVILEGE = PRIV_MODE_M;

  MCAUSE = ['1', interrupt_code[30:0]];

  if MTVEC_MODE == MTVEC_MODE_DIRECT then
    PC = [ MTVEC_BASE, '00' ];
  else
    PC = [ MTVEC_BASE, '00' ] + (4 * interrupt_code);
  end
end

// Return TRUE if the interrupt is handled
func CheckInterrupt() => boolean
begin
  // First check interrupt pending is high or low
  // Multiple simultaneous interrupts destined for M-mode are handled in the
  // following decreasing priority order: MEI, MSI, MTI, SEI, SSI, STI, LCOFI.
  var interrupt : integer{0,1,3,5,7,9,11,13} = CheckSupervisorInterrupt();
  if (MTIE AND getExternal_MTIP) == '1' then
    interrupt = 7;
  end
  if (MSIE AND getExternal_MSIP) == '1' then
    interrupt = 3;
  end
  if (MEIE AND getExternal_MEIP) == '1' then
    interrupt = 11;
  end

  // If no interrupt
  if interrupt == 0 then
    return FALSE;
  end

  assert (interrupt > 0 && interrupt < 14) && (interrupt REM 2 == 1);

  if InterruptDelegatable(interrupt) then
    return TrapSupervisorInterrupt(interrupt as integer{1,5,9});
  end

  // lower privilege trap immediately, whereas in M mode MIE mask is respected
  if (CURRENT_PRIVILEGE == PRIV_MODE_M && MSTATUS_MIE == '0') then
    return FALSE;
  end

  TrapInterrupt(interrupt as integer{1,3,5,7,9,11,13});
  return TRUE;
end
