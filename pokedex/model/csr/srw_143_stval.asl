//! ---
//! csr: "stval"
//! mode: "srw"
//! id: 0x143
//! tag: "s_mode"
//! ---
//! The stval CSR is an SXLEN-bit read-write register. When a trap is
//! taken into S-mode, stval is written with exception-specific information to assist
//! software in handling the trap.
//!
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_STVAL() => bits(SXLEN)
begin
  return STVAL;
end

func Read_STVAL() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_STVAL());
end

func Write_STVAL(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  // Trim the last bit to hold only valid virtual address
  STVAL = value[SXLEN - 1 : 0];

  return Retired();
end
