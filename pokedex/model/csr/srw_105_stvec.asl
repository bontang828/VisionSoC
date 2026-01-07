//! ---
//! csr: "stvec"
//! mode: "srw"
//! id: 0x105
//! tag: "s_mode"
//! ---
//! The stvec register is an SXLEN-bit read/write register that holds trap vector
//! configuration, consisting of a vector base address (BASE) and a vector mode
//! (MODE).
//!
//! - Behavior: The stvec.MODE fields supports only Direct or Vectored mode. Other
//!   value are ignored.
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_STVEC() => bits(SXLEN)
begin
  return [
    STVEC_BASE,
    '0',
    STVEC_MODE
  ];
end

func Read_STVEC() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_STVEC());
end

func Write_STVEC(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  STVEC_MODE = value[0];
  STVEC_BASE = value[SXLEN-1:2];

  return Retired();
end
