/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gpuunlock.sys - minimal kernel helper for the Windows gpu-unlock build.
 *
 * A Windows user-mode process cannot map a PCI BAR, allocate physically
 * contiguous DMA memory, write PCI configuration space or trigger a PCIe FLR.
 * This driver exposes exactly those mechanical operations through IOCTLs; the
 * unlock policy and firmware layout remain in the cross-platform C sources.
 *
 * The device is created with an administrator-only DACL. The driver is
 * intentionally small: no PnP participation, no power management, no file I/O.
 */
#include <ntddk.h>
#include <wdmsec.h>
#include "gpuunlock_ioctl.h"

#define GU_DEVICE_NAME L"\\Device\\GpuUnlock"
#define GU_SYMLINK_NAME L"\\DosDevices\\GpuUnlock"

#define GU_MAX_BAR_LENGTH (64u * 1024u * 1024u)
#define GU_MAX_DMA_SIZE (16u * 1024u * 1024u)
/* The GSP image is mapped one 4 KiB page at a time, so a real .fwimage needs
 * tens of thousands of allocations (128 MiB / 4 KiB plus radix tables). */
#define GU_MAX_DMA_SLOTS 33000

struct gu_dma_slot {
	PVOID va;
	SIZE_T size;
	PHYSICAL_ADDRESS pa;
	BOOLEAN used;
};

static KSPIN_LOCK gu_cfg_lock;
static FAST_MUTEX gu_bar_lock;
static FAST_MUTEX gu_dma_lock;
static PVOID gu_bar;
static ULONG gu_bar_length;
static PHYSICAL_ADDRESS gu_bar_phys;
static struct gu_dma_slot gu_dma[GU_MAX_DMA_SLOTS];
static ULONG gu_dma_hint;
static PDEVICE_OBJECT gu_device;

static const GUID gu_device_class =
	{ 0x2f0c1a9e, 0x5d3b, 0x4a12,
	  { 0x9b, 0x77, 0x33, 0x11, 0x44, 0x55, 0x66, 0x77 } };

/* ------------------------------------------------------------- helpers */

static BOOLEAN gu_bdf_valid(gu_u32 bus, gu_u32 device, gu_u32 function)
{
	return bus <= 0xff && device <= 0x1f && function <= 0x7;
}

static ULONG gu_cfg_address(gu_u32 bus, gu_u32 device, gu_u32 function,
			    gu_u32 offset)
{
	return 0x80000000u | (bus << 16) | (device << 11) | (function << 8) |
	       (offset & 0xfc);
}

/* Prefer the HAL's access method: on MMCONFIG-only platforms CF8/CFC does not
 * decode at all. The legacy path stays as a fallback for older chipsets. */
static ULONG gu_cfg_read32(gu_u32 bus, gu_u32 device, gu_u32 function,
			   gu_u32 offset)
{
	ULONG slot = (device << 16) | (function << 8);
	ULONG value = 0;
	KIRQL irql;

	if (HalGetBusDataByOffset(PCIConfiguration, bus, slot, &value, offset,
				  sizeof(value)) == sizeof(value))
		return value;

	KeAcquireSpinLock(&gu_cfg_lock, &irql);
	WRITE_PORT_ULONG((PULONG)(ULONG_PTR)0xcf8,
			 gu_cfg_address(bus, device, function, offset));
	value = READ_PORT_ULONG((PULONG)(ULONG_PTR)0xcfc);
	KeReleaseSpinLock(&gu_cfg_lock, irql);
	return value;
}

static VOID gu_cfg_write32(gu_u32 bus, gu_u32 device, gu_u32 function,
			   gu_u32 offset, ULONG value)
{
	ULONG slot = (device << 16) | (function << 8);
	KIRQL irql;

	if (HalSetBusDataByOffset(PCIConfiguration, bus, slot, &value, offset,
				  sizeof(value)) == sizeof(value))
		return;

	KeAcquireSpinLock(&gu_cfg_lock, &irql);
	WRITE_PORT_ULONG((PULONG)(ULONG_PTR)0xcf8,
			 gu_cfg_address(bus, device, function, offset));
	WRITE_PORT_ULONG((PULONG)(ULONG_PTR)0xcfc, value);
	KeReleaseSpinLock(&gu_cfg_lock, irql);
}

static NTSTATUS gu_cfg_read(gu_u32 bus, gu_u32 device, gu_u32 function,
			    gu_u32 offset, gu_u32 size, gu_u32 *out)
{
	ULONG aligned, value, shift;

	if (!gu_bdf_valid(bus, device, function) || offset + size > 256 ||
	    (size != 1 && size != 2 && size != 4))
		return STATUS_INVALID_PARAMETER;
	aligned = offset & ~3u;
	value = gu_cfg_read32(bus, device, function, aligned);
	shift = (offset & 3) * 8;
	if (size == 4)
		*out = value;
	else if (size == 2)
		*out = (value >> shift) & 0xffff;
	else
		*out = (value >> shift) & 0xff;
	return STATUS_SUCCESS;
}

static NTSTATUS gu_cfg_write(gu_u32 bus, gu_u32 device, gu_u32 function,
			     gu_u32 offset, gu_u32 size, gu_u32 value)
{
	ULONG aligned, old, mask, shift;

	if (!gu_bdf_valid(bus, device, function) || offset + size > 256 ||
	    (size != 1 && size != 2 && size != 4))
		return STATUS_INVALID_PARAMETER;
	aligned = offset & ~3u;
	shift = (offset & 3) * 8;
	if (size == 4) {
		gu_cfg_write32(bus, device, function, aligned, value);
		return STATUS_SUCCESS;
	}
	mask = (size == 2 ? 0xffffu : 0xffu) << shift;
	old = gu_cfg_read32(bus, device, function, aligned);
	old = (old & ~mask) | ((value << shift) & mask);
	gu_cfg_write32(bus, device, function, aligned, old);
	return STATUS_SUCCESS;
}

static void gu_sleep_ms(LONGLONG milliseconds)
{
	LARGE_INTEGER interval;

	interval.QuadPart = -milliseconds * 10000;
	KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

/* Walk the standard capability list; offsets are relative to 0x40. */
static gu_u32 gu_find_cap(gu_u32 bus, gu_u32 device, gu_u32 function,
			  UCHAR id)
{
	gu_u32 ptr, i;

	{
		gu_u32 status;

		if (NT_SUCCESS(gu_cfg_read(bus, device, function, 0x06, 2, &status)) &&
		    !(status & 0x10))
			return 0;
	}
	if (!NT_SUCCESS(gu_cfg_read(bus, device, function, 0x34, 1, &ptr)))
		return 0;
	ptr &= 0xfc;
	for (i = 0; i < 48 && ptr >= 0x40 && ptr + 4 <= 0xfc; i++) {
		gu_u32 header;

		if (!NT_SUCCESS(gu_cfg_read(bus, device, function, ptr, 4, &header)))
			return 0;
		if ((header & 0xff) == id)
			return ptr;
		ptr = (header >> 8) & 0xfc;
	}
	return 0;
}

/* ---------------------------------------------------------- operations */

static NTSTATUS gu_bar_map(struct gu_bar_map_req *req)
{
	gu_u32 lo, hi = 0, type;
	PHYSICAL_ADDRESS pa;
	PVOID mapping;

	if (!req || !gu_bdf_valid(req->bus, req->device, req->function))
		return STATUS_INVALID_PARAMETER;
	if (req->bar != 0)
		return STATUS_NOT_SUPPORTED;
	if (!req->length || req->length > GU_MAX_BAR_LENGTH)
		return STATUS_INVALID_PARAMETER;
	if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function, 0x10,
				    4, &lo)) ||
	    lo == 0xffffffffu || !lo)
		return STATUS_DEVICE_NOT_READY;
	if (lo & 0x1)
		return STATUS_NOT_SUPPORTED;
	type = (lo >> 1) & 0x3;
	if (type == 2 &&
	    !NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function, 0x14,
				    4, &hi)))
		return STATUS_DEVICE_NOT_READY;
	pa.QuadPart = ((gu_u64)hi << 32) | (lo & ~0xfu);
	if (!pa.QuadPart)
		return STATUS_DEVICE_NOT_READY;

	mapping = MmMapIoSpaceEx(pa, req->length, PAGE_READWRITE | PAGE_NOCACHE);
	if (!mapping)
		return STATUS_INSUFFICIENT_RESOURCES;

	ExAcquireFastMutex(&gu_bar_lock);
	if (gu_bar)
		MmUnmapIoSpace(gu_bar, gu_bar_length);
	gu_bar = mapping;
	gu_bar_length = req->length;
	gu_bar_phys = pa;
	ExReleaseFastMutex(&gu_bar_lock);

	req->phys = pa.QuadPart;
	req->status = STATUS_SUCCESS;
	return STATUS_SUCCESS;
}

static VOID gu_bar_unmap(void)
{
	ExAcquireFastMutex(&gu_bar_lock);
	if (gu_bar)
		MmUnmapIoSpace(gu_bar, gu_bar_length);
	gu_bar = NULL;
	gu_bar_length = 0;
	gu_bar_phys.QuadPart = 0;
	ExReleaseFastMutex(&gu_bar_lock);
}

static NTSTATUS gu_mmio_read(struct gu_mmio_req *req)
{
	if (!req)
		return STATUS_INVALID_PARAMETER;
	if (!gu_bar || req->offset + 4 > gu_bar_length)
		return STATUS_INVALID_ADDRESS;
	req->value = READ_REGISTER_ULONG((PULONG)((PUCHAR)gu_bar + req->offset));
	return STATUS_SUCCESS;
}

static NTSTATUS gu_mmio_write(struct gu_mmio_req *req)
{
	if (!req)
		return STATUS_INVALID_PARAMETER;
	if (!gu_bar || req->offset + 4 > gu_bar_length)
		return STATUS_INVALID_ADDRESS;
	WRITE_REGISTER_ULONG((PULONG)((PUCHAR)gu_bar + req->offset), req->value);
	return STATUS_SUCCESS;
}

static NTSTATUS gu_dma_alloc(struct gu_dma_alloc_req *req)
{
	PHYSICAL_ADDRESS low, high, boundary;
	struct gu_dma_slot *slot = NULL;
	PVOID va;
	ULONG i;

	if (!req || !req->size || req->size > GU_MAX_DMA_SIZE)
		return STATUS_INVALID_PARAMETER;
	low.QuadPart = 0;
	/* The kernel sources program a 32-bit DMA mask, so every buffer must be
	 * reachable below 4 GiB exactly as dma_alloc_coherent would return it. */
	high.QuadPart = 0xffffffffull;
	boundary.QuadPart = 0;
	va = MmAllocateContiguousMemorySpecifyCache((SIZE_T)req->size, low, high,
						    boundary, MmNonCached);
	if (!va)
		return STATUS_INSUFFICIENT_RESOURCES;

	ExAcquireFastMutex(&gu_dma_lock);
	for (i = gu_dma_hint; i < GU_MAX_DMA_SLOTS; i++)
		if (!gu_dma[i].used) {
			slot = &gu_dma[i];
			break;
		}
	if (!slot)
		for (i = 0; i < gu_dma_hint; i++)
			if (!gu_dma[i].used) {
				slot = &gu_dma[i];
				break;
			}
	if (slot)
		gu_dma_hint = (ULONG)(slot - gu_dma + 1) % GU_MAX_DMA_SLOTS;
	if (!slot) {
		ExReleaseFastMutex(&gu_dma_lock);
		MmFreeContiguousMemory(va);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	slot->used = TRUE;
	slot->va = va;
	slot->size = (SIZE_T)req->size;
	slot->pa = MmGetPhysicalAddress(va);
	req->handle = i + 1;
	req->phys = slot->pa.QuadPart;
	req->status = STATUS_SUCCESS;
	ExReleaseFastMutex(&gu_dma_lock);
	return STATUS_SUCCESS;
}

static NTSTATUS gu_dma_free(struct gu_dma_copy_req *req)
{
	struct gu_dma_slot *slot;
	PVOID va;
	SIZE_T size;

	if (!req || !req->handle || req->handle > GU_MAX_DMA_SLOTS)
		return STATUS_INVALID_PARAMETER;
	ExAcquireFastMutex(&gu_dma_lock);
	slot = &gu_dma[req->handle - 1];
	if (!slot->used) {
		ExReleaseFastMutex(&gu_dma_lock);
		return STATUS_INVALID_PARAMETER;
	}
	va = slot->va;
	size = slot->size;
	slot->used = FALSE;
	slot->va = NULL;
	slot->size = 0;
	slot->pa.QuadPart = 0;
	ExReleaseFastMutex(&gu_dma_lock);
	MmFreeContiguousMemory(va);
	(void)size;
	return STATUS_SUCCESS;
}

static struct gu_dma_slot *gu_dma_lookup(gu_u32 handle)
{
	if (!handle || handle > GU_MAX_DMA_SLOTS)
		return NULL;
	return gu_dma[handle - 1].used ? &gu_dma[handle - 1] : NULL;
}

/* ------------------------------------------------ PCIe reset / power */

static NTSTATUS gu_flr(struct gu_bdf_req *req)
{
	gu_u32 cap, devcap;
	gu_u32 devsta, devctl;
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;

	if (!req || !gu_bdf_valid(req->bus, req->device, req->function))
		return STATUS_INVALID_PARAMETER;
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;
	cap = gu_find_cap(req->bus, req->device, req->function, 0x10);
	if (!cap)
		return STATUS_NOT_SUPPORTED;
	if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function, cap + 4,
				    4, &devcap)) ||
	    !(devcap & (1u << 28)))
		return STATUS_NOT_SUPPORTED;

	/* Pending transactions must drain before the function-level reset. */
	for (i = 0; i < 100; i++) {
		if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function,
					    cap + 10, 2, &devsta)))
			return STATUS_DEVICE_NOT_READY;
		if (!(devsta & 0x20))
			break;
		gu_sleep_ms(10);
	}

	if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function, cap + 8,
				    2, &devctl)))
		return STATUS_DEVICE_NOT_READY;
	status = gu_cfg_write(req->bus, req->device, req->function, cap + 8, 2,
			      devctl | (1u << 15));
	if (!NT_SUCCESS(status))
		return status;

	for (i = 0; i < 100; i++) {
		if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function,
					    cap + 8, 2, &devctl)))
			return STATUS_DEVICE_NOT_READY;
		if (!(devctl & (1u << 15)))
			break;
		gu_sleep_ms(10);
	}
	if (i == 100)
		return STATUS_TIMEOUT;
	gu_sleep_ms(100);
	req->status = STATUS_SUCCESS;
	return STATUS_SUCCESS;
}

static NTSTATUS gu_power_on(struct gu_bdf_req *req)
{
	gu_u32 cap, pmcsr;
	NTSTATUS status;

	if (!req || !gu_bdf_valid(req->bus, req->device, req->function))
		return STATUS_INVALID_PARAMETER;
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;
	cap = gu_find_cap(req->bus, req->device, req->function, 0x01);
	if (!cap)
		return STATUS_NOT_SUPPORTED;
	status = gu_cfg_read(req->bus, req->device, req->function, cap + 4, 2,
			     &pmcsr);
	if (!NT_SUCCESS(status))
		return status;
	if (pmcsr == 0xffffu)
		return STATUS_DEVICE_POWER_FAILURE;
	if ((pmcsr & 3) != 0) {
		status = gu_cfg_write(req->bus, req->device, req->function,
				      cap + 4, 2, pmcsr & ~3u);
		if (!NT_SUCCESS(status))
			return status;
		gu_sleep_ms(10);
		if (!NT_SUCCESS(gu_cfg_read(req->bus, req->device, req->function,
					    cap + 4, 2, &pmcsr)))
			return STATUS_DEVICE_NOT_READY;
		if ((pmcsr & 3) != 0)
			return STATUS_DEVICE_POWER_FAILURE;
	}
	req->status = STATUS_SUCCESS;
	return STATUS_SUCCESS;
}

static NTSTATUS gu_info(struct gu_info_req *info)
{
	ULONG i;

	if (!info)
		return STATUS_INVALID_PARAMETER;
	info->version = GU_DRIVER_VERSION;
	info->vendor = 0x10de;
	info->device = 0;
	info->bar_length = gu_bar_length;
	info->bar_mapped = gu_bar ? 1 : 0;
	info->pcie_cap = 0;
	info->pm_cap = 0;
	info->dma_allocations = 0;
	for (i = 0; i < GU_MAX_DMA_SLOTS; i++)
		if (gu_dma[i].used)
			info->dma_allocations++;
	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------ dispatch */

/* A client that dies mid-transaction must not leave the BAR mapped or the
 * contiguous DMA slots occupied; the next run would exhaust all 33000 slots. */
static void gu_dma_free_all(void)
{
	ULONG i;

	ExAcquireFastMutex(&gu_dma_lock);
	for (i = 0; i < GU_MAX_DMA_SLOTS; i++) {
		if (gu_dma[i].used) {
			PVOID va = gu_dma[i].va;

			gu_dma[i].used = FALSE;
			gu_dma[i].va = NULL;
			gu_dma[i].size = 0;
			MmFreeContiguousMemory(va);
		}
	}
	gu_dma_hint = 0;
	ExReleaseFastMutex(&gu_dma_lock);
}

static NTSTATUS gu_ioctl(PIRP irp, PIO_STACK_LOCATION irp_sp, ULONG *info)
{
	PVOID buffer = irp->AssociatedIrp.SystemBuffer;
	ULONG in_len = irp_sp->Parameters.DeviceIoControl.InputBufferLength;
	ULONG out_len = irp_sp->Parameters.DeviceIoControl.OutputBufferLength;
	ULONG code = irp_sp->Parameters.DeviceIoControl.IoControlCode;
	NTSTATUS status = STATUS_INVALID_PARAMETER;

	switch (code) {
	case GU_IOCTL_VERSION:
		if (buffer && out_len >= sizeof(gu_u32)) {
			*(gu_u32 *)buffer = GU_DRIVER_VERSION;
			*info = sizeof(gu_u32);
			status = STATUS_SUCCESS;
		}
		break;
	case GU_IOCTL_CONFIG_READ:
		if (buffer && in_len >= sizeof(struct gu_config_req) &&
		    out_len >= sizeof(struct gu_config_req)) {
			struct gu_config_req *req = buffer;

			status = gu_cfg_read(req->bus, req->device, req->function,
					     req->offset, req->size, &req->value);
			*info = status == STATUS_SUCCESS ? sizeof(*req) : 0;
		}
		break;
	case GU_IOCTL_CONFIG_WRITE:
		if (buffer && in_len >= sizeof(struct gu_config_req)) {
			struct gu_config_req *req = buffer;

			status = gu_cfg_write(req->bus, req->device, req->function,
					      req->offset, req->size, req->value);
		}
		break;
	case GU_IOCTL_MAP_BAR:
		if (buffer && in_len >= sizeof(struct gu_bar_map_req)) {
			struct gu_bar_map_req *req = buffer;

			status = gu_bar_map(req);
			*info = status == STATUS_SUCCESS ? sizeof(*req) : 0;
		}
		break;
	case GU_IOCTL_UNMAP_BAR:
		gu_bar_unmap();
		status = STATUS_SUCCESS;
		break;
	case GU_IOCTL_MMIO_READ:
		if (buffer && in_len >= sizeof(struct gu_mmio_req) &&
		    out_len >= sizeof(struct gu_mmio_req)) {
			status = gu_mmio_read(buffer);
			*info = status == STATUS_SUCCESS ? sizeof(struct gu_mmio_req) : 0;
		}
		break;
	case GU_IOCTL_MMIO_WRITE:
		if (buffer && in_len >= sizeof(struct gu_mmio_req))
			status = gu_mmio_write(buffer);
		break;
	case GU_IOCTL_DMA_ALLOC:
		if (buffer && in_len >= sizeof(struct gu_dma_alloc_req) &&
		    out_len >= sizeof(struct gu_dma_alloc_req)) {
			status = gu_dma_alloc(buffer);
			*info = status == STATUS_SUCCESS ? sizeof(struct gu_dma_alloc_req) : 0;
		}
		break;
	case GU_IOCTL_DMA_FREE:
		if (buffer && in_len >= sizeof(struct gu_dma_copy_req))
			status = gu_dma_free(buffer);
		break;
	case GU_IOCTL_DMA_WRITE: {
		struct gu_dma_copy_req *req = buffer;
		struct gu_dma_slot *slot;

		if (!buffer || in_len < sizeof(*req))
			break;
		slot = gu_dma_lookup(req->handle);
		if (!slot || req->offset + req->size > slot->size ||
		    in_len - sizeof(*req) < req->size)
			break;
		RtlCopyMemory((PUCHAR)slot->va + req->offset,
			      (PUCHAR)buffer + sizeof(*req), req->size);
		KeMemoryBarrier();
		req->status = STATUS_SUCCESS;
		status = STATUS_SUCCESS;
		break;
	}
	case GU_IOCTL_DMA_READ: {
		struct gu_dma_copy_req *req = buffer;
		struct gu_dma_slot *slot;

		if (!buffer || in_len < sizeof(*req) ||
		    out_len < sizeof(*req) || out_len - sizeof(*req) < req->size)
			break;
		slot = gu_dma_lookup(req->handle);
		if (!slot || req->offset + req->size > slot->size)
			break;
		RtlCopyMemory((PUCHAR)buffer + sizeof(*req),
			      (PUCHAR)slot->va + req->offset, req->size);
		req->status = STATUS_SUCCESS;
		*info = sizeof(*req) + req->size;
		status = STATUS_SUCCESS;
		break;
	}
	case GU_IOCTL_FLR:
		if (buffer && in_len >= sizeof(struct gu_bdf_req) &&
		    out_len >= sizeof(struct gu_bdf_req)) {
			status = gu_flr(buffer);
			*info = status == STATUS_SUCCESS ? sizeof(struct gu_bdf_req) : 0;
		}
		break;
	case GU_IOCTL_POWER_ON:
		if (buffer && in_len >= sizeof(struct gu_bdf_req) &&
		    out_len >= sizeof(struct gu_bdf_req)) {
			status = gu_power_on(buffer);
			*info = status == STATUS_SUCCESS ? sizeof(struct gu_bdf_req) : 0;
		}
		break;
	case GU_IOCTL_INFO:
		if (buffer && out_len >= sizeof(struct gu_info_req)) {
			status = gu_info(buffer);
			*info = status == STATUS_SUCCESS ? sizeof(struct gu_info_req) : 0;
		}
		break;
	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
	break;
	}
	return status;
}

static NTSTATUS gu_dispatch(PDEVICE_OBJECT device, PIRP irp)
{
	PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
	ULONG info = 0;

	UNREFERENCED_PARAMETER(device);
	switch (sp->MajorFunction) {
	case IRP_MJ_CREATE:
	case IRP_MJ_CLOSE:
		status = STATUS_SUCCESS;
		break;
	case IRP_MJ_CLEANUP:
		gu_bar_unmap();
		gu_dma_free_all();
		status = STATUS_SUCCESS;
		break;
	case IRP_MJ_DEVICE_CONTROL:
		status = gu_ioctl(irp, sp, &info);
		break;
	default:
		break;
	}
	irp->IoStatus.Status = status;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return status;
}

static VOID gu_unload(PDRIVER_OBJECT driver)
{
	UNICODE_STRING symlink;
	ULONG i;

	gu_bar_unmap();
	for (i = 0; i < GU_MAX_DMA_SLOTS; i++) {
		if (gu_dma[i].used) {
			MmFreeContiguousMemory(gu_dma[i].va);
			gu_dma[i].used = FALSE;
		}
	}
	RtlInitUnicodeString(&symlink, GU_SYMLINK_NAME);
	IoDeleteSymbolicLink(&symlink);
	if (gu_device)
		IoDeleteDevice(gu_device);
	UNREFERENCED_PARAMETER(driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
	UNICODE_STRING name, symlink;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(registry);
	KeInitializeSpinLock(&gu_cfg_lock);
	ExInitializeFastMutex(&gu_bar_lock);
	ExInitializeFastMutex(&gu_dma_lock);

	RtlInitUnicodeString(&name, GU_DEVICE_NAME);
	status = IoCreateDeviceSecure(driver, 0, &name, FILE_DEVICE_UNKNOWN,
				      FILE_DEVICE_SECURE_OPEN, FALSE,
				      &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
				      &gu_device_class, &gu_device);
	if (!NT_SUCCESS(status))
		return status;
	RtlInitUnicodeString(&symlink, GU_SYMLINK_NAME);
	status = IoCreateSymbolicLink(&symlink, &name);
	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(gu_device);
		return status;
	}

	driver->MajorFunction[IRP_MJ_CREATE] = gu_dispatch;
	driver->MajorFunction[IRP_MJ_CLOSE] = gu_dispatch;
	driver->MajorFunction[IRP_MJ_CLEANUP] = gu_dispatch;
	driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = gu_dispatch;
	driver->DriverUnload = gu_unload;
	return STATUS_SUCCESS;
}
