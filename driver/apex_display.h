/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APEX_DISPLAY_H
#define APEX_DISPLAY_H

#include <linux/interrupt.h>
#include <linux/types.h>

struct apex_cp;
struct apex_display;
struct pci_dev;
typedef void (*apex_display_fatal_error_t)(void *context);

/* Without scanout (a device without CAP_DISPLAY) the DRM device carries GEM,
 * VMs and queues and no KMS objects, and start does nothing. */
struct apex_display *apex_display_create(struct pci_dev *pdev,
					 void __iomem *bar0, u64 local_bytes,
					 bool scanout,
					 apex_display_fatal_error_t fatal_error,
					 void *fatal_error_context);
/* Register the DRM device (KMS, GEM, VMs and queues through cp) before
 * asynchronous display discovery. */
int apex_display_register(struct apex_display *display, struct apex_cp *cp);
void apex_display_start(struct apex_display *display);
void apex_display_poison(struct apex_display *display);
/* Scanout status and cumulative counters, read from BAR0. */
struct apex_display_counters {
	u32 status, phy_status, vblank_count, underflow_lines, read_errors, gt_status;
};
void apex_display_get_counters(struct apex_display *display,
			       struct apex_display_counters *counters);
/* A native reset drops the display engine's in-flight memory accesses:
 * begin stops scanout fetches and holds the scanout until end, which restores
 * the scanout when the reset completed. */
void apex_display_reset_begin(struct apex_display *display);
void apex_display_reset_end(struct apex_display *display, bool restore);
void apex_display_stop(struct apex_display *display);
void apex_display_destroy(struct apex_display *display);
irqreturn_t apex_display_irq(int irq, void *data);

#endif
