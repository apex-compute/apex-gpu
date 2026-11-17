# Flash Images on the Kintex-7 Board

How to switch the board between its factory image and the Apex GPU image, and
how to update the factory image. Run the commands from the `apex-gpu` folder,
with `BDF` set to the card's PCI address
([step 2 of the README](README.md#2-flash-the-gpu-image)):

```bash
# Replace with the card's address
export BDF=0000:01:00.0
```

## Flash Layout

The board's 64 MiB configuration flash holds two images:

| Slot | Flash address | Image | PCI ID | Kernel driver |
| --- | --- | --- | --- | --- |
| `andromeda` | 0 | Factory image, loaded at power-up | `10ee:7024` | `xdma` |
| `gpu` | 32 MiB | The Apex GPU (`image/gpu_k7_flash.bin`) | `10ee:a15e` | `apex_gpu` |

`apex_flash.py boot` reloads the FPGA from a slot without a power cycle (a warm
boot), then removes and rescans the card, which comes back with the other PCI
ID. Check which image runs:

```bash
# 10ee:a15e = GPU image, 10ee:7024 = factory image
lspci -nnk -s $BDF
```

> **Tip:** Before switching, stop every program that uses the card and unbind
> its driver: the flash tool needs the card with no driver bound.

## Switch Between the Factory Image and the GPU Image

### 1. From the GPU Image to the Factory Image

```bash
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind

# Slots, and how the FPGA last configured
sudo python3 -I tools/apex_flash.py status --bdf $BDF

sudo python3 -I tools/apex_flash.py boot --slot andromeda --bdf $BDF

# 10ee:7024; the xdma driver binds if installed
lspci -nnk -s $BDF
```

### 2. From the Factory Image Back to the GPU Image

```bash
# Only if xdma is bound
echo $BDF | sudo tee /sys/bus/pci/drivers/xdma/unbind

sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF

# 10ee:a15e
lspci -nnk -s $BDF
```

If the `apex_gpu` driver is already loaded, it binds on its own after the
rescan. If it is not loaded, load it as in
[step 3 of the README](README.md#3-build-and-load-the-kernel-driver), then
check it with `vulkaninfo --summary`
([step 6](README.md#6-check-vulkan)).

> **Tip:** If the probe fails with `-16` (`sudo dmesg | grep apex`), resize
> BAR2 to 2 GiB and bind it again:
> ```bash
> echo 11   | sudo tee /sys/bus/pci/devices/$BDF/resource2_resize
> echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/bind
> ```

## Update the Factory Image

The factory image is in slot 0: the image the FPGA loads at power-up and falls
back to when the GPU slot fails.

> **Warning:** Program slot 0 only from the GPU image, and only with a factory
> image meant for this board (an `xc7k480t` BPI x16 flash binary from Apex
> Compute). If slot 0 does not configure, the card does not come up at the next
> power-up and has to be reprogrammed over JTAG. Do not reboot or power off the
> host while programming.

### 1. Program and Verify Slot 0

From the GPU image, with no driver bound:

```bash
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind
sudo python3 -I tools/apex_flash.py program FACTORY_IMAGE.bin --slot andromeda --bdf $BDF
sudo python3 -I tools/apex_flash.py verify  FACTORY_IMAGE.bin --slot andromeda --bdf $BDF
```

`program` erases, writes and reads back only the slot's blocks; it refuses an
image that would run into the GPU slot.

### 2. Test It With a Warm Boot

Boot the new factory image before the next power cycle, then switch back:

```bash
sudo python3 -I tools/apex_flash.py boot --slot andromeda --bdf $BDF

# 10ee:7024
lspci -nnk -s $BDF

# Only if xdma is bound
echo $BDF | sudo tee /sys/bus/pci/drivers/xdma/unbind
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
```
