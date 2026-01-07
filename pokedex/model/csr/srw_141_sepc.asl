//! ---
//! csr: "sepc"
//! mode: "srw"
//! id: 0x141
//! tag: "s_mode"
//! ---
//! sepc is an SXLEN-bit read/write CSR. When a trap is taken into S-mode,
//! sepc is written with the virtual address of the instruction that was
//! interrupted or that encountered the exception.
//!
//! - Behavior: The least significant bit is hard-wired to 0.
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_SEPC() => bits(SXLEN)
begin
  return [SEPC, '0'];
end

func Read_SEPC() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_SEPC());
end

func Write_SEPC(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  SEPC = value[SXLEN-1:1];

  return Retired();
end
