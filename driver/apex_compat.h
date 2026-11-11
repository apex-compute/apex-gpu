/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel API differences the driver builds across, for both boards (U50 and
 * Kintex-7). The driver is developed against a recent kernel; these let it
 * also build on older ones, e.g. 6.11.
 */
#ifndef APEX_COMPAT_H
#define APEX_COMPAT_H

#include <linux/version.h>

/* pci_resize_resource() takes a mask of BARs to keep assigned (6.18). */
#define APEX_RESIZE_KEEPS_BARS (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0))
/* drm_mode_size_dumb() and <drm/drm_dumb_buffers.h> (6.18). */
#define APEX_HAS_DRM_MODE_SIZE_DUMB (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0))
/* .fb_create and drm_gem_fb_create_with_dirty() take the format info (6.17). */
#define APEX_FB_CREATE_HAS_INFO (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0))

/* drm_connector_helper_funcs.mode_valid takes a const mode (6.15). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define APEX_MODE_CONST const
#else
#define APEX_MODE_CONST
#endif

/* drm_gem_is_imported() (6.15). */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
#define drm_gem_is_imported(obj) (!!(obj)->import_attach)
#endif

#endif /* APEX_COMPAT_H */
