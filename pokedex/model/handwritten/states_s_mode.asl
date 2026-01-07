constant SXLEN : integer = XLEN;

func FFI_supervisor_software_interrupt_pending() => bit;
func FFI_supervisor_timer_interrupt_pending() => bit;
func FFI_supervisor_external_interrupt_pending() => bit;

var MSTATUS_SIE : bit;
var MSTATUS_SPIE : bit;
var MSTATUS_SPP : bit;
var MSTATUS_SUM : bit;
var MSTATUS_MXR : bit;

var MEDELEG_MISALIGNED_FETCH : bit;
var MEDELEG_FETCH_ACCESS : bit;
var MEDELEG_ILLEGAL_INSTRUCTION : bit;
var MEDELEG_BREAKPOINT : bit;
var MEDELEG_MISALIGNED_LOAD : bit;
var MEDELEG_LOAD_ACCESS : bit;
var MEDELEG_MISALIGNED_STORE : bit;
var MEDELEG_STORE_ACCESS : bit;
var MEDELEG_USER_ECALL : bit;
var MEDELEG_SUPERVISOR_ECALL : bit;
var MEDELEG_FETCH_PAGE_FAULT : bit;
var MEDELEG_LOAD_PAGE_FAULT : bit;
var MEDELEG_STORE_PAGE_FAULT : bit;
var MEDELEG_SOFTWARE_CHECK_FAULT : bit;
var MEDELEG_HARDWARE_ERROR_FAULT : bit;

var MIDELEG_SSI : bit;
var MIDELEG_STI : bit;
var MIDELEG_SEI : bit;

var SSIE : bit;
var STIE : bit;
var SEIE : bit;

var SSIP : bit;
var SEIP : bit;
// TODO: once we supports stimecmp register, remove this state.
var STIP : bit;

var SCAUSE_INTERRUPT : bit;
var SCAUSE_XCPT_CODE : bits(SXLEN - 1);

// FIXME: do we need to support non-C ISA? then we can save extra bit here.
var SEPC : bits(SXLEN - 1);

var STVAL : bits(SXLEN);

var STVEC_BASE : bits(SXLEN-2);
var STVEC_MODE : bit;

var SATP_MODE : bit;
var SATP_ASID : bits(9);
var SATP_PPN : bits(22);

func resetArchStateSMode()
begin
  MSTATUS_SIE = '0';
  MSTATUS_SPIE = '0';
  MSTATUS_SPP = '0';
  MSTATUS_SUM = '0';
  MSTATUS_MXR = '0';
end

func TryDelegateTrap(cause : integer{0..63}, trap_value : bits(32)) => boolean
begin
  if CURRENT_PRIVILEGE == PRIV_MODE_M then
    return FALSE;
  end

  // Current implementation hard-wired MEDELEG_H to zero
  if cause > 31 then
    return FALSE;
  end

  let medeleg_bits : bits(32) = GetRaw_MEDELEG();
  let not_delegatable : boolean = medeleg_bits[cause] == '0';
  if not_delegatable then
    return FALSE;
  end

  SCAUSE_INTERRUPT = '0';
  SCAUSE_XCPT_CODE = cause[SXLEN - 2:0];
  SEPC = PC[XLEN - 1:1];
  MSTATUS_SPIE = MSTATUS_SIE;
  MSTATUS_SIE = '0';

  assert CURRENT_PRIVILEGE != PRIV_MODE_M;
  if CURRENT_PRIVILEGE == PRIV_MODE_U then
    MSTATUS_SPP = '0';
  else
    MSTATUS_SPP = '1';
  end

  CURRENT_PRIVILEGE = PRIV_MODE_S;
  STVAL = trap_value[SXLEN - 1:0];

  PC = [STVEC_BASE, '00'];

  return TRUE;
end

func TrapSupervisorInterrupt(icode : integer{1,5,9}) => boolean
begin
  // - input constraint -
  assert icode == 1 || icode == 5 || icode == 9;

  // - check mask -
  // Supervisor interrupt is masked when in M mode or SIE is low
  if CURRENT_PRIVILEGE == PRIV_MODE_M ||
      (CURRENT_PRIVILEGE == PRIV_MODE_S &&
        MSTATUS_SIE == '0') then
    return FALSE;
  end

  // - write states -
  MSTATUS_SPIE = MSTATUS_SIE;
  MSTATUS_SIE = '0';

  assert CURRENT_PRIVILEGE != PRIV_MODE_M;
  if CURRENT_PRIVILEGE == PRIV_MODE_U then
    MSTATUS_SPP = '0';
  else
    MSTATUS_SPP = '1';
  end

  SEPC = PC[XLEN - 1:1];
  SCAUSE_INTERRUPT = '1';
  SCAUSE_XCPT_CODE = icode[SXLEN - 2:0];

  CURRENT_PRIVILEGE = PRIV_MODE_S;

  if STVEC_MODE == '0' then
    // bare
    PC = [STVEC_BASE, '00'];
  else
    // vectored
    PC = [STVEC_BASE, '00'] + (4 * icode);
  end

  return TRUE;
end


// Return TRUE if there were supervisor interrupts. Return 0 if no supervisor
// interrupt or supervisor interrupts are masked.
func CheckSupervisorInterrupt() => integer{0, 1, 5, 9}
begin
  var interrupt : integer{0,1,5,9} = 0;

  let timer = STIE AND (FFI_supervisor_timer_interrupt_pending() OR STIP);
  if timer == '1' then
    interrupt = 5;
  end

  let software = SSIE AND (FFI_supervisor_software_interrupt_pending() OR SSIP);
  if software == '1' then
    interrupt = 1;
  end

  let external = SEIE AND (FFI_supervisor_external_interrupt_pending() OR SEIP);
  if external == '1' then
    interrupt = 9;
  end

  return interrupt;
end

// Return TRUE if the interrupt can be delegated to supervisor mode
func InterruptDelegatable(icode : integer{0..SXLEN-1}) => boolean
begin
  let mideleg_bits : bits(32) = GetRaw_MIDELEG();
  return mideleg_bits[icode] == '1';
end
