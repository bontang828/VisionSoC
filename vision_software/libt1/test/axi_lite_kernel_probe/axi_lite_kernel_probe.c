// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel-side AXI-Lite read probe for the KV260 bring-up issue.
 *
 * This bypasses UIO and /dev/mem userspace mappings. If readl() through
 * ioremap_np()/ioremap() sees the same 16-byte-aligned-only data pattern,
 * the bug is below Linux userspace pgprot handling.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>

static ulong base = 0xa0010000;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "Physical AXI-Lite base address to probe");

static uint size = 0x10000;
module_param(size, uint, 0444);
MODULE_PARM_DESC(size, "Mapping size");

static const u32 probe_offsets[] = {
	0x00, 0x04, 0x08, 0x0c,
	0x10, 0x14, 0x18, 0x1c,
	0x30, 0x34, 0x38, 0x3c,
	0x40, 0x44, 0x48, 0x4c,
	0x50, 0x54,
};

static int __init axi_lite_kernel_probe_init(void)
{
	void __iomem *regs;
	size_t i;

	regs = ioremap_np(base, size);
	if (!regs) {
		pr_info("axi_lite_kernel_probe: ioremap_np unavailable/failed, trying ioremap\n");
		regs = ioremap(base, size);
	}
	if (!regs) {
		pr_err("axi_lite_kernel_probe: failed to map phys 0x%lx size 0x%x\n",
		       base, size);
		return -ENOMEM;
	}

	pr_info("axi_lite_kernel_probe: base=0x%lx size=0x%x virt=%p\n",
		base, size, regs);
	for (i = 0; i < ARRAY_SIZE(probe_offsets); i++) {
		u32 off = probe_offsets[i];
		u32 val = readl(regs + off);

		pr_info("axi_lite_kernel_probe: [0x%04x] = 0x%08x\n", off, val);
	}

	iounmap(regs);
	return 0;
}

static void __exit axi_lite_kernel_probe_exit(void)
{
	pr_info("axi_lite_kernel_probe: exit\n");
}

module_init(axi_lite_kernel_probe_init);
module_exit(axi_lite_kernel_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VisionSoC AXI-Lite kernel read probe");
