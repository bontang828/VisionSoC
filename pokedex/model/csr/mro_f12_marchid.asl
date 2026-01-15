//! ---
//! csr: "marchid"
//! mode: "mro"
//! id: 0xF12
//! tag: "m_mode"
//! ---
//! The marchid (Machine Architecture ID Register) is an MXLEN-bit read-only register
//! accessible exclusively in Machine Mode.
//!
//! - Exceptions:
//!   - Attempt to write (`marchid` is read only).
//!   - Attempt to access from a privilege level lower than M.

func Read_MARCHID() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(CFG_MARCHID);
end

func GetRaw_MARCHID() => bits(XLEN)
begin
  return CFG_MARCHID;
end
