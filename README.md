<p align="center">
  <img src="black-logo-with-text.png" alt="Apex Compute" width="300">
</p>

<p align="center">
  A Vulkan 1.4 GPU for the AMD Kintex-7 (<code>xc7k480t</code>) PCIe board
</p>

<p align="center">
  <a href="https://discord.gg/SqprdvQXnC"><img src="https://img.shields.io/badge/Discord-Join%20Us-7289da?logo=discord&logoColor=white" alt="Discord"></a>
</p>

# Apex GPU Setup and Usage Guide

This guide takes a new host from a fresh install to `vkcube` spinning on the
card. You build both drivers yourself: the kernel driver from `driver/`, and
the Vulkan driver from the `mesa/` submodule. The GPU image is prebuilt and
encrypted for the board's key.

| Folder | Contents |
| --- | --- |
| `image/` | GPU image for the board's flash |
| `tools/` | `apex_flash.py`, which programs and boots the flash over PCIe |
| `driver/` | Linux kernel driver (`apex_pci`) |
| `mesa/` | Vulkan driver source: the `mesa-kintex7` branch of [apex-compute/mesa](https://github.com/apex-compute/mesa) (submodule) |
| `demos/` | Extra demos, such as the Apex Compute cube |
| `requirements.txt` | Ubuntu packages for building and running the drivers |
| `FLASH.md` | Switching between the factory and GPU images |

## Prerequisites

- A Kintex-7 board from Apex Compute in a PCIe slot.
- An x86-64 host running Ubuntu 24.04, with `sudo` and internet access.
- Linux 6.11 or newer (tested on 6.11). Check with `uname -r`.
- Secure Boot disabled, or a way to sign kernel modules. Check with
  `mokutil --sb-state`.
- An X11 display for the cube's window: a desktop on the host, or a remote
  desktop session.

> **Tip:** Ubuntu 24.04's original kernel is 6.8. Install the HWE kernel and
> reboot into it:
> ```bash
> sudo apt install linux-generic-hwe-24.04
> ```

## Installation

### 1. Get the Repository and Packages

```bash
sudo apt update
sudo apt install git
git clone https://github.com/apex-compute/apex-gpu.git
cd apex-gpu
sudo apt install linux-headers-$(uname -r) $(grep -v '^#' requirements.txt)
```

Run the remaining steps from the `apex-gpu` folder.

### 2. Flash the GPU Image

Find the card, then program the image into its GPU slot (a few minutes):

```bash
# The card is 10ee:7024 (factory image) or 10ee:a15e (GPU image)
lspci -D -nn -d 10ee:

# Replace with the card's address
export BDF=0000:01:00.0

sudo python3 -I tools/apex_flash.py program image/gpu_k7_flash.bin --slot gpu --bdf $BDF
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF

# The GPU image shows up as 10ee:a15e
lspci -s $BDF -n
```

> **Warning:** Do not reboot or power off the host while programming.
> Programming is needed once; the image stays in the flash.

### 3. Build and Load the Kernel Driver

```bash
make -C driver
sudo modprobe -a drm_exec drm_gpuvm gpu_sched
sudo insmod driver/apex_pci.ko

# A new render node appears
ls /dev/dri/renderD*
```

> **Tip:** If `modprobe` cannot find `drm_gpuvm`, install the extra modules
> for your kernel:
> ```bash
> sudo apt install linux-modules-extra-$(uname -r)
> ```

> **Tip:** If `insmod` fails with `Key was rejected by service`, Secure Boot is
> on: disable it in the firmware setup, or sign the module.

> **Tip:** On kernels older than 6.18 the driver can fail to probe with error
> `-16` (`sudo dmesg | grep apex`). Resize BAR2 to 2 GiB and bind it again:
> ```bash
> echo 11   | sudo tee /sys/bus/pci/devices/$BDF/resource2_resize
> echo $BDF | sudo tee /sys/bus/pci/drivers/apex_gpu/bind
> ```

`insmod` loads the driver until the next reboot; repeat this step after one.

### 4. Give Your User Access to the Card

```bash
sudo usermod -aG render $USER
```

Log out and back in (or start a new SSH session) so the group applies, then
go back to the folder and set the card's address again:

```bash
cd apex-gpu
export BDF=0000:01:00.0
```

### 5. Build the Vulkan Driver

Mesa needs meson 1.4 or newer and Rust 1.85 or newer, which are newer than the
Ubuntu 24.04 packages. The submodule fetch downloads only the pinned commit;
the build takes a few minutes:

```bash
# meson and Rust
pipx install 'meson>=1.4' && pipx ensurepath && export PATH=$HOME/.local/bin:$PATH
curl https://sh.rustup.rs -sSf | sh -s -- -y && . "$HOME/.cargo/env"

# Fetch and build the Vulkan driver
git submodule update --init mesa
meson setup build/mesa mesa -Dbuildtype=release -Dstrip=true \
  -Dapex-compiler=true -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms=x11 \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled
ninja -C build/mesa src/apex/vulkan/libvulkan_apex.so \
  src/apex/vulkan/apex_development_icd.x86_64.json
```

This builds `libvulkan_apex.so` and its manifest,
`build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json`.

> **Tip:** To move to the newest `mesa-kintex7`, update the submodule and run
> the `ninja` command again:
> ```bash
> git submodule update --remote mesa
> ```

### 6. Check Vulkan

```bash
export VK_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json

# deviceName = Apex, apiVersion = 1.4.0
vulkaninfo --summary
```

`VK_DRIVER_FILES` points the Vulkan loader at the driver you built. It is not
installed system-wide, so set it in every shell that runs Vulkan.

### 7. Run vkcube

`vkcube` draws a spinning textured cube. The card renders each frame and the
CPU copies it to an X11 window (`MESA_VK_WSI_DEBUG=sw`), because the card has
no display path to the X server. Expect about 9 frames per second at 512x512.

Without a display, render 300 frames into a virtual X server; it exits with
status 0:

```bash
export VK_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json
export MESA_VK_WSI_DEBUG=sw
xvfb-run -a vkcube --c 300 --width 512 --height 512; echo "exit $?"
```

To watch it, run it in an X11 desktop on the host. It prints
`Selected GPU 0: Apex, type: DiscreteGpu`; close the window to stop it:

```bash
export VK_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json
export MESA_VK_WSI_DEBUG=sw
vkcube --width 512 --height 512
```

> **Tip:** On a host without a desktop, install a remote desktop server, connect
> with any RDP client (Windows Remote Desktop, Remmina, Windows App), and run
> the commands above in a terminal there, from the `apex-gpu` folder:
> ```bash
> sudo apt install xrdp xterm openbox
> ```

### 8. Run More Demos

Set the driver and presentation the same way as for `vkcube`, then run any of
these. Each one renders on the card; add `xvfb-run -a` in front to run it
without a display.

```bash
export VK_DRIVER_FILES=$PWD/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json
export MESA_VK_WSI_DEBUG=sw
```

**vkcubepp**, the C++ version of `vkcube` (from `vulkan-tools`):

```bash
vkcubepp --width 512 --height 512
```

**vkmark**, a Vulkan benchmark: 13 scenes (vertex, texture, shading, 2D
effects, desktop, cube, clear), each for 10 seconds, then a score (the average
frames per second; about 3 on the Kintex-7 at its default 800x600):

```bash
vkmark --winsys xcb -p fifo

# Only some scenes
vkmark --winsys xcb -p fifo -b cube -b texture -b shading:shading=phong
```

**Apex Compute cube**: `vkcube` with the Apex Compute logo as its texture.
`demos/apex-cube/build.sh` fetches Khronos Vulkan-Tools, swaps in the logo and
builds it into `build/apex-cube/vkcube`:

```bash
demos/apex-cube/build.sh
build/apex-cube/vkcube --width 512 --height 512
```

---

## After a Power Cycle

The board loads its factory image at power-up. Boot the GPU slot again (no
need to program it), then load the kernel driver ([step 3](#3-build-and-load-the-kernel-driver)):

```bash
cd apex-gpu
export BDF=0000:01:00.0
sudo python3 -I tools/apex_flash.py boot --slot gpu --bdf $BDF
```

## Switch Images and Update the Factory Image

The board's flash also holds a factory image. To switch between it and the GPU
image, or to update it, see [FLASH.md](FLASH.md).

## License

The kernel driver is GPL-2.0; everything else is MIT. See each file's SPDX
header and `LICENSE`.
