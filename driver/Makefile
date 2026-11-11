ifneq ($(KERNELRELEASE),)
obj-m += apex_pci.o
apex_pci-y := apex_pci_main.o apex_display.o apex_cp.o apex_flash.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
