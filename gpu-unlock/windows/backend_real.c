/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Real-hardware backend for the Windows build.
 *
 * Hardware access is forwarded to gpuunlock.sys. Firmware buffers are mirrored
 * in user memory and flushed to the driver's physically contiguous allocations
 * at the barrier points the kernel sources already use (dma_wmb()).
 *
 * This backend is used only by the `real` command. The simulated card remains
 * the default so the executable still runs without a signed driver.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include "hal.h"
#include "gfw_eval.h"
#include "driver/gpuunlock_ioctl.h"

#define GU_REAL_BAR_LENGTH (16u * 1024u * 1024u)
#define GU_REAL_BAR_TOKEN ((uintptr_t)0x0000000100000000ull)
/* Matches GU_MAX_DMA_SLOTS in gpuunlock.sys: the GSP radix image is allocated
 * one 4 KiB page at a time. */
#define GU_REAL_MAX_DMA 33000

struct gu_real_dma {
	void *cpu;
	size_t size;
	gu_u32 handle;
	gu_u64 phys;
	int used;
};

struct gu_real_state {
	HANDLE device;
	int open;
	gu_u32 bus, device_id, function;
	gu_u32 pcie_cap, pm_cap;
	int bar_mapped;
	u8 saved[256];
	int saved_valid;
	u64 dma_mask;
	u64 coherent_mask;
	struct pci_dev pdev;
	struct gu_real_dma dma[GU_REAL_MAX_DMA];
	CRITICAL_SECTION lock;
};

static struct gu_real_state real;
static unsigned int gu_real_dma_hint;
static DWORD gu_real_last_error;

static int gu_real_ioctl(DWORD code, void *input, DWORD input_length,
			 void *output, DWORD output_length)
{
	DWORD returned = 0;

	if (!real.open || !real.device)
		return -ENODEV;
	if (!DeviceIoControl(real.device, code, input, input_length,
			     output, output_length, &returned, NULL)) {
		gu_real_last_error = GetLastError();
		return -EIO;
	}
	return 0;
}

static int gu_real_cfg_read(u32 where, u32 size, u32 *value)
{
	struct gu_config_req req;
	int ret;

	req.bus = real.bus;
	req.device = real.device_id;
	req.function = real.function;
	req.offset = where;
	req.size = size;
	req.value = 0;
	ret = gu_real_ioctl(GU_IOCTL_CONFIG_READ, &req, sizeof(req), &req,
			    sizeof(req));
	if (ret)
		return ret;
	*value = req.value;
	return 0;
}

static int gu_real_cfg_write(u32 where, u32 size, u32 value)
{
	struct gu_config_req req;

	req.bus = real.bus;
	req.device = real.device_id;
	req.function = real.function;
	req.offset = where;
	req.size = size;
	req.value = value;
	return gu_real_ioctl(GU_IOCTL_CONFIG_WRITE, &req, sizeof(req), NULL, 0);
}

static gu_u32 gu_real_find_cap(u8 id)
{
	u32 status = 0, ptr = 0;
	unsigned int i;

	if (gu_real_cfg_read(0x06, 2, &status) || !(status & 0x10))
		return 0;
	if (gu_real_cfg_read(0x34, 1, &ptr))
		return 0;
	ptr &= 0xfc;
	for (i = 0; i < 48 && ptr >= 0x40 && ptr + 4 <= 0xfc; i++) {
		u32 header = 0;

		if (gu_real_cfg_read(ptr, 4, &header))
			return 0;
		if ((header & 0xff) == id)
			return ptr;
		ptr = (header >> 8) & 0xfc;
	}
	return 0;
}

int gu_real_open(const char *bdf)
{
	unsigned int domain = 0, bus = 0, dev = 0, func = 0;
	u32 id = 0, pmcsr = 0;
	struct gu_bdf_req power;
	int ret;

	if (!bdf ||
	    sscanf(bdf, "%x:%x:%x.%x", &domain, &bus, &dev, &func) != 4)
		return -EINVAL;
	if (domain != 0)
		return -EINVAL;
	if (bus > 0xff || dev > 0x1f || func > 0x7)
		return -EINVAL;

	memset(&real, 0, sizeof(real));
	real.device = CreateFileW(L"\\\\.\\GpuUnlock", GENERIC_READ | GENERIC_WRITE,
				  0, NULL, OPEN_EXISTING, 0, NULL);
	if (real.device == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "  CreateFile \\\\.\\GpuUnlock failed (Win32 %lu)\n",
			(unsigned long)GetLastError());
		real.device = NULL;
		return -ENODEV;
	}
	real.open = 1;
	real.bus = bus;
	real.device_id = dev;
	real.function = func;

	power.bus = bus;
	power.device = dev;
	power.function = func;
	power.status = 0;
	ret = gu_real_ioctl(GU_IOCTL_POWER_ON, &power, sizeof(power), &power,
			    sizeof(power));
	if (ret) {
		if (gu_real_last_error == ERROR_NOT_SUPPORTED) {
			/* The function did not expose a readable PM capability; the
			 * Windows PnP stack already keeps it in D0, so continue and let
			 * the config-space probe report what is actually reachable. */
			fprintf(stderr, "  POWER_ON: PM capability not found\n");
		} else {
			fprintf(stderr, "  POWER_ON ioctl failed (Win32 %lu, NTSTATUS %08lx)\n",
				(unsigned long)gu_real_last_error,
				(unsigned long)power.status);
			goto fail;
		}
	}

	if (gu_real_cfg_read(0, 4, &id) || id == 0xffffffffu) {
		static const u32 offsets[] = { 0x00, 0x04, 0x06, 0x34, 0x3c };
		u32 sample = 0, saved_bus = real.bus;
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(offsets); i++) {
			gu_real_cfg_read(offsets[i], 4, &sample);
			fprintf(stderr, "  cfg[%02x]=%08x\n", offsets[i], sample);
		}
		real.bus = 0;
		gu_real_cfg_read(0, 4, &sample);
		fprintf(stderr, "  bus0:00.0 cfg00=%08x\n", sample);
		for (i = 0; i < 256; i++) {
			real.bus = i;
			if (!gu_real_cfg_read(0, 4, &sample) &&
			    sample != 0xffffffffu)
				fprintf(stderr, "  scan %02x:00.0 = %08x\n", i,
					sample);
		}
		real.bus = saved_bus;
		fprintf(stderr, "  config read at 0 failed: id=%08x (Win32 %lu)\n",
			id, (unsigned long)gu_real_last_error);
		ret = -ENODEV;
		goto fail;
	}
	real.pcie_cap = gu_real_find_cap(0x10);
	real.pm_cap = gu_real_find_cap(0x01);
	fprintf(stderr, "  id=%08x pcie_cap=%02x pm_cap=%02x\n", id,
		real.pcie_cap, real.pm_cap);
	real.dma_mask = 0xffffffffull;
	real.coherent_mask = 0xffffffffull;

	InitializeCriticalSection(&real.lock);
	real.pdev.vendor = (unsigned short)(id & 0xffff);
	real.pdev.device = (unsigned short)(id >> 16);
	real.pdev.driver = NULL;
	real.pdev.current_state = PCI_D0;
	real.pdev.pm_cap = (int)real.pm_cap;
	real.pdev.dev_flags = 0;
	real.pdev.bar_len = GU_REAL_BAR_LENGTH;
	real.pdev.refs = 1;
	real.pdev.wdev = &real;
	real.pdev.lock = &real.lock;
	real.pdev.lock_init = 1;
	snprintf(real.pdev.name, sizeof(real.pdev.name), "%04x:%02x:%02x.%u",
		 bus, dev, func, func);
	real.pdev.name[sizeof(real.pdev.name) - 1] = '\0';
	real.pdev.dev.dma_mask = &real.dma_mask;
	real.pdev.dev.coherent_dma_mask = real.coherent_mask;
	real.pdev.dev.devres_head.next = &real.pdev.dev.devres_head;
	real.pdev.dev.devres_head.prev = &real.pdev.dev.devres_head;
	real.pdev.dev.devres_head.owner = &real.pdev.dev.devres_head;
	real.pdev.dev.devres_head.count = 0;

	/* Confirm D0; the caller runs after the display driver is disabled. */
	if (real.pm_cap) {
		if (gu_real_cfg_read(real.pm_cap + PCI_PM_CTRL, 2, &pmcsr) ||
		    (pmcsr & PCI_PM_CTRL_STATE_MASK) != PCI_D0) {
			fprintf(stderr, "  PMCSR=%04x at cap+4=%02x (Win32 %lu)\n",
				pmcsr, real.pm_cap,
				(unsigned long)gu_real_last_error);
			ret = -EOPNOTSUPP;
			goto fail;
		}
	}
	return 0;

fail:
	gu_real_close();
	return ret;
}

void gu_real_close(void)
{
	unsigned int i;

	if (real.open) {
		for (i = 0; i < GU_REAL_MAX_DMA; i++) {
			if (real.dma[i].used) {
				struct gu_dma_copy_req req;

				req.handle = real.dma[i].handle;
				req.offset = 0;
				req.size = 0;
				req.status = 0;
				gu_real_ioctl(GU_IOCTL_DMA_FREE, &req, sizeof(req),
					      NULL, 0);
				free(real.dma[i].cpu);
				real.dma[i].used = 0;
			}
		}
		if (real.bar_mapped) {
			gu_real_ioctl(GU_IOCTL_UNMAP_BAR, NULL, 0, NULL, 0);
			real.bar_mapped = 0;
		}
		CloseHandle(real.device);
	}
	if (real.pdev.lock_init) {
		DeleteCriticalSection((CRITICAL_SECTION *)real.pdev.lock);
		real.pdev.lock_init = 0;
	}
	if (real.device)
		memset(&real, 0, sizeof(real));
}

int gu_real_is_open(void)
{
	return real.open;
}

struct pci_dev *gu_real_pci_get_device(unsigned int vendor,
				       unsigned int device,
				       struct pci_dev *from)
{
	if (!real.open || from)
		return NULL;
	if (vendor != PCI_VENDOR_ID_NVIDIA)
		return NULL;
	if (device != PCI_ANY_ID && (unsigned short)device != real.pdev.device)
		return NULL;
	return &real.pdev;
}

int gu_real_read_config_word(struct pci_dev *pdev, int where, u16 *value)
{
	u32 word = 0;
	int ret;

	(void)pdev;
	ret = gu_real_cfg_read((u32)where, 2, &word);
	if (!ret)
		*value = (u16)word;
	return ret;
}

int gu_real_read_config_dword(struct pci_dev *pdev, int where, u32 *value)
{
	(void)pdev;
	return gu_real_cfg_read((u32)where, 4, value);
}

int gu_real_write_config_word(struct pci_dev *pdev, int where, u16 value)
{
	(void)pdev;
	return gu_real_cfg_write((u32)where, 2, value);
}

int gu_real_cap_read_word(struct pci_dev *pdev, int where, u16 *value)
{
	u32 word = 0;
	int ret;

	(void)pdev;
	if (!real.pcie_cap)
		return -EOPNOTSUPP;
	ret = gu_real_cfg_read(real.pcie_cap + (u32)where, 2, &word);
	if (!ret)
		*value = (u16)word;
	return ret;
}

int gu_real_cap_read_dword(struct pci_dev *pdev, int where, u32 *value)
{
	(void)pdev;
	if (!real.pcie_cap)
		return -EOPNOTSUPP;
	return gu_real_cfg_read(real.pcie_cap + (u32)where, 4, value);
}

int gu_real_is_pcie(struct pci_dev *pdev)
{
	(void)pdev;
	return real.pcie_cap != 0;
}

int gu_real_select_bars(struct pci_dev *pdev, int type)
{
	u32 bar = 0;
	int mask = 0;
	unsigned int i;

	(void)pdev;
	for (i = 0; i < 6; i++) {
		if (gu_real_cfg_read(PCI_BASE_ADDRESS_0 + i * 4, 4, &bar))
			continue;
		if (bar && bar != 0xffffffffu && !(bar & 1))
			mask |= 1 << i;
	}
	(void)type;
	return mask;
}

int gu_real_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	(void)pdev;
	(void)name;
	return bar == 0 && real.open ? 0 : -EINVAL;
}

void gu_real_release_region(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	(void)bar;
}

int gu_real_enable_device_mem(struct pci_dev *pdev)
{
	u16 command = 0;
	int ret;

	(void)pdev;
	ret = gu_real_read_config_word(NULL, PCI_COMMAND, &command);
	if (ret)
		return ret;
	if (!(command & PCI_COMMAND_MEMORY)) {
		command |= PCI_COMMAND_MEMORY;
		ret = gu_real_write_config_word(NULL, PCI_COMMAND, command);
		if (ret)
			return ret;
	}
	ret = gu_real_read_config_word(NULL, PCI_COMMAND, &command);
	if (ret || command == 0xffff || !(command & PCI_COMMAND_MEMORY))
		return -EIO;
	return 0;
}

void gu_real_disable_device(struct pci_dev *pdev)
{
	u16 command = 0;

	(void)pdev;
	if (!gu_real_read_config_word(NULL, PCI_COMMAND, &command) &&
	    (command & PCI_COMMAND_MEMORY))
		gu_real_write_config_word(NULL, PCI_COMMAND,
					  command & ~PCI_COMMAND_MEMORY);
}

resource_size_t gu_real_resource_len(struct pci_dev *pdev, int bar)
{
	(void)pdev;
	return bar == 0 ? GU_REAL_BAR_LENGTH : 0;
}

u8 *gu_real_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	struct gu_bar_map_req req;

	(void)pdev;
	(void)maxlen;
	req.bus = real.bus;
	req.device = real.device_id;
	req.function = real.function;
	req.bar = (gu_u32)bar;
	req.length = GU_REAL_BAR_LENGTH;
	req.status = 0;
	req.phys = 0;
	if (gu_real_ioctl(GU_IOCTL_MAP_BAR, &req, sizeof(req), &req,
			  sizeof(req)))
		return NULL;
	real.bar_mapped = 1;
	return (u8 *)(uintptr_t)GU_REAL_BAR_TOKEN;
}

void gu_real_iounmap(struct pci_dev *pdev, u8 *addr)
{
	(void)pdev;
	(void)addr;
	if (real.bar_mapped) {
		gu_real_ioctl(GU_IOCTL_UNMAP_BAR, NULL, 0, NULL, 0);
		real.bar_mapped = 0;
	}
}

int gu_real_save_state(struct pci_dev *pdev)
{
	unsigned int i;

	(void)pdev;
	for (i = 0; i < sizeof(real.saved) / 4; i++) {
		u32 value = 0;

		if (gu_real_cfg_read(i * 4, 4, &value))
			return -EIO;
		memcpy(real.saved + i * 4, &value, 4);
	}
	real.saved_valid = 1;
	return 0;
}

void gu_real_restore_state(struct pci_dev *pdev)
{
	unsigned int i;

	(void)pdev;
	if (!real.saved_valid)
		return;
	/* Only the writable identity of the function is restored: command,
	 * BARs and the ROM BAR. Capability pointers are read-only. */
	gu_real_cfg_write(PCI_COMMAND, 2,
			  real.saved[PCI_COMMAND] |
			  ((u16)real.saved[PCI_COMMAND + 1] << 8));
	for (i = 0; i < 6; i++) {
		u32 value;

		memcpy(&value, real.saved + PCI_BASE_ADDRESS_0 + i * 4, 4);
		if (value && value != 0xffffffffu)
			gu_real_cfg_write(PCI_BASE_ADDRESS_0 + i * 4, 4, value);
	}
}

void gu_real_set_master(struct pci_dev *pdev)
{
	u16 command = 0;

	(void)pdev;
	if (gu_real_read_config_word(NULL, PCI_COMMAND, &command))
		return;
	if (!(command & PCI_COMMAND_MASTER))
		gu_real_write_config_word(NULL, PCI_COMMAND,
					  command | PCI_COMMAND_MASTER);
}

void gu_real_clear_master(struct pci_dev *pdev)
{
	u16 command = 0;

	(void)pdev;
	if (gu_real_read_config_word(NULL, PCI_COMMAND, &command))
		return;
	if (command & PCI_COMMAND_MASTER)
		gu_real_write_config_word(NULL, PCI_COMMAND,
					  command & ~PCI_COMMAND_MASTER);
}

int gu_real_flr(struct pci_dev *pdev)
{
	struct gu_bdf_req req;

	(void)pdev;
	req.bus = real.bus;
	req.device = real.device_id;
	req.function = real.function;
	req.status = 0;
	if (gu_real_ioctl(GU_IOCTL_FLR, &req, sizeof(req), &req, sizeof(req)))
		return -EIO;
	return 0;
}

/* Clear an already-initialised GSP (WPR2 up) before running the cold path:
 * save the writable identity, reset the function, then put the BARs and memory
 * decoding back so the mapping made by gu_real_open stays valid. */
int gu_real_pre_flr(void)
{
	u16 command = 0;
	u32 bars[6];
	unsigned int i;

	if (!real.open || !real.pcie_cap)
		return -EOPNOTSUPP;
	if (gu_real_enable_device_mem(NULL))
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(bars); i++)
		if (gu_real_cfg_read(PCI_BASE_ADDRESS_0 + i * 4, 4, &bars[i]))
			return -EIO;
	if (gu_real_flr(NULL))
		return -EIO;
	Sleep(100);
	if (gu_real_read_config_word(NULL, PCI_COMMAND, &command) ||
	    command == 0xffff)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(bars); i++)
		if (bars[i] && bars[i] != 0xffffffffu)
			gu_real_cfg_write(PCI_BASE_ADDRESS_0 + i * 4, 4, bars[i]);
	if (gu_real_enable_device_mem(NULL))
		return -EIO;
	Sleep(100);
	if (gu_real_read_config_word(NULL, PCI_COMMAND, &command) ||
	    command == 0xffff || !(command & PCI_COMMAND_MEMORY))
		return -EIO;
	return 0;
}

/* ---------------------------------------------------------------- dma */

void *gu_real_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
			int flags)
{
	struct gu_dma_alloc_req req;
	struct gu_real_dma *slot = NULL;
	void *mirror;
	unsigned int i;

	(void)dev;
	(void)flags;
	if (!real.open || !size || size > 16u * 1024u * 1024u)
		return NULL;
	req.size = size;
	req.phys = 0;
	req.handle = 0;
	req.status = 0;
	if (gu_real_ioctl(GU_IOCTL_DMA_ALLOC, &req, sizeof(req), &req,
			  sizeof(req)))
		return NULL;
	mirror = calloc(1, size);
	if (!mirror) {
		struct gu_dma_copy_req free_req;

		free_req.handle = req.handle;
		free_req.offset = 0;
		free_req.size = 0;
		free_req.status = 0;
		gu_real_ioctl(GU_IOCTL_DMA_FREE, &free_req, sizeof(free_req),
			      NULL, 0);
		return NULL;
	}
	EnterCriticalSection(&real.lock);
	for (i = gu_real_dma_hint; i < GU_REAL_MAX_DMA; i++)
		if (!real.dma[i].used) {
			slot = &real.dma[i];
			break;
		}
	if (!slot)
		for (i = 0; i < gu_real_dma_hint; i++)
			if (!real.dma[i].used) {
				slot = &real.dma[i];
				break;
			}
	if (slot) {
		gu_real_dma_hint = (unsigned int)(slot - real.dma + 1) % GU_REAL_MAX_DMA;
		slot->used = 1;
		slot->cpu = mirror;
		slot->size = size;
		slot->handle = req.handle;
		slot->phys = req.phys;
	}
	LeaveCriticalSection(&real.lock);
	if (!slot) {
		free(mirror);
		return NULL;
	}
	*dma = (dma_addr_t)req.phys;
	return mirror;
}

void gu_real_dma_free(struct device *dev, size_t size, void *cpu,
		      dma_addr_t dma)
{
	struct gu_dma_copy_req req;
	unsigned int i;

	(void)dev;
	(void)size;
	(void)dma;
	EnterCriticalSection(&real.lock);
	for (i = 0; i < GU_REAL_MAX_DMA; i++) {
		if (real.dma[i].used && real.dma[i].cpu == cpu) {
			req.handle = real.dma[i].handle;
			req.offset = 0;
			req.size = 0;
			req.status = 0;
			gu_real_ioctl(GU_IOCTL_DMA_FREE, &req, sizeof(req),
				      NULL, 0);
			free(real.dma[i].cpu);
			real.dma[i].used = 0;
			break;
		}
	}
	LeaveCriticalSection(&real.lock);
}

/* Copy every mirror to the driver's contiguous allocation. The kernel sources
 * call dma_wmb() immediately before a Falcon DMA starts, which is the only
 * point where device-visible contents must be current. */
void gu_real_dma_flush(void)
{
	unsigned int i;

	if (!real.open)
		return;
	EnterCriticalSection(&real.lock);
	for (i = 0; i < GU_REAL_MAX_DMA; i++) {
		struct gu_dma_copy_req *req;
		size_t total;

		if (!real.dma[i].used)
			continue;
		total = sizeof(*req) + real.dma[i].size;
		req = malloc(total);
		if (!req)
			continue;
		req->handle = real.dma[i].handle;
		req->offset = 0;
		req->size = (gu_u32)real.dma[i].size;
		req->status = 0;
		memcpy((u8 *)req + sizeof(*req), real.dma[i].cpu,
		       real.dma[i].size);
		gu_real_ioctl(GU_IOCTL_DMA_WRITE, req, (DWORD)total, NULL, 0);
		free(req);
	}
	LeaveCriticalSection(&real.lock);
}

/* --------------------------------------------------------------- mmio */

u32 gu_real_readl(const void *addr)
{
	struct gu_mmio_req req;
	u64 offset = (u64)(uintptr_t)addr - GU_REAL_BAR_TOKEN;

	if (!real.open || (uintptr_t)addr < GU_REAL_BAR_TOKEN ||
	    offset + 4 > GU_REAL_BAR_LENGTH)
		return 0xffffffffu;
	req.offset = (gu_u32)offset;
	req.value = 0;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req,
			  sizeof(req)))
		return 0xffffffffu;
	return req.value;
}

void gu_real_writel(u32 value, void *addr)
{
	struct gu_mmio_req req;
	u64 offset = (u64)(uintptr_t)addr - GU_REAL_BAR_TOKEN;

	if (!real.open || (uintptr_t)addr < GU_REAL_BAR_TOKEN ||
	    offset + 4 > GU_REAL_BAR_LENGTH)
		return;
	req.offset = (gu_u32)offset;
	req.value = value;
	gu_real_ioctl(GU_IOCTL_MMIO_WRITE, &req, sizeof(req), NULL, 0);
}

/* Read-back check equivalent to gu_sim_unlocked(), straight from the card. */
int gu_real_unlocked(void)
{
	struct gu_mmio_req req;
	u32 ss0 = 0, ss1 = 0, wpr_hi = 0, cpuctl = 0, plm = 0, boot = 0;
	u32 id = 0;

	if (!real.open)
		return 0;
	if (!real.bar_mapped && !gu_real_iomap(NULL, 0, 0))
		return 0;
	req.offset = GU_FEAT_SS0_OFFSET;
	req.value = 0;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	ss0 = req.value;
	req.offset = GU_FEAT_SS1_OFFSET;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	ss1 = req.value;
	req.offset = 0x001fa828;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	wpr_hi = req.value;
	req.offset = GU_GFW_CPUCTL_OFFSET;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	cpuctl = req.value;
	req.offset = GU_GFW_PLM_OFFSET;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	plm = req.value;
	req.offset = GU_GFW_BOOT_OFFSET;
	if (gu_real_ioctl(GU_IOCTL_MMIO_READ, &req, sizeof(req), &req, sizeof(req)))
		return 0;
	boot = req.value;
	if (gu_real_cfg_read(0, 4, &id) || id == 0xffffffffu)
		return 0;
	if (id != ((u32)real.pdev.device << 16 | real.pdev.vendor))
		return 0;
	return ss0 == 0x88888888u && ss1 == 0x8u && !(wpr_hi >> 4) &&
	       (cpuctl & GU_GFW_CPUCTL_HALTED) &&
	       (plm & GU_GFW_PLM_LEVEL0) &&
	       (boot & 0xffu) == GU_GFW_BOOT_COMPLETED;
}

u32 gu_real_vendor(void)
{
	return real.pdev.vendor;
}

u32 gu_real_device(void)
{
	return real.pdev.device;
}
