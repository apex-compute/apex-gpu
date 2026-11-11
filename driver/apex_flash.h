/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APEX_FLASH_H
#define APEX_FLASH_H

#include <linux/io.h>

#define APEX_FLASH_BASE 0xf000
#define APEX_FLASH_REG_BYTES 0x80

struct pci_dev;
struct spi_controller;

struct spi_controller *apex_flash_register(struct pci_dev *pdev, void __iomem *bar0);

#endif
