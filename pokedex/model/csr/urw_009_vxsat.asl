//! ---
//! csr: "vxsat"
//! mode: "urw"
//! id: 0x009
//! tag: "vector"
//! ---
//! The vxsat (Vector Fixed-Point Saturation) register is an XLEN-bit read/write register
//!
//！- Side Effects:
//!   - Write: writes will set `mstatus.vs` to dirty, even if the value is unchanged.
//! - Exceptions:
//!   - Attempt to access if `mstatus.vs` is disabled.


func Read_VXSAT() => CsrReadResult
begin
  if !isEnabled_VS() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk([Zeros(31), VXSAT]);
end

func GetRaw_VXSAT() => bits(XLEN)
begin
  return [Zeros(31), VXSAT];
end

func Write_VXSAT(value: bits(32)) => Result
begin
  if !isEnabled_VS() then
    return IllegalInstruction();
  end

  // uppper bits are ignored, required by spec
  VXSAT = value[0];

  logWrite_VCSR();

  // set mstatus.vs = dirty
  makeDirty_VS();

  return Retired();
end
