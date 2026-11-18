// SPDX-License-Identifier: GPL-2.0-only

#include "apex_compat.h"
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "apex_cms_edid.h"
#include "apex_cp.h"
#include "apex_display.h"

#define APEX_DISPLAY_ID 0x0400
#define APEX_DISPLAY_COMMAND 0x0404
#define APEX_DISPLAY_PIXEL_CLOCK 0x0408
#define APEX_DISPLAY_H_ACTIVE_FRONT 0x040c
#define APEX_DISPLAY_H_SYNC_BACK 0x0410
#define APEX_DISPLAY_V_ACTIVE_FRONT 0x0414
#define APEX_DISPLAY_V_SYNC_BACK 0x0418
#define APEX_DISPLAY_MODE_FLAGS 0x041c
#define APEX_DISPLAY_SURFACE_LOW 0x0420
#define APEX_DISPLAY_SURFACE_HIGH 0x0424
#define APEX_DISPLAY_PITCH 0x0428
#define APEX_DISPLAY_SAFE_COLOR 0x042c
#define APEX_DISPLAY_DOORBELL 0x0430
#define APEX_DISPLAY_STATUS 0x0434
#define APEX_DISPLAY_COMPLETED_ID 0x043c
#define APEX_DISPLAY_COMPLETED_RESULT 0x0440
#define APEX_DISPLAY_UNDERFLOW_LINES 0x0444
#define APEX_DISPLAY_VBLANK_COUNT 0x0448
#define APEX_DISPLAY_READ_ERRORS 0x044c
#define APEX_DISPLAY_COMPLETION_ACK 0x0450
#define APEX_DISPLAY_PHY_STATUS 0x0454
#define APEX_DISPLAY_PHY_CLOCK 0x0458
#define APEX_DISPLAY_PHY_COMPLETION 0x045c
#define APEX_DISPLAY_HOTPLUG 0x047c
#define APEX_DISPLAY_GT_CONFIG 0x0484
#define APEX_DISPLAY_GT_FRACTION 0x0488
#define APEX_DISPLAY_GT_CONTROL 0x048c
#define APEX_DISPLAY_GT_STATUS 0x0490

#define APEX_DISPLAY_CMD_DISABLE 0
#define APEX_DISPLAY_CMD_MODESET 1
#define APEX_DISPLAY_CMD_FLIP 2
#define APEX_DISPLAY_RESULT_SUCCESS 0
#define APEX_DISPLAY_ENABLED BIT(1)
#define APEX_DISPLAY_COMMAND_BUSY BIT(3)
#define APEX_DISPLAY_COMPLETION_VALID BIT(4)
#define APEX_DISPLAY_PHY_PENDING BIT(0)
#define APEX_DISPLAY_PHY_ENABLE BIT(1)

#define APEX_DISPLAY_GT_START 1
#define APEX_DISPLAY_GT_DISABLE 2
#define APEX_DISPLAY_GT_BUSY BIT(0)
#define APEX_DISPLAY_GT_READY BIT(1)
#define APEX_DISPLAY_GT_ERROR BIT(2)
#define APEX_DISPLAY_GT_ERROR_CODE(s) (((s) >> 8) & 0xff)
#define APEX_DISPLAY_GT_PRESENT BIT(31)
#define APEX_DISPLAY_U50_GT_REFCLK_X2_HZ 322265625ULL
#define APEX_DISPLAY_GT_SAMPLE_THRESHOLD_HZ 50000000U

#define APEX_CMS_REG_MAP_ID 0x1000
#define APEX_CMS_FW_VERSION 0x1004
#define APEX_CMS_STATUS 0x1008
#define APEX_CMS_ERROR 0x100c
#define APEX_CMS_PROFILE 0x1014
#define APEX_CMS_CONTROL 0x1018
#define APEX_CMS_HOST_MESSAGE_OFFSET 0x1300
#define APEX_CMS_HOST_MESSAGE_ERROR 0x1304
#define APEX_CMS_HOST_STATUS2 0x130c
#define APEX_CMS_RESET 0x3000
#define APEX_CMS_REG_MAP_ID_VALUE 0x74736574
#define APEX_CMS_U50_PROFILE 0x55353041
#define APEX_CMS_SC_MODE(status) (((status) >> 28) & 0xf)
#define APEX_CMS_SC_MODE_NORMAL 1
#define APEX_CMS_COMM_VERSION(status) (((status) >> 24) & 0xf)
#define APEX_CMS_REG_MAP_READY BIT(0)
#define APEX_CMS_CLEAR_ERROR BIT(1)
#define APEX_CMS_MAILBOX_OWNER BIT(5)
#define APEX_CMS_PACKET_ERROR BIT(26)
#define APEX_CMS_HBM_TEMPERATURE BIT(27)
#define APEX_CMS_BAR_BASE 0x1000
#define APEX_CMS_WINDOW_BYTES 0x2000
#define APEX_CMS_REG_MAP_BYTES 0x1000
#define APEX_CMS_INVALID_MAILBOX 0xdeadfa11
#define APEX_CMS_QSFP_IO_READ 0x0d
#define APEX_CMS_PACKET(opcode) ((u32)(opcode) << 24)
#define APEX_CMS_MAX_PACKET_BYTES \
	(APEX_CMS_EDID_RESPONSE_WORDS * sizeof(u32))

#define APEX_PIXEL_CLOCK_MIN_HZ 25000000U
#define APEX_PIXEL_CLOCK_MAX_HZ 340000000U
#define APEX_SCANOUT_BANDWIDTH 1500000000ULL
#define APEX_DISPLAY_TIMEOUT_US 2000000
#define APEX_CMS_TIMEOUT_US 10000000
#define APEX_CMS_POLL_US 1000
#define APEX_HOTPLUG_POLL msecs_to_jiffies(1000)
#define APEX_CMS_HELD_LIMIT (60 * HZ)
#define APEX_CMS_RESET_BACKOFF_MAX 300
#define APEX_EDID_BACKOFF_MAX 32
#define APEX_DISPLAY_IRQ_STORM_LIMIT 1000
static char *apex_u50_edid_firmware;
module_param_named(u50_edid_firmware, apex_u50_edid_firmware, charp, 0444);
MODULE_PARM_DESC(u50_edid_firmware,
		 "explicit firmware EDID for a fixed U50 sink; physical presence remains live");

struct apex_display {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *bar0;
	/* Allocatable LOCAL extent; every scanout surface lies within it. */
	u64 local_bytes;
	struct apex_cp *cp;
	apex_display_fatal_error_t fatal_error;
	void *fatal_error_context;
	struct mutex io_lock;
	struct mutex command_lock;
	struct mutex scanout_lock;
	struct mutex state_lock;
	wait_queue_head_t command_wait;
	struct delayed_work physical_work;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	const struct drm_edid *edid;
	u32 command_id;
	u32 observed_vblank;
	unsigned long irq_window;
	u32 irq_count;
	/* Mode, LOCAL address and pitch the display engine fetches while active. */
	struct drm_display_mode mode;
	u64 surface;
	u32 pitch;
	/* The enabled CRTC's scanout is mode, surface and pitch; whenever the
	 * engine stops scanning it out (a disconnect, PHY loss, a failed flip or
	 * reset), the physical work scans it out again. */
	bool committed;
	/* Scanout stopped and scanout_lock held across a native reset. */
	bool reset_held;
	bool connected;
	bool hardware_active;
	bool physical_ready;
	bool cms_ready;
	bool cms_failed;
	bool failed;
	bool irq_disabled;
	bool registered;
	bool started;
	bool stopping;
	bool modeset_retry_pending;
	u32 cms_mailbox;
	u8 cms_communication_version;
	/* Posting time of a request the firmware has not yet released, or 0. */
	unsigned long cms_held_since;
	/* A failed mailbox resets the CMS MicroBlaze at cms_reset_at; the
	 * backoff doubles per reset without an intervening success. */
	unsigned long cms_reset_at;
	unsigned int cms_reset_backoff;
	/* EDID reads during a DDC outage cost the firmware seconds each. */
	unsigned long edid_retry_at;
	unsigned int edid_backoff;
	/* A present sink whose live EDID read failed scans out the fallback
	 * mode until a read succeeds. */
	bool edid_fallback;
};

struct apex_cp *apex_display_cp(struct drm_device *drm)
{
	return READ_ONCE(container_of(drm, struct apex_display, drm)->cp);
}

static int apex_wait_register(void __iomem *reg, u32 mask, u32 expected,
			      unsigned int timeout_us)
{
	u32 value;

	return readl_poll_timeout(reg, value, (value & mask) == expected,
				  50, timeout_us);
}

static int apex_cms_fail_locked(struct apex_display *display, int error,
				const char *reason)
{
	lockdep_assert_held(&display->io_lock);
	display->cms_reset_backoff = clamp(display->cms_reset_backoff * 2, 1U,
					   (unsigned int)APEX_CMS_RESET_BACKOFF_MAX);
	if (!display->cms_failed)
		dev_err(&display->pdev->dev,
			"CMS mailbox disabled, MicroBlaze reset in %u s: %s (%d)\n",
			display->cms_reset_backoff, reason, error);
	display->cms_ready = false;
	display->cms_failed = true;
	display->cms_reset_at = jiffies + display->cms_reset_backoff * HZ;
	return error;
}

/* PG348 MB_RESETN_REG: hold the CMS MicroBlaze in reset; the next
 * initialization releases it and waits for the register map. */
static void apex_cms_reset(struct apex_display *display)
{
	mutex_lock(&display->io_lock);
	writel(0, display->bar0 + APEX_CMS_RESET);
	display->cms_failed = false;
	display->cms_held_since = 0;
	mutex_unlock(&display->io_lock);
}

/* The firmware owns the mailbox: busy, or failed once held for 60 s. */
static int apex_cms_held_locked(struct apex_display *display)
{
	lockdep_assert_held(&display->io_lock);
	if (!display->cms_held_since)
		display->cms_held_since = jiffies ?: 1;
	else if (time_after(jiffies, display->cms_held_since + APEX_CMS_HELD_LIMIT))
		return apex_cms_fail_locked(display, -ETIMEDOUT,
					    "firmware held the mailbox for 60 s");
	return -EBUSY;
}

static int apex_cms_initialize(struct apex_display *display)
{
	u32 control, firmware, mailbox_offset, reset, status;
	int ret = 0;

	mutex_lock(&display->io_lock);
	if (display->cms_ready)
		goto out;
	if (display->cms_failed) {
		ret = -EIO;
		goto out;
	}

	reset = readl(display->bar0 + APEX_CMS_RESET);
	if (!(reset & BIT(0))) {
		writel(BIT(0), display->bar0 + APEX_CMS_RESET);
		reset = readl(display->bar0 + APEX_CMS_RESET);
		if (!(reset & BIT(0)))
			ret = apex_cms_fail_locked(display, -EIO,
						   "MicroBlaze reset did not release");
		else
			ret = -EAGAIN;
		goto out;
	}
	if (!(readl(display->bar0 + APEX_CMS_HOST_STATUS2) &
	      APEX_CMS_REG_MAP_READY)) {
		ret = -EAGAIN;
		goto out;
	}
	if (readl(display->bar0 + APEX_CMS_REG_MAP_ID) !=
	    APEX_CMS_REG_MAP_ID_VALUE) {
		ret = apex_cms_fail_locked(display, -EPROTO,
						   "firmware reported an invalid register-map identity");
		goto out;
	}
	if (readl(display->bar0 + APEX_CMS_PROFILE) != APEX_CMS_U50_PROFILE) {
		ret = apex_cms_fail_locked(display, -EPROTO,
						   "firmware profile is not U50");
		goto out;
	}
	status = readl(display->bar0 + APEX_CMS_STATUS);
	if (APEX_CMS_SC_MODE(status) != APEX_CMS_SC_MODE_NORMAL) {
		ret = -EAGAIN;
		goto out;
	}
	control = readl(display->bar0 + APEX_CMS_CONTROL);
	if (control & APEX_CMS_MAILBOX_OWNER) {
		ret = apex_cms_held_locked(display);
		goto out;
	}
	display->cms_held_since = 0;
	mailbox_offset = readl(display->bar0 + APEX_CMS_HOST_MESSAGE_OFFSET);
	if (mailbox_offset == APEX_CMS_INVALID_MAILBOX ||
	    !IS_ALIGNED(mailbox_offset, sizeof(u32)) ||
	    mailbox_offset < APEX_CMS_REG_MAP_BYTES ||
	    mailbox_offset > APEX_CMS_WINDOW_BYTES - APEX_CMS_MAX_PACKET_BYTES) {
		ret = apex_cms_fail_locked(display, -EPROTO,
					   "firmware reported an invalid mailbox offset");
		goto out;
	}
	display->cms_mailbox = APEX_CMS_BAR_BASE + mailbox_offset;
	/* PG348: U50 HBM temperature reporting needs CONTROL_REG bit 27. */
	writel(control | APEX_CMS_HBM_TEMPERATURE, display->bar0 + APEX_CMS_CONTROL);
	display->cms_communication_version = APEX_CMS_COMM_VERSION(status);
	display->cms_ready = true;
	firmware = readl(display->bar0 + APEX_CMS_FW_VERSION);
	dev_info(&display->pdev->dev,
		 "U50 CMS firmware 0x%08x ready: SC communication version %u, mailbox %#x\n",
		 firmware, APEX_CMS_COMM_VERSION(status), display->cms_mailbox);
out:
	mutex_unlock(&display->io_lock);
	return ret;
}

static int apex_cms_command(struct apex_display *display,
			    u8 opcode, const u32 *packet,
			    unsigned int request_words, void *response,
			    size_t response_bytes)
{
	u32 control, error, header, message_error;
	int ret = 0;

	if (!packet || !request_words || !response ||
	    response_bytes < sizeof(u32) ||
	    response_bytes > APEX_CMS_MAX_PACKET_BYTES ||
	    packet[0] != APEX_CMS_PACKET(opcode))
		return -EINVAL;
	if (request_words * sizeof(packet[0]) > APEX_CMS_MAX_PACKET_BYTES)
		return -EINVAL;

	mutex_lock(&display->io_lock);
	if (!display->cms_ready || display->cms_failed) {
		ret = -EIO;
		goto out;
	}
	/*
	 * The firmware services host packets between its own satellite
	 * controller transactions, so a request can take longer than the
	 * poll period. A late release and a mailbox still owned by the
	 * firmware are transient conditions; only protocol violations
	 * disable the mailbox.
	 */
	control = readl(display->bar0 + APEX_CMS_CONTROL);
	if (control & APEX_CMS_MAILBOX_OWNER) {
		dev_warn_ratelimited(&display->pdev->dev,
				     "CMS mailbox still owned by firmware\n");
		ret = apex_cms_held_locked(display);
		goto out;
	}
	display->cms_held_since = 0;
	/* Errors present now belong to an abandoned, timed-out request. */
	if ((readl(display->bar0 + APEX_CMS_ERROR) & APEX_CMS_PACKET_ERROR) ||
	    readl(display->bar0 + APEX_CMS_HOST_MESSAGE_ERROR)) {
		writel(control | APEX_CMS_CLEAR_ERROR,
		       display->bar0 + APEX_CMS_CONTROL);
		control = readl(display->bar0 + APEX_CMS_CONTROL);
	}
	memcpy_toio(display->bar0 + display->cms_mailbox, packet,
		    request_words * sizeof(packet[0]));
	wmb();
	writel(control | APEX_CMS_MAILBOX_OWNER,
	       display->bar0 + APEX_CMS_CONTROL);
	display->cms_held_since = jiffies ?: 1;
	ret = readl_poll_timeout(display->bar0 + APEX_CMS_CONTROL, control,
				 !(control & APEX_CMS_MAILBOX_OWNER),
				 APEX_CMS_POLL_US, APEX_CMS_TIMEOUT_US);
	if (ret) {
		dev_warn_ratelimited(&display->pdev->dev,
				     "CMS firmware did not release the mailbox within %u ms\n",
				     APEX_CMS_TIMEOUT_US / 1000);
		goto out;
	}
	display->cms_held_since = 0;
	error = readl(display->bar0 + APEX_CMS_ERROR);
	message_error = readl(display->bar0 + APEX_CMS_HOST_MESSAGE_ERROR);
	if ((error & APEX_CMS_PACKET_ERROR) || message_error) {
		control = readl(display->bar0 + APEX_CMS_CONTROL);
		writel(control | APEX_CMS_CLEAR_ERROR,
		       display->bar0 + APEX_CMS_CONTROL);
		dev_err_ratelimited(&display->pdev->dev,
				    "CMS rejected QSFP request: error=%u status=0x%08x\n",
				    message_error, error);
		ret = -EREMOTEIO;
		goto out;
	}
	header = readl(display->bar0 + display->cms_mailbox);
	if (header != APEX_CMS_PACKET(opcode)) {
		ret = apex_cms_fail_locked(display, -EPROTO,
					   "firmware returned a malformed packet header");
		goto out;
	}
	memcpy_fromio(response, display->bar0 + display->cms_mailbox,
		      response_bytes);
	display->cms_reset_backoff = 0;
out:
	mutex_unlock(&display->io_lock);
	return ret;
}

static int apex_u50_connected(struct apex_display *display, bool *connected)
{
	u32 packet[2] = { APEX_CMS_PACKET(APEX_CMS_QSFP_IO_READ) };
	u32 response[3];
	int ret;

	ret = apex_cms_command(display, APEX_CMS_QSFP_IO_READ, packet,
			       ARRAY_SIZE(packet), response, sizeof(response));
	if (ret)
		return ret;
	*connected = !(response[2] & BIT(3));
	writel(*connected, display->bar0 + APEX_DISPLAY_HOTPLUG);
	return 0;
}

static int apex_u50_read_edid_block(struct apex_display *display, u8 *data,
					    unsigned int block)
{
	u32 packet[APEX_CMS_EDID_REQUEST_WORDS];
	u32 response[APEX_CMS_EDID_RESPONSE_WORDS];
	unsigned int byte;
	int ret;

	/*
	 * SC 5.2.20 communication protocol 9 accepts the module byte-read
	 * command for the Plugcat's shared HDMI DDC bus. On the qualified
	 * Plugcat/JetKVM connection, its block-read command instead returned 128
	 * bytes of invalid data.
	 */
	for (byte = 0; byte < EDID_LENGTH; byte++) {
		ret = apex_cms_edid_byte_request(packet, block, byte);
		if (ret)
			return ret;
		ret = apex_cms_command(display, APEX_CMS_MODULE_BYTE_READ, packet,
				       ARRAY_SIZE(packet), response, sizeof(response));
		if (ret) {
			dev_err_ratelimited(&display->pdev->dev,
					    "CMS EDID byte read failed at block %u offset %u: %d\n",
					    block, byte, ret);
			return ret;
		}
		ret = apex_cms_edid_byte_response(response, &data[byte]);
		if (ret) {
			dev_err_ratelimited(&display->pdev->dev,
					    "CMS returned malformed EDID byte at block %u offset %u\n",
					    block, byte);
			return ret;
		}
	}
	return 0;
}

/*
 * The protocol 9 byte interface addresses only the unpaged 256 bytes and has
 * no E-DDC segment pointer, so the base block may declare at most one
 * extension. An HF-EEODB count beyond those blocks is truncated: DRM parses
 * only the allocated blocks, but drm_edid_valid() rejects the larger count.
 */
static const struct drm_edid *apex_u50_live_edid(struct apex_display *display)
{
	u8 data[2 * EDID_LENGTH];
	size_t size = EDID_LENGTH;

	if (apex_u50_read_edid_block(display, data, 0))
		return NULL;
	if (!apex_cms_edid_extension_count_supported(data[126])) {
		dev_err_ratelimited(&display->pdev->dev,
				    "U50 CMS byte interface cannot address %u EDID extension blocks\n",
				    data[126]);
		return NULL;
	}
	if (data[126]) {
		if (apex_u50_read_edid_block(display, data + EDID_LENGTH, 1))
			return NULL;
		size += EDID_LENGTH;
	}
	if (!drm_edid_is_valid((struct edid *)data))
		return NULL;
	return drm_edid_alloc(data, size);
}

static int apex_u50_gt_rate(u32 pixel_clock, u32 *config, u32 *fraction,
			    u8 *sample_rate)
{
	u64 line_rate, scaled, vco;
	u8 feedback, link_div, pll_div, rate, video_div;
	bool qpll1;

	if (pixel_clock < APEX_PIXEL_CLOCK_MIN_HZ ||
	    pixel_clock > APEX_PIXEL_CLOCK_MAX_HZ)
		return -ERANGE;
	*sample_rate = pixel_clock < APEX_DISPLAY_GT_SAMPLE_THRESHOLD_HZ ?
			       2 : 1;
	line_rate = (u64)pixel_clock * 10 * *sample_rate;
	if (line_rate <= 812500000ULL) {
		qpll1 = true;
		pll_div = 16;
	} else if (line_rate <= 1000000000ULL) {
		qpll1 = false;
		pll_div = 16;
	} else if (line_rate <= 1625000000ULL) {
		qpll1 = true;
		pll_div = 8;
	} else if (line_rate <= 2000000000ULL) {
		qpll1 = false;
		pll_div = 8;
	} else if (line_rate <= 3250000000ULL) {
		qpll1 = true;
		pll_div = 4;
	} else if (line_rate <= 3400000000ULL) {
		qpll1 = false;
		pll_div = 4;
	} else {
		return -ERANGE;
	}

	if (pll_div == 16) {
		rate = 0x5;
		link_div = 3;
		video_div = *sample_rate == 2 ? 7 : 3;
	} else if (pll_div == 8) {
		rate = 0x4;
		link_div = 1;
		video_div = 1;
	} else {
		rate = 0x3;
		link_div = 0;
		video_div = 0;
	}
	vco = line_rate * pll_div;
	scaled = DIV_ROUND_CLOSEST_ULL(vco * BIT_ULL(25),
				       APEX_DISPLAY_U50_GT_REFCLK_X2_HZ);
	feedback = scaled >> 24;
	*fraction = scaled & GENMASK(23, 0);
	*config = qpll1 | (u32)rate << 1 | (u32)link_div << 4 |
		  (u32)video_div << 7 | (u32)feedback << 10;
	return 0;
}

static int apex_display_gt_disable(struct apex_display *display)
{
	u32 status;

	writel(APEX_DISPLAY_GT_DISABLE,
	       display->bar0 + APEX_DISPLAY_GT_CONTROL);
	return readl_poll_timeout(display->bar0 + APEX_DISPLAY_GT_STATUS, status,
				  (status & APEX_DISPLAY_GT_PRESENT) &&
				  !(status & (APEX_DISPLAY_GT_BUSY |
					      APEX_DISPLAY_GT_READY |
					      APEX_DISPLAY_GT_ERROR)),
				  50, APEX_DISPLAY_TIMEOUT_US);
}

static int apex_display_gt_start(struct apex_display *display, u32 config,
				 u32 fraction)
{
	u32 status;
	int ret;

	writel(config, display->bar0 + APEX_DISPLAY_GT_CONFIG);
	writel(fraction, display->bar0 + APEX_DISPLAY_GT_FRACTION);
	writel(APEX_DISPLAY_GT_START, display->bar0 + APEX_DISPLAY_GT_CONTROL);
	ret = readl_poll_timeout(display->bar0 + APEX_DISPLAY_GT_STATUS,
				 status,
				 status & (APEX_DISPLAY_GT_READY |
					   APEX_DISPLAY_GT_ERROR),
				 50, APEX_DISPLAY_TIMEOUT_US);
	if (ret) {
		dev_err_ratelimited(&display->pdev->dev,
				    "display GT rate change timed out: status=0x%08x\n",
				    status);
		return ret;
	}
	if (status & APEX_DISPLAY_GT_ERROR) {
		dev_err_ratelimited(&display->pdev->dev,
				    "display GT rate change failed: status=0x%08x code=%u\n",
				    status, APEX_DISPLAY_GT_ERROR_CODE(status));
		return -EIO;
	}
	return 0;
}

static int apex_u50_gt_program(struct apex_display *display, u32 pixel_clock,
			       u8 *sample_rate)
{
	u32 config, fraction;
	int ret;

	ret = apex_u50_gt_rate(pixel_clock, &config, &fraction, sample_rate);
	if (ret)
		return ret;
	ret = apex_display_gt_disable(display);
	if (ret)
		return ret;
	ret = apex_display_gt_start(display, config, fraction);
	if (ret)
		return ret;
	dev_dbg(&display->pdev->dev,
		"U50 display GT configured for %u Hz, sample factor %u\n",
		pixel_clock, *sample_rate);
	return 0;
}

static int apex_service_phy(struct apex_display *display)
{
	u32 status, pixel_clock;
	u8 sample_rate = 1;
	int ret;

	if (READ_ONCE(display->failed))
		return -EIO;
	if (!display->physical_ready)
		return -ENODEV;
	status = readl(display->bar0 + APEX_DISPLAY_PHY_STATUS);
	if (!(status & APEX_DISPLAY_PHY_PENDING))
		return 0;
	if (status & APEX_DISPLAY_PHY_ENABLE) {
		pixel_clock = readl(display->bar0 + APEX_DISPLAY_PHY_CLOCK);
		ret = apex_u50_gt_program(display, pixel_clock, &sample_rate);
	} else {
		ret = apex_display_gt_disable(display);
	}
	writel(ret ? 0 : BIT(0) | sample_rate << 1,
	       display->bar0 + APEX_DISPLAY_PHY_COMPLETION);
	return ret;
}

/* A command the engine never completes leaves through a link-down cycle:
 * with its rate controller disabled the engine stops fetch, fails the
 * command and returns idle and disabled, and the physical poll scans the
 * committed mode out again. An engine that stays busy fails the display
 * alone; rendering and LOCAL mappings stay up. */
static int apex_display_abandon(struct apex_display *display)
{
	unsigned long deadline = jiffies + usecs_to_jiffies(APEX_DISPLAY_TIMEOUT_US);
	u32 status;
	int ret;

	mutex_lock(&display->state_lock);
	ret = apex_display_gt_disable(display);
	mutex_unlock(&display->state_lock);
	for (;;) {
		status = readl(display->bar0 + APEX_DISPLAY_STATUS);
		if (ret || !(status & APEX_DISPLAY_COMMAND_BUSY) ||
		    (status & APEX_DISPLAY_COMPLETION_VALID))
			break;
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		mutex_lock(&display->state_lock);
		apex_service_phy(display);
		mutex_unlock(&display->state_lock);
		usleep_range(50, 100);
	}
	if (ret) {
		dev_err(&display->pdev->dev,
			"display engine stayed busy through a link-down cycle: status=0x%08x; display disabled until reload\n",
			status);
		apex_display_poison(display);
		return -EIO;
	}
	if (status & APEX_DISPLAY_COMPLETION_VALID)
		writel(readl(display->bar0 + APEX_DISPLAY_COMPLETED_ID),
		       display->bar0 + APEX_DISPLAY_COMPLETION_ACK);
	dev_warn(&display->pdev->dev, "display left the stalled command through a link-down cycle\n");
	return -ETIMEDOUT;
}

static int apex_display_command(struct apex_display *display, u32 command,
				const struct drm_display_mode *mode, u64 base,
				u32 pitch)
{
	unsigned long deadline = jiffies + usecs_to_jiffies(APEX_DISPLAY_TIMEOUT_US);
	u32 id, status, completed_id, result, flags = 0;
	int ret = 0;

	mutex_lock(&display->command_lock);
	if (READ_ONCE(display->failed)) {
		ret = -EIO;
		goto out;
	}
	status = readl(display->bar0 + APEX_DISPLAY_STATUS);
	if (status & APEX_DISPLAY_COMPLETION_VALID) {
		completed_id = readl(display->bar0 + APEX_DISPLAY_COMPLETED_ID);
		writel(completed_id, display->bar0 + APEX_DISPLAY_COMPLETION_ACK);
		ret = apex_wait_register(display->bar0 + APEX_DISPLAY_STATUS,
					 APEX_DISPLAY_COMPLETION_VALID, 0,
					 APEX_DISPLAY_TIMEOUT_US);
		if (ret)
			goto out;
		status = readl(display->bar0 + APEX_DISPLAY_STATUS);
	}
	if (status & APEX_DISPLAY_COMMAND_BUSY) {
		ret = -EBUSY;
		goto out;
	}
	id = ++display->command_id;
	writel(id, display->bar0 + APEX_DISPLAY_ID);
	writel(command, display->bar0 + APEX_DISPLAY_COMMAND);
	if (mode) {
		writel(mode->clock * 1000, display->bar0 + APEX_DISPLAY_PIXEL_CLOCK);
		writel(mode->hdisplay |
		       ((u32)(mode->hsync_start - mode->hdisplay) << 16),
		       display->bar0 + APEX_DISPLAY_H_ACTIVE_FRONT);
		writel((mode->hsync_end - mode->hsync_start) |
		       ((u32)(mode->htotal - mode->hsync_end) << 16),
		       display->bar0 + APEX_DISPLAY_H_SYNC_BACK);
		writel(mode->vdisplay |
		       ((u32)(mode->vsync_start - mode->vdisplay) << 16),
		       display->bar0 + APEX_DISPLAY_V_ACTIVE_FRONT);
		writel((mode->vsync_end - mode->vsync_start) |
		       ((u32)(mode->vtotal - mode->vsync_end) << 16),
		       display->bar0 + APEX_DISPLAY_V_SYNC_BACK);
		if (mode->flags & DRM_MODE_FLAG_PHSYNC)
			flags |= BIT(0);
		if (mode->flags & DRM_MODE_FLAG_PVSYNC)
			flags |= BIT(1);
		writel(flags, display->bar0 + APEX_DISPLAY_MODE_FLAGS);
		writel(lower_32_bits(base), display->bar0 + APEX_DISPLAY_SURFACE_LOW);
		writel(upper_32_bits(base), display->bar0 + APEX_DISPLAY_SURFACE_HIGH);
		writel(pitch, display->bar0 + APEX_DISPLAY_PITCH);
		writel(0, display->bar0 + APEX_DISPLAY_SAFE_COLOR);
	}
	writel(1, display->bar0 + APEX_DISPLAY_DOORBELL);

	for (;;) {
		if (READ_ONCE(display->failed) || READ_ONCE(display->stopping)) {
			ret = -ENODEV;
			goto out;
		}
		mutex_lock(&display->state_lock);
		apex_service_phy(display);
		mutex_unlock(&display->state_lock);
		status = readl(display->bar0 + APEX_DISPLAY_STATUS);
		completed_id = readl(display->bar0 + APEX_DISPLAY_COMPLETED_ID);
		if ((status & APEX_DISPLAY_COMPLETION_VALID) && completed_id == id)
			break;
		if (time_after_eq(jiffies, deadline)) {
			dev_err(&display->pdev->dev,
				"display command %u id %u timed out: status=0x%08x phy=0x%08x vblank=%u underflows=%u read-errors=%u\n",
				command, id, status,
				readl(display->bar0 + APEX_DISPLAY_PHY_STATUS),
				readl(display->bar0 + APEX_DISPLAY_VBLANK_COUNT),
				readl(display->bar0 + APEX_DISPLAY_UNDERFLOW_LINES),
				readl(display->bar0 + APEX_DISPLAY_READ_ERRORS));
			ret = apex_display_abandon(display);
			goto out;
		}
		wait_event_timeout(display->command_wait,
			READ_ONCE(display->failed) ||
			READ_ONCE(display->stopping) ||
			(readl(display->bar0 + APEX_DISPLAY_STATUS) &
				APEX_DISPLAY_COMPLETION_VALID) ||
			(readl(display->bar0 + APEX_DISPLAY_PHY_STATUS) &
				APEX_DISPLAY_PHY_PENDING),
			deadline - jiffies);
	}
	result = readl(display->bar0 + APEX_DISPLAY_COMPLETED_RESULT) & 7;
	writel(completed_id, display->bar0 + APEX_DISPLAY_COMPLETION_ACK);
	if (result != APEX_DISPLAY_RESULT_SUCCESS) {
		dev_err(&display->pdev->dev,
			"display command %u id %u failed: result=%u status=0x%08x\n",
			command, id, result, status);
		ret = -EIO;
	}
out:
	mutex_unlock(&display->command_lock);
	return ret;
}

static enum drm_mode_status apex_mode_valid(struct apex_display *display,
					    const struct drm_display_mode *mode)
{
	u64 line_bytes, pitch, extent, frame_bytes, bandwidth;

	if (mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN |
			   DRM_MODE_FLAG_3D_MASK))
		return MODE_NO_INTERLACE;
	if (mode->clock < APEX_PIXEL_CLOCK_MIN_HZ / 1000)
		return MODE_CLOCK_LOW;
	if (mode->clock > APEX_PIXEL_CLOCK_MAX_HZ / 1000)
		return MODE_CLOCK_HIGH;
	if (mode->hdisplay <= 0 || mode->hsync_start <= mode->hdisplay ||
	    mode->hsync_end <= mode->hsync_start || mode->htotal <= mode->hsync_end ||
	    mode->vdisplay <= 0 || mode->vsync_start <= mode->vdisplay ||
	    mode->vsync_end <= mode->vsync_start || mode->vtotal <= mode->vsync_end ||
	    mode->htotal > U16_MAX || mode->vtotal > U16_MAX)
		return MODE_BAD;
	if (mode->hdisplay > S16_MAX)
		return MODE_BAD_HVALUE;
	if (mode->vdisplay > S16_MAX)
		return MODE_BAD_VVALUE;
	line_bytes = ALIGN((u64)mode->hdisplay * 4, 64);
	pitch = line_bytes;
	extent = pitch * (mode->vdisplay - 1) + line_bytes;
	if (extent > display->local_bytes)
		return MODE_MEM;
	frame_bytes = line_bytes * mode->vdisplay;
	bandwidth = mul_u64_u64_div_u64(frame_bytes, (u64)mode->clock * 1000,
					 (u64)mode->htotal * mode->vtotal);
	if (bandwidth > APEX_SCANOUT_BANDWIDTH)
		return MODE_BAD;
	return MODE_OK;
}

static enum drm_mode_status
apex_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct apex_display *display = container_of(crtc, struct apex_display, crtc);

	return apex_mode_valid(display, mode);
}

static enum drm_mode_status
apex_connector_mode_valid(struct drm_connector *connector,
			  APEX_MODE_CONST struct drm_display_mode *mode)
{
	struct apex_display *display = container_of(connector, struct apex_display,
							 connector);

	return apex_mode_valid(display, mode);
}

static enum drm_connector_status
apex_connector_detect(struct drm_connector *connector, bool force)
{
	struct apex_display *display = container_of(connector, struct apex_display,
							 connector);
	bool connected;
	int idx;

	if (READ_ONCE(display->failed) || !drm_dev_enter(&display->drm, &idx))
		return connector_status_disconnected;
	mutex_lock(&display->state_lock);
	connected = display->physical_ready && display->connected &&
		    (display->edid || display->edid_fallback);
	mutex_unlock(&display->state_lock);
	drm_dev_exit(idx);
	return connected ? connector_status_connected : connector_status_disconnected;
}

/* CEA-861 VIC 16, 1920x1080 at 60 Hz: every HDMI sink accepts it. */
static const struct drm_display_mode apex_fallback_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008, 2052, 2200, 0,
		 1080, 1084, 1089, 1125, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	.picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
};

static int apex_connector_get_modes(struct drm_connector *connector)
{
	struct apex_display *display = container_of(connector, struct apex_display,
							 connector);
	const struct drm_edid *drm_edid;
	bool fallback;
	int idx;
	int modes;

	if (READ_ONCE(display->failed) || !drm_dev_enter(&display->drm, &idx))
		return 0;
	mutex_lock(&display->state_lock);
	drm_edid = drm_edid_dup(display->edid);
	fallback = display->edid_fallback;
	mutex_unlock(&display->state_lock);
	if (!drm_edid) {
		struct drm_display_mode *mode = NULL;

		drm_edid_connector_update(connector, NULL);
		if (fallback)
			mode = drm_mode_duplicate(connector->dev, &apex_fallback_mode);
		if (mode) {
			mode->type |= DRM_MODE_TYPE_PREFERRED;
			drm_mode_probed_add(connector, mode);
		}
		drm_dev_exit(idx);
		return mode ? 1 : 0;
	}
	drm_edid_connector_update(connector, drm_edid);
	modes = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);
	drm_dev_exit(idx);
	return modes;
}

static const struct drm_connector_helper_funcs apex_connector_helper_funcs = {
	.get_modes = apex_connector_get_modes,
	.mode_valid = apex_connector_mode_valid,
};

static const struct drm_connector_funcs apex_connector_funcs = {
	.detect = apex_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* The engine fetches whole 64-byte lines of the source rectangle straight
 * from the framebuffer's LOCAL object. */
static int apex_validate_surface(struct drm_plane_state *plane_state, u64 *base,
				 u32 *pitch)
{
	struct drm_framebuffer *fb = plane_state->fb;
	const struct drm_rect *source = &plane_state->src;
	struct drm_gem_object *object;
	u64 local, offset, extent;

	if (!fb || source->x1 < 0 || source->y1 < 0 ||
	    source->x2 < 0 || source->y2 < 0 ||
	    (u32)(source->x2 >> 16) > fb->width ||
	    (u32)(source->y2 >> 16) > fb->height ||
	    ((source->x1 | source->y1 | source->x2 | source->y2) & 0xffff) ||
	    drm_rect_width(source) <= 0 || drm_rect_height(source) <= 0)
		return -EINVAL;
	object = drm_gem_fb_get_obj(fb, 0);
	if (!object || !apex_cp_gem_local(object, &local))
		return -EINVAL;
	offset = fb->offsets[0] + (u64)(source->y1 >> 16) * fb->pitches[0] +
		 (u64)(source->x1 >> 16) * 4;
	extent = (u64)fb->pitches[0] * ((drm_rect_height(source) >> 16) - 1) +
		 ALIGN((u64)(drm_rect_width(source) >> 16) * 4, 64);
	if (!IS_ALIGNED(offset | fb->pitches[0], 64) ||
	    extent > object->size || offset > object->size - extent)
		return -EINVAL;
	if (base)
		*base = local + offset;
	if (pitch)
		*pitch = fb->pitches[0];
	return 0;
}

/* The primary plane covers the whole CRTC without scaling. */
static int apex_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_state *state)
{
	struct apex_display *display = container_of(plane, struct apex_display, plane);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, &display->crtc);
	bool ready;
	int ret;

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret || !plane_state->visible)
		return ret;
	if (READ_ONCE(display->failed))
		return -ENODEV;
	mutex_lock(&display->state_lock);
	ready = display->physical_ready && display->connected &&
		(display->edid || display->edid_fallback);
	mutex_unlock(&display->state_lock);
	if (!ready)
		return -ENODEV;
	return apex_validate_surface(plane_state, NULL, NULL);
}

static void apex_complete_crtc_event(struct drm_crtc *crtc, bool on_vblank)
{
	unsigned long flags;
	bool armed = false;
	int ret = -EINVAL;

	if (on_vblank)
		ret = drm_crtc_vblank_get(crtc);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (crtc->state->event) {
		if (!ret) {
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);
			armed = true;
		} else {
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		}
		crtc->state->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	if (!ret && !armed)
		drm_crtc_vblank_put(crtc);
}

static void apex_schedule_modeset_retry(struct apex_display *display)
{
	bool schedule;

	mutex_lock(&display->state_lock);
	schedule = !display->stopping && !READ_ONCE(display->failed);
	if (schedule)
		display->modeset_retry_pending = true;
	mutex_unlock(&display->state_lock);
	if (schedule)
		mod_delayed_work(system_wq, &display->physical_work, 0);
}

/* An enabled CRTC scans out its primary plane. */
static int apex_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (crtc_state->enable) {
		ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
		if (ret)
			return ret;
	}
	return drm_atomic_add_affected_planes(state, crtc);
}

static void apex_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct apex_display *display = container_of(crtc, struct apex_display, crtc);
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_plane_state *plane_state = display->plane.state;
	u64 base;
	u32 pitch;
	int idx;
	int ret;

	mutex_lock(&display->scanout_lock);
	WRITE_ONCE(display->hardware_active, false);
	if (READ_ONCE(display->failed) || !drm_dev_enter(&display->drm, &idx))
		goto failed;
	ret = apex_validate_surface(plane_state, &base, &pitch);
	display->committed = !ret;
	if (!ret) {
		drm_mode_copy(&display->mode, &crtc_state->adjusted_mode);
		display->surface = base;
		display->pitch = pitch;
		ret = apex_display_command(display, APEX_DISPLAY_CMD_MODESET,
					   &crtc_state->adjusted_mode, base, pitch);
	}
	if (ret) {
		apex_display_command(display, APEX_DISPLAY_CMD_DISABLE, NULL, 0, 0);
		dev_err(&display->pdev->dev, "display modeset failed: %d\n", ret);
		apex_schedule_modeset_retry(display);
	} else {
		WRITE_ONCE(display->hardware_active, true);
		drm_crtc_vblank_on(crtc);
		drm_dev_exit(idx);
		mutex_unlock(&display->scanout_lock);
		apex_complete_crtc_event(crtc, true);
		return;
	}
	drm_dev_exit(idx);
failed:
	mutex_unlock(&display->scanout_lock);
	apex_complete_crtc_event(crtc, false);
}

static void apex_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct apex_display *display = container_of(crtc, struct apex_display, crtc);
	int idx;
	int ret;

	mutex_lock(&display->scanout_lock);
	drm_crtc_vblank_off(crtc);
	WRITE_ONCE(display->hardware_active, false);
	display->committed = false;
	if (!drm_dev_enter(&display->drm, &idx))
		goto complete_event;
	ret = apex_display_command(display, APEX_DISPLAY_CMD_DISABLE, NULL, 0, 0);
	if (ret)
		dev_err(&display->pdev->dev, "display disable failed: %d\n", ret);
	drm_dev_exit(idx);
complete_event:
	mutex_unlock(&display->scanout_lock);
	if (!crtc->state->active)
		apex_complete_crtc_event(crtc, false);
}

/* Prepare_fb waited for the framebuffer's fences; a flip completes once the
 * engine retires the previous surface at a frame boundary, after which the
 * atomic helpers may release its framebuffer. That boundary is the vblank the
 * new surface latched on, so its event is sent at once: arming it would
 * report every flip a frame late and halve the compositor's frame rate. */
static void apex_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct apex_display *display = container_of(plane, struct apex_display, plane);
	struct drm_crtc *crtc = &display->crtc;
	struct drm_plane_state *plane_state = plane->state;
	u64 base;
	u32 pitch;
	int idx;
	int ret;

	mutex_lock(&display->scanout_lock);
	if (READ_ONCE(display->failed))
		goto complete_event;
	if (!plane_state->visible || !plane_state->fb)
		goto complete_event;
	if (drm_atomic_crtc_needs_modeset(crtc->state)) {
		mutex_unlock(&display->scanout_lock);
		return;
	}
	if (!READ_ONCE(display->hardware_active))
		goto complete_event;
	ret = apex_validate_surface(plane_state, &base, &pitch);
	if (ret)
		goto failed;
	if (base == display->surface && pitch == display->pitch)
		goto arm_event;
	if (!drm_dev_enter(&display->drm, &idx))
		goto complete_event;
	ret = apex_display_command(display, APEX_DISPLAY_CMD_FLIP,
				   &crtc->state->adjusted_mode, base, pitch);
	drm_dev_exit(idx);
	/* A failed flip still commits its surface for the restore. */
	display->surface = base;
	display->pitch = pitch;
	if (ret)
		goto failed;
	goto complete_event;
arm_event:
	mutex_unlock(&display->scanout_lock);
	apex_complete_crtc_event(crtc, true);
	return;
failed:
	dev_err_ratelimited(&display->pdev->dev, "display flip failed: %d\n", ret);
	WRITE_ONCE(display->hardware_active, false);
	drm_crtc_vblank_off(crtc);
complete_event:
	mutex_unlock(&display->scanout_lock);
	apex_complete_crtc_event(crtc, false);
}

static int apex_enable_vblank(struct drm_crtc *crtc)
{
	struct apex_display *display = container_of(crtc, struct apex_display, crtc);

	return READ_ONCE(display->hardware_active) &&
	       !READ_ONCE(display->failed) ? 0 : -EINVAL;
}

static void apex_disable_vblank(struct drm_crtc *crtc)
{
}

/* The engine counts every vblank in VBLANK_COUNT, with or without the
 * interrupt, so DRM counts from it across vblank-off periods. A failed or
 * unplugged device reports the last count the interrupt saw. */
static u32 apex_get_vblank_counter(struct drm_crtc *crtc)
{
	struct apex_display *display = container_of(crtc, struct apex_display, crtc);
	u32 count;
	int idx;

	if (READ_ONCE(display->failed) || !drm_dev_enter(&display->drm, &idx))
		return READ_ONCE(display->observed_vblank);
	count = readl(display->bar0 + APEX_DISPLAY_VBLANK_COUNT);
	drm_dev_exit(idx);
	return count;
}

static const struct drm_crtc_helper_funcs apex_crtc_helper_funcs = {
	.mode_valid = apex_crtc_mode_valid,
	.atomic_check = apex_crtc_atomic_check,
	.atomic_enable = apex_crtc_atomic_enable,
	.atomic_disable = apex_crtc_atomic_disable,
};

static const struct drm_crtc_funcs apex_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = apex_enable_vblank,
	.disable_vblank = apex_disable_vblank,
	.get_vblank_counter = apex_get_vblank_counter,
};

static const struct drm_plane_helper_funcs apex_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = apex_plane_atomic_check,
	.atomic_update = apex_plane_atomic_update,
};

static bool apex_format_mod_supported(struct drm_plane *plane, u32 format, u64 modifier)
{
	return modifier == DRM_FORMAT_MOD_LINEAR;
}

static const struct drm_plane_funcs apex_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported = apex_format_mod_supported,
};

/* The primary plane is opaque: the engine ignores the top byte, so ARGB8888
 * scans out as XRGB8888 and compositors can show client buffers of either. */
static const u32 apex_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
static const u64 apex_modifiers[] = { DRM_FORMAT_MOD_LINEAR,
				      DRM_FORMAT_MOD_INVALID };

static struct drm_framebuffer *
apex_fb_create(struct drm_device *drm, struct drm_file *file,
#if APEX_FB_CREATE_HAS_INFO
	       const struct drm_format_info *info,
#endif
	       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer *fb;
	u64 local;

	/* Scanout reads the object in place: only single-range LOCAL objects of
	 * this device, including PRIME imports of their exports, with 64-byte
	 * aligned offset and pitch qualify. */
#if APEX_FB_CREATE_HAS_INFO
	fb = drm_gem_fb_create_with_dirty(drm, file, info, mode_cmd);
#else
	fb = drm_gem_fb_create_with_dirty(drm, file, mode_cmd);
#endif
	if (IS_ERR(fb))
		return fb;
	if (!apex_cp_gem_local(drm_gem_fb_get_obj(fb, 0), &local) ||
	    !IS_ALIGNED(fb->offsets[0] | fb->pitches[0], 64)) {
		drm_framebuffer_put(fb);
		return ERR_PTR(-EINVAL);
	}
	return fb;
}

/* A flip returns once the engine has retired the previous surface, so the
 * commit releases it at flip-done rather than after another vblank. */
static void apex_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, 0);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_flip_done(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}

static const struct drm_encoder_funcs apex_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_mode_config_helper_funcs apex_mode_config_helpers = {
	.atomic_commit_tail = apex_commit_tail,
};

static const struct drm_mode_config_funcs apex_mode_config_funcs = {
	.fb_create = apex_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int apex_display_register(struct apex_display *display, struct apex_cp *cp)
{
	int ret;

	WRITE_ONCE(display->cp, cp);
	ret = drm_dev_register(&display->drm, 0);
	if (ret)
		return ret;
	WRITE_ONCE(display->registered, true);
	return 0;
}

static const struct drm_edid *apex_u50_firmware_edid(struct apex_display *display)
{
	const struct firmware *firmware;
	const struct drm_edid *edid;
	int ret;

	ret = request_firmware_direct(&firmware, apex_u50_edid_firmware,
				      &display->pdev->dev);
	if (ret) {
		dev_err_ratelimited(&display->pdev->dev,
			"U50 EDID firmware %s unavailable: %d\n",
			apex_u50_edid_firmware, ret);
		return NULL;
	}
	edid = drm_edid_alloc(firmware->data, firmware->size);
	release_firmware(firmware);
	if (!edid || !drm_edid_valid(edid)) {
		drm_edid_free(edid);
		return NULL;
	}
	dev_info(&display->pdev->dev, "using explicit U50 EDID firmware %s\n",
		 apex_u50_edid_firmware);
	return edid;
}

/* Scans out the committed mode and surface again after the engine stopped:
 * compositors need not handle a link-status change or commit again. A
 * failure leaves the scanout to the next poll. */
static void apex_display_restore(struct apex_display *display)
{
	bool active;
	int idx, ret;

	mutex_lock(&display->scanout_lock);
	if (!display->committed || display->reset_held || READ_ONCE(display->failed) ||
	    (readl(display->bar0 + APEX_DISPLAY_STATUS) &
	     (APEX_DISPLAY_ENABLED | APEX_DISPLAY_COMMAND_BUSY)) ||
	    !drm_dev_enter(&display->drm, &idx)) {
		mutex_unlock(&display->scanout_lock);
		return;
	}
	ret = apex_display_command(display, APEX_DISPLAY_CMD_MODESET, &display->mode,
				   display->surface, display->pitch);
	/* A mode the engine cannot finish waits for the next commit. */
	if (ret == -ETIMEDOUT)
		display->committed = false;
	active = !ret;
	if (active != READ_ONCE(display->hardware_active)) {
		WRITE_ONCE(display->hardware_active, active);
		if (READ_ONCE(display->registered)) {
			if (active)
				drm_crtc_vblank_on(&display->crtc);
			else
				drm_crtc_vblank_off(&display->crtc);
		}
	}
	drm_dev_exit(idx);
	mutex_unlock(&display->scanout_lock);
}

static void apex_physical_work(struct work_struct *work)
{
	struct apex_display *display = container_of(to_delayed_work(work),
							 struct apex_display,
							 physical_work);
	const struct drm_edid *drm_edid = NULL;
	bool connected, changed = false, modeset_retry, restore, read_edid;
	u32 display_status;

	/*
	 * Only this work writes the physical, presence and EDID state. The CMS
	 * mailbox I/O (serialized by io_lock) takes milliseconds per command and
	 * seconds for an EDID during a DDC outage, so it runs outside
	 * state_lock, which flips and atomic checks take.
	 */
	if (READ_ONCE(display->stopping) || READ_ONCE(display->failed))
		return;
	if (!display->physical_ready) {
		if (readl(display->bar0 + APEX_DISPLAY_GT_STATUS) & APEX_DISPLAY_GT_PRESENT) {
			mutex_lock(&display->state_lock);
			display->physical_ready = true;
			mutex_unlock(&display->state_lock);
		} else {
			dev_err_ratelimited(&display->pdev->dev,
					    "display physical initialization failed: %d\n",
					    -EAGAIN);
		}
	}
	display_status = readl(display->bar0 + APEX_DISPLAY_STATUS);
	if (display->cms_failed && time_after_eq(jiffies, display->cms_reset_at)) {
		dev_warn(&display->pdev->dev, "resetting the CMS MicroBlaze\n");
		apex_cms_reset(display);
	}
	if (display->physical_ready && !display->cms_ready &&
	    !display->cms_failed)
		apex_cms_initialize(display);
	/*
	 * A failed presence read leaves the last known state and the hot-plug
	 * register untouched; the engine disables scanout on a disconnect it is
	 * told about.
	 */
	connected = display->connected;
	if (display->physical_ready && display->cms_ready)
		apex_u50_connected(display, &connected);

	mutex_lock(&display->state_lock);
	if (connected != display->connected) {
		display->connected = connected;
		changed = true;
		display->edid_backoff = 0;
		display->edid_retry_at = jiffies;
		display->edid_fallback = false;
		if (!connected) {
			drm_edid_free(display->edid);
			display->edid = NULL;
		}
	}
	read_edid = connected && !display->edid &&
		    time_after_eq(jiffies, display->edid_retry_at);
	mutex_unlock(&display->state_lock);

	if (read_edid) {
		if (apex_u50_edid_firmware && *apex_u50_edid_firmware)
			drm_edid = apex_u50_firmware_edid(display);
		else if (display->cms_communication_version >= 9)
			drm_edid = apex_u50_live_edid(display);
		mutex_lock(&display->state_lock);
		if (drm_edid) {
			display->edid = drm_edid;
			drm_edid = NULL;
			display->edid_backoff = 0;
			display->edid_fallback = false;
			changed = true;
		} else {
			/* An explicit override never falls back to another source. */
			if (!(apex_u50_edid_firmware && *apex_u50_edid_firmware) &&
			    !display->edid_fallback) {
				display->edid_fallback = true;
				changed = true;
				dev_warn(&display->pdev->dev,
					 "display EDID unavailable; driving 1920x1080 at 60 Hz until a read succeeds\n");
			}
			display->edid_backoff = clamp(display->edid_backoff * 2, 1U,
						      (unsigned int)APEX_EDID_BACKOFF_MAX);
			display->edid_retry_at = jiffies + display->edid_backoff * HZ;
			dev_err_ratelimited(&display->pdev->dev,
					    "display EDID read failed or is invalid; retry in %u s\n",
					    display->edid_backoff);
		}
		mutex_unlock(&display->state_lock);
	}

	mutex_lock(&display->state_lock);
	if (display->physical_ready)
		apex_service_phy(display);
	restore = connected && !(display_status & APEX_DISPLAY_ENABLED) &&
		  !(display_status & APEX_DISPLAY_COMMAND_BUSY);
	modeset_retry = display->modeset_retry_pending;
	display->modeset_retry_pending = false;
	mutex_unlock(&display->state_lock);

	if (restore)
		apex_display_restore(display);

	if (modeset_retry && connected && READ_ONCE(display->registered)) {
		mutex_lock(&display->drm.mode_config.mutex);
		drm_connector_set_link_status_property(
			&display->connector, DRM_MODE_LINK_STATUS_BAD);
		mutex_unlock(&display->drm.mode_config.mutex);
		drm_kms_helper_hotplug_event(&display->drm);
	} else if (changed && READ_ONCE(display->registered)) {
		drm_kms_helper_hotplug_event(&display->drm);
	}
	if (!READ_ONCE(display->failed))
		schedule_delayed_work(&display->physical_work, APEX_HOTPLUG_POLL);
}

struct apex_display *apex_display_create(struct pci_dev *pdev,
					 void __iomem *bar0, u64 local_bytes,
					 bool scanout,
					 apex_display_fatal_error_t fatal_error,
					 void *fatal_error_context)
{
	struct apex_display *display;
	int ret;

	if (!pdev || !bar0 || !local_bytes)
		return ERR_PTR(-EINVAL);
	display = devm_drm_dev_alloc(&pdev->dev, &apex_drm_driver,
				    struct apex_display, drm);
	if (IS_ERR(display))
		return display;
	display->pdev = pdev;
	display->edid_retry_at = jiffies;
	display->bar0 = bar0;
	display->local_bytes = local_bytes;
	display->fatal_error = fatal_error;
	display->fatal_error_context = fatal_error_context;
	mutex_init(&display->io_lock);
	mutex_init(&display->command_lock);
	mutex_init(&display->scanout_lock);
	mutex_init(&display->state_lock);
	init_waitqueue_head(&display->command_wait);
	INIT_DELAYED_WORK(&display->physical_work, apex_physical_work);
	if (!scanout) {
		display->drm.driver_features &= ~(DRIVER_MODESET | DRIVER_ATOMIC);
		return display;
	}
	ret = drmm_mode_config_init(&display->drm);
	if (ret)
		return ERR_PTR(ret);
	display->drm.mode_config.min_width = 1;
	display->drm.mode_config.max_width = S16_MAX;
	display->drm.mode_config.min_height = 1;
	display->drm.mode_config.max_height = S16_MAX;
	display->drm.mode_config.funcs = &apex_mode_config_funcs;
	display->drm.mode_config.helper_private = &apex_mode_config_helpers;
	drm_connector_helper_add(&display->connector, &apex_connector_helper_funcs);
	ret = drm_connector_init(&display->drm, &display->connector,
				 &apex_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(&display->plane, &apex_plane_helper_funcs);
	ret = drm_universal_plane_init(&display->drm, &display->plane, 0,
				       &apex_plane_funcs, apex_formats,
				       ARRAY_SIZE(apex_formats), apex_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(&display->crtc, &apex_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(&display->drm, &display->crtc, &display->plane,
					NULL, &apex_crtc_funcs, NULL);
	if (ret)
		return ERR_PTR(ret);
	display->encoder.possible_crtcs = drm_crtc_mask(&display->crtc);
	ret = drm_encoder_init(&display->drm, &display->encoder, &apex_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ERR_PTR(ret);
	ret = drm_connector_attach_encoder(&display->connector, &display->encoder);
	if (ret)
		return ERR_PTR(ret);
	display->drm.max_vblank_count = U32_MAX;
	ret = drm_vblank_init(&display->drm, 1);
	if (ret)
		return ERR_PTR(ret);
	drm_mode_config_reset(&display->drm);
	return display;
}

void apex_display_start(struct apex_display *display)
{
	if (!display || display->started || READ_ONCE(display->failed) ||
	    !drm_core_check_feature(&display->drm, DRIVER_MODESET))
		return;
	display->started = true;
	schedule_delayed_work(&display->physical_work, 0);
}

void apex_display_poison(struct apex_display *display)
{
	if (!display)
		return;
	WRITE_ONCE(display->failed, true);
	wake_up_all(&display->command_wait);
	cancel_delayed_work(&display->physical_work);
}

void apex_display_get_counters(struct apex_display *display,
			       struct apex_display_counters *counters)
{
	*counters = (struct apex_display_counters) {
		.status = readl(display->bar0 + APEX_DISPLAY_STATUS),
		.phy_status = readl(display->bar0 + APEX_DISPLAY_PHY_STATUS),
		.vblank_count = readl(display->bar0 + APEX_DISPLAY_VBLANK_COUNT),
		.underflow_lines = readl(display->bar0 + APEX_DISPLAY_UNDERFLOW_LINES),
		.read_errors = readl(display->bar0 + APEX_DISPLAY_READ_ERRORS),
		.gt_status = readl(display->bar0 + APEX_DISPLAY_GT_STATUS),
	};
}

void apex_display_reset_begin(struct apex_display *display)
{
	if (!display)
		return;
	mutex_lock(&display->scanout_lock);
	display->reset_held = READ_ONCE(display->hardware_active) && !READ_ONCE(display->failed);
	if (!display->reset_held)
		return;
	/* The reset may clear VBLANK_COUNT, so DRM stops counting from it until
	 * scanout resumes. */
	if (READ_ONCE(display->registered))
		drm_crtc_vblank_off(&display->crtc);
	WRITE_ONCE(display->hardware_active, false);
	if (apex_display_command(display, APEX_DISPLAY_CMD_DISABLE, NULL, 0, 0))
		display->reset_held = false;
}

void apex_display_reset_end(struct apex_display *display, bool restore)
{
	if (!display)
		return;
	if (display->reset_held && restore) {
		/* A failure leaves the scanout to apex_display_restore(). */
		if (!apex_display_command(display, APEX_DISPLAY_CMD_MODESET, &display->mode,
					  display->surface, display->pitch)) {
			WRITE_ONCE(display->hardware_active, true);
			if (READ_ONCE(display->registered))
				drm_crtc_vblank_on(&display->crtc);
		}
	}
	display->reset_held = false;
	mutex_unlock(&display->scanout_lock);
}

void apex_display_stop(struct apex_display *display)
{
	if (!display || display->stopping)
		return;
	mutex_lock(&display->state_lock);
	display->stopping = true;
	mutex_unlock(&display->state_lock);
	wake_up_all(&display->command_wait);
	cancel_delayed_work_sync(&display->physical_work);
	if (READ_ONCE(display->registered)) {
		drm_dev_unplug(&display->drm);
		if (drm_core_check_feature(&display->drm, DRIVER_MODESET))
			drm_atomic_helper_shutdown(&display->drm);
		WRITE_ONCE(display->registered, false);
	}
}

void apex_display_destroy(struct apex_display *display)
{
	if (!display)
		return;
	apex_display_stop(display);
	drm_edid_free(display->edid);
}

irqreturn_t apex_display_irq(int irq, void *data)
{
	struct apex_display *display = data;
	u32 vblank;

	if (READ_ONCE(display->failed) || READ_ONCE(display->stopping))
		return IRQ_HANDLED;
	if (time_after(jiffies, display->irq_window + HZ)) {
		display->irq_window = jiffies;
		display->irq_count = 0;
	}
	if (++display->irq_count > APEX_DISPLAY_IRQ_STORM_LIMIT) {
		if (!display->irq_disabled) {
			display->irq_disabled = true;
			disable_irq_nosync(irq);
			dev_err(&display->pdev->dev,
				"display interrupt storm; display disabled until reprobe\n");
			apex_display_poison(display);
			if (display->fatal_error)
				display->fatal_error(display->fatal_error_context);
		}
		return IRQ_HANDLED;
	}
	/* The vblank is counted before a command waiter wakes, so a flip
	 * completed at this boundary reports its count and time. */
	vblank = readl(display->bar0 + APEX_DISPLAY_VBLANK_COUNT);
	if (vblank != display->observed_vblank) {
		display->observed_vblank = vblank;
		if (READ_ONCE(display->registered))
			drm_crtc_handle_vblank(&display->crtc);
	}
	wake_up(&display->command_wait);
	return IRQ_HANDLED;
}
