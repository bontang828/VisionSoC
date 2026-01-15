//! ---
//! csr: "mconfigptr"
//! mode: "mro"
//! id: 0xF15
//! tag: "m_mode"
//! ---
//! The mconfigptr (Machine Configuration Pointer Register) is an MXLEN-bit read-only register
//! accessible exclusively in Machine Mode.
//!
//! - Exceptions:
//!   - Attempt to write (`mconfigptr` is read only).
//!   - Attempt to access from a privilege level lower than M.

func Read_MCONFIGPTR() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(CFG_MCONFIGPTR);
end

func GetRaw_MCONFIGPTR() => bits(XLEN)
begin
  return CFG_MCONFIGPTR;
end
