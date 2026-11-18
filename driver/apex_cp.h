/* SPDX-License-Identifier: GPL-2.0-only */
/* Render memory, GPU VMs and command-processor queues. */
#ifndef APEX_CP_H
#define APEX_CP_H

#include <linux/interrupt.h>
#include <linux/types.h>

struct apex_cp;
struct dma_buf;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_driver;
struct pci_dev;
struct vm_area_struct;

struct apex_cp_ops {
	/* Hardware can no longer be trusted: fence the device (clears Bus
	 * Master Enable) and poison every owner. Callable from any context. */
	void (*failed)(void *owner);
	int (*master_get)(void *owner);
	void (*master_put)(void *owner);
	/* Around a native reset: stop and restore the display's scanout. */
	void (*reset_begin)(void *owner);
	void (*reset_end)(void *owner, bool restore);
};

#define APEX_PROFILE_U50 2
#define APEX_PROFILE_K7 3

/* LOCAL [0, usable) belongs to the allocator. The caller has verified a
 * translated IOMMU domain. `profile` is the BAR0 board profile. */
struct apex_cp *apex_cp_create(struct pci_dev *pdev, void __iomem *bar0, u32 profile,
			       u64 usable, const struct apex_cp_ops *ops, void *owner);
/* Start the command processor after the IRQ is requested. Fails when the
 * image has no supported command processor. */
int apex_cp_start(struct apex_cp *cp);
irqreturn_t apex_cp_irq(int irq, void *data);
/* IRQ-safe latch: fail waits and admissions, no MMIO. */
void apex_cp_poison(struct apex_cp *cp);
/* Remove: halt the command processor and stop MMIO. drained reports that
 * Bus Master Enable was cleared and pending transactions completed; host
 * memory the device might still address leaks otherwise. */
void apex_cp_quiesce(struct apex_cp *cp);
void apex_cp_remove(struct apex_cp *cp, bool drained);
void apex_cp_put(struct apex_cp *cp);

/* The DRM driver: KMS from the display owner, render and queue ioctls here. */
extern const struct drm_driver apex_drm_driver;
/* KMS: a LOCAL object's physical address, for direct scanout. */
bool apex_cp_gem_local(struct drm_gem_object *object, u64 *address);

/* Implemented by the display owner: the command processor bound to drm. */
struct apex_cp *apex_display_cp(struct drm_device *drm);

#endif
