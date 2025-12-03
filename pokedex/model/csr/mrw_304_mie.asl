//! ---
//! csr: "mie"
//! mode: "mrw"
//! id: 0x304
//! tag: "m_mode"
//! ---
//! The mie (Machine Interrupt Enable) register is an MXLEN-bit read/write register
//! containing interrupt enable bits for various interrupt sources.
//!
//! - Implemented Fields: SSIE, MSIE, STIE, MTIE, SEIE, MEIE.
//! - Exceptions:
//!     - Illegal Instruction if accessed from a privilege level lower than Machine Mode.

func Read_MIE() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MIE());
end

func Write_MIE(value: bits(32)) => Result
begin
  if !IsPrivAtLeast_M() then
    return IllegalInstruction();
  end

  SSIE = value[1];
  MSIE = value[3];
  STIE = value[5];
  MTIE = value[7];
  SEIE = value[9];
  MEIE = value[11];

  logWrite_MIE();

  return Retired();
end

// utility functions

func GetRaw_MIE() => bits(32)
begin
  var value : bits(32) = Zeros(32);
  value[1] = SSIE;
  value[3] = MSIE;
  value[5] = STIE;
  value[7] = MTIE;
  value[9] = SEIE;
  value[11] = MEIE;
  return value;
end

func logWrite_MIE()
begin
  FFI_write_CSR_hook(CSR_MIE);
end
