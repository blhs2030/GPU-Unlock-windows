/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows user-mode HAL for the Linux kernel API surface in compat.h.
 *
 * Everything is forwarded to the simulated GA104 backend, except firmware
 * lookup (built-in synthetic assets or --assets) and enumeration (SetupAPI,
 * backend_win32.c). No real PCI configuration space or BAR is touched.
 */
#include <windows.h>
#include <stdarg.h>
#include "hal.h"

static volatile LONG64 hal_clock_us;
static int hal_quiet;
static int hal_real;
static LARGE_INTEGER hal_qpc_freq;

static void hal_ensure_once(void);

static INIT_ONCE hal_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION hal_meta_lock;
static CRITICAL_SECTION hal_log_lock;

static BOOL CALLBACK hal_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	InitializeCriticalSection(&hal_meta_lock);
	InitializeCriticalSection(&hal_log_lock);
	return TRUE;
}

static void hal_ensure_once(void)
{
	InitOnceExecuteOnce(&hal_once, hal_init_once, NULL, NULL);
}

void gu_hal_init(void)
{
	hal_ensure_once();
	hal_clock_us = 0;
}

void gu_hal_set_quiet(int quiet)
{
	hal_quiet = quiet;
}

void gu_hal_set_real(int real)
{
	hal_real = real != 0;
}

int gu_hal_real_mode(void)
{
	return hal_real;
}

/* ------------------------------------------------------------- logging */

void win_log(int error, const char *format, ...)
{
	char buffer[1024];
	va_list args;

	if (!error && hal_quiet)
		return;
	hal_ensure_once();
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	EnterCriticalSection(&hal_log_lock);
	fputs(buffer, error ? stderr : stdout);
	fflush(error ? stderr : stdout);
	LeaveCriticalSection(&hal_log_lock);
}

void win_dev_log(struct device *dev, int error, const char *format, ...)
{
	/* struct device is the first member of struct pci_dev. */
	const struct pci_dev *pdev = (const struct pci_dev *)dev;
	char buffer[1024];
	va_list args;

	if (!error && hal_quiet)
		return;
	hal_ensure_once();
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	EnterCriticalSection(&hal_log_lock);
	fprintf(error ? stderr : stdout, "%s: %s", pdev->name, buffer);
	fflush(error ? stderr : stdout);
	LeaveCriticalSection(&hal_log_lock);
}

/* --------------------------------------------------------------- time */

ktime_t ktime_get(void)
{
	LARGE_INTEGER now;

	if (!hal_real)
		return (ktime_t)InterlockedCompareExchange64(&hal_clock_us, 0, 0);
	/* Real hardware is polled against wall-clock deadlines, so the timeout
	 * budgets in unlock_610.c mean milliseconds, not loop iterations. */
	if (!hal_qpc_freq.QuadPart)
		QueryPerformanceFrequency(&hal_qpc_freq);
	QueryPerformanceCounter(&now);
	if (!hal_qpc_freq.QuadPart)
		return 0;
	return (ktime_t)(now.QuadPart * 1000000ll / hal_qpc_freq.QuadPart);
}

void usleep_range(unsigned long min, unsigned long max)
{
	LARGE_INTEGER start, now, ticks;

	(void)min;
	if (hal_real) {
		if (!max)
			return;
		/* Sleep() only has ~15 ms granularity, which would blow every
		 * sub-millisecond firmware poll budget, so spin on the QPC. */
		if (max >= 1000) {
			Sleep((DWORD)((max + 999) / 1000));
			return;
		}
		if (!hal_qpc_freq.QuadPart)
			QueryPerformanceFrequency(&hal_qpc_freq);
		if (!hal_qpc_freq.QuadPart) {
			Sleep(0);
			return;
		}
		QueryPerformanceCounter(&start);
		ticks.QuadPart = (LONGLONG)max * hal_qpc_freq.QuadPart / 1000000ll;
		do {
			QueryPerformanceCounter(&now);
		} while (now.QuadPart - start.QuadPart < ticks.QuadPart);
		return;
	}
	InterlockedAdd64(&hal_clock_us, (LONG64)max);
}

void msleep(unsigned int msecs)
{
	if (hal_real) {
		Sleep((DWORD)msecs);
		return;
	}
	InterlockedAdd64(&hal_clock_us, (LONG64)msecs * 1000);
}

/* ---------------------------------------------------------------- pci */

struct pci_dev *pci_dev_get(struct pci_dev *pdev)
{
	if (pdev)
		InterlockedIncrement((volatile LONG *)&pdev->refs);
	return pdev;
}

void pci_dev_put(struct pci_dev *pdev)
{
	if (pdev)
		InterlockedDecrement((volatile LONG *)&pdev->refs);
}

void pci_dev_lock(struct pci_dev *pdev)
{
	if (pdev && pdev->lock)
		EnterCriticalSection((CRITICAL_SECTION *)pdev->lock);
}

void pci_dev_unlock(struct pci_dev *pdev)
{
	if (pdev && pdev->lock)
		LeaveCriticalSection((CRITICAL_SECTION *)pdev->lock);
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
			       struct pci_dev *from)
{
	if (hal_real)
		return gu_real_pci_get_device(vendor, device, from);
	return gu_sim_pci_get_device(vendor, device, from);
}

int pci_read_config_word(struct pci_dev *pdev, int where, u16 *value)
{
	if (hal_real)
		return gu_real_read_config_word(pdev, where, value);
	return gu_sim_read_config_word(pdev, where, value);
}

int pci_read_config_dword(struct pci_dev *pdev, int where, u32 *value)
{
	if (hal_real)
		return gu_real_read_config_dword(pdev, where, value);
	return gu_sim_read_config_dword(pdev, where, value);
}

int pci_write_config_word(struct pci_dev *pdev, int where, u16 value)
{
	if (hal_real)
		return gu_real_write_config_word(pdev, where, value);
	return gu_sim_write_config_word(pdev, where, value);
}

int pcie_capability_read_word(struct pci_dev *pdev, int where, u16 *value)
{
	if (hal_real)
		return gu_real_cap_read_word(pdev, where, value);
	return gu_sim_cap_read_word(pdev, where, value);
}

int pcie_capability_read_dword(struct pci_dev *pdev, int where, u32 *value)
{
	if (hal_real)
		return gu_real_cap_read_dword(pdev, where, value);
	return gu_sim_cap_read_dword(pdev, where, value);
}

int pci_is_pcie(struct pci_dev *pdev)
{
	if (hal_real)
		return gu_real_is_pcie(pdev);
	return gu_sim_is_pcie(pdev);
}

int pci_select_bars(struct pci_dev *pdev, int type)
{
	if (hal_real)
		return gu_real_select_bars(pdev, type);
	return gu_sim_select_bars(pdev, type);
}

int pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	if (hal_real)
		return gu_real_request_region(pdev, bar, name);
	return gu_sim_request_region(pdev, bar, name);
}

void pci_release_region(struct pci_dev *pdev, int bar)
{
	if (hal_real) {
		gu_real_release_region(pdev, bar);
		return;
	}
	gu_sim_release_region(pdev, bar);
}

int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev,
					    const char *name)
{
	/* The simulated card drives no display; nothing conflicts. */
	(void)pdev;
	(void)name;
	return 0;
}

int pci_enable_device_mem(struct pci_dev *pdev)
{
	if (hal_real)
		return gu_real_enable_device_mem(pdev);
	return gu_sim_enable_device_mem(pdev);
}

void pci_disable_device(struct pci_dev *pdev)
{
	if (hal_real) {
		gu_real_disable_device(pdev);
		return;
	}
	gu_sim_disable_device(pdev);
}

resource_size_t pci_resource_len(struct pci_dev *pdev, int bar)
{
	if (hal_real)
		return gu_real_resource_len(pdev, bar);
	return gu_sim_resource_len(pdev, bar);
}

u8 *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	if (hal_real)
		return gu_real_iomap(pdev, bar, maxlen);
	return gu_sim_iomap(pdev, bar, maxlen);
}

void pci_iounmap(struct pci_dev *pdev, u8 *addr)
{
	if (hal_real) {
		gu_real_iounmap(pdev, addr);
		return;
	}
	gu_sim_iounmap(pdev, addr);
}

int pci_save_state(struct pci_dev *pdev)
{
	if (hal_real)
		return gu_real_save_state(pdev);
	return gu_sim_save_state(pdev);
}

void pci_restore_state(struct pci_dev *pdev)
{
	if (hal_real) {
		gu_real_restore_state(pdev);
		return;
	}
	gu_sim_restore_state(pdev);
}

void pci_set_master(struct pci_dev *pdev)
{
	if (hal_real) {
		gu_real_set_master(pdev);
		return;
	}
	gu_sim_set_master(pdev);
}

void pci_clear_master(struct pci_dev *pdev)
{
	if (hal_real) {
		gu_real_clear_master(pdev);
		return;
	}
	gu_sim_clear_master(pdev);
}

int pcie_flr(struct pci_dev *pdev)
{
	if (hal_real)
		return gu_real_flr(pdev);
	return gu_sim_flr(pdev);
}

/* ---------------------------------------------------------------- dma */

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma,
			 int flags)
{
	if (hal_real)
		return gu_real_dma_alloc(dev, size, dma, flags);
	return gu_sim_dma_alloc(dev, size, dma, flags);
}

void dma_free_coherent(struct device *dev, size_t size, void *cpu,
		       dma_addr_t dma)
{
	if (hal_real) {
		gu_real_dma_free(dev, size, cpu, dma);
		return;
	}
	gu_sim_dma_free(dev, size, cpu, dma);
}

int dma_set_mask(struct device *dev, u64 mask)
{
	if (!dev || !dev->dma_mask || mask < 0xffffffffull)
		return -EINVAL;
	*dev->dma_mask = mask;
	return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
	if (!dev)
		return -EINVAL;
	dev->coherent_dma_mask = mask;
	return 0;
}

/* ------------------------------------------------------ firmware/devres */

int request_firmware(const struct firmware **fw, const char *name,
		     struct device *dev)
{
	const u8 *data = NULL;
	size_t size = 0;
	struct firmware *image;
	int ret;

	(void)dev;
	ret = gu_asset_lookup(name, &data, &size);
	if (ret)
		return ret;
	image = malloc(sizeof(*image));
	if (!image)
		return -ENOMEM;
	image->size = size;
	image->data = data;
	*fw = image;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	free((void *)fw);
}

void *devres_open_group(struct device *dev, void *id, int flags)
{
	(void)id;
	(void)flags;
	return dev;
}

int devres_release_group(struct device *dev, void *id)
{
	(void)dev;
	(void)id;
	return 0;
}

/* -------------------------------------------------------------- mmio */

u32 readl(const volatile void *addr)
{
	if (hal_real)
		return gu_real_readl((const void *)addr);
	return gu_sim_readl((const void *)addr);
}

void writel(u32 value, volatile void *addr)
{
	if (hal_real) {
		gu_real_writel(value, (void *)addr);
		return;
	}
	gu_sim_writel(value, (void *)addr);
}

void win_dma_wmb(void)
{
	if (hal_real)
		gu_real_dma_flush();
}

/* `work->bar` is an opaque token in real mode, so the ROM read cannot be a
 * plain memcpy. Fetch it a dword at a time through the driver like readl(). */
void win_memcpy_fromio(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;
	size_t i;

	if (!hal_real) {
		memcpy(dst, src, n);
		return;
	}
	for (i = 0; i + 4 <= n; i += 4) {
		u32 value = gu_real_readl(s + i);

		d[i] = (u8)value;
		d[i + 1] = (u8)(value >> 8);
		d[i + 2] = (u8)(value >> 16);
		d[i + 3] = (u8)(value >> 24);
	}
	if (i < n) {
		u32 value = gu_real_readl(s + (i & ~(size_t)3));

		for (; i < n; i++)
			d[i] = (u8)(value >> (8 * (i & 3)));
	}
}

/* ------------------------------------------------------------- async */

struct async_job {
	async_func_t fn;
	void *data;
	async_cookie_t cookie;
};

static volatile LONG async_cookie;

static DWORD WINAPI async_thread(LPVOID argument)
{
	struct async_job *job = argument;

	job->fn(job->data, job->cookie);
	free(job);
	return 0;
}

async_cookie_t async_schedule_domain(async_func_t fn, void *data,
				     struct async_domain *domain)
{
	struct async_job *job;
	async_cookie_t cookie;
	HANDLE thread;

	hal_ensure_once();
	job = malloc(sizeof(*job));
	if (!job || !fn || !domain)
		return 0;
	cookie = (async_cookie_t)InterlockedIncrement(&async_cookie);
	job->fn = fn;
	job->data = data;
	job->cookie = cookie;
	EnterCriticalSection(&hal_meta_lock);
	if (!domain->init) {
		CRITICAL_SECTION *lock = malloc(sizeof(*lock));

		if (lock) {
			InitializeCriticalSection(lock);
			domain->lock = lock;
			domain->init = 1;
		}
	}
	if (domain->init) {
		EnterCriticalSection((CRITICAL_SECTION *)domain->lock);
		if (domain->count < sizeof(domain->threads) / sizeof(domain->threads[0])) {
			thread = CreateThread(NULL, 0, async_thread, job, 0, NULL);
			if (thread) {
				domain->threads[domain->count++] = thread;
			} else {
				free(job);
				job = NULL;
			}
		} else {
			free(job);
			job = NULL;
		}
		LeaveCriticalSection((CRITICAL_SECTION *)domain->lock);
	} else {
		free(job);
		job = NULL;
	}
	LeaveCriticalSection(&hal_meta_lock);
	return job ? cookie : 0;
}

void async_synchronize_full_domain(struct async_domain *domain)
{
	unsigned int i;

	if (!domain || !domain->init)
		return;
	EnterCriticalSection((CRITICAL_SECTION *)domain->lock);
	for (i = 0; i < domain->count; i++) {
		HANDLE thread = domain->threads[i];

		if (thread)
			WaitForSingleObject(thread, INFINITE);
		if (thread)
			CloseHandle(thread);
	}
	domain->count = 0;
	LeaveCriticalSection((CRITICAL_SECTION *)domain->lock);
}

/* ----------------------------------------------------------- suspend */

static struct notifier_block *pm_notifier;

int register_pm_notifier(struct notifier_block *nb)
{
	pm_notifier = nb;
	return 0;
}

int unregister_pm_notifier(struct notifier_block *nb)
{
	if (pm_notifier == nb)
		pm_notifier = NULL;
	return 0;
}

void win_pm_resume(void)
{
	if (pm_notifier && pm_notifier->notifier_call)
		pm_notifier->notifier_call(pm_notifier, PM_POST_SUSPEND, NULL);
}
