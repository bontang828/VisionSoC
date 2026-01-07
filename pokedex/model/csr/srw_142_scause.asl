//! ---
//! csr: "scause"
//! mode: "srw"
//! id: 0x142
//! tag: "s_mode"
//! ---
//! The scause CSR is an SXLEN-bit read-write register. When a trap is taken into S-mode,
//! scause is written with a code indicating the event that caused the trap.
//!
//! - Behavior: SCAUSE holds any value given from software.
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_SCAUSE() => bits(SXLEN)
begin
  return [
    SCAUSE_INTERRUPT, // [SXLEN-1]
    SCAUSE_XCPT_CODE  // [SXLEN-2:0]
  ];
end

func Read_SCAUSE() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_SCAUSE());
end

func Write_SCAUSE(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  SCAUSE_INTERRUPT = value[SXLEN - 1];
  SCAUSE_XCPT_CODE = value[SXLEN - 2 : 0];

  return Retired();
end
