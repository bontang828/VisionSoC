use crate::bus::{AtomicOp, Bus, BusError, BusResult};
use crate::model::{Loader, ModelHandle, PokedexCallbackMem, StepCode, StepDetail};

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
    pub fn from_bits(raw: u32) -> Self {
        let mode = raw >> 31 & 0x1;
        let asid = raw >> 22 & 0x1ff;
        let ppn = raw & 0x3fffff;

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

impl VirtAddr {
    pub fn from_32b(raw: u32) -> Self {
        let vpn_1 = (raw >> 22) & 0x3ff;
        let vpn_0 = (raw >> 12) & 0x3ff;
        let page_offset = raw & 0xfff;

        Self {
            vpn: [vpn_0, vpn_1],
            page_offset,
        }
    }
}

pub struct Global {
    pub(crate) bus: Bus,
    pub(crate) stats: Statistic,
}

impl Global {
    pub fn rv32_translate(&mut self, addr: u32, satp: u32) -> u32 {
        let satp = Satp::from_bits(satp);
        // vm_mode is calculated with current privilege and satp.MODE
        if satp.mode.is_bare() {
            return addr;
        }

        const SV32_LVL: u32 = 2;
        const PAGE_SIZE: u32 = 4096;
        const PTE_SIZE: u32 = 4;

        let a = satp.ppn * PAGE_SIZE;
        let mut i = SV32_LVL - 1;
        while i >= 0 {
            let va = VirtAddr::from_32b(addr);
            let pte_addr = a + (va.vpn[i as usize] * PTE_SIZE);
            let mut pte = [0; 4];
            self.bus.read(pte_addr, &mut pte);
        }

        todo!()
    }
}

impl PokedexCallbackMem for Global {
    type CbMemError = BusError;

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
