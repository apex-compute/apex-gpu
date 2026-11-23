#!/usr/bin/env python3
"""Program and boot the Kintex-7 configuration flash over PCIe.

    apex_flash.py status  --bdf BDF
    apex_flash.py program IMAGE.bin --slot gpu --bdf BDF
    apex_flash.py verify  IMAGE.bin --slot gpu --bdf BDF
    apex_flash.py boot    --slot gpu --bdf BDF

The flash holds two images: the unified-engine image at 0, which the FPGA
loads at power-up and falls back to, and the GPU image at 32 MiB. `program` erases,
writes and reads back the GPU slot. `boot` reloads the FPGA from the slot and
rescans the bus; the card returns as 10ee:a15e. `status` reports how the FPGA
last configured.

Run as root with no driver bound to the card. Never reboot or power off the
host while programming.
"""
import argparse
import hashlib
import mmap
import os
import struct
import subprocess
import sys
import time

PCI_COMMAND, PCI_COMMAND_MEMORY = 0x04, 0x2
GPU_DEVICE, UNIFIED_ENGINE_DEVICE = "0xa15e", "0x7024"
MAGIC = 0x41504558
PROFILE_U50, PROFILE_K7 = 2, 3
FLASH_BYTES = 64 << 20
SLOT_BYTES = 32 << 20
SLOTS = {"unified-engine": 0x0000000, "gpu": 0x2000000}
BLOCK_BYTES = 256 << 10
BUFFER_BYTES = 1024
RELOAD_SECONDS = 8
ID_VALUE = 0x4B374250
IPROG_KEY = 0x49505247

# GPU image: BAR5 is 64 MiB, its control page the top 4 KiB.
CONTROL = 0x3FFF000
C_ID, C_WBSTAR, C_IPROG, C_BOOTSTS, C_STATUS = 0x000, 0x004, 0x008, 0x00C, 0x010
# Older GPU images: controls in BAR0, a paged 4 KiB BAR0 window or a 64 MiB
# BAR5 onto the flash.
CAP_BPI_FLASH = 1 << 10
WINDOW, WINDOW_BYTES = 0x1000, 0x1000
L_ID, L_PAGE, L_WBSTAR, L_IPROG = 0xF000, 0xF004, 0xF008, 0xF00C
# The unified-engine image's warm-boot block.
A_BASE = 0x02030000
A_ID, A_WBSTAR, A_CTRL = 0x0, 0x4, 0x8
A_ID_VALUE = 0xB0071CA2

# MT28GU512AAA commands, written as a 32-bit pair: low half first, then the
# high half at the next word address.
READ_ARRAY, READ_STATUS, CLEAR_STATUS, READ_ID = 0xFF, 0x70, 0x50, 0x90
UNLOCK, ERASE, BUFFERED, CONFIRM = 0x60, 0x20, 0xE9, 0xD0
READY, ERRORS = 0x80, 0x3A

# BOOTSTS (UG470): status of the latest configuration in bits 7:0 and of the
# one before it in bits 15:8.
BOOTSTS_BITS = ["VALID", "FALLBACK", "IPROG", "WTO_ERROR", "ID_ERROR", "CRC_ERROR",
                "WRAP_ERROR", "HMAC_ERROR"]


def mapping(path, index, size):
    fd = os.open(f"{path}/resource{index}", os.O_RDWR | os.O_SYNC)
    region = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    os.close(fd)
    return memoryview(region).cast("I")


class Card:
    def __init__(self, bdf):
        self.bdf = bdf
        self.path = f"/sys/bus/pci/devices/{bdf}"
        if not os.path.exists(self.path):
            sys.exit(f"{bdf} is not present")
        ids = tuple(open(f"{self.path}/{n}").read().strip() for n in ("vendor", "device"))
        if ids not in (("0x10ee", GPU_DEVICE), ("0x10ee", UNIFIED_ENGINE_DEVICE)):
            sys.exit(f"{bdf} is {ids[0]}:{ids[1]}, neither the Apex GPU nor unified-engine")
        if os.path.exists(f"{self.path}/driver"):
            sys.exit(f"{bdf} has a driver bound; unload it first")
        # Set Memory Space in the command register through config, which a udev
        # rule can open to a group; the enable attribute is root-only.
        with open(f"{self.path}/config", "r+b") as f:
            f.seek(PCI_COMMAND)
            command = int.from_bytes(f.read(2), "little")
            if not command & PCI_COMMAND_MEMORY:
                f.seek(PCI_COMMAND)
                f.write((command | PCI_COMMAND_MEMORY).to_bytes(2, "little"))
        with open(f"{self.path}/resource") as f:
            sizes = [end - start + 1 if end else 0
                     for start, end, _ in (map(lambda x: int(x, 16), line.split()) for line in f)]
        self.flash = None
        self.page = None
        if ids[1] == UNIFIED_ENGINE_DEVICE:
            self.kind = "unified-engine"
            self.bar = mapping(self.path, 0, sizes[0])
            if self.reg(A_BASE + A_ID) != A_ID_VALUE:
                sys.exit(f"unified-engine image without icap_warmboot (ID {self.reg(A_BASE + A_ID):#010x})")
            return
        bar0 = mapping(self.path, 0, 65536)
        if bar0[0] != MAGIC or bar0[2] != PROFILE_K7:
            sys.exit(f"{bdf} is not a Kintex-7 GPU image")
        bar5 = len(sizes) > 5 and sizes[5] == FLASH_BYTES
        # The image BAR answers with the flash ID in its control page. Images
        # m14 and m15 had the window in BAR0 instead and set capability bit 10
        # (CAP_BPI_FLASH); newer images may reuse that bit, so the ID decides.
        if bar5:
            image = mapping(self.path, 5, FLASH_BYTES)
            if image[(CONTROL + C_ID) >> 2] == ID_VALUE:
                self.kind = "image-bar"
                self.bar = self.flash = image
                return
        if not bar0[3] & CAP_BPI_FLASH:
            if not bar5:
                sys.exit(f"{bdf} has no image BAR (BAR5 unassigned: the slot's memory window is too small)")
            sys.exit(f"image BAR identity {image[(CONTROL + C_ID) >> 2]:#010x}, expected {ID_VALUE:#010x}")
        self.kind = "legacy"
        self.bar = bar0
        if self.reg(L_ID) != ID_VALUE:
            sys.exit(f"BAR0 flash identity {self.reg(L_ID):#010x}, expected {ID_VALUE:#010x}")
        if bar5:
            self.flash = image

    def reg(self, offset):
        return self.bar[offset >> 2]

    def set_reg(self, offset, value):
        self.bar[offset >> 2] = value & 0xFFFFFFFF

    def control(self, name):
        offsets = {"image-bar": {"wbstar": CONTROL + C_WBSTAR, "iprog": CONTROL + C_IPROG},
                   "legacy": {"wbstar": L_WBSTAR, "iprog": L_IPROG},
                   "unified-engine": {"wbstar": A_BASE + A_WBSTAR, "iprog": A_BASE + A_CTRL}}
        return offsets[self.kind][name]

    def _window(self, address):
        page = address // WINDOW_BYTES
        if page != self.page:
            self.set_reg(L_PAGE, page)
            self.page = page
        return WINDOW + address % WINDOW_BYTES

    def flash_read(self, address):
        if self.flash is not None:
            return self.flash[address >> 2]
        return self.reg(self._window(address))

    def flash_write(self, address, value):
        if self.flash is not None:
            self.flash[address >> 2] = value & 0xFFFFFFFF
        else:
            self.set_reg(self._window(address), value)

    def command(self, address, first, second=None):
        self.flash_write(address, first | ((first if second is None else second) << 16))

    def wait_ready(self, address, seconds):
        deadline = time.monotonic() + seconds
        while True:
            status = self.flash_read(address) & 0xFFFF
            if status & READY:
                break
            if time.monotonic() > deadline:
                raise RuntimeError(f"flash busy at {address:#x}, status {status:#06x}")
        if status & ERRORS:
            self.command(address, CLEAR_STATUS)
            self.command(address, READ_ARRAY)
            raise RuntimeError(f"flash error at {address:#x}, status {status:#06x}")
        self.command(address, READ_ARRAY)


def decode_bootsts(value):
    parts = []
    for index, shift in (("latest", 0), ("previous", 8)):
        bits = [name for i, name in enumerate(BOOTSTS_BITS) if value >> (shift + i) & 1]
        parts.append(f"{index}: {' '.join(bits) if bits else 'none'}")
    return "; ".join(parts)


def status(card):
    names = {"image-bar": "GPU image, image BAR (BAR5)", "unified-engine": "unified-engine image",
             "legacy": "older GPU image, BAR0 controls"}
    print(f"{card.bdf}: {names[card.kind]}")
    if card.kind == "image-bar":
        ready = card.reg(CONTROL + C_STATUS)
        bootsts = card.reg(CONTROL + C_BOOTSTS)
        if ready & 2:
            print(f"BOOTSTS {bootsts:#010x} ({decode_bootsts(bootsts)})")
        else:
            print("BOOTSTS not captured" if ready & 1 else "BOOTSTS read pending")
    if card.kind != "unified-engine":
        card.command(0, CLEAR_STATUS)
        card.command(0, READ_ID)
        ident = card.flash_read(0)
        card.command(0, READ_ARRAY)
        print(f"flash manufacturer {ident & 0xFFFF:#06x} device {ident >> 16:#06x}, through "
              f"{'BAR5' if card.flash is not None else 'the BAR0 window'}")
        # An image opens with 0xFF padding before its bus-width pattern at
        # byte 0x20, so a slot is empty only if its first 64 bytes are.
        for name, offset in SLOTS.items():
            empty = all(card.flash_read(offset + 4 * i) == 0xFFFFFFFF for i in range(16))
            print(f"slot {name} at {offset:#09x}: {'empty' if empty else 'programmed'}")
    print(f"WBSTAR {card.reg(card.control('wbstar')):#010x}")


def target(args):
    if args.slot is not None:
        return SLOTS[args.slot]
    return args.offset


def image_kind(data):
    """Whether a BPIx16 flash image is AES-encrypted, from its clear header.

    write_cfgmem reverses the bits of each byte and swaps the bytes of each
    16-bit word; the header up to the encrypted payload stays clear. CTL0 bit
    6 (DEC) enables the decryptor and bit 31 (EFUSE_KEY) selects the eFUSE key
    (UG470, Control Register 0).
    """
    head = bytearray(int(f"{b:08b}"[::-1], 2) for b in data[:4096])
    head[0::2], head[1::2] = head[1::2], head[0::2]
    sync = head.find(bytes.fromhex("aa995566"))
    if sync < 0 or sync % 4:
        return "not a BPIx16 bitstream"
    mask = ctl0 = 0
    for offset in range(sync + 4, len(head) - 7, 4):
        word, value = struct.unpack_from(">II", head, offset)
        if word == 0x3000C001:  # type 1 write, MASK, 1 word
            mask = value
        elif word == 0x3000A001:  # type 1 write, CTL0, 1 word
            ctl0 = ctl0 & ~mask | value & mask
        elif word == 0x30034001 or word & 0xFFFFE000 == 0x30004000:
            break  # the encrypted word count, or frame data (FDRI) follows
    if not ctl0 & 0x40:
        return "unencrypted"
    return f"AES-encrypted for the {'eFUSE' if ctl0 >> 31 else 'battery-backed'} key"


def load(path, offset):
    data = open(path, "rb").read()
    print(f"{path}: {image_kind(data)}")
    if offset % BLOCK_BYTES or offset + len(data) > CONTROL:
        sys.exit("the image must start on a 256 KiB block and end below the image BAR's control page")
    for other in SLOTS.values():
        if offset < other < offset + len(data):
            sys.exit(f"the image at {offset:#x} would overwrite the slot at {other:#x}")
    return data + b"\xff" * (-len(data) % 4)


def program(card, data, offset):
    end = offset + len(data)
    for block in range(offset, end, BLOCK_BYTES):
        card.command(block, CLEAR_STATUS)
        card.command(block, UNLOCK, CONFIRM)
        card.command(block, ERASE, CONFIRM)
        card.wait_ready(block, 8)
        print(f"erased {block:#010x}", end="\r", flush=True)
    print()
    for start in range(0, len(data), BUFFER_BYTES):
        chunk = data[start:start + BUFFER_BYTES]
        address = offset + start
        card.command(address, CLEAR_STATUS, READ_STATUS)
        card.command(address, BUFFERED, len(chunk) // 2 - 1)
        for i, (word,) in enumerate(struct.iter_unpack("<I", chunk)):
            card.flash_write(address + 4 * i, word)
        card.command(address, CONFIRM, READ_STATUS)
        card.wait_ready(address, 1)
        if start % (1 << 20) == 0:
            print(f"programmed {start >> 20} MiB", end="\r", flush=True)
    print()


def verify(card, data, offset):
    card.command(offset, READ_ARRAY)
    for i, (word,) in enumerate(struct.iter_unpack("<I", data)):
        got = card.flash_read(offset + 4 * i)
        if got != word:
            sys.exit(f"verify failed at {offset + 4 * i:#010x}: {got:#010x} != {word:#010x}")
    print(f"verified {len(data)} bytes at {offset:#x}")


def boot(card, wbstar, rescan):
    card.set_reg(card.control("wbstar"), wbstar)
    if card.reg(card.control("wbstar")) != wbstar:
        sys.exit("WBSTAR did not read back")
    card.set_reg(card.control("iprog"), IPROG_KEY)
    if rescan:
        with open(f"{card.path}/remove", "w") as f:
            f.write("1")
    print(f"IPROG from WBSTAR {wbstar:#x} (flash byte {wbstar * 2:#x}); "
          "the link is down while the FPGA reloads")
    if rescan:
        time.sleep(RELOAD_SECONDS)
        with open("/sys/bus/pci/rescan", "w") as f:
            f.write("1")
        path = f"/sys/bus/pci/devices/{card.bdf}"
        ids = [open(f"{path}/{n}").read().strip() for n in ("vendor", "device")] \
            if os.path.exists(path) else ["absent"]
        print(f"after rescan {card.bdf}: {':'.join(ids)}")


def board(bdf):
    """u50 or k7, from the device ID and the BAR0 profile."""
    path = f"/sys/bus/pci/devices/{bdf}"
    if not os.path.exists(path):
        sys.exit(f"{bdf} is not present")
    ids = tuple(open(f"{path}/{n}").read().strip() for n in ("vendor", "device"))
    if ids == ("0x10ee", UNIFIED_ENGINE_DEVICE):
        return "k7"
    if ids != ("0x10ee", GPU_DEVICE):
        sys.exit(f"{bdf} is {ids[0]}:{ids[1]}, neither the Apex GPU nor unified-engine")
    # A bound apex_pci publishes the profile it read from BAR0.
    if os.path.exists(f"{path}/profile"):
        profile = int(open(f"{path}/profile").read())
        if profile in (PROFILE_U50, PROFILE_K7):
            return "u50" if profile == PROFILE_U50 else "k7"
    with open(f"{path}/config", "r+b") as f:
        f.seek(PCI_COMMAND)
        command = int.from_bytes(f.read(2), "little")
        if not command & PCI_COMMAND_MEMORY:
            f.seek(PCI_COMMAND)
            f.write((command | PCI_COMMAND_MEMORY).to_bytes(2, "little"))
    try:
        fd = os.open(f"{path}/resource0", os.O_RDONLY | os.O_SYNC)
        try:
            region = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)
    except OSError as error:
        sys.exit(f"cannot read {bdf}'s BAR0 profile ({error}); unload its driver and retry")
    bar0 = memoryview(region).cast("I")
    magic, profile = bar0[0], bar0[2]
    bar0.release()
    region.close()
    if magic != MAGIC or profile not in (PROFILE_U50, PROFILE_K7):
        sys.exit(f"{bdf} BAR0 magic {magic:#010x} profile {profile} is not a U50 or Kintex-7 GPU image")
    return "u50" if profile == PROFILE_U50 else "k7"


# The U50 (profile 2) backend: the QSPI flash sequence. The
# driver, loaded with flash=1, exposes the 128 MiB QSPI as two MTD partitions:
# apex-u50-golden (0 to 0x01002000, read-only) and apex-u50-user (from
# 0x01002000). This writes only the user partition, through flashcp, and checks
# that the golden digest does not change. It has no warm boot: the new image
# loads at the next configuration (power cycle, or JTAG boot_hw_device).
U50_GOLDEN, U50_USER = "apex-u50-golden", "apex-u50-user"
U50_USER_OFFSET, U50_FLASH_BYTES, U50_ERASE_BYTES = 0x01002000, 128 << 20, 4096


def run(*command, **kwargs):
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def u50_reload(bdf, flash):
    """Reload apex_pci, with or without flash=1, and wait for it to bind."""
    run("modprobe", "-r", "apex_pci")
    if flash:
        run("modprobe", "spi-nor")
    run("modprobe", "apex_pci", *(["flash=1"] if flash else []))
    driver = f"/sys/bus/pci/devices/{bdf}/driver"
    deadline = time.monotonic() + 10
    while not os.path.exists(driver):
        if time.monotonic() > deadline:
            sys.exit(f"apex_pci did not bind {bdf}")
        time.sleep(0.1)


def u50_mtd(bdf):
    """The golden and user MTD devices of this card's SPI controller, by name."""
    found = {}
    for entry in sorted(os.listdir("/sys/class/mtd")):
        path = f"/sys/class/mtd/{entry}"
        if not os.path.exists(f"{path}/name") or entry.endswith("ro"):
            continue
        name = open(f"{path}/name").read().strip()
        # The partitions' SPI-NOR parent hangs off the card's PCI function;
        # the host's BIOS MTD devices do not.
        if name in (U50_GOLDEN, U50_USER) and f"/{bdf}/" in os.path.realpath(path):
            if name in found:
                sys.exit(f"two {name} MTD devices under {bdf}")
            found[name] = entry
    for name in (U50_GOLDEN, U50_USER):
        if name not in found:
            sys.exit(f"no {name} MTD device under {bdf}: load apex_pci with flash=1 on an image with CAP_FLASH")
    return found[U50_GOLDEN], found[U50_USER]


def u50_attribute(entry, name):
    return int(open(f"/sys/class/mtd/{entry}/{name}").read().strip(), 0)


def u50_geometry(golden, user):
    """Check the partition layout before anything is written."""
    MTD_WRITEABLE = 0x400
    checks = [
        (u50_attribute(golden, "size"), U50_USER_OFFSET, "golden size"),
        (u50_attribute(golden, "size") + u50_attribute(user, "size"), U50_FLASH_BYTES, "flash size"),
        (u50_attribute(user, "erasesize"), U50_ERASE_BYTES, "user erase size"),
        (u50_attribute(golden, "flags") & MTD_WRITEABLE, 0, "golden writeable flag"),
    ]
    for got, expected, what in checks:
        if got != expected:
            sys.exit(f"{what} is {got:#x}, expected {expected:#x}")
    print(f"/dev/{golden} {U50_GOLDEN} {U50_USER_OFFSET:#x} bytes read-only; "
          f"/dev/{user} {U50_USER} {u50_attribute(user, 'size'):#x} bytes, 4 KiB erase")


def u50_digest(entry):
    digest = hashlib.sha256()
    with open(f"/dev/{entry}", "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def u50_image(path, user):
    data = open(path, "rb").read()
    if data[:4] == b"\x00\x09\x0f\xf0" or path.endswith((".bit", ".mcs")):
        sys.exit(f"{path} is a .bit or MCS; program the raw SPIx4 binary (gpu_u50_flash.bin)")
    if len(data) > u50_attribute(user, "size"):
        sys.exit(f"{path} is {len(data)} bytes, larger than the user partition")
    return data


def u50_verify(path, data, user):
    with open(f"/dev/{user}", "rb") as f:
        got = f.read(len(data))
    if got != data:
        offset = next(i for i, (a, b) in enumerate(zip(got, data)) if a != b) if len(got) == len(data) \
            else len(got)
        sys.exit(f"verify failed at user offset {offset:#x}")
    print(f"verified {len(data)} bytes of {path} in /dev/{user}")


def u50_main(args):
    if args.command == "boot":
        sys.exit("boot is for the Kintex-7: the U50 loads its QSPI image at the next configuration")
    if args.slot is not None or args.offset is not None:
        sys.exit("the U50 takes no --slot or --offset: it writes only the apex-u50-user partition")
    if not os.path.exists(f"/sys/bus/pci/devices/{args.bdf}/driver/module/parameters/flash") or \
            open(f"/sys/bus/pci/devices/{args.bdf}/driver/module/parameters/flash").read().strip() != "Y":
        if args.command == "status":
            sys.exit("apex_pci is not loaded with flash=1; program and verify load it")
        u50_reload(args.bdf, True)
    golden, user = u50_mtd(args.bdf)
    u50_geometry(golden, user)
    if args.command == "status":
        return
    image = os.path.abspath(args.image)
    data = u50_image(image, user)
    before = u50_digest(golden)
    print(f"golden sha256 {before}")
    if args.command == "program":
        # A one-shot unit that a lost session cannot interrupt; it starts in /,
        # so the image path is absolute.
        run("systemd-run", "--wait", "--collect", "flashcp", "-v", image, f"/dev/{user}")
    u50_verify(image, data, user)
    after = u50_digest(golden)
    if after != before:
        sys.exit(f"golden sha256 changed to {after}")
    print("golden unchanged")
    if args.command == "program":
        u50_reload(args.bdf, False)
        print("the running FPGA keeps its image until the next configuration")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["status", "program", "verify", "boot"])
    parser.add_argument("image", nargs="?")
    parser.add_argument("--bdf", required=True, help="the card's PCI address, e.g. 0000:01:00.0")
    place = parser.add_mutually_exclusive_group()
    place.add_argument("--slot", choices=sorted(SLOTS))
    place.add_argument("--offset", type=lambda v: int(v, 0))
    place.add_argument("--wbstar", type=lambda v: int(v, 0))
    parser.add_argument("--no-rescan", action="store_true")
    args = parser.parse_args()
    if (args.command in ("program", "verify")) != (args.image is not None):
        parser.error("program and verify take an image; status and boot do not")
    if board(args.bdf) == "u50":
        if args.wbstar is not None or args.no_rescan:
            parser.error("--wbstar and --no-rescan are for the Kintex-7")
        u50_main(args)
        return
    if args.command in ("program", "verify") and args.slot is None and args.offset is None:
        parser.error(f"{args.command} needs --slot or --offset")
    if args.command == "boot" and args.slot is None and args.wbstar is None:
        parser.error("boot needs --slot or --wbstar")
    if args.command != "boot" and args.wbstar is not None:
        parser.error("--wbstar is for boot")
    if args.command == "boot" and args.offset is not None:
        parser.error("boot takes --slot or --wbstar, not --offset")

    card = Card(args.bdf)
    if card.kind == "unified-engine" and args.command in ("program", "verify"):
        sys.exit("the unified-engine image can only warm-boot here; boot the GPU image "
                 "(boot --slot gpu) and program the flash from it")
    if args.command == "status":
        status(card)
    elif args.command == "program":
        offset = target(args)
        data = load(args.image, offset)
        program(card, data, offset)
        verify(card, data, offset)
    elif args.command == "verify":
        offset = target(args)
        verify(card, load(args.image, offset), offset)
    else:
        wbstar = SLOTS[args.slot] // 2 if args.slot is not None else args.wbstar
        boot(card, wbstar, not args.no_rescan)


if __name__ == "__main__":
    main()
