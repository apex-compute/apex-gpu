#!/usr/bin/env bash
# Nightly test of the apex-gpu guide on a Kintex-7 host, as an unprivileged
# runner user (see .github/nightly/README.md for the host setup):
#   .github/nightly/nightly.sh build|program|switch|smoke|cts|collect
# Results go to $NIGHTLY_OUT (default build/nightly);
# the CTS build persists in $NIGHTLY_STATE (default ~/apex-gpu-nightly).
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
export PATH="$HOME/.cargo/bin:$HOME/.local/bin:$PATH"
OUT=${NIGHTLY_OUT:-$root/build/nightly}
STATE=${NIGHTLY_STATE:-$HOME/apex-gpu-nightly}
CTS_TAG=${CTS_TAG:-vulkan-cts-1.4.7.1}
CTS=$STATE/cts/$CTS_TAG
VKDIR=$CTS/build/external/vulkancts/modules/vulkan
MUSTPASS=$CTS/external/vulkancts/mustpass/main
ICD=$root/build/mesa/src/apex/vulkan/apex_development_icd.x86_64.json
LOCK=/tmp/pcie_ci_hw_$(hostname).lock
# Cases per deqp-vk run.
CHUNK=500
mkdir -p "$OUT" "$STATE"

die() { echo "::error::$*" >&2; summary "- **failed:** $*"; exit 1; }
summary() { echo "$*" | tee -a "$OUT/summary.md" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"; }
locked() { exec 9< "$LOCK"; flock 9; }
unlocked() { flock -u 9; exec 9<&-; }

# The card: the Kintex-7 as the GPU (10ee:a15e) or as unified-engine (10ee:7024).
card() {
	local d
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor")" = 0x10ee ] || continue
		case $(cat "$d/device") in 0xa15e | 0x7024) basename "$d"; return ;; esac
	done
	die "no Kintex-7 (10ee:a15e or 10ee:7024) on the PCI bus"
}
sysfs() { echo "/sys/bus/pci/devices/$(card)"; }
device() { cat "$(sysfs)/device"; }
driver() { local d; d=$(sysfs)/driver; [ -e "$d" ] && basename "$(readlink "$d")" || true; }
unbind() { local drv; drv=$(driver); [ -z "$drv" ] || echo "$(card)" > "/sys/bus/pci/drivers/$drv/unbind"; }
flash() { python3 -I tools/apex_flash.py "$@" --bdf "$(card)"; }

# Bind apex_gpu. On a kernel before 6.18 the probe cannot resize BAR2 after a
# rescan, so resize it to its largest size while unbound first.
bind_gpu() {
	local d bits n
	udevadm settle
	d=$(sysfs)
	[ "$(device)" = 0xa15e ] || die "card is $(device), not the GPU image"
	if [ -z "$(driver)" ] && [ -e "$d/resource2_resize" ]; then
		bits=$((16#$(cat "$d/resource2_resize")))
		for ((n = 63; n >= 0; n--)); do ((bits >> n & 1)) && break; done
		echo "$n" > "$d/resource2_resize"
	fi
	[ "$(driver)" = apex_gpu ] || echo "$(card)" > /sys/bus/pci/drivers/apex_gpu/bind
	udevadm settle
	ls "$d/drm" | grep -q '^renderD' || die "apex_gpu bound but no render node"
}

build() {
	[ "$(id -u)" != 0 ] || die "run as the runner user, not root"
	id -nG | grep -qw render || die "$(id -un) is not in group render"
	journalctl -k -n 1 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p' > "$OUT/journal.cursor" || true
	(cd image && sha256sum -c SHA256SUMS) > "$OUT/image_check.txt" 2>&1 || die "image checksum mismatch"
	local kind
	kind=$(python3 -I -c "import sys; sys.path.insert(0, 'tools'); import apex_flash as a; print(a.image_kind(open(sys.argv[1], 'rb').read()))" image/*.bin)
	case $kind in *encrypted*) ;; *) die "image/*.bin is $kind" ;; esac
	make -s -C driver > "$OUT/driver_build.log" 2>&1 || die "kernel driver build failed (driver_build.log)"
	local built installed
	built=$(modinfo -F srcversion driver/apex_pci.ko)
	installed=$(cat /sys/module/apex_pci/srcversion 2>/dev/null || true)
	[ "$built" = "$installed" ] ||
		echo "::warning::the loaded apex_pci ($installed) is not this checkout's ($built); install it on the host to test this driver"
	[ -f build/mesa/build.ninja ] || meson setup build/mesa mesa -Dbuildtype=release -Dstrip=true \
		-Dapex-compiler=true -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms=x11 \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled > "$OUT/mesa_setup.log" 2>&1 ||
		die "meson setup failed (mesa_setup.log)"
	ninja -C build/mesa src/apex/vulkan/libvulkan_apex.so \
		src/apex/vulkan/apex_development_icd.x86_64.json > "$OUT/mesa_build.log" 2>&1 ||
		die "Vulkan driver build failed (mesa_build.log)"
	if [ ! -x "$VKDIR/deqp-vk" ]; then
		[ -d "$CTS" ] || git clone -q --depth 1 --branch "$CTS_TAG" https://github.com/KhronosGroup/VK-GL-CTS "$CTS"
		(cd "$CTS" && python3 external/fetch_sources.py && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release &&
			ninja -C build deqp-vk) > "$OUT/cts_build.log" 2>&1 || die "CTS build failed (cts_build.log)"
	fi
	summary "## apex-gpu nightly, $(git log -1 --format='%h %s')"
	summary "- image: $(ls image/*.bin | xargs -n1 basename), $kind"
	summary "- kernel driver: built $built, loaded ${installed:-none}"
	summary "- Mesa: $(git -C mesa log -1 --format=%h)"
}

# FLASH.md Flash mode step 1: program image/*.bin into the GPU slot, verify it,
# boot it and check that the FPGA configured from it.
program() {
	locked
	[ "$(device)" = 0xa15e ] || { unbind; flash boot --slot gpu; udevadm settle; }
	[ "$(device)" = 0xa15e ] || die "the card does not boot the GPU slot: program it over JTAG"
	bind_gpu
	unbind
	flash program image/*.bin --slot gpu 2>&1 | tr '\r' '\n' | tail -3 > "$OUT/flash_program.txt"
	[ "${PIPESTATUS[0]}" = 0 ] || die "apex_flash.py program failed (flash_program.txt)"
	flash verify image/*.bin --slot gpu > "$OUT/flash_verify.txt" 2>&1 || die "apex_flash.py verify failed (flash_verify.txt)"
	flash boot --slot gpu > "$OUT/flash_boot.txt" 2>&1 || die "boot --slot gpu failed after programming"
	udevadm settle
	[ "$(device)" = 0xa15e ] || die "the programmed image did not configure: the card fell back to unified-engine"
	bind_gpu
	unbind
	flash status > "$OUT/flash_status_after.txt" || die "apex_flash.py status failed"
	grep -q 'latest: VALID IPROG' "$OUT/flash_status_after.txt" || die "BOOTSTS does not report a clean boot (flash_status_after.txt)"
	bind_gpu
	unlocked
	summary "- flash: programmed, verified and booted $(ls image/*.bin | xargs -n1 basename) in the GPU slot; $(grep -o 'BOOTSTS[^)]*)' "$OUT/flash_status_after.txt"): pass"
}

# FLASH.md Boot mode: unified-engine from the GPU image, then the GPU image back.
switch() {
	locked
	[ "$(device)" = 0xa15e ] || { unbind; flash boot --slot gpu; udevadm settle; }
	bind_gpu
	unbind
	flash status > "$OUT/flash_status.txt" || die "apex_flash.py status failed"
	flash boot --slot unified-engine > "$OUT/boot_unified_engine.txt" 2>&1 || die "boot --slot unified-engine failed"
	udevadm settle
	[ "$(device)" = 0x7024 ] || die "after boot --slot unified-engine the card is $(device), not 10ee:7024"
	local ue_driver; ue_driver=$(driver)
	unbind
	flash boot --slot gpu > "$OUT/boot_gpu.txt" 2>&1 || die "boot --slot gpu failed"
	udevadm settle
	[ "$(device)" = 0xa15e ] || die "after boot --slot gpu the card is $(device): the GPU slot did not configure"
	bind_gpu
	unlocked
	summary "- switch: GPU -> unified-engine (10ee:7024, driver ${ue_driver:-none}) -> GPU (10ee:a15e, apex_gpu): pass"
}

# FLASH.md step 3, with the kernel journal in place of sudo dmesg.
smoke() {
	local bdf node log
	bdf=$(card)
	locked
	{
		lspci -nnk -s "$bdf"
		ls "$(sysfs)/drm/"
		journalctl -k -b --no-pager 2>/dev/null | grep "apex_gpu $bdf" | tail -2
		VK_DRIVER_FILES=$ICD MESA_VK_WSI_DEBUG=sw vulkaninfo --summary 2>&1 | grep -E "deviceName|apiVersion"
		VK_DRIVER_FILES=$ICD MESA_VK_WSI_DEBUG=sw timeout 300 xvfb-run -a vkcube --c 300 --width 512 --height 512; echo "vkcube exit $?"
	} > "$OUT/smoke.log" 2>&1
	unlocked
	log=$OUT/smoke.log
	grep -q '\[10ee:a15e\]' "$log" || die "smoke: the card is not 10ee:a15e"
	grep -q 'Kernel driver in use: apex_gpu' "$log" || die "smoke: apex_gpu is not bound"
	node=$(grep -o 'renderD[0-9]*' "$log" | head -1)
	[ -n "$node" ] || die "smoke: no render node"
	grep -q 'deviceName *= Apex' "$log" || die "smoke: Vulkan does not list the Apex device"
	grep -q 'apiVersion *= 1\.4' "$log" || die "smoke: Vulkan API version is not 1.4"
	grep -q '^vkcube exit 0$' "$log" || die "smoke: vkcube did not exit 0 (smoke.log)"
	summary "- smoke test: 10ee:a15e, apex_gpu, $node, Vulkan Apex 1.4, vkcube 300 frames: pass"
}

# Run a case list through deqp-vk in chunks, each under the card lock, and
# append "<status> <case>" to $2. A deqp-vk that dies records its running case
# as Crash and carries on with the rest.
run_cases() {
	local list=$1 results=$2 chunk=$OUT/chunk.txt log=$OUT/chunk.log rc
	local todo=$OUT/todo.txt
	cp "$list" "$todo"
	while [ -s "$todo" ]; do
		head -n "$CHUNK" "$todo" > "$chunk"
		locked
		rc=0
		(cd "$VKDIR" && VK_DRIVER_FILES=$ICD timeout -k 10 3600 ./deqp-vk --deqp-caselist-file="$chunk" \
			--deqp-log-filename=/dev/null --deqp-log-images=disable --deqp-log-shader-sources=disable) > "$log" 2>&1 || rc=$?
		unlocked
		awk '/^Test case /{c=$3; gsub(/^'"'"'|'"'"'\.\.$/, "", c); next}
			c != "" && /^  [A-Za-z]+ \(/{print $1, c; c=""}' "$log" > "$OUT/chunk_results.txt"
		cat "$OUT/chunk_results.txt" >> "$results"
		if ! grep -q '^DONE!' "$log"; then
			# The first case without a result is the one that was running.
			local crashed
			crashed=$(awk 'NR == FNR {done[$2] = 1; next} !($1 in done) {print; exit}' "$OUT/chunk_results.txt" "$chunk")
			[ -z "$crashed" ] || echo "$([ $rc = 124 ] && echo Timeout || echo Crash) $crashed" >> "$results"
			cp "$log" "$OUT/crash_$(date +%s).log"
		fi
		awk 'NR == FNR {done[$2] = 1; next} !($1 in done)' "$results" "$todo" > "$todo.new" && mv "$todo.new" "$todo"
	done
	rm -f "$todo" "$chunk"
}

count() { awk '{print $1}' "$1" | sort | uniq -c | sort -rn | awk '{printf "%s%s %s", (NR > 1 ? ", " : ""), $2, $1}'; }

# The CTS mandatory list and the API smoke cases: every case must pass or be
# unsupported.
cts() {
	local list=$OUT/cts_cases.txt results=$OUT/cts.txt bad
	{ cat "$MUSTPASS/vk-fraction-mandatory-tests.txt"; grep '^dEQP-VK\.api\.smoke\.' "$MUSTPASS/vk-default/api.txt"; } |
		awk 'NF && !seen[$0]++' > "$list"
	: > "$results"
	run_cases "$list" "$results"
	summary "- CTS mandatory and api.smoke ($(wc -l < "$results") cases): $(count "$results")"
	bad=$(awk '$1 !~ /^(Pass|NotSupported|QualityWarning|CompatibilityWarning)$/' "$results")
	[ -z "$bad" ] || { summary "$(echo "$bad" | sed 's/^/  - /')"; die "CTS cases failed"; }
}

collect() {
	[ -s "$OUT/journal.cursor" ] && journalctl -k --after-cursor="$(cat "$OUT/journal.cursor")" > "$OUT/kernel.log" 2>&1 || true
	grep -iE "apex_gpu.*(fault|missed its deadline|hardware failed|did not halt|not drain)" "$OUT/kernel.log" > "$OUT/kernel_errors.txt" 2>/dev/null || true
	[ ! -s "$OUT/kernel_errors.txt" ] || summary "- kernel: $(wc -l < "$OUT/kernel_errors.txt") apex_gpu error lines (kernel_errors.txt)"
}

"$@"
