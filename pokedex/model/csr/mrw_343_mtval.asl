//! ---
//! csr: "mtval"
//! mode: "mrw"
//! id: 0x343
//! tag: "m_mode"
//! ---
//! The mtval register is an MXLEN-bit read-write register. When a trap is
//! taken into M-mode, mtval is either set to zero or written with exception-specific information to assist
//! software in handling the trap.
//!
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Machine Mode.

func GetRaw_MTVAL() => bits(XLEN)
begin
  return MTVAL;
end

func Read_MTVAL() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_M) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MTVAL());
end

func Write_MTVAL(value : bits(XLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_M) then
    return IllegalInstruction();
  end

  // Trim the last bit to hold only valid virtual address
  MTVAL = value[XLEN - 1 : 0];

  return Retired();
end
