// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iopoll.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/pci.h>
#include <linux/spi/flash.h>
#include <linux/spi/spi.h>

#include "apex_flash.h"

/* PG153 legacy AXI Quad SPI, mode 0, 8 bits, 256-entry FIFOs.
 * SPI-NOR owns flash commands. This transport bounds RX waits even when TX
 * becomes empty without producing the expected RX bytes.
 */
#define SPI_RESET 0x40
#define SPI_CONTROL 0x60
#define SPI_STATUS 0x64
#define SPI_TX 0x68
#define SPI_RX 0x6c
#define SPI_SELECT 0x70
#define SPI_RX_OCCUPANCY 0x78
#define SPI_RX_EMPTY BIT(0)
#define SPI_INHIBIT BIT(8)
#define SPI_RUN (BIT(1) | BIT(2) | BIT(7))
#define SPI_FIFO_RESET (BIT(5) | BIT(6))
#define SPI_FIFO_BYTES 256

struct apex_flash {
	void __iomem *regs;
	bool failed;
};

static void apex_flash_set_cs(struct spi_device *spi, bool high)
{
	struct apex_flash *flash = spi_controller_get_devdata(spi->controller);

	writel(high ? 1 : 0, flash->regs + SPI_SELECT);
}

static int apex_flash_transfer(struct spi_controller *controller,
			       struct spi_device *spi, struct spi_transfer *transfer)
{
	struct apex_flash *flash = spi_controller_get_devdata(controller);
	const u8 *tx = transfer->tx_buf;
	u8 *rx = transfer->rx_buf;
	unsigned int offset, count, i;
	u32 occupancy;
	int ret;

	if (flash->failed)
		return -EIO;
	for (offset = 0; offset < transfer->len; offset += count) {
		count = min_t(unsigned int, SPI_FIFO_BYTES, transfer->len - offset);
		/* Keep CS asserted across FIFO chunks and SPI message transfers. */
		writel(SPI_RUN | SPI_INHIBIT, flash->regs + SPI_CONTROL);
		for (i = 0; i < count; i++)
			writel(tx ? tx[offset + i] : 0xff, flash->regs + SPI_TX);
		writel(SPI_RUN, flash->regs + SPI_CONTROL);
		/* Occupancy is count minus one and is undefined while RX is empty.
		 * 256 bytes take 164 us at 12.5 MHz; allow 100 ms for host scheduling.
		 */
		ret = read_poll_timeout(readl, occupancy,
			!(readl(flash->regs + SPI_STATUS) & SPI_RX_EMPTY) &&
			occupancy == count - 1, 10, 100000, false,
			flash->regs + SPI_RX_OCCUPANCY);
		writel(SPI_RUN | SPI_INHIBIT, flash->regs + SPI_CONTROL);
		if (ret) {
			/* Fail closed until reprobe. A partial command can leave flash
			 * busy, so never retry it or reset/reconfigure the board here.
			 */
			flash->failed = true;
			writel(1, flash->regs + SPI_SELECT);
			dev_err(&controller->dev, "QSPI receive timeout; maintenance stopped\n");
			return ret;
		}
		for (i = 0; i < count; i++) {
			u8 data = readl(flash->regs + SPI_RX);

			if (rx)
				rx[offset + i] = data;
		}
	}
	return 0;
}

/* The user image begins 8 KiB past the 16 MiB golden region. Both ranges
 * include whole 4 KiB subsectors; Linux must retain that erase granularity.
 * These are MTD write permissions, not protection against privileged raw MMIO.
 */
static struct mtd_partition apex_flash_parts[] = {
	{
		.name = "apex-u50-golden",
		.offset = 0,
		.size = 0x01002000,
		.mask_flags = MTD_WRITEABLE,
	},
	{
		.name = "apex-u50-user",
		.offset = 0x01002000,
		.size = 0x06ffe000,
	},
};

static struct flash_platform_data apex_flash_data = {
	.parts = apex_flash_parts,
	.nr_parts = ARRAY_SIZE(apex_flash_parts),
};

static struct spi_board_info apex_flash_device = {
	.modalias = "spi-nor",
	.platform_data = &apex_flash_data,
	.max_speed_hz = 12500000,
	.chip_select = 0,
	.mode = SPI_MODE_0,
};

struct spi_controller *apex_flash_register(struct pci_dev *pdev, void __iomem *bar0)
{
	const u8 startup_bytes[] = { 0x9f, 0, 0 };
	struct spi_transfer startup = {
		.tx_buf = startup_bytes,
		.len = sizeof(startup_bytes),
	};
	struct spi_controller *controller;
	struct apex_flash *flash;
	int ret;

	if (pci_resource_len(pdev, 0) < APEX_FLASH_BASE + APEX_FLASH_REG_BYTES)
		return ERR_PTR(-ENODEV);
	controller = spi_alloc_host(&pdev->dev, sizeof(*flash));
	if (!controller)
		return ERR_PTR(-ENOMEM);
	flash = spi_controller_get_devdata(controller);
	flash->regs = bar0 + APEX_FLASH_BASE;
	controller->bus_num = -1;
	controller->num_chipselect = 1;
	controller->bits_per_word_mask = SPI_BPW_MASK(8);
	controller->min_speed_hz = 12500000;
	controller->max_speed_hz = 12500000;
	controller->set_cs = apex_flash_set_cs;
	controller->transfer_one = apex_flash_transfer;
	writel(0xa, flash->regs + SPI_RESET);
	writel(1, flash->regs + SPI_SELECT);
	writel(SPI_RUN | SPI_INHIBIT | SPI_FIFO_RESET, flash->regs + SPI_CONTROL);
	ret = spi_register_controller(controller);
	if (ret) {
		spi_controller_put(controller);
		return ERR_PTR(ret);
	}
	/* AMD XSpi_CfgInitialize, CR 721229: prime the STARTUP clock path
	 * before the first flash command. CS stays high; no command reaches
	 * the flash. Use the same bounded transfer and drain its RX bytes.
	 */
	ret = apex_flash_transfer(controller, NULL, &startup);
	if (ret) {
		spi_unregister_controller(controller);
		return ERR_PTR(ret);
	}
	if (!spi_new_device(controller, &apex_flash_device)) {
		spi_unregister_controller(controller);
		return ERR_PTR(-ENODEV);
	}
	return controller;
}
