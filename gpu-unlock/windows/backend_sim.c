/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Simulated GA104 (RTX 3070, 10de:2484) backend.
 *
 * The register model reproduces the cold card state captured from the tested
 * Palit board, plus the observable effects of FWSEC-FRTS, the Booter, the
 * SS0/SS1 selector writes and the PCIe FLR. It is a model, not a passthrough:
 * no real PCI configuration space or BAR is touched.
 */
#include <windows.h>
#include "hal.h"
#include "../kernel/gpu_unlock/gfw_eval.h"
#include "../kernel/gpu_unlock/prep_610.h"

#define SIM_BAR_SIZE 0x900000u
#define SIM_SEC2_BASE 0x00840000u
#define SIM_SEC2_FALCON2 0x00841000u
#define SIM_GSP_BASE 0x00110000u
#define SIM_GSP_FALCON2 0x00111000u
#define SIM_ROM_OFFSET 0x300000u
#define SIM_ROM_SIZE (1024u * 1024u)
#define SIM_BARS 6
/* layout of this board: 24 GiB usable FB, VGA window at FB-1MiB, 1 MiB FRTS */
#define SIM_FRTS_OFFSET 0x5ffe00000ull
#define SIM_WPR_END 0x5fff00000ull

struct sim_dma {
	dma_addr_t addr;
	size_t size;
	void *cpu;
	int live;
};

struct sim_card {
	struct pci_dev pdev;
	u16 cmd, saved;
	int busmaster;
	int region;
	int flrs, gsp_starts, booter_starts, selector_writes;
	int meta_error;
	u64 dma_mask_value;
	u64 coherent_mask_value;
	dma_addr_t next_dma;
	struct sim_dma *dma;
	size_t dma_count, dma_cap;
	u32 bars[SIM_BARS];
	CRITICAL_SECTION cs;
};

static u32 regs[SIM_BAR_SIZE / 4];
static struct sim_card sim;
static int sim_lock_ready;
static int sim_ready;
static struct pci_driver nvidia_driver = { "nvidia" };

static u64 get_le64(const u8 *p)
{
	u64 value = 0;
	unsigned int i;

	for (i = 0; i < 8; i++)
		value |= (u64)p[i] << (8 * i);
	return value;
}

static void sim_dma_reset(void)
{
	size_t i;

	for (i = 0; i < sim.dma_count; i++)
		if (sim.dma[i].live) {
			free(sim.dma[i].cpu);
			sim.dma[i].live = 0;
		}
	sim.dma_count = 0;
	sim.next_dma = 0x00100000u;
}

static void sim_cold_state(void)
{
	struct pci_dev *pdev = &sim.pdev;
	struct list_head *devres = &pdev->dev.devres_head;

	memset(regs, 0, sizeof(regs));
	sim_dma_reset();
	sim.cmd = PCI_COMMAND_MEMORY;
	sim.saved = PCI_COMMAND_MEMORY;
	sim.busmaster = 0;
	sim.region = 0;
	sim.flrs = 0;
	sim.gsp_starts = 0;
	sim.booter_starts = 0;
	sim.selector_writes = 0;
	sim.meta_error = 0;
	sim.dma_mask_value = 0xffffffffffffull;
	sim.coherent_mask_value = 0xfffffffffull;

	pdev->vendor = PCI_VENDOR_ID_NVIDIA;
	pdev->device = 0x2484;	/* GA104 RTX 3070 */
	pdev->driver = NULL;
	pdev->current_state = PCI_D0;
	pdev->pm_cap = 0;
	pdev->dev_flags = 0;
	pdev->bar_len = SIM_BAR_SIZE;
	pdev->refs = 1;
	pdev->wdev = &sim;
	pdev->lock = &sim.cs;
	pdev->lock_init = sim_lock_ready;
	strcpy(pdev->name, "0000:21:00.0");
	pdev->dev.dma_mask = &sim.dma_mask_value;
	pdev->dev.coherent_dma_mask = sim.coherent_mask_value;
	pdev->dev.devres_open = 0;
	pdev->dev.devres_before = 0;
	devres->next = devres->prev = devres->owner = devres;
	devres->count = 0;

	sim.bars[0] = 0xfb000000u;
	sim.bars[1] = 0xf000000cu;
	sim.bars[2] = 0xf200000cu;
	sim.bars[3] = 0xf400000cu;
	sim.bars[4] = 0xf600000cu;
	sim.bars[5] = 0xf800000cu;

	/* PMC_BOOT_0 chip identity at BAR0 offset 0. */
	regs[0] = 0x172000a1u;
	/* Both Falcon engines are out of reset and their DMAs are idle. */
	regs[(SIM_SEC2_BASE + 0xf4) / 4] = 1u << 31;
	regs[(SIM_GSP_BASE + 0xf4) / 4] = 1u << 31;
	regs[(SIM_SEC2_BASE + 0x118) / 4] = 2;
	regs[(SIM_GSP_BASE + 0x118) / 4] = 2;

	/* Cold GFW: completed, halted, PLM closed; WPR2 down. */
	regs[GU_GFW_PLM_OFFSET / 4] = 1;
	regs[GU_GFW_BOOT_OFFSET / 4] = 0xff;
	regs[GU_GFW_CPUCTL_OFFSET / 4] = 0x10;
	regs[GU_FEAT_PLM_OFFSET / 4] = 0xffffff8fu;
	regs[GU_FEAT_SS0_OFFSET / 4] = 0x40171022u;
	regs[GU_FEAT_SS1_OFFSET / 4] = 0x2u;
	regs[0x1fa824 / 4] = 0xfffffff0u;
	regs[0x1fa828 / 4] = 0;

	/* Layout inputs captured before NVIDIA binds: 24 GiB, VGA at FB-1MiB,
	 * MMU lock below the FB top (so it is ignored), PLM open. */
	regs[0x1183a4 / 4] = 0x6000;
	regs[0x625f04 / 4] = 1;
	regs[0x1fa7c8 / 4] = 0x45cb8f;
	regs[0x1fa82c / 4] = 0x100000;
	regs[0x1fa830 / 4] = 0x5cffff0;

	/* Booter ucode 3 and FWSEC ucode 9 are at fuse version 1. */
	regs[(GU_FUSE_SEC2_UCODE1 + 2 * 4) / 4] = 1;
	regs[(0x008241c0 + 8 * 4) / 4] = 1;

	gu_asset_vbios((u8 *)regs + SIM_ROM_OFFSET, SIM_ROM_SIZE);
}

void gu_sim_init(void)
{
	if (!sim_lock_ready) {
		memset(&sim, 0, sizeof(sim));
		InitializeCriticalSection(&sim.cs);
		sim_lock_ready = 1;
	}
	sim_ready = 1;
	sim_cold_state();
}

void gu_sim_shutdown(void)
{
	sim_dma_reset();
	sim_ready = 0;
}

/* nvidia suspended the card; power was lost, then the board came back with
 * its cold selectors and WPR2 down. The driver still owns the card. */
void gu_sim_prepare_resume(void)
{
	sim_dma_reset();
	sim.cmd = PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	sim.saved = sim.cmd;
	sim.busmaster = 1;
	sim.region = 0;
	sim.flrs = 0;
	sim.gsp_starts = 0;
	sim.booter_starts = 0;
	sim.selector_writes = 0;
	sim.meta_error = 0;
	sim.pdev.driver = &nvidia_driver;
	sim.pdev.dev.devres_head.next = &sim.pdev.dev.devres_head;
	sim.pdev.dev.devres_head.prev = &sim.pdev.dev.devres_head;
	sim.pdev.dev.devres_head.owner = &sim.pdev.dev.devres_head;
	sim.pdev.dev.devres_head.count = 1;	/* nvidia's BAR region */

	regs[GU_GFW_PLM_OFFSET / 4] = 1;
	regs[GU_GFW_BOOT_OFFSET / 4] = 0xff;
	regs[GU_GFW_CPUCTL_OFFSET / 4] = 0x10;
	regs[GU_FEAT_PLM_OFFSET / 4] = 0xffffff8f;
	regs[GU_FEAT_SS0_OFFSET / 4] = 0x40171022u;
	regs[GU_FEAT_SS1_OFFSET / 4] = 0x2u;
	regs[0x1fa824 / 4] = 0xfffffff0u;
	regs[0x1fa828 / 4] = 0;
	regs[0x1400 + 0x0e * 4] = 0;
	regs[(SIM_SEC2_BASE + 0x3c0) / 4] = 0;
	regs[(SIM_GSP_BASE + 0x3c0) / 4] = 0;
	regs[(SIM_SEC2_BASE + 0x118) / 4] = 2;
	regs[(SIM_GSP_BASE + 0x118) / 4] = 2;
	regs[(SIM_SEC2_BASE + 0xf4) / 4] |= 1u << 31;
	regs[(SIM_GSP_BASE + 0xf4) / 4] |= 1u << 31;
}

int gu_sim_unlocked(void)
{
	return sim_ready &&
	       regs[GU_FEAT_SS0_OFFSET / 4] == 0x88888888u &&
	       regs[GU_FEAT_SS1_OFFSET / 4] == 0x8u &&
	       !(regs[0x1fa828 / 4] >> 4) &&
	       (regs[GU_GFW_CPUCTL_OFFSET / 4] & GU_GFW_CPUCTL_HALTED) &&
	       (regs[GU_GFW_PLM_OFFSET / 4] & GU_GFW_PLM_LEVEL0) &&
	       (regs[GU_GFW_BOOT_OFFSET / 4] & 0xff) == GU_GFW_BOOT_COMPLETED &&
	       !regs[(SIM_GSP_BASE + 0x3c0) / 4] &&
	       sim.flrs == 1 && sim.gsp_starts == 1 && sim.booter_starts == 1;
}

/* ------------------------------------------------------------- DMA map */

static struct sim_dma *sim_dma_find(dma_addr_t addr)
{
	size_t lo = 0, hi = sim.dma_count;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (sim.dma[mid].addr == addr)
			return sim.dma[mid].live ? &sim.dma[mid] : NULL;
		if (sim.dma[mid].addr < addr)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

static struct sim_dma *sim_dma_add(dma_addr_t addr, size_t size, void *cpu)
{
	if (sim.dma_count == sim.dma_cap) {
		size_t cap = sim.dma_cap ? sim.dma_cap * 2 : 64;
		struct sim_dma *grown = realloc(sim.dma, cap * sizeof(*grown));

		if (!grown)
			return NULL;
		sim.dma = grown;
		sim.dma_cap = cap;
	}
	sim.dma[sim.dma_count].addr = addr;
	sim.dma[sim.dma_count].size = size;
	sim.dma[sim.dma_count].cpu = cpu;
	sim.dma[sim.dma_count].live = 1;
	return &sim.dma[sim.dma_count++];
}

void *gu_sim_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
		       int flags)
{
	dma_addr_t addr;
	void *cpu;

	(void)flags;
	if (!sim_ready || !size || !dma)
		return NULL;
	addr = (sim.next_dma + 0xfffull) & ~0xfffull;
	if (addr + size < addr || addr + size - 1 > dev->coherent_dma_mask)
		return NULL;
	cpu = calloc(1, size);
	if (!cpu)
		return NULL;
	if (!sim_dma_add(addr, size, cpu)) {
		free(cpu);
		return NULL;
	}
	sim.next_dma = addr + size;
	*dma = addr;
	return cpu;
}

void gu_sim_dma_free(struct device *dev, size_t size, void *cpu, dma_addr_t dma)
{
	struct sim_dma *entry = sim_dma_find(dma);

	(void)dev;
	(void)size;
	if (!entry)
		return;
	entry->live = 0;
	free(cpu);
}

/* ---------------------------------------------------------- config */

int gu_sim_read_config_word(struct pci_dev *pdev, int where, u16 *value)
{
	(void)pdev;
	switch (where) {
	case PCI_COMMAND:
		*value = sim.cmd;
		break;
	case PCI_VENDOR_ID:
		*value = PCI_VENDOR_ID_NVIDIA;
		break;
	default:
		*value = 0;
		break;
	}
	return 0;
}

int gu_sim_read_config_dword(struct pci_dev *pdev, int where, u32 *value)
{
	(void)pdev;
	if (where == PCI_VENDOR_ID) {
		*value = (u32)sim.pdev.device << 16 | sim.pdev.vendor;
	} else if (where >= PCI_BASE_ADDRESS_0 &&
		   where < PCI_BASE_ADDRESS_0 + SIM_BARS * 4 &&
		   (where - PCI_BASE_ADDRESS_0) % 4 == 0) {
		*value = sim.bars[(where - PCI_BASE_ADDRESS_0) / 4];
	} else {
		*value = 0;
	}
	return 0;
}

int gu_sim_write_config_word(struct pci_dev *pdev, int where, u16 value)
{
	(void)pdev;
	if (where == PCI_COMMAND) {
		sim.cmd = value;
		sim.busmaster = !!(value & PCI_COMMAND_MASTER);
	}
	return 0;
}

int gu_sim_cap_read_word(struct pci_dev *pdev, int where, u16 *value)
{
	(void)pdev;
	*value = where == PCI_EXP_DEVSTA ? 0 : 0;
	return 0;
}

int gu_sim_cap_read_dword(struct pci_dev *pdev, int where, u32 *value)
{
	(void)pdev;
	*value = where == PCI_EXP_DEVCAP ? PCI_EXP_DEVCAP_FLR : 0;
	return 0;
}

int gu_sim_is_pcie(struct pci_dev *pdev)
{
	(void)pdev;
	return 1;
}

int gu_sim_select_bars(struct pci_dev *pdev, int type)
{
	(void)pdev;
	(void)type;
	return 0x7;
}

int gu_sim_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	(void)pdev;
	(void)name;
	if (bar)
		return -EINVAL;
	sim.region++;
	return 0;
}

void gu_sim_release_region(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	(void)bar;
	if (sim.region)
		sim.region--;
}

int gu_sim_enable_device_mem(struct pci_dev *pdev)
{
	(void)pdev;
	sim.cmd |= PCI_COMMAND_MEMORY;
	return 0;
}

void gu_sim_disable_device(struct pci_dev *pdev)
{
	(void)pdev;
}

resource_size_t gu_sim_resource_len(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	(void)bar;
	return SIM_BAR_SIZE;
}

u8 *gu_sim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	(void)pdev;
	(void)bar;
	(void)maxlen;
	return (u8 *)regs;
}

void gu_sim_iounmap(struct pci_dev *pdev, u8 *addr)
{
	(void)pdev;
	(void)addr;
}

int gu_sim_save_state(struct pci_dev *pdev)
{
	(void)pdev;
	sim.saved = sim.cmd;
	return 0;
}

void gu_sim_restore_state(struct pci_dev *pdev)
{
	(void)pdev;
	sim.cmd = sim.saved;
	sim.busmaster = !!(sim.cmd & PCI_COMMAND_MASTER);
}

void gu_sim_set_master(struct pci_dev *pdev)
{
	(void)pdev;
	sim.cmd |= PCI_COMMAND_MASTER;
	sim.busmaster = 1;
}

void gu_sim_clear_master(struct pci_dev *pdev)
{
	(void)pdev;
	sim.cmd &= (u16)~PCI_COMMAND_MASTER;
	sim.busmaster = 0;
}

int gu_sim_flr(struct pci_dev *pdev)
{
	(void)pdev;
	sim.flrs++;
	/* FLR disposes of the old GSP execution and returns the boot ROM to a
	 * completed, halted GFW. Selectors survive the reset. */
	regs[(SIM_SEC2_BASE + 0x3c0) / 4] = 0;
	regs[(SIM_GSP_BASE + 0x3c0) / 4] = 0;
	regs[GU_GFW_CPUCTL_OFFSET / 4] = 0x10;
	regs[GU_GFW_PLM_OFFSET / 4] = 1;
	regs[GU_GFW_BOOT_OFFSET / 4] = 0xff;
	regs[0x1fa824 / 4] = 0;
	regs[0x1fa828 / 4] = 0;
	return 0;
}

/* ------------------------------------------------------------- MMIO */

u32 gu_sim_readl(const void *addr)
{
	uintptr_t off = (uintptr_t)addr - (uintptr_t)regs;

	if (!sim_ready || off + 4 > sizeof(regs))
		return 0xffffffffu;
	return regs[off / 4];
}

/* The Booter is only allowed to start after the V67 metadata was published:
 * its mailbox carries the DMA address of the 256-byte metadata block. */
static int sim_validate_meta(void)
{
	u64 addr = ((u64)regs[(SIM_SEC2_BASE + 0x44) / 4] << 32) |
		   regs[(SIM_SEC2_BASE + 0x40) / 4];
	struct sim_dma *meta = sim_dma_find(addr);
	const u8 *p;
	u64 radix, image, boot, boot_size, signature, signature_size;

	if (!meta || meta->size < 256)
		return 0;
	p = meta->cpu;
	if (get_le64(p) != GU_WPR_META_MAGIC || get_le64(p + 8) != 1)
		return 0;
	radix = get_le64(p + 16);
	image = get_le64(p + 24);
	boot = get_le64(p + 32);
	boot_size = get_le64(p + 40);
	signature = get_le64(p + 72);
	signature_size = get_le64(p + 80);
	if (!image || !boot_size || signature_size != GU_V67_SIZE)
		return 0;
	if (!sim_dma_find(radix) || !sim_dma_find(boot) ||
	    !sim_dma_find(signature))
		return 0;
	return 1;
}

void gu_sim_writel(u32 value, void *addr)
{
	uintptr_t off = (uintptr_t)addr - (uintptr_t)regs;

	if (!sim_ready || off + 4 > sizeof(regs))
		return;
	regs[off / 4] = value;

	if (off == GU_FEAT_SS0_OFFSET)
		sim.selector_writes++;
	if (off == GU_FEAT_SS1_OFFSET)
		regs[off / 4] = value & 0xfu;	/* 4-bit register */
	if (off == SIM_SEC2_BASE + 0x118 || off == SIM_GSP_BASE + 0x118)
		regs[off / 4] = 2;		/* DMA completes immediately */
	if (off == SIM_SEC2_FALCON2 + 0x668 || off == SIM_GSP_FALCON2 + 0x668)
		regs[off / 4] = 1;		/* RISC-V reset switch settled */

	/* FWSEC-FRTS ran to completion: it enabled WPR2 for the FRTS region and
	 * left the GSP Falcon halted for the Booter. */
	if (off == SIM_GSP_BASE + 0x100 || off == SIM_GSP_BASE + 0x130) {
		sim.gsp_starts++;
		regs[0x1fa824 / 4] = (u32)(SIM_FRTS_OFFSET >> 12) << 4;
		regs[0x1fa828 / 4] = (u32)((SIM_WPR_END - 4096) >> 12) << 4;
		regs[GU_GFW_CPUCTL_OFFSET / 4] = 0x10;
	}
	/* SEC2 Booter start: it may only run after the V67 image is published. */
	if (off == SIM_SEC2_BASE + 0x100 || off == SIM_SEC2_BASE + 0x130) {
		sim.booter_starts++;
		if (sim_validate_meta())
			regs[GU_FEAT_PLM_OFFSET / 4] = ~0u;
		else
			sim.meta_error = 1;
	}
}

/* ------------------------------------------------------- enumeration */

struct pci_dev *gu_sim_pci_get_device(unsigned int vendor, unsigned int device,
				      struct pci_dev *from)
{
	pci_dev_put(from);
	if (!sim_ready || from)
		return NULL;
	if (vendor != PCI_VENDOR_ID_NVIDIA)
		return NULL;
	if (device != PCI_ANY_ID && device != sim.pdev.device)
		return NULL;
	return pci_dev_get(&sim.pdev);
}
