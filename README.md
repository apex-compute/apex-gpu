# Apex GPU

A Vulkan 1.4 GPU for the AMD Kintex-7 (`xc7k480t`) PCIe board.

| Folder | Contents |
|---|---|
| `image/` | GPU image for the board's flash |
| `tools/` | `apex_flash.py`, which programs and boots the flash over PCIe |
| `driver/` | Linux kernel driver (`apex_pci`) |
| `vulkan/` | Vulkan driver (ICD) for x86-64 Linux, prebuilt |

The steps below take a new host from a fresh install to `vkcube` spinning on
the card, using the prebuilt files. The GPU image is encrypted for the
board's key.

## 1. What you need

- A Kintex-7 board from Apex Compute in a PCIe slot.
- An x86-64 host running Ubuntu 24.04 (other recent Linux distributions work
  with the equivalent packages), with `sudo` and internet access.
- An X11 display for the window: a desktop on the host, or a remote desktop
  session (step 7).

## 2. Install the host packages and get the repository

```sh
sudo apt update
sudo apt install git build-essential linux-headers-$(uname -r) python3 \
  pciutils libvulkan1 vulkan-tools xvfb
git clone https://github.com/apex-compute/apex-gpu.git
cd apex-gpu
```

## 3. Flash the GPU image

Find the card and program the image into its GPU slot (a few minutes):

```sh
lspci -D -d 10ee:                 # the card's address, e.g. 0000:01:00.0
export BDF=0000:01:00.0           # replace with your card's address
sudo python3 -I tools/apex_flash.py program image/gpu_k7_flash.bin --slot gpu --bdf $BDF
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
lspci -s $BDF -n                  # the GPU image shows up as 10ee:a15e
```

Do not reboot or power off the host while programming. Programming is needed
once; the image stays in the flash.

## 4. Load the kernel driver

```sh
make -C driver
sudo modprobe -a drm_exec drm_gpuvm gpu_sched
sudo insmod driver/apex_pci.ko
ls /dev/dri/renderD*              # a new render node appears
```

On kernels older than 6.18 the driver can fail to probe with error `-16`
(`sudo dmesg | grep apex`). Resize BAR2 to 2 GiB and bind it again:

```sh
echo 11   | sudo tee /sys/bus/pci/devices/$BDF/resource2_resize
echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/bind
```

## 5. Give your user access to the card

```sh
sudo usermod -aG render $USER
```

Log out and back in (or start a new SSH session) so the group applies.

## 6. Check Vulkan

```sh
export VK_DRIVER_FILES=$PWD/vulkan/apex_icd.x86_64.json
vulkaninfo --summary              # deviceName = Apex, apiVersion = 1.4.0
```

`VK_DRIVER_FILES` points the Vulkan loader at the Apex driver; it is not
installed system-wide, so set it in every shell that runs Vulkan.

## 7. Run vkcube

`vkcube` draws a spinning textured cube. The card renders each frame and the
CPU copies it to an X11 window, because the card has no display path to the
X server (`MESA_VK_WSI_DEBUG=sw`). Expect about 9 frames per second at
512x512.

Without a display, render 300 frames into a virtual X server; it exits with
status 0:

```sh
export VK_DRIVER_FILES=$PWD/vulkan/apex_icd.x86_64.json
export MESA_VK_WSI_DEBUG=sw
xvfb-run -a vkcube --c 300 --width 512 --height 512; echo "exit $?"
```

To watch it, run it in an X11 desktop on the host:

```sh
export VK_DRIVER_FILES=$PWD/vulkan/apex_icd.x86_64.json
export MESA_VK_WSI_DEBUG=sw
vkcube --width 512 --height 512   # prints: Selected GPU 0: Apex, type: DiscreteGpu
```

On a host without a desktop, install a remote desktop server and connect to
the host with any RDP client (Windows Remote Desktop, Remmina, Windows App),
then run the commands above in a terminal there:

```sh
sudo apt install xrdp xterm openbox
```

Close the window to stop it.

## After a power cycle

The board loads its default image at power-up. Boot the GPU slot again (no
need to program it), then repeat step 4:

```sh
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
```

## License

The kernel driver is GPL-2.0; everything else is MIT. See each file's SPDX
header and `LICENSE`.
