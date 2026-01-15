//! ---
//! csr: "mimpid"
//! mode: "mro"
//! id: 0xF13
//! tag: "m_mode"
//! ---
//! The mimpid (Machine Implementation ID Register) is an MXLEN-bit read-only register
//! accessible exclusively in Machine Mode.
//!
//! - Exceptions:
//!   - Attempt to write (`mimplid` is read only).
//!   - Attempt to access from a privilege level lower than M.

func Read_MIMPID() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(CFG_MIMPID);
end

func GetRaw_MIMPID() => bits(XLEN)
begin
  return CFG_MIMPID;
end
