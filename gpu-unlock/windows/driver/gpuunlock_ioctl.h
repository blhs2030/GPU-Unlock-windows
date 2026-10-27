/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared IOCTL interface between gpuunlock.sys and the user-mode build.
 *
 * The driver exposes only the mechanical operations a Windows user-mode
 * process cannot do itself: PCI configuration space access, BAR mapping,
 * physically contiguous DMA memory, PCIe FLR and a D0 transition. The unlock
 * policy stays in the user-mode sources.
 */
#ifndef GPUUNLOCK_IOCTL_H
#define GPUUNLOCK_IOCTL_H

#ifdef _KERNEL_MODE
#include <wdm.h>
typedef UINT32 gu_u32;
typedef UINT64 gu_u64;
#else
#include <stdint.h>
typedef uint32_t gu_u32;
typedef uint64_t gu_u64;
#endif

#define GU_DRIVER_VERSION 1

struct gu_config_req {
	gu_u32 bus;
	gu_u32 device;
	gu_u32 function;
	gu_u32 offset;
	gu_u32 size;	/* 1, 2 or 4 */
	gu_u32 value;
};

struct gu_bar_map_req {
	gu_u32 bus;
	gu_u32 device;
	gu_u32 function;
	gu_u32 bar;
	gu_u32 length;
	gu_u32 status;	/* NTSTATUS subset */
	gu_u64 phys;
};

struct gu_mmio_req {
	gu_u32 offset;
	gu_u32 value;
};

struct gu_dma_alloc_req {
	gu_u64 size;
	gu_u64 phys;
	gu_u32 handle;
	gu_u32 status;
};

struct gu_dma_copy_req {
	gu_u32 handle;
	gu_u32 offset;
	gu_u32 size;
	gu_u32 status;
};

struct gu_bdf_req {
	gu_u32 bus;
	gu_u32 device;
	gu_u32 function;
	gu_u32 status;
};

struct gu_info_req {
	gu_u32 version;
	gu_u32 vendor;
	gu_u32 device;
	gu_u32 bar_length;
	gu_u32 bar_mapped;
	gu_u32 dma_allocations;
	gu_u32 pcie_cap;
	gu_u32 pm_cap;
};

#define GU_DEVICE_TYPE 0x8330

#define GU_IOCTL_VERSION \
	CTL_CODE(GU_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_CONFIG_READ \
	CTL_CODE(GU_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_CONFIG_WRITE \
	CTL_CODE(GU_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_MAP_BAR \
	CTL_CODE(GU_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_UNMAP_BAR \
	CTL_CODE(GU_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_MMIO_READ \
	CTL_CODE(GU_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_MMIO_WRITE \
	CTL_CODE(GU_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_DMA_ALLOC \
	CTL_CODE(GU_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_DMA_FREE \
	CTL_CODE(GU_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_DMA_WRITE \
	CTL_CODE(GU_DEVICE_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_DMA_READ \
	CTL_CODE(GU_DEVICE_TYPE, 0x80a, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_FLR \
	CTL_CODE(GU_DEVICE_TYPE, 0x80b, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_POWER_ON \
	CTL_CODE(GU_DEVICE_TYPE, 0x80c, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GU_IOCTL_INFO \
	CTL_CODE(GU_DEVICE_TYPE, 0x80d, METHOD_BUFFERED, FILE_ANY_ACCESS)

#endif
