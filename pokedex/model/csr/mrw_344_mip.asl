//! ---
//! csr: "mip"
//! mode: "mrw"
//! id: 0x344
//! tag: "m_mode"
//! ---
//! The mip (Machine Interrupt Pending) register is an MXLEN-bit read/write register
//! containing information on pending interrupts.
//!
//! - Implemented Fields: MEIP, MTIP, MSIP, SEIP, STIP, SSIP
//! - Behavior: Write operations are currently no-ops as these bits are driven by external signals.
//! - Exceptions:
//!     - Illegal Instruction if accessed from a privilege level lower than Machine Mode.

func Read_MIP() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MIP());
end

func GetRaw_MIP() => bits(XLEN)
begin
  var value : bits(32) = Zeros(32);
  value[1] = SSIP;
  value[3] = FFI_machine_software_interrupt_pending();
  value[5] = STIP;
  value[7] = getExternal_MTIP;
  value[9] = FFI_machine_external_interrupt_pending() OR SEIP;
  value[11] = getExternal_MEIP;

  return value;
end

func Write_MIP(value : bits(32)) => Result
begin
  if !IsPrivAtLeast_M() then
    return IllegalInstruction();
  end

  SSIP = value[1];
  STIP = value[5];
  SEIP = value[9];

  return Retired();
end
