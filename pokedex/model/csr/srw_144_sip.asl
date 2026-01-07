//! ---
//! csr: "sip"
//! mode: "srw"
//! id: 0x144
//! tag: "s_mode"
//! ---
//! The sip register is an SXLEN-bit read/write register containing information
//! on pending interrupts.
//!
//! LCOFIP is not implemented.
//!
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_SIP() => bits(SXLEN)
begin
  var value : bits(SXLEN) = Zeros(SXLEN);
  value[1] = SSIP;
  value[5] = STIP;
  value[9] = FFI_supervisor_external_interrupt_pending() OR SEIP;
  return value;
end

func Read_SIP() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_SIP());
end

func Write_SIP(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  SSIP = value[1];

  return Retired();
end
