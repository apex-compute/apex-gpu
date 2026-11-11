# Apex GPU

A Vulkan 1.4 GPU for the AMD Kintex-7 (`xc7k480t`) PCIe board.

| Folder | Contents |
|---|---|
| `image/` | GPU image for the board's flash |
| `tools/` | `apex_flash.py`, which programs and boots the flash over PCIe |
| `driver/` | Linux kernel driver (`apex_pci`) |
| `vulkan/` | Vulkan driver (ICD) for x86-64 Linux |

## Requirements

- A Kintex-7 board from Apex Compute, in a PCIe slot. The GPU image is
  encrypted for that board.
- Linux x86-64 with kernel headers for the running kernel, and Python 3.
- The Vulkan loader (`libvulkan1`), and `vulkan-tools` for `vulkaninfo`.

## Flash the GPU image

```sh
lspci -D -d 10ee:         # find the card's address
BDF=0000:01:00.0          # replace with your card's address
sudo python3 -I tools/apex_flash.py program image/gpu_k7_flash.bin --slot gpu --bdf $BDF
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
lspci -s $BDF -n          # the GPU image shows up as 10ee:a15e
```

Do not reboot or power off the host while programming.

## Load the driver

```sh
make -C driver
sudo modprobe -a drm_exec drm_gpuvm gpu_sched
sudo insmod driver/apex_pci.ko
ls /dev/dri/renderD*
```

On kernels older than 6.18, the driver can fail to probe after a boot with
error `-16`. To fix it, resize BAR2 to 2 GiB and bind the driver again:

```sh
echo 11   | sudo tee /sys/bus/pci/devices/$BDF/resource2_resize
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/bind
```

## Run Vulkan

Your user needs access to the render node (the `render` group).

```sh
export APEX_DEVELOPMENT=1
export VK_DRIVER_FILES=$PWD/vulkan/apex_icd.x86_64.json
vulkaninfo --summary
```

## License

The kernel driver is GPL-2.0; everything else is MIT. See each file's SPDX
header and `LICENSE`.
