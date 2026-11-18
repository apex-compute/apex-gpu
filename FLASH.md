# Boot and Flash Images on the Kintex-7 Board

The board runs one of two images: the
[unified-engine](https://github.com/apex-compute/unified-engine) image or the
Apex GPU image. This guide covers both modes:

- [Boot mode](#boot-mode): switch the running image without a power cycle. The
  flash does not change.
- [Flash mode](#flash-mode): write a new image into the flash.

Run the commands from the `apex-gpu` folder, with `BDF` set to the card's PCI
address ([step 2 of the README](README.md#2-flash-the-gpu-image)):

```bash
# Replace with the card's address
export BDF=0000:01:00.0
```

## Flash Layout

The board's 64 MiB configuration flash holds two images:

| Slot | Flash address | Image | PCI ID | Kernel driver |
| --- | --- | --- | --- | --- |
| `unified-engine` | 0 | unified-engine, loaded at power-up | `10ee:7024` | `xdma` |
| `gpu` | 32 MiB | The Apex GPU (`image/gpu_k7_flash.bin`) | `10ee:a15e` | `apex_gpu` |

The FPGA loads the unified-engine image at power-up, and falls back to it if
the GPU slot does not configure. Check which image runs:

```bash
# 10ee:a15e = GPU image, 10ee:7024 = unified-engine image
lspci -nnk -s $BDF
```

> **Tip:** Before booting or flashing, stop every program that uses the card
> and unbind its driver: `apex_flash.py` needs the card with no driver bound.

---

## Boot Mode

`apex_flash.py boot` reloads the FPGA from a slot (a warm boot), then removes
and rescans the card, which comes back with the other PCI ID. It works from
either image.

### 1. Boot the GPU Image From unified-engine

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

> **Tip:** If the card comes back as `10ee:7024`, the GPU slot is empty or
> holds an image that does not configure, and the FPGA fell back to
> unified-engine. See [Program the GPU Slot](#1-program-the-gpu-slot-from-unified-engine).

### 2. Boot unified-engine From the GPU Image

```bash
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind

# Slots, and how the FPGA last configured
sudo python3 -I tools/apex_flash.py status --bdf $BDF

sudo python3 -I tools/apex_flash.py boot --slot unified-engine --bdf $BDF

# 10ee:7024; the xdma driver binds if installed
lspci -nnk -s $BDF
```

---

## Flash Mode

`apex_flash.py program` erases, writes and reads back only the blocks of one
slot, and refuses an image that would run into the other slot. It programs from
the GPU image only: the unified-engine image reaches only its own half of the
flash, so it cannot write the GPU slot.

> **Warning:** Do not reboot or power off the host while programming.

### 1. Program the GPU Slot From unified-engine

Boot the GPU image first ([Boot mode, step 1](#1-boot-the-gpu-image-from-unified-engine)),
then program the new GPU image into its own slot. The running FPGA keeps the old
image until the next boot:

```bash
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind

sudo python3 -I tools/apex_flash.py program image/gpu_k7_flash.bin --slot gpu --bdf $BDF
sudo python3 -I tools/apex_flash.py verify  image/gpu_k7_flash.bin --slot gpu --bdf $BDF

# Load the new image
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF

# 10ee:a15e
lspci -nnk -s $BDF
```

> **Note:** If the GPU slot is empty or does not configure, the card cannot
> boot the GPU image, and the GPU slot has to be programmed once over JTAG with
> Vivado (a BPI x16 flash at offset `0x2000000`). After that, update it as
> above.

### 2. Program the unified-engine Slot From the GPU Image

> **Warning:** Program slot 0 only with a unified-engine image meant for this
> board (an `xc7k480t` BPI x16 flash binary, such as the `update_<hash>.bin`
> from the [unified-engine releases](https://github.com/apex-compute/unified-engine/releases)).
> If slot 0 does not configure, the card does not come up at the next power-up
> and has to be reprogrammed over JTAG.

From the GPU image, with no driver bound:

```bash
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind

sudo python3 -I tools/apex_flash.py program UNIFIED_ENGINE_IMAGE.bin --slot unified-engine --bdf $BDF
sudo python3 -I tools/apex_flash.py verify  UNIFIED_ENGINE_IMAGE.bin --slot unified-engine --bdf $BDF
```

Test the new image before the next power cycle, then boot the GPU image again:

```bash
sudo python3 -I tools/apex_flash.py boot --slot unified-engine --bdf $BDF

# 10ee:7024
lspci -nnk -s $BDF

# Only if xdma is bound
echo $BDF | sudo tee /sys/bus/pci/drivers/xdma/unbind
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
```

> **Tip:** While unified-engine runs, its own `./update_fpga.sh` also updates
> slot 0 (see the [unified-engine README](https://github.com/apex-compute/unified-engine#readme)).
> It writes only slot 0, so it leaves the GPU slot as it is.
