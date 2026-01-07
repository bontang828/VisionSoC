//! ---
//! csr: "satp"
//! mode: "srw"
//! id: 0x180
//! tag: "s_mode"
//! ---
//! The satp CSR is an SXLEN-bit read/write register which controls supervisor-mode
//! address translation and protection.
//!
//! This register holds the physical page number (PPN) of the root page table,
//! i.e., its supervisor physical address divided by 4 KiB; an address space
//! identifier (ASID), which facilitates address-translation fences on a
//! per-address-space basis; and the MODE field, which selects the current
//! address-translation scheme.
//!
//! - Behaviors:
//!   * Attempting to select MODE=Bare with a nonzero pattern in the remaining
//!     fields will have MMU disable translation and ignore garbage bits in PPN.
//! - Exceptions: An Illegal Instruction Exception is raised if the register is
//!   accessed from a privilege level lower than Supervisor Mode.

func GetRaw_SATP() => bits(SXLEN)
begin
  return [
    SATP_MODE,
    SATP_ASID,
    SATP_PPN
  ];
end

func Read_SATP() => CsrReadResult
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return CsrReadIllegalInstruction();
  end

  return CsrReadOk(GetRaw_SATP());
end

func Write_SATP(value : bits(SXLEN)) => Result
begin
  if !IsPrivAtLeast(PRIV_MODE_S) then
    return IllegalInstruction();
  end

  SATP_MODE = value[31];
  SATP_ASID = value[30:22];
  SATP_PPN = value[21:0];

  return Retired();
end
