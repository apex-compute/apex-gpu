/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APEX_CMS_EDID_H
#define APEX_CMS_EDID_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

#define APEX_CMS_MODULE_BYTE_READ 0x0f
#define APEX_CMS_EDID_REQUEST_WORDS 5
#define APEX_CMS_EDID_RESPONSE_WORDS 6

static inline int apex_cms_edid_byte_request(u32 *packet, unsigned int block,
					      unsigned int byte)
{
	if (!packet || block > 1 || byte >= 128)
		return -EINVAL;

	packet[0] = (u32)APEX_CMS_MODULE_BYTE_READ << 24;
	packet[1] = 0; /* U50 cage zero. */
	packet[2] = 0; /* EDID occupies the unpaged 256-byte address space. */
	packet[3] = block; /* Lower page zero, upper page one. */
	packet[4] = block * 128 + byte;
	return 0;
}

static inline int apex_cms_edid_byte_response(const u32 *response, u8 *data)
{
	if (!response || !data)
		return -EINVAL;
	if (response[5] & ~0xffU)
		return -EPROTO;

	*data = response[5];
	return 0;
}

static inline int apex_cms_edid_extension_count_supported(u8 count)
{
	return count <= 1;
}

#endif
