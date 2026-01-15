//! ---
//! csr: "mvendorid"
//! mode: "mro"
//! id: 0xF11
//! tag: "m_mode"
//! ---
//! The mvendorid (Machine Vendor ID Register) is an MXLEN-bit read-only register
//! accessible exclusively in Machine Mode.
//!
//! - Exceptions:
//!   - Attempt to write (`mvendorid` is read only).
//!   - Attempt to access from a privilege level lower than M.

func Read_MVENDORID() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(CFG_MVENDORID);
end

func GetRaw_MVENDORID() => bits(XLEN)
begin
  return CFG_MVENDORID;
end
