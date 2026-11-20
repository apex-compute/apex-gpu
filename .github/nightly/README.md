# Nightly Kintex-7 Test

`.github/workflows/nightly.yml` runs the guide on a lab host with a Kintex-7
every night (06:00 UTC) and on a manual run from the Actions tab. Pull requests
never start it. Each night it:

1. Builds the kernel driver and the Vulkan driver from the checkout, and checks
   that `image/*.bin` matches `SHA256SUMS` and is encrypted.
2. Programs `image/*.bin` into the GPU slot, verifies it and boots it
   ([FLASH.md Flash mode](../../FLASH.md#flash-mode)).
3. Switches the card to unified-engine and back to the GPU image
   ([FLASH.md Boot mode](../../FLASH.md#boot-mode)).
4. Runs the [smoke test](../../FLASH.md#3-smoke-test-the-gpu-image).
5. Runs a short Vulkan CTS check, the mandatory list and `dEQP-VK.api.smoke.*`
   (31 cases, a few minutes): every case must pass or be unsupported.

The whole run takes about 15 minutes.

The results are on the run page and in its `nightly-<run>` artifact. A failed
night opens (or comments on) the issue labelled `nightly`.

The test rewrites only the GPU slot, never unified-engine; if the new image
does not configure, the card falls back to unified-engine and the night fails.
It never loads a kernel driver: it tests the driver already on the host, and
warns when the loaded driver differs from the checkout's.

## Host Setup

The runner runs as an unprivileged user (here `ci`), on the host the card is
in, with the [README's packages](../../README.md#1-get-the-repository-and-packages),
meson and Rust installed for that user.

### 1. Groups

```bash
sudo groupadd -f apexci
sudo usermod -aG render,systemd-journal,apexci ci
```

### 2. Card Access

The test unbinds and binds the card's driver, programs and reads the flash and
warm-boots the card without root. Give the `apexci` group the card's sysfs files and the
PCI rescan:

```bash
sudo tee /etc/udev/rules.d/81-apex-gpu-nightly.rules <<'EOF'
# Kintex-7 as the GPU (10ee:a15e) or unified-engine (10ee:7024)
ACTION=="add|bind", SUBSYSTEM=="pci", ATTR{vendor}=="0x10ee", ATTR{device}=="0x7024|0xa15e", \
  RUN+="/bin/sh -c 'cd /sys%p && chgrp apexci config remove resource resource[0-5] resource2_resize 2>/dev/null; chmod g+rw config resource[0-5] 2>/dev/null; chmod g+w remove resource2_resize 2>/dev/null; true'"
ACTION=="add", SUBSYSTEM=="drivers", KERNEL=="xdma|apex_gpu", \
  RUN+="/bin/sh -c 'cd /sys%p && chgrp apexci bind unbind && chmod g+w bind unbind'"
EOF
echo 'z /sys/bus/pci/rescan 0220 root apexci -' | sudo tee /etc/tmpfiles.d/apex-gpu-nightly.conf
sudo udevadm control --reload
sudo systemd-tmpfiles --create /etc/tmpfiles.d/apex-gpu-nightly.conf
```

Install the kernel driver (README step 3) so that it loads when the card comes
back after a warm boot, and keep `/tmp/pcie_ci_hw_$(hostname).lock` readable by
`ci`: every step that uses the card holds that lock, so other jobs on the card
wait for it.

### 3. Runner

Register a runner for this repository with the label `apex-gpu-k7`
(Settings → Actions → Runners → New self-hosted runner), as `ci`:

```bash
./config.sh --url https://github.com/apex-compute/apex-gpu --token <TOKEN> \
  --name apex-gpu-k7 --labels apex-gpu-k7 --work _work
sudo ./svc.sh install ci && sudo ./svc.sh start
```

The first night also clones and builds the Vulkan CTS into `~/apex-gpu-nightly`
(about an hour); later nights reuse it.
