// SPDX-License-Identifier: GPL-2.0-only

#include "apex_compat.h"
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hwmon.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/sizes.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>

#include "apex_cp.h"
#include "apex_display.h"
#include "apex_flash.h"

#define APEX_MAGIC 0x0000
#define APEX_ABI 0x0004
#define APEX_PROFILE 0x0008
#define APEX_CAPS 0x000c
#define APEX_MEMORY_LOW 0x0010
#define APEX_MEMORY_HIGH 0x0014
#define APEX_CHANNELS 0x0018
#define APEX_TIMEBASE 0x001c
#define APEX_LINK 0x0020
#define APEX_COMMAND_ID 0x0100
#define APEX_COMMAND_QUERY 0x0104
#define APEX_COMMAND_DOORBELL 0x0108
#define APEX_COMMAND_STATUS 0x010c
#define APEX_RESPONSE_ID 0x0110
#define APEX_RESPONSE_VALUE_LOW 0x0114
#define APEX_RESPONSE_VALUE_HIGH 0x0118
#define APEX_RESPONSE_STATUS 0x011c
#define APEX_RESPONSE_ACK 0x0120
#define APEX_MSIX_PBA 0x9000

#define APEX_MAGIC_VALUE 0x41504558
#define APEX_ABI_REVISION 1
#define APEX_CAP_CONTROL BIT(0)
#define APEX_CAP_MSIX BIT(1)
#define APEX_CAP_FLR BIT(2)
#define APEX_CAP_REBAR BIT(5)
#define APEX_CAP_DISPLAY BIT(6)
#define APEX_CAP_FLASH BIT(8)
#define APEX_CAPS_COMMON (APEX_CAP_CONTROL | APEX_CAP_MSIX | APEX_CAP_FLR)
#define APEX_COMPLETE BIT(1)
#define APEX_QUERY_ABI 0
#define APEX_RESPONSE_SUCCESS 0
#define APEX_TIMEOUT msecs_to_jiffies(1000)
#define APEX_IRQ_STORM_LIMIT 1000

static bool apex_flash_enabled;
module_param_named(flash, apex_flash_enabled, bool, 0444);
MODULE_PARM_DESC(flash, "expose U50 QSPI MTD partitions for maintenance (requires CAP_FLASH)");

struct apex_device {
	struct mutex bus_master_lock;
	struct rw_semaphore access_lock;
	struct work_struct fault_work;
	struct pci_dev *pdev;
	void __iomem *bar0;
	struct apex_display *display;
	struct apex_cp *cp;
	struct spi_controller *flash;
	struct device *hwmon;
	struct completion bootstrap_done;
	u64 memory;
	u32 command_id;
	u32 profile;
	u32 caps;
	unsigned int bus_master_users;
	unsigned long control_irq_window;
	u32 control_irq_count;
	bool display_master;
	bool control_irq_disabled;
	bool hardware_failed;
	bool removing;
};

static void apex_mark_failed(struct apex_device *apex)
{
	/* Latch command-processor failure synchronously, including IRQ/AER callers. */
	if (!READ_ONCE(apex->hardware_failed))
		dev_warn(&apex->pdev->dev, "hardware failed from %pS\n", __builtin_return_address(0));
	apex_cp_poison(READ_ONCE(apex->cp));
	WRITE_ONCE(apex->hardware_failed, true);
	complete_all(&apex->bootstrap_done);
	apex_display_poison(apex->display);
	if (!READ_ONCE(apex->removing)) schedule_work(&apex->fault_work);
}

static void apex_display_failed(void *context)
{
	apex_mark_failed(context);
}

static irqreturn_t apex_irq(int irq, void *data)
{
	struct apex_device *apex = data;

	if (READ_ONCE(apex->hardware_failed) || READ_ONCE(apex->removing))
		return IRQ_HANDLED;
	if (readl(apex->bar0 + APEX_COMMAND_STATUS) & APEX_COMPLETE) {
		complete(&apex->bootstrap_done);
		return IRQ_HANDLED;
	}
	/* Only vectors with nothing to complete count toward the storm limit. */
	if (time_after(jiffies, apex->control_irq_window + HZ)) {
		apex->control_irq_window = jiffies;
		apex->control_irq_count = 0;
	}
	if (++apex->control_irq_count > APEX_IRQ_STORM_LIMIT && !apex->control_irq_disabled) {
		apex->control_irq_disabled = true;
		disable_irq_nosync(irq);
		dev_err(&apex->pdev->dev,
			"control interrupt storm; vector and device disabled until reprobe\n");
		apex_mark_failed(apex);
	}
	return IRQ_NONE;
}

static void apex_device_free(struct apex_device *apex)
{
	apex_cp_put(apex->cp);
	kfree(apex);
}

static bool apex_has_translated_iommu(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	return domain && iommu_is_dma_domain(domain);
}

static int apex_bus_master_get(struct apex_device *apex)
{
	u16 command;
	int ret = 0;

	mutex_lock(&apex->bus_master_lock);
	if (READ_ONCE(apex->removing)) {
		ret = -ENODEV;
		goto out;
	}
	if (READ_ONCE(apex->hardware_failed)) {
		ret = -EIO;
		goto out;
	}
	if (!apex->bus_master_users) {
		pci_set_master(apex->pdev);
		ret = pci_read_config_word(apex->pdev, PCI_COMMAND, &command);
		if (ret || !(command & PCI_COMMAND_MASTER)) {
			pci_clear_master(apex->pdev);
			ret = ret ?: -EIO;
			goto out;
		}
	}
	apex->bus_master_users++;
out:
	mutex_unlock(&apex->bus_master_lock);
	return ret;
}

static void apex_bus_master_put(struct apex_device *apex)
{
	mutex_lock(&apex->bus_master_lock);
	if (!apex->bus_master_users) {
		WARN_ON(!READ_ONCE(apex->hardware_failed) &&
			!READ_ONCE(apex->removing));
		goto out;
	}
	if (!--apex->bus_master_users)
		pci_clear_master(apex->pdev);
out:
	mutex_unlock(&apex->bus_master_lock);
}

static void apex_fault_work(struct work_struct *work)
{
	struct apex_device *apex = container_of(work, struct apex_device,
						 fault_work);

	mutex_lock(&apex->bus_master_lock);
	apex->bus_master_users = 0;
	apex->display_master = false;
	pci_clear_master(apex->pdev);
	mutex_unlock(&apex->bus_master_lock);
}

static void apex_cp_failed(void *owner)
{
	apex_mark_failed(owner);
}

static int apex_cp_master_get(void *owner)
{
	return apex_bus_master_get(owner);
}

static void apex_cp_master_put(void *owner)
{
	apex_bus_master_put(owner);
}

static void apex_cp_reset_begin(void *owner)
{
	struct apex_device *apex = owner;

	apex_display_reset_begin(apex->display);
}

static void apex_cp_reset_end(void *owner, bool restore)
{
	struct apex_device *apex = owner;

	apex_display_reset_end(apex->display, restore);
}

static const struct apex_cp_ops cp_ops = {
	.failed = apex_cp_failed,
	.master_get = apex_cp_master_get,
	.master_put = apex_cp_master_put,
	.reset_begin = apex_cp_reset_begin,
	.reset_end = apex_cp_reset_end,
};

static int apex_bootstrap_query(struct apex_device *apex)
{
	u64 value;
	u32 id, response_id, status;
	long waited;

	id = ++apex->command_id;
	reinit_completion(&apex->bootstrap_done);
	writel(id, apex->bar0 + APEX_COMMAND_ID);
	writel(APEX_QUERY_ABI, apex->bar0 + APEX_COMMAND_QUERY);
	writel(1, apex->bar0 + APEX_COMMAND_DOORBELL);
	waited = wait_for_completion_timeout(&apex->bootstrap_done, APEX_TIMEOUT);
	if (READ_ONCE(apex->hardware_failed))
		return -EIO;
	if (!waited)
		return -ETIMEDOUT;
	status = readl(apex->bar0 + APEX_COMMAND_STATUS);
	if (!(status & APEX_COMPLETE))
		return -EPROTO;
	response_id = readl(apex->bar0 + APEX_RESPONSE_ID);
	value = readl(apex->bar0 + APEX_RESPONSE_VALUE_LOW) |
		((u64)readl(apex->bar0 + APEX_RESPONSE_VALUE_HIGH) << 32);
	status = readl(apex->bar0 + APEX_RESPONSE_STATUS);
	writel(response_id, apex->bar0 + APEX_RESPONSE_ACK);
	return response_id == id && status == APEX_RESPONSE_SUCCESS &&
	       value == APEX_ABI_REVISION ? 0 : -EPROTO;
}

/* U50 CMS sensor registers (PG348 REG_MAP at BAR0 0x1000), degrees C:
 * FPGA, HBM stack 1 and HBM stack 2 instantaneous temperatures. */
static const u32 apex_u50_temperatures[] = {0x1100, 0x1268, 0x12bc};

static umode_t apex_u50_hwmon_is_visible(const void *data,
		enum hwmon_sensor_types type, u32 attr, int channel)
{
	return type == hwmon_temp && (attr == hwmon_temp_input || attr == hwmon_temp_label) ? 0444 : 0;
}

static int apex_u50_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, long *val)
{
	struct apex_device *apex = dev_get_drvdata(dev);
	int ret = 0;

	if (type != hwmon_temp || attr != hwmon_temp_input ||
	    channel >= ARRAY_SIZE(apex_u50_temperatures))
		return -EOPNOTSUPP;
	down_read(&apex->access_lock);
	if (READ_ONCE(apex->removing)) {
		ret = -ENODEV;
		goto out;
	}
	*val = readl(apex->bar0 + apex_u50_temperatures[channel]) * 1000L;
	/* The CMS leaves its sensor words zero until the satellite controller
	 * reports. */
	if (!*val)
		ret = -ENODATA;
out:
	up_read(&apex->access_lock);
	return ret;
}

static int apex_u50_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
		u32 attr, int channel, const char **str)
{
	static const char *const labels[] = {"fpga", "hbm1", "hbm2"};

	if (type != hwmon_temp || attr != hwmon_temp_label || channel >= ARRAY_SIZE(labels))
		return -EOPNOTSUPP;
	*str = labels[channel];
	return 0;
}

static const struct hwmon_ops apex_u50_hwmon_ops = {
	.is_visible = apex_u50_hwmon_is_visible, .read = apex_u50_hwmon_read,
	.read_string = apex_u50_hwmon_read_string,
};
static const struct hwmon_channel_info *const apex_u50_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};
static const struct hwmon_chip_info apex_u50_chip_info = {
	.ops = &apex_u50_hwmon_ops, .info = apex_u50_hwmon_info,
};

/* Scanout status and cumulative counters in the device's display/ group. */
static ssize_t apex_display_show(struct device *dev, char *buf, size_t offset, bool hex)
{
	struct apex_device *apex = dev_get_drvdata(dev);
	struct apex_display_counters counters;
	ssize_t ret = -ENODEV;
	u32 value;

	down_read(&apex->access_lock);
	if (!READ_ONCE(apex->removing) && !READ_ONCE(apex->hardware_failed)) {
		apex_display_get_counters(apex->display, &counters);
		memcpy(&value, (u8 *)&counters + offset, sizeof(value));
		ret = sysfs_emit(buf, hex ? "%#x\n" : "%u\n", value);
	}
	up_read(&apex->access_lock);
	return ret;
}

#define APEX_DISPLAY_ATTR(field, hex)							\
static ssize_t field##_show(struct device *dev, struct device_attribute *attr,	\
			    char *buf)							\
{											\
	return apex_display_show(dev, buf,						\
				 offsetof(struct apex_display_counters, field), hex);	\
}											\
static DEVICE_ATTR_RO(field)

APEX_DISPLAY_ATTR(status, true);
APEX_DISPLAY_ATTR(phy_status, true);
APEX_DISPLAY_ATTR(gt_status, true);
APEX_DISPLAY_ATTR(vblank_count, false);
APEX_DISPLAY_ATTR(underflow_lines, false);
APEX_DISPLAY_ATTR(read_errors, false);

static struct attribute *apex_display_attrs[] = {
	&dev_attr_status.attr, &dev_attr_phy_status.attr, &dev_attr_gt_status.attr,
	&dev_attr_vblank_count.attr, &dev_attr_underflow_lines.attr,
	&dev_attr_read_errors.attr, NULL
};
static umode_t apex_display_attr_visible(struct kobject *kobj, struct attribute *attr,
					 int n)
{
	struct apex_device *apex = dev_get_drvdata(kobj_to_dev(kobj));

	return apex->caps & APEX_CAP_DISPLAY ? attr->mode : 0;
}

static const struct attribute_group apex_display_group = {
	.name = "display", .attrs = apex_display_attrs,
	.is_visible = apex_display_attr_visible,
};
/* The BAR0 profile, which names the board: 2 U50, 3 Kintex-7 (apexgpu-smi). */
static ssize_t profile_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct apex_device *apex = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", apex->profile);
}
static DEVICE_ATTR_RO(profile);

static struct attribute *apex_device_attrs[] = { &dev_attr_profile.attr, NULL };
static const struct attribute_group apex_device_group = { .attrs = apex_device_attrs };
static const struct attribute_group *apex_groups[] = {
	&apex_device_group, &apex_display_group, NULL
};

/* Every probe starts from a reset that clears what a failed instance left.
 * FLR resets the DMA engines, GPU owners and completer but not the
 * endpoint block, which a DMA hang can leave mid-request; a secondary bus
 * reset retrains the link and restarts both. Use it when the card is alone
 * below a bridge without hot-plug, else FLR. */
static int apex_probe_reset(struct pci_dev *pdev)
{
	struct pci_dev *bridge = pdev->bus->self;
	int ret;

	if (!bridge || bridge->is_hotplug_bridge || !list_is_singular(&pdev->bus->devices))
		return pci_reset_function_locked(pdev);
	pci_save_state(pdev);
	pci_cfg_access_lock(bridge);
	ret = pci_bridge_secondary_bus_reset(bridge);
	pci_cfg_access_unlock(bridge);
	pci_restore_state(pdev);
	return ret;
}

static int apex_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct apex_device *apex;
	u32 abi;
	u16 command, device_control;
	u64 possible;
	int rebar_size, ret;

	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;
	/* A stale or malformed image must not retain host-memory write authority. */
	pci_clear_master(pdev);
	ret = apex_probe_reset(pdev);
	if (ret) {
		dev_err(&pdev->dev, "probe reset failed: %d\n", ret);
		goto disable;
	}
	ret = pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &device_control);
	if (ret || !(device_control & PCI_EXP_DEVCTL_EXT_TAG)) {
		dev_err(&pdev->dev, "8-bit PCIe tags are required\n");
		ret = -ENODEV;
		goto disable;
	}
	ret = pci_request_selected_regions(pdev, BIT(0), "apex_gpu");
	if (ret)
		goto disable;
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < APEX_MSIX_PBA + sizeof(u64)) {
		ret = -ENODEV;
		goto release0;
	}
	apex = kzalloc(sizeof(*apex), GFP_KERNEL);
	if (!apex) {
		ret = -ENOMEM;
		goto release0;
	}
	mutex_init(&apex->bus_master_lock);
	init_rwsem(&apex->access_lock);
	INIT_WORK(&apex->fault_work, apex_fault_work);
	init_completion(&apex->bootstrap_done);
	apex->pdev = pdev;
	apex->bar0 = pci_iomap(pdev, 0, 0);
	if (!apex->bar0) {
		ret = -ENOMEM;
		goto free_apex;
	}
	pci_set_drvdata(pdev, apex);
	abi = readl(apex->bar0 + APEX_ABI);
	apex->profile = readl(apex->bar0 + APEX_PROFILE);
	apex->caps = readl(apex->bar0 + APEX_CAPS);
	apex->memory = readl(apex->bar0 + APEX_MEMORY_LOW) |
		((u64)readl(apex->bar0 + APEX_MEMORY_HIGH) << 32);
	if (readl(apex->bar0 + APEX_MAGIC) != APEX_MAGIC_VALUE ||
	    abi != APEX_ABI_REVISION) {
		ret = -ENODEV;
		goto unmap0;
	}
	if (apex->profile != APEX_PROFILE_U50 && apex->profile != APEX_PROFILE_K7) {
		ret = -ENODEV;
		goto unmap0;
	}
	if ((apex->caps & APEX_CAPS_COMMON) != APEX_CAPS_COMMON ||
	    !is_power_of_2(apex->memory) ||
	    !(pci_resource_flags(pdev, 2) & IORESOURCE_MEM_64) ||
	    !(pci_resource_flags(pdev, 2) & IORESOURCE_PREFETCH)) {
		ret = -ENODEV;
		goto unmap0;
	}
	/* Without CAP_REBAR, BAR2 is fixed at the LOCAL extent. */
	if (apex->caps & APEX_CAP_REBAR) {
		rebar_size = pci_rebar_bytes_to_size(apex->memory);
		possible = pci_rebar_get_possible_sizes(pdev, 2);
		if (rebar_size < 0 || !(possible & BIT_ULL(rebar_size))) {
			ret = -ENODEV; goto unmap0;
		}
	}
	if ((apex->caps & APEX_CAP_REBAR) && pci_resource_len(pdev, 2) != apex->memory) {
		pci_read_config_word(pdev, PCI_COMMAND, &command);
		pci_write_config_word(pdev, PCI_COMMAND, command & ~PCI_COMMAND_MEMORY);
#if APEX_RESIZE_KEEPS_BARS
		ret = pci_resize_resource(pdev, 2, rebar_size, BIT(0));
#else
		/* Older kernels resize without excluding BAR0. */
		ret = pci_resize_resource(pdev, 2, rebar_size);
#endif
		pci_write_config_word(pdev, PCI_COMMAND, command);
		if (ret)
			goto unmap0;
	}
	if (pci_resource_len(pdev, 2) != apex->memory) {
		ret = -ENODEV;
		goto unmap0;
	}
	ret = pci_request_selected_regions(pdev, BIT(2), "apex_gpu");
	if (ret)
		goto unmap0;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(52));
	if (ret)
		goto release2;
	/* The command processor and every bus-master path require IOMMU
	 * containment of the device's host-memory accesses. */
	if (!apex_has_translated_iommu(&pdev->dev)) {
		dev_err(&pdev->dev, "a translated IOMMU domain is required\n");
		ret = -ENODEV;
		goto release2;
	}
	if (pci_msix_vec_count(pdev) != 32) {
		ret = -ENODEV;
		goto release2;
	}
	apex->display = apex_display_create(pdev, apex->bar0, apex->memory,
					    apex->caps & APEX_CAP_DISPLAY,
					    apex_display_failed, apex);
	if (IS_ERR(apex->display)) {
		ret = PTR_ERR(apex->display);
		apex->display = NULL;
		goto release2;
	}
	apex->cp = apex_cp_create(pdev, apex->bar0, apex->profile, apex->memory, &cp_ops, apex);
	if (IS_ERR(apex->cp)) {
		ret = PTR_ERR(apex->cp);
		apex->cp = NULL;
		goto destroy_display;
	}
	ret = pci_alloc_irq_vectors(pdev, 3, 3, PCI_IRQ_MSIX);
	if (ret < 0)
		goto remove_cp;
	ret = request_irq(pci_irq_vector(pdev, 0), apex_irq, 0, "apex_gpu", apex);
	if (ret)
		goto free_vectors;
	ret = request_irq(pci_irq_vector(pdev, 1), apex_display_irq, 0,
			  "apex_gpu_display", apex->display);
	if (ret)
		goto free_control_irq;
	ret = request_irq(pci_irq_vector(pdev, 2), apex_cp_irq, 0, "apex_gpu_cp", apex->cp);
	if (ret)
		goto free_display_irq;
	ret = apex_bootstrap_query(apex);
	if (ret)
		goto free_cp_irq;
	/* The CMS temperatures exist only on the U50. */
	if (apex->profile == APEX_PROFILE_U50) {
		apex->hwmon = hwmon_device_register_with_info(&pdev->dev, "apex_gpu", apex,
							      &apex_u50_chip_info, NULL);
		if (IS_ERR(apex->hwmon)) {
			ret = PTR_ERR(apex->hwmon);
			apex->hwmon = NULL;
			goto free_cp_irq;
		}
	}
	ret = apex_cp_start(apex->cp);
	if (ret)
		goto unregister_hwmon;
	ret = apex_display_register(apex->display, apex->cp);
	if (ret)
		goto unregister_hwmon;
	if (apex->caps & APEX_CAP_DISPLAY) {
		ret = apex_bus_master_get(apex);
		if (ret) {
			dev_err(&pdev->dev, "display disabled: bus mastering failed: %d\n", ret);
		} else {
			apex->display_master = true;
			apex_display_start(apex->display);
		}
	}
	if (apex_flash_enabled) {
		if (apex->caps & APEX_CAP_FLASH)
			apex->flash = apex_flash_register(pdev, apex->bar0);
		else
			apex->flash = ERR_PTR(-EOPNOTSUPP);
		if (IS_ERR(apex->flash)) {
			dev_err(&pdev->dev, "QSPI maintenance unavailable: %ld\n",
				PTR_ERR(apex->flash));
			apex->flash = NULL;
		}
	}
	dev_info(&pdev->dev, "profile=%u memory=%llu MiB\n",
		 apex->profile, (unsigned long long)(apex->memory >> 20));
	return 0;
unregister_hwmon:
	if (apex->hwmon)
		hwmon_device_unregister(apex->hwmon);
	apex_cp_quiesce(apex->cp);
free_cp_irq:
	free_irq(pci_irq_vector(pdev, 2), apex->cp);
free_display_irq:
	free_irq(pci_irq_vector(pdev, 1), apex->display);
free_control_irq:
	free_irq(pci_irq_vector(pdev, 0), apex);
free_vectors:
	cancel_work_sync(&apex->fault_work);
	pci_free_irq_vectors(pdev);
remove_cp:
	apex_cp_quiesce(apex->cp);
	pci_clear_master(pdev);
	apex_cp_remove(apex->cp, pci_wait_for_pending_transaction(pdev));
destroy_display:
	apex_display_destroy(apex->display);
release2:
	pci_release_selected_regions(pdev, BIT(2));
unmap0:
	pci_iounmap(pdev, apex->bar0);
free_apex:
	pci_set_drvdata(pdev, NULL);
	apex_device_free(apex);
release0:
	pci_release_selected_regions(pdev, BIT(0));
disable:
	pci_disable_device(pdev);
	return ret;
}

static void apex_remove(struct pci_dev *pdev)
{
	struct apex_device *apex = pci_get_drvdata(pdev);
	bool drained;

	if (apex->flash)
		spi_unregister_controller(apex->flash);
	WRITE_ONCE(apex->removing, true);
	complete_all(&apex->bootstrap_done);
	apex_display_poison(apex->display);
	apex_display_stop(apex->display);
	cancel_work_sync(&apex->fault_work);
	if (apex->display_master) {
		apex->display_master = false;
		apex_bus_master_put(apex);
	}
	apex_cp_quiesce(apex->cp);
	down_write(&apex->access_lock);
	if (apex->hwmon)
		hwmon_device_unregister(apex->hwmon);
	pci_clear_master(pdev);
	/* A failed drain is not discharged by the PCIe transaction snapshot:
	 * the cleared bus master fences the device and the command processor's
	 * host backing leaks so a reprobe can proceed. */
	drained = pci_wait_for_pending_transaction(pdev);
	free_irq(pci_irq_vector(pdev, 2), apex->cp);
	free_irq(pci_irq_vector(pdev, 1), apex->display);
	free_irq(pci_irq_vector(pdev, 0), apex);
	pci_free_irq_vectors(pdev);
	apex_cp_remove(apex->cp, drained);
	apex_display_destroy(apex->display);
	pci_iounmap(pdev, apex->bar0);
	pci_release_selected_regions(pdev, BIT(2) | BIT(0));
	pci_disable_device(pdev);
	up_write(&apex->access_lock);
	apex_device_free(apex);
}

static pci_ers_result_t apex_error_detected(struct pci_dev *pdev,
					    pci_channel_state_t state)
{
	struct apex_device *apex = pci_get_drvdata(pdev);

	if (apex) {
		dev_err(&pdev->dev,
			"PCIe channel error %u; device disabled until remove and reprobe\n",
			state);
		apex_mark_failed(apex);
	}
	return PCI_ERS_RESULT_DISCONNECT;
}

static void apex_reset_prepare(struct pci_dev *pdev)
{
	struct apex_device *apex = pci_get_drvdata(pdev);

	if (apex) {
		/* Poisoning closes admission and quarantines GPU backing; a
		 * function reset is deliberately not a proof that it drained.
		 * BAR0 readers drain through access_lock. */
		apex_mark_failed(apex);
		down_write(&apex->access_lock);
		up_write(&apex->access_lock);
	}
}

static const struct pci_error_handlers apex_error_handlers = {
	.error_detected = apex_error_detected,
	.reset_prepare = apex_reset_prepare,
};

static const struct pci_device_id apex_pci_ids[] = {
	{ PCI_DEVICE(0x10ee, 0xa15e) }, { }
};
MODULE_DEVICE_TABLE(pci, apex_pci_ids);
static struct pci_driver apex_pci_driver = {
	.name = "apex_gpu", .id_table = apex_pci_ids,
	.probe = apex_probe, .remove = apex_remove,
	.err_handler = &apex_error_handlers,
	.dev_groups = apex_groups,
};
module_pci_driver(apex_pci_driver);
MODULE_AUTHOR("APEX Compute");
MODULE_DESCRIPTION("APEX GPU PCI control, command processor and display driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
