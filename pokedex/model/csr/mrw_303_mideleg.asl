//! ---
//! csr: "mideleg"
//! mode: "mrw"
//! id: 0x303
//! tag: "m_mode"
//! ---
//! The mideleg CSR is an MXLEN-bit read/write register which controls interrupt delegation.
//!
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Machine Mode.

func GetRaw_MIDELEG() => bits(SXLEN)
begin
  return [
    Zeros(XLEN - 10),
    MIDELEG_SEI, // [9]
    '000',
    MIDELEG_STI, // [5]
    '000',
    MIDELEG_SSI, // [1]
    '0'
  ];
end

func Read_MIDELEG() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_M) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MIDELEG());
end

func Write_MIDELEG(value : bits(XLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_M) then
    return IllegalInstruction();
  end

  MIDELEG_SSI = value[1];
  MIDELEG_STI = value[5];
  MIDELEG_SEI = value[9];

  return Retired();
end
