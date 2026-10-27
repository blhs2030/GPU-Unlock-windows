/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal Linux-kernel API surface for the Windows user-mode build.
 *
 * The unmodified kernel sources (main.c, unlock_610.c, prep_610.c,
 * layout_610.c, vbios_610.c, gfw_eval.c) are compiled against this header.
 * Everything that needs real hardware access is implemented in hal.c and
 * dispatched to a backend; the simulated backend is backend_sim.c.
 */
#ifndef GPU_UNLOCK_WINDOWS_COMPAT_H
#define GPU_UNLOCK_WINDOWS_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

/* Linux errno values that the Windows CRT does not define. */
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef EPROTO
#define EPROTO 71
#endif
#ifndef EALREADY
#define EALREADY 114
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef uint64_t ktime_t;

#define __init
#define __exit
#define __iomem
#define __must_check

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define lower_32_bits(v) ((u32)(u64)(v))
#define upper_32_bits(v) ((u32)((u64)(v) >> 32))
#define DMA_BIT_MASK(n) (((u64)1 << (n)) - 1)
#define GFP_KERNEL 0

#define THIS_MODULE ((void *)0)
#define __module_get(m) ((void)(m))
#define MODULE_FIRMWARE(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_PARM_DESC(name, desc)
#define module_param(name, type, perm)
/* main.c is included by gpu_unlock_win.c; the CLI calls the entry points. */
#define module_init(fn)
#define module_exit(fn)

/* ---------------------------------------------------------------- types */

struct list_head {
	struct list_head *next, *prev, *owner;
	unsigned int count;	/* cards and devres bookkeeping both use this */
};

#define LIST_HEAD_INIT(name) { &(name), &(name), &(name), 0 }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline bool list_empty(const struct list_head *head)
{
	return head->count == 0;
}

static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
	node->prev = head->prev;
	node->next = head;
	node->owner = head;
	head->prev->next = node;
	head->prev = node;
	head->count++;
}

static inline void list_del(struct list_head *node)
{
	if (node->owner)
		node->owner->count--;
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->next = node->prev = node->owner = NULL;
}

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each_entry(pos, head, member)					\
	for (pos = list_entry((head)->next, __typeof__(*pos), member);		\
	     &pos->member != (head);						\
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)				\
	for (pos = list_entry((head)->next, __typeof__(*pos), member),		\
	     n = list_entry(pos->member.next, __typeof__(*pos), member);	\
	     &pos->member != (head);						\
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

struct device {
	u64 *dma_mask;
	u64 coherent_dma_mask;
	struct list_head devres_head;
	int devres_lock;
	int devres_open;
	int devres_before;
};

struct pci_driver {
	const char *name;
};

struct pci_dev {
	struct device dev;	/* must stay first: logs use container_of() */
	unsigned short vendor, device;
	struct pci_driver *driver;
	int current_state, pm_cap, refs;
	unsigned int dev_flags;
	size_t bar_len;
	char name[16];
	void *wdev;		/* backend-private device */
	void *lock;		/* CRITICAL_SECTION *, owned by the backend */
	int lock_init;
};

struct firmware {
	size_t size;
	const u8 *data;
};

struct spinlock { int unused; };

#define spin_lock_irqsave(lock, flags) \
	do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(lock); (void)(flags); } while (0)

/* ------------------------------------------------------------- pci ids */

#define PCI_VENDOR_ID_NVIDIA 0x10de
#define PCI_ANY_ID (~0u)
#define PCI_D0 0
#define PCI_UNKNOWN 5
#define PCI_DEV_FLAGS_NO_FLR_RESET (1u << 10)
#define PCI_VENDOR_ID 0
#define PCI_COMMAND 4
#define PCI_COMMAND_IO 1
#define PCI_COMMAND_MEMORY 2
#define PCI_COMMAND_MASTER 4
#define PCI_PM_CTRL 4
#define PCI_PM_CTRL_STATE_MASK 3
#define PCI_EXP_DEVCAP 4
#define PCI_EXP_DEVCAP_FLR (1u << 28)
#define PCI_EXP_DEVSTA 10
#define PCI_EXP_DEVSTA_TRPND 32
#define PCI_BASE_ADDRESS_0 16
#define IORESOURCE_MEM 0x200

/* ------------------------------------------------------------- helpers */

static inline void *kzalloc(size_t size, int flags)
{
	(void)flags;
	return calloc(1, size);
}

#define kfree free
#define kvfree free
#define kvcalloc(n, s, f) calloc((n), (s))
#define kvmalloc(n, f) malloc(n)

static inline char *win_strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}
#define strchrnul win_strchrnul
#define strncasecmp _strnicmp
#define strcasecmp _stricmp

static inline ktime_t ktime_add_us(ktime_t t, unsigned long us)
{
	return t + us;
}

static inline int ktime_compare(ktime_t a, ktime_t b)
{
	return a > b ? 1 : a < b ? -1 : 0;
}

/* Flush CPU-side DMA buffers to the device. A no-op for the simulated card;
 * the real backend copies each mirror into the driver's contiguous buffer. */
void win_dma_wmb(void);

static inline void dma_wmb(void)
{
	win_dma_wmb();
}

/* --------------------------------------------------- kernel entry calls */

void win_log(int error, const char *format, ...);
void win_dev_log(struct device *dev, int error, const char *format, ...);

#define pr_err(...) win_log(1, __VA_ARGS__)
#define pr_info(...) win_log(0, __VA_ARGS__)
#define pr_warn(...) win_log(1, __VA_ARGS__)
#define dev_err(dev, ...) win_dev_log((dev), 1, __VA_ARGS__)
#define dev_info(dev, ...) win_dev_log((dev), 0, __VA_ARGS__)
#define dev_warn(dev, ...) win_dev_log((dev), 1, __VA_ARGS__)
#define pci_err(pdev, ...) win_dev_log(&(pdev)->dev, 1, __VA_ARGS__)
#define pci_info(pdev, ...) win_dev_log(&(pdev)->dev, 0, __VA_ARGS__)
static inline const char *pci_name(const struct pci_dev *pdev)
{
	return pdev->name;
}

/* --------------------------------------------------------------- pci */

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
			       struct pci_dev *from);
struct pci_dev *pci_dev_get(struct pci_dev *pdev);
void pci_dev_put(struct pci_dev *pdev);
void pci_dev_lock(struct pci_dev *pdev);
void pci_dev_unlock(struct pci_dev *pdev);

int pci_read_config_word(struct pci_dev *pdev, int where, u16 *value);
int pci_read_config_dword(struct pci_dev *pdev, int where, u32 *value);
int pci_write_config_word(struct pci_dev *pdev, int where, u16 value);
int pcie_capability_read_word(struct pci_dev *pdev, int where, u16 *value);
int pcie_capability_read_dword(struct pci_dev *pdev, int where, u32 *value);
int pci_is_pcie(struct pci_dev *pdev);
int pci_select_bars(struct pci_dev *pdev, int type);
int pci_request_region(struct pci_dev *pdev, int bar, const char *name);
void pci_release_region(struct pci_dev *pdev, int bar);
int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev,
					    const char *name);
int pci_enable_device_mem(struct pci_dev *pdev);
void pci_disable_device(struct pci_dev *pdev);
resource_size_t pci_resource_len(struct pci_dev *pdev, int bar);
u8 *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void pci_iounmap(struct pci_dev *pdev, u8 *addr);
void *pci_map_rom(struct pci_dev *pdev, size_t *size);
void pci_unmap_rom(struct pci_dev *pdev, void *rom);
int pci_save_state(struct pci_dev *pdev);
void pci_restore_state(struct pci_dev *pdev);
void pci_set_master(struct pci_dev *pdev);
void pci_clear_master(struct pci_dev *pdev);
int pcie_flr(struct pci_dev *pdev);

/* ------------------------------------------------------------- dma */

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma,
			 int flags);
void dma_free_coherent(struct device *dev, size_t size, void *cpu,
		       dma_addr_t dma);
int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
void win_memcpy_fromio(void *dst, const void *src, size_t n);
#define memcpy_fromio win_memcpy_fromio

/* ------------------------------------------------------- firmware/devres */

int request_firmware(const struct firmware **fw, const char *name,
		     struct device *dev);
void release_firmware(const struct firmware *fw);
void *devres_open_group(struct device *dev, void *id, int flags);
int devres_release_group(struct device *dev, void *id);

/* ------------------------------------------------------- time/sleep */

void msleep(unsigned int msecs);
void usleep_range(unsigned long min, unsigned long max);
ktime_t ktime_get(void);

/* ------------------------------------------------------- mmio */

u32 readl(const volatile void *addr);
void writel(u32 value, volatile void *addr);

/* ------------------------------------------------------------ async */

typedef uint64_t async_cookie_t;
typedef void (*async_func_t)(void *data, async_cookie_t cookie);

struct async_domain {
	void *lock;		/* CRITICAL_SECTION *, allocated lazily */
	void *threads[64];
	unsigned int count;
	int init;
};

#define ASYNC_DOMAIN_EXCLUSIVE(name) struct async_domain name = { 0 }

async_cookie_t async_schedule_domain(async_func_t fn, void *data,
				     struct async_domain *domain);
void async_synchronize_full_domain(struct async_domain *domain);

/* ------------------------------------------------------- suspend */

#define PM_SUSPEND_PREPARE 0x0003
#define PM_POST_SUSPEND 0x0004
#define NOTIFY_DONE 0x0000

struct notifier_block {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
	int priority;
};

int register_pm_notifier(struct notifier_block *nb);
int unregister_pm_notifier(struct notifier_block *nb);
void win_pm_resume(void);

#endif
