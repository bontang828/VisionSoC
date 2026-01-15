//! ---
//! csr: "mhartid"
//! mode: "mro"
//! id: 0xF14
//! tag: "m_mode"
//! ---
//! The mhartid (Machine Hart ID Register) is an MXLEN-bit read-only register
//! accessible exclusively in Machine Mode.
//!
//! - Exceptions:
//!   - Attempt to write (`mhartid` is read only).
//!   - Attempt to access from a privilege level lower than M.

func Read_MHARTID() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(CFG_MHARTID);
end

func GetRaw_MHARTID() => bits(XLEN)
begin
  return CFG_MHARTID;
end
