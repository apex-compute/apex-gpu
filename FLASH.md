# Flash images on the Kintex-7 board

How to switch the board between its factory image and the Apex GPU image, and
how to update the factory image. Run the commands from the `apex-gpu` folder,
with `BDF` set to the card's PCI address (see step 3 of the
[README](README.md)):

```sh
export BDF=0000:01:00.0           # replace with the card's address
```

## Switch between the factory image and the GPU image

The board's 64 MiB configuration flash holds two images:

| Slot | Flash address | Image | PCI ID | Kernel driver |
|---|---|---|---|---|
| `andromeda` | 0 | Factory image, loaded at power-up | `10ee:7024` | `xdma` |
| `gpu` | 32 MiB | The Apex GPU (`image/gpu_k7_flash.bin`) | `10ee:a15e` | `apex_gpu` |

`apex_flash.py boot` reloads the FPGA from a slot without a power cycle (a
warm boot), then removes and rescans the card, which comes back with the
other PCI ID. Before switching, stop every program that uses the card and
unbind its driver; the flash tool needs the card with no driver bound.

Check which image runs:

```sh
lspci -nnk -s $BDF                # 10ee:a15e = GPU image, 10ee:7024 = factory image
```

From the GPU image to the factory image:

```sh
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind
sudo python3 -I tools/apex_flash.py status --bdf $BDF   # slots, and how the FPGA last configured
sudo python3 -I tools/apex_flash.py boot --slot andromeda --bdf $BDF
lspci -nnk -s $BDF                # 10ee:7024; the xdma driver binds if installed
```

From the factory image back to the GPU image:

```sh
echo $BDF | sudo tee /sys/bus/pci/drivers/xdma/unbind   # only if xdma is bound
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
lspci -nnk -s $BDF                # 10ee:a15e
```

If the `apex_gpu` driver is already loaded, it binds on its own after the
rescan; if the probe fails with `-16`, use the BAR2 fix from
[step 4 of the README](README.md#4-build-and-load-the-kernel-driver). If it is
not loaded, load it as in step 4. Then check it with `vulkaninfo --summary`
([step 7](README.md#7-check-vulkan)).

## Update the factory image

The factory image is in slot 0, the image the FPGA loads at power-up and falls
back to when the GPU slot fails. Program it only from the GPU image, and only
with a factory image meant for this board (an `xc7k480t` BPI x16 flash binary
from Apex Compute): if slot 0 does not configure, the card does not come up
at the next power-up and has to be reprogrammed over JTAG.

From the GPU image, with no driver bound:

```sh
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/unbind
sudo python3 -I tools/apex_flash.py program FACTORY_IMAGE.bin --slot andromeda --bdf $BDF
sudo python3 -I tools/apex_flash.py verify  FACTORY_IMAGE.bin --slot andromeda --bdf $BDF
```

`program` erases, writes and reads back only the slot's blocks; it refuses an
image that would run into the GPU slot. Then test the new factory image with
a warm boot before the next power cycle, and switch back:

```sh
sudo python3 -I tools/apex_flash.py boot --slot andromeda --bdf $BDF
lspci -nnk -s $BDF                # 10ee:7024
echo $BDF | sudo tee /sys/bus/pci/drivers/xdma/unbind   # only if xdma is bound
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
```

Do not reboot or power off the host while programming.
