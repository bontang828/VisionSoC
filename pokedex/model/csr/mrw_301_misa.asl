//! ---
//! csr: "misa"
//! mode: "mrw"
//! id: 0x301
//! tag: "m_mode"
//! ---
//! The misa (Machine ISA Register) is an MXLEN-bit read/write register accessible
//! exclusively in Machine Mode. It reports the Instruction Set Architecture (ISA)
//! extensions supported by the hart.
//! - Value: The register contains a bitmask indicating the supported
//!   extensions (bits 0–25) and the machine XLEN (bits MXLEN-1:MXLEN-2).
//!
//! - Impl-defined Behaviors:
//!   - Write: writes are ignored.
//! - Exceptions:
//!   - Attempt to access from a privilege level lower than M.

func Read_MISA() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MISA());
end

func GetRaw_MISA() => bits(XLEN)
begin
  // Generated from templates/config.asl.j2
  return POKEDEX_CONFIG_RAW_MISA;
end

func Write_MISA(value: bits(32)) => Result
begin
  if !IsPrivAtLeast_M() then
    return IllegalInstruction();
  end

  // NOTE : impl defined behavior
  //
  // we have no writable bits,
  // thus writing is no-op
  return Retired();
end
