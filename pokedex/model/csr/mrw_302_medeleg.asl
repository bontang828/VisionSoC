//! ---
//! csr: "medeleg"
//! mode: "mrw"
//! id: 0x302
//! tag: "m_mode"
//! ---
//! The machine exception delegation register (medeleg) is a 32-bit read/write register.
//! An Illegal Instruction Exception is raised if the register is accessed from a privilege
//! level lower than Machine Mode.
//!
//! Current implementation supports following trap delegation:
//!
//! - MISALIGNED_FETCH
//! - FETCH_ACCESS
//! - ILLEGAL_INSTRUCTION
//! - BREAKPOINT
//! - MISALIGNED_LOAD
//! - LOAD_ACCESS
//! - MISALIGNED_STORE
//! - STORE_ACCESS
//! - USER_ECALL
//! - SUPERVISOR_ECALL
//! - FETCH_PAGE_FAULT
//! - LOAD_PAGE_FAULT
//! - STORE_PAGE_FAULT
//! - SOFTWARE_CHECK_FAULT
//! - HARDWARE_ERROR_FAULT

func GetRaw_MEDELEG() => bits(32)
begin
  return [
    Zeros(32 - 20),
    MEDELEG_HARDWARE_ERROR_FAULT, // [19]
    MEDELEG_SOFTWARE_CHECK_FAULT, // [18]
    '00',                         // [16:17]
    MEDELEG_STORE_PAGE_FAULT,     // [15]
    '0',                          // [14]
    MEDELEG_LOAD_PAGE_FAULT,      // [13]
    MEDELEG_FETCH_PAGE_FAULT,     // [12]
    '00',                         // [11:10]
    MEDELEG_SUPERVISOR_ECALL,     // [9]
    MEDELEG_USER_ECALL,           // [8]
    MEDELEG_STORE_ACCESS,         // [7]
    MEDELEG_MISALIGNED_STORE,     // [6]
    MEDELEG_LOAD_ACCESS,          // [5]
    MEDELEG_MISALIGNED_LOAD,      // [4]
    MEDELEG_BREAKPOINT,           // [3]
    MEDELEG_ILLEGAL_INSTRUCTION,  // [2]
    MEDELEG_FETCH_ACCESS,         // [1]
    MEDELEG_MISALIGNED_FETCH      // [0]
  ];
end

func Read_MEDELEG() => CsrReadResult
begin
  if !IsPrivAtLeast_M() then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_MSTATUS());
end

func Write_MEDELEG(value : bits(32)) => Result
begin
  if !IsPrivAtLeast_M() then
    return IllegalInstruction();
  end

  MEDELEG_MISALIGNED_FETCH     = value[0];
  MEDELEG_FETCH_ACCESS         = value[1];
  MEDELEG_ILLEGAL_INSTRUCTION  = value[2];
  MEDELEG_BREAKPOINT           = value[3];
  MEDELEG_MISALIGNED_LOAD      = value[4];
  MEDELEG_LOAD_ACCESS          = value[5];
  MEDELEG_MISALIGNED_STORE     = value[6];
  MEDELEG_STORE_ACCESS         = value[7];
  MEDELEG_USER_ECALL           = value[8];
  MEDELEG_SUPERVISOR_ECALL     = value[9];
  MEDELEG_FETCH_PAGE_FAULT     = value[12];
  MEDELEG_LOAD_PAGE_FAULT      = value[13];
  MEDELEG_STORE_PAGE_FAULT     = value[15];
  MEDELEG_SOFTWARE_CHECK_FAULT = value[18];
  MEDELEG_HARDWARE_ERROR_FAULT = value[19];

  logWrite_MEDELEG();

  return Retired();
end

func logWrite_MEDELEG()
begin
  FFI_write_CSR_hook(CSR_MEDELEG);
end
