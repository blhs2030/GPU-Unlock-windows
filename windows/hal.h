/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows user-mode HAL: Linux-kernel-shaped PCI/DMA/firmware/time services.
 *
 * hal.c implements the API declared in compat.h and forwards hardware access
 * to the simulated GA104 backend (backend_sim.c) or, for enumeration only,
 * to the real SetupAPI backend (backend_win32.c).
 */
#ifndef GPU_UNLOCK_WINDOWS_HAL_H
#define GPU_UNLOCK_WINDOWS_HAL_H

#include "compat.h"

void gu_hal_init(void);
void gu_hal_set_quiet(int quiet);
/* Switch the hardware API surface between the simulated card (0, default) and
 * the gpuunlock.sys-backed real card (1). */
void gu_hal_set_real(int real);
int gu_hal_real_mode(void);

/* ------------------------------------------------------------ assets */
/* Directory holding real gpu-unlock/610.43.03 assets; NULL means built-in
 * synthetic assets. Lookup accepts the kernel's full firmware names and
 * matches on the final path component. */
int gu_assets_set_dir(const char *dir);
int gu_asset_lookup(const char *name, const u8 **data, size_t *size);
void gu_assets_free(void);
/* One MiB board ROM image for the simulated card, laid out like a real dump. */
void gu_asset_vbios(u8 *rom, size_t size);

/* ------------------------------------------------------ simulated card */
void gu_sim_init(void);
void gu_sim_shutdown(void);
void gu_sim_prepare_resume(void);
int gu_sim_unlocked(void);

struct pci_dev *gu_sim_pci_get_device(unsigned int vendor, unsigned int device,
				      struct pci_dev *from);
int gu_sim_read_config_word(struct pci_dev *pdev, int where, u16 *value);
int gu_sim_read_config_dword(struct pci_dev *pdev, int where, u32 *value);
int gu_sim_write_config_word(struct pci_dev *pdev, int where, u16 value);
int gu_sim_cap_read_word(struct pci_dev *pdev, int where, u16 *value);
int gu_sim_cap_read_dword(struct pci_dev *pdev, int where, u32 *value);
int gu_sim_is_pcie(struct pci_dev *pdev);
int gu_sim_select_bars(struct pci_dev *pdev, int type);
int gu_sim_request_region(struct pci_dev *pdev, int bar, const char *name);
void gu_sim_release_region(struct pci_dev *pdev, int bar);
int gu_sim_enable_device_mem(struct pci_dev *pdev);
void gu_sim_disable_device(struct pci_dev *pdev);
resource_size_t gu_sim_resource_len(struct pci_dev *pdev, int bar);
u8 *gu_sim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void gu_sim_iounmap(struct pci_dev *pdev, u8 *addr);
int gu_sim_save_state(struct pci_dev *pdev);
void gu_sim_restore_state(struct pci_dev *pdev);
void gu_sim_set_master(struct pci_dev *pdev);
void gu_sim_clear_master(struct pci_dev *pdev);
int gu_sim_flr(struct pci_dev *pdev);
void *gu_sim_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
		       int flags);
void gu_sim_dma_free(struct device *dev, size_t size, void *cpu, dma_addr_t dma);
u32 gu_sim_readl(const void *addr);
void gu_sim_writel(u32 value, void *addr);

/* ------------------------------------------------------ real hardware */
/* backend_real.c forwards every call to gpuunlock.sys. */
int gu_real_open(const char *bdf);
void gu_real_close(void);
int gu_real_is_open(void);
int gu_real_unlocked(void);
u32 gu_real_vendor(void);
u32 gu_real_device(void);
void gu_real_dma_flush(void);

struct pci_dev *gu_real_pci_get_device(unsigned int vendor, unsigned int device,
				       struct pci_dev *from);
int gu_real_read_config_word(struct pci_dev *pdev, int where, u16 *value);
int gu_real_read_config_dword(struct pci_dev *pdev, int where, u32 *value);
int gu_real_write_config_word(struct pci_dev *pdev, int where, u16 value);
int gu_real_cap_read_word(struct pci_dev *pdev, int where, u16 *value);
int gu_real_cap_read_dword(struct pci_dev *pdev, int where, u32 *value);
int gu_real_is_pcie(struct pci_dev *pdev);
int gu_real_select_bars(struct pci_dev *pdev, int type);
int gu_real_request_region(struct pci_dev *pdev, int bar, const char *name);
void gu_real_release_region(struct pci_dev *pdev, int bar);
int gu_real_enable_device_mem(struct pci_dev *pdev);
void gu_real_disable_device(struct pci_dev *pdev);
resource_size_t gu_real_resource_len(struct pci_dev *pdev, int bar);
u8 *gu_real_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void gu_real_iounmap(struct pci_dev *pdev, u8 *addr);
int gu_real_save_state(struct pci_dev *pdev);
void gu_real_restore_state(struct pci_dev *pdev);
void gu_real_set_master(struct pci_dev *pdev);
void gu_real_clear_master(struct pci_dev *pdev);
int gu_real_flr(struct pci_dev *pdev);
int gu_real_pre_flr(void);
void *gu_real_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
			int flags);
void gu_real_dma_free(struct device *dev, size_t size, void *cpu, dma_addr_t dma);
u32 gu_real_readl(const void *addr);
void gu_real_writel(u32 value, void *addr);

/* --------------------------------------------------- real enumeration */
struct gu_gpu_info {
	char bdf[24];
	char name[128];
	char service[64];
	unsigned short vendor, device;
};

int gu_win32_enum_gpus(struct gu_gpu_info *out, int max);
/* DICS_DISABLE unloads nvlddmkm; DICS_ENABLE lets PnP bind it again.
 * Return 0 on success, -1 when the function cannot be found or changed. */
int gu_win32_disable_device(const char *bdf);
int gu_win32_enable_device(const char *bdf);

#endif
