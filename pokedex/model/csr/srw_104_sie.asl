//! ---
//! csr: "sie"
//! mode: "srw"
//! id: 0x104
//! tag: "s_mode"
//! ---
//! The sie is the corresponding SXLEN-bit read/write register containing
//! interrupt enable bits.
//!
//! LCOFIE is not implemented.
//!
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_SIE() => bits(SXLEN)
begin
  var value : bits(SXLEN) = Zeros(SXLEN);
  value[1] = SSIE;
  value[5] = STIE;
  value[9] = SEIE;
  return value;
end

func Read_SIE() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_SIE());
end

func Write_SIE(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  SSIE = value[1];
  STIE = value[5];
  SEIE = value[9];

  return Retired();
end
