use crate::bus::{AtomicOp, Bus, BusError, BusResult};
use crate::model::{Loader, ModelHandle, PokedexCallbackMem, StepCode, StepDetail, VirtMemReqInfo};

pub struct Simulator {
    core: ModelHandle,

    pub(crate) global: Global,
}

impl Simulator {
    pub fn new(model_loader: Loader, bus: Bus) -> Self {
        let global = Global {
            bus,

            stats: Statistic::new(),
        };

        let core = ModelHandle::new(model_loader);

        Simulator { core, global }
    }

    pub fn stats(&self) -> &Statistic {
        &self.global.stats
    }
}

impl Simulator {
    pub fn reset_core(&mut self, pc: u32) {
        // may uncomment to debug issue inside model reset
        // debug!("reset core with pc={pc:#010x}");

        self.core.reset(pc);
    }

    pub fn step(&mut self) -> StepCode {
        // pre-step book keeping
        self.global.stats.step_count += 1;

        self.core.step(&mut self.global)
    }

    pub fn step_trace(&mut self) -> StepDetail<'_> {
        // pre-step book keeping
        self.global.stats.step_count += 1;

        self.core.step_trace(&mut self.global)
    }

    pub fn is_exited(&self) -> Option<u32> {
        self.global.bus.try_get_exit_code()
    }

    pub fn core(&self) -> &ModelHandle {
        &self.core
    }
}

pub enum VirtualMemoryMode {
    Bare,
    Sv32,
}

impl VirtualMemoryMode {
    pub fn is_bare(&self) -> bool {
        match self {
            Self::Bare => true,
            Self::Sv32 => false,
        }
    }
}

pub struct Satp {
    mode: VirtualMemoryMode,
    asid: u32,
    ppn: u32,
}

impl Satp {
    const MODE_MASK: u32 = 0x8000_0000;
    const ASID_MASK: u32 = 0x7FC0_0000;
    const PPN_MASK: u32 = 0x003F_FFFF;

    pub fn from_bits(raw: u32) -> Self {
        let mode = (raw & Self::MODE_MASK) >> 31;
        let asid = (raw & Self::ASID_MASK) >> 22;
        let ppn = raw & Self::PPN_MASK;

        let mode = match mode {
            0 => VirtualMemoryMode::Bare,
            1 => VirtualMemoryMode::Sv32,
            _ => unreachable!(),
        };

        Satp { mode, asid, ppn }
    }
}

pub struct VirtAddr {
    vpn: [u32; 2],
    page_offset: u32,
}

pub struct PhsyAddr {
    ppn: [u32; 2],
    page_offset: u32,
}
impl PhsyAddr {
    pub fn new(ppn_hi: u32, ppn_lo: u32, page_offset: u32) -> Self {
        Self {
            ppn: [ppn_hi, ppn_lo],
            page_offset,
        }
    }

    pub fn to_u32(self) -> u32 {
        self.ppn[0] << 22 | self.ppn[1] << 12 | self.page_offset & 0xFFF
    }
}

impl VirtAddr {
    const VPN1_MASK: u32 = 0xFFC0_0000;
    const VPN0_MASK: u32 = 0x003F_F000;
    const PAGE_OFFSET_MASK: u32 = 0x0000_0FFF;

    pub fn from_32b(raw: u32) -> Self {
        let vpn_1 = (raw & Self::VPN1_MASK) >> 22;
        let vpn_0 = (raw & Self::VPN0_MASK) >> 12;
        let page_offset = raw & Self::PAGE_OFFSET_MASK;

        Self {
            vpn: [vpn_0, vpn_1],
            page_offset,
        }
    }
}

pub struct PageTableEntry {
    ppn: [u32; 2],
    dirty: bool,
    access: bool,
    global: bool,
    user: bool,
    execute: bool,
    write: bool,
    read: bool,
    valid: bool,
}

impl PageTableEntry {
    const PPN_1: u32 = 0xFFF0_0000;
    const PPN_0: u32 = 0x000F_FC00;

    pub fn from_32b(raw: u32) -> Self {
        let ppn1 = (raw & Self::PPN_1) >> 20;
        let ppn0 = (raw & Self::PPN_0) >> 10;
        let dirty = (raw >> 7) & 0x1;
        let access = (raw >> 6) & 0x1;
        let global = (raw >> 5) & 0x1;
        let user = (raw >> 4) & 0x1;
        let execute = (raw >> 3) & 0x1;
        let write = (raw >> 2) & 0x1;
        let read = (raw >> 1) & 0x1;
        let valid = raw & 0x1;

        Self {
            ppn: [ppn0, ppn1],
            dirty: dirty == 1,
            access: access == 1,
            global: global == 1,
            user: user == 1,
            execute: execute == 1,
            write: write == 1,
            read: read == 1,
            valid: valid == 1,
        }
    }
}

pub struct Global {
    pub(crate) bus: Bus,
    pub(crate) stats: Statistic,
}

#[derive(Debug)]
pub enum VirtAddrTrasnlateError {
    AccessFault,
    PageFault,
}

impl Global {
    pub fn sv32_walk(&mut self, vm_info: &VirtMemReqInfo) -> Result<u32, VirtAddrTrasnlateError> {
        let satp = Satp::from_bits(vm_info.satp);
        // mode is calculated with current privilege and satp.MODE at C-ABI side
        if satp.mode.is_bare() {
            return Ok(vm_info.addr);
        }

        assert!(
            vm_info.priv_ < 2,
            "Translate with reserved privilege mode or machine mode"
        );

        const PAGE_SIZE: u64 = 4096;
        const PTE_SIZE: u64 = 4;

        // we might support 34-bit Bus
        let mut a: u64 = (satp.ppn as u64) * PAGE_SIZE;
        let mut i: i32 = 1;
        while i >= 0 {
            let va = VirtAddr::from_32b(vm_info.addr);
            let pte_addr = a + ((va.vpn[i as usize] as u64) * PTE_SIZE);
            let mut pte = [0; 4];
            let pte_addr: u32 = pte_addr
                .try_into()
                .expect("Get 34-bit address that is not support in this platform");
            // TODO: PMA & PMP
            if self.bus.read(pte_addr, &mut pte).is_err() {
                // caller responsibility to return corresponding access fault reason
                return Err(VirtAddrTrasnlateError::AccessFault);
            }
            let pte = PageTableEntry::from_32b(u32::from_le_bytes(pte));
            if !pte.valid || (!pte.read && pte.write) || (!pte.execute && pte.write && !pte.read) {
                // not valid OR note readable but writable OR reserved
                return Err(VirtAddrTrasnlateError::PageFault);
            }
            if !pte.read && !pte.execute {
                i -= 1;
                a = ((pte.ppn[1] << 10) | pte.ppn[0]) as u64 * PAGE_SIZE;
                continue;
            }
            if i > 0 && pte.ppn[0] != 0 {
                // a super page misaligned
                return Err(VirtAddrTrasnlateError::PageFault);
            }
            if vm_info.priv_ == 0 && !pte.user {
                // not accessible for user mode
                return Err(VirtAddrTrasnlateError::PageFault);
            }

            let mstatus_sum = |mstatus: u32| mstatus >> 18 & 0x1;
            let mstatus_mxr = |mstatus: u32| mstatus >> 19 & 0x1;

            if vm_info.priv_ == 1 && pte.user {
                // access user mode memory in supervisor mode depends on mstatus.SUM
                let sum = mstatus_sum(vm_info.mstatus);
                if sum == 0 {
                    return Err(VirtAddrTrasnlateError::PageFault);
                }
            }

            let mxr = mstatus_mxr(vm_info.mstatus);
            // 0=fetch, 1=read, 2=write. See include/pokedex_interface.h
            match vm_info.access_type {
                0 if !pte.execute => return Err(VirtAddrTrasnlateError::PageFault),
                1 if !pte.read && !(pte.execute && mxr == 1) => {
                    return Err(VirtAddrTrasnlateError::PageFault);
                }
                2 if !pte.write => return Err(VirtAddrTrasnlateError::PageFault),
                0 | 1 | 2 => (),
                _ => panic!("Internal ABI error: unknown access_type when translating memory"),
            }

            if !pte.access || vm_info.access_type == 2 && !pte.dirty {
                return Err(VirtAddrTrasnlateError::PageFault);
            }

            // now the translation is successful
            let phsy_addr: PhsyAddr;
            if i > 0 {
                // superpage
                phsy_addr = PhsyAddr::new(pte.ppn[1], va.vpn[0], va.page_offset);
            } else {
                phsy_addr = PhsyAddr::new(pte.ppn[1], pte.ppn[0], va.page_offset);
            }

            return Ok(phsy_addr.to_u32());
        }

        Err(VirtAddrTrasnlateError::PageFault)
    }
}

impl PokedexCallbackMem for Global {
    type CbMemError = BusError;

    fn handle_virtual_address(
        &mut self,
        vm_info: &mut VirtMemReqInfo,
    ) -> Result<(), Self::CbMemError> {
        // TODO: TLB
        let addr = self.sv32_walk(vm_info);
        vm_info.t_addr = addr.unwrap();

        Ok(())
    }

    fn inst_fetch_2(&mut self, addr: u32, satp: u32) -> BusResult<u16> {
        assert!(addr.is_multiple_of(2));

        self.stats.fetch_count += 1;

        let mut data = [0; 2];
        self.bus
            .read(addr, &mut data)
            .map(|_| u16::from_le_bytes(data))
    }

    fn read_mem_u8(&mut self, addr: u32, satp: u32) -> BusResult<u8> {
        let mut data = [0; 1];
        self.bus
            .read(addr, &mut data)
            .map(|_| u8::from_le_bytes(data))
    }

    fn read_mem_u16(&mut self, addr: u32, satp: u32) -> BusResult<u16> {
        assert!(addr.is_multiple_of(2));

        let mut data = [0; 2];
        self.bus
            .read(addr, &mut data)
            .map(|_| u16::from_le_bytes(data))
    }

    fn read_mem_u32(&mut self, addr: u32, satp: u32) -> BusResult<u32> {
        assert!(addr.is_multiple_of(4));

        let mut data = [0; 4];
        self.bus
            .read(addr, &mut data)
            .map(|_| u32::from_le_bytes(data))
    }

    fn write_mem_u8(&mut self, addr: u32, value: u8, satp: u32) -> BusResult<()> {
        self.bus.write(addr, &value.to_le_bytes())
    }

    fn write_mem_u16(&mut self, addr: u32, value: u16, satp: u32) -> BusResult<()> {
        self.bus.write(addr, &value.to_le_bytes())
    }

    fn write_mem_u32(&mut self, addr: u32, value: u32, satp: u32) -> BusResult<()> {
        self.bus.write(addr, &value.to_le_bytes())
    }

    fn amo_mem_u32(&mut self, addr: u32, op: AtomicOp, value: u32, satp: u32) -> BusResult<u32> {
        // TODO: currently we simulate AMO using read-modify-write.
        // Consider forward it directly to bus later

        let mut read_bytes = [0; 4];
        self.bus.read(addr, &mut read_bytes)?;
        let read_value = u32::from_le_bytes(read_bytes);

        let write_value: u32 = op.do_arith_u32(read_value, value);
        self.bus.write(addr, &write_value.to_le_bytes())?;

        Ok(read_value)
    }
}

#[derive(Debug, Clone, Default)]
pub struct Statistic {
    pub fetch_count: u64,
    pub step_count: u64,
}

impl Statistic {
    pub fn new() -> Self {
        Self::default()
    }
}
