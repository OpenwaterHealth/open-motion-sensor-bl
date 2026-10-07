#!/usr/bin/env python3
"""
flash_firmware.py — Pure-Python installer for the openmotion-bl SECURE bootloader.

Flashes a SIGNED firmware image (produced by sign_firmware.py) into the SBSFU
active slot at 0x08020000 over USB DFU, then resets the device so the bootloader
verifies the signature and launches the application. This is the pure-Python
equivalent of:
    dfu-util -D app_signed.bin -a 0 -s 0x08020000:leave

The input MUST be a signed image (header + clear firmware at offset 0x400), not a
raw application .bin. See sign_firmware.py / README.md.

Before anything is erased the image is checked on the host (signature, FwTag,
size, downgrade) with verify_firmware.py, so a bad file is refused while the
installed firmware is still intact.

Usage:
    python flash_firmware.py app_signed.bin           # install signed image to slot
    python flash_firmware.py flash app_signed.bin --verify
    python flash_firmware.py list                      # list connected DFU devices
    python flash_firmware.py read 0x08020000 320       # dump the signed header
    python flash_firmware.py leave                     # reset device into the app
"""

import argparse
import os
import sys
import time

# Allow running from any directory
sys.path.insert(0, os.path.dirname(__file__))
from stm32dfu import STM32DFU, DFUError
from verify_firmware import (
    DEFAULT_PUBLIC_KEY,
    ImageVerificationError,
    read_header_version,
    verify_image,
)


# ── defaults ─────────────────────────────────────────────────────────────────

DEFAULT_BIN  = None              # require an explicit signed image
DEFAULT_ADDR = 0x08020000        # SBSFU active slot (signed image base)


# ── progress display ─────────────────────────────────────────────────────────

def _progress(done, total, msg=""):
    if msg:
        print(f"  {msg}")
        return
    if total == 0:
        return
    pct   = done * 100 // total
    bar   = "#" * (pct // 2) + "-" * (50 - pct // 2)   # ASCII (Windows cp1252-safe)
    kb    = done // 1024
    total_kb = total // 1024
    print(f"\r  [{bar}] {pct:3d}%  {kb}/{total_kb} KB", end="", flush=True)
    if done >= total:
        print()


# ── sub-commands ──────────────────────────────────────────────────────────────

def cmd_list(_args):
    devices = STM32DFU.list_devices()
    if not devices:
        print("No STM32 DFU devices found.")
        return
    print(f"Found {len(devices)} DFU device(s):\n")
    for d in devices:
        print(f"  Product : {d['product']}")
        print(f"  Serial  : {d['serial']}")
        print(f"  VID/PID : {d['vid']:#06x} / {d['pid']:#06x}")
        print(f"  Bus/Addr: {d['bus']}.{d['address']}")
        print()


def cmd_flash(args):
    bin_path = args.binary or DEFAULT_BIN
    address  = args.addr

    if not bin_path:
        print("Error: no firmware image given.\n"
              "       Provide a SIGNED image, e.g.:\n"
              "         python flash_firmware.py app_signed.bin\n"
              "       Create one with sign_firmware.py first.")
        sys.exit(1)

    if not os.path.isfile(bin_path):
        print(f"Error: binary not found: {bin_path}")
        sys.exit(1)

    with open(bin_path, "rb") as f:
        firmware = f.read()

    # Pre-flight: run the bootloader's checks on the file while the installed
    # image is still intact. The download erases the slot first and SBSFU only
    # verifies on the next boot, so a bad image caught there has already cost the
    # device its working firmware (it stays in DFU until a good one is flashed).
    info = None
    if args.allow_unverified:
        print("WARNING: pre-flight verification skipped (--allow-unverified).\n"
              "         If the bootloader rejects this image the device will stay\n"
              "         in DFU mode with no application installed.")
    else:
        with open(args.public_key, "rb") as f:
            public_key_pem = f.read()
        try:
            info = verify_image(firmware, public_key_pem)
        except ImageVerificationError as e:
            print(f"Error: image rejected, nothing was flashed: {e}")
            sys.exit(1)

    size_kb = len(firmware) / 1024
    print(f"Firmware : {bin_path}")
    print(f"Size     : {size_kb:.1f} KB  ({len(firmware)} bytes)")
    if info is not None:
        print(f"FwVersion: {info.version_str}  (signature and FwTag verified)")
    print(f"Target   : {address:#010x}  (SBSFU active slot)")
    print()

    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        _, _, state, _ = dfu.get_status()
        print(f"Connected. DFU state: {state}")
        print()

        # Downgrade pre-flight: the bootloader destroys a downloaded image that
        # is older than the one it replaced, so refuse before the erase instead.
        if info is not None:
            installed = read_header_version(dfu.upload(address, 8))
            if info.fw_version < installed:
                print(f"Error: image FwVersion {info.fw_version} is older than the installed "
                      f"{installed}; the bootloader\n"
                      "       refuses downgrades. Nothing was flashed.")
                sys.exit(1)

        print("Programming...")
        t0 = time.monotonic()
        dfu.download(address, firmware, progress_cb=_progress)
        elapsed = time.monotonic() - t0
        print(f"Download complete in {elapsed:.2f}s")

    # Device has reset — wait for it to re-enumerate if we want to verify
    if args.verify:
        print("\nWaiting for device to re-enumerate...")
        time.sleep(3.0)
        print("Verifying...")
        with STM32DFU() as dfu:
            dfu.connect(serial=args.serial)
            try:
                dfu.verify(address, firmware, progress_cb=_progress)
                print("Verify OK — all bytes match.")
            except DFUError as e:
                print(f"Verify FAILED: {e}")
                sys.exit(1)

    print("\nDone. Device will boot the new firmware.")


def cmd_read(args):
    address = args.addr
    length  = args.length

    print(f"Reading {length} bytes from {address:#010x}...")
    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        data = dfu.upload(address, length)

    # Hex dump
    for row in range(0, len(data), 16):
        chunk = data[row:row + 16]
        hex_part  = " ".join(f"{b:02x}" for b in chunk)
        ascii_part= "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {address + row:#010x}  {hex_part:<48}  {ascii_part}")


def cmd_erase(args):
    print("Erasing the application slot (0x08020000–0x081FFFFF)...")
    print()
    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        t0 = time.monotonic()
        dfu.erase_all()
        elapsed = time.monotonic() - t0
    print(f"\nErase complete in {elapsed:.1f}s.")


def cmd_leave(args):
    print("Sending leave-DFU command (device will reset and boot app)...")
    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        dfu.leave_dfu()
    print("Done.")


def cmd_version(args):
    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        version = dfu.read_version()
    print(version)


# ── argument parsing ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Flash / inspect STM32 via USB DFU bootloader")
    parser.add_argument(
        "--serial", metavar="SN", default=None,
        help="Target a specific device by USB serial number")

    sub = parser.add_subparsers(dest="command")

    # list
    sub.add_parser("list", help="List connected DFU devices")

    # flash (default)
    p_flash = sub.add_parser("flash", help="Install a signed firmware image (default command)")
    p_flash.add_argument(
        "binary", nargs="?", default=None,
        help="Path to the SIGNED .bin (from sign_firmware.py)")
    p_flash.add_argument(
        "--addr", type=lambda x: int(x, 0), default=DEFAULT_ADDR,
        help=f"Slot address (default: {DEFAULT_ADDR:#010x})")
    p_flash.add_argument(
        "--verify", action="store_true",
        help="Read back and verify after download")
    p_flash.add_argument(
        "--public-key", default=str(DEFAULT_PUBLIC_KEY),
        help="ECDSA public key PEM the bootloader was built with, for the "
             "pre-flight image check (default: py-tools/keys/ecdsa_public.pem)")
    p_flash.add_argument(
        "--allow-unverified", action="store_true",
        help="Skip the pre-flight image check. Bench use only (negative-path "
             "tests); the bootloader still verifies the image at boot")

    # read
    p_read = sub.add_parser("read", help="Read and hex-dump device memory")
    p_read.add_argument(
        "addr", type=lambda x: int(x, 0),
        help="Start address (hex or decimal)")
    p_read.add_argument(
        "length", type=lambda x: int(x, 0),
        help="Number of bytes to read")

    # erase
    sub.add_parser("erase", help="Erase all application flash sectors")

    # leave
    sub.add_parser("leave", help="Reset device into application")

    # version (alias: dfu_ver)
    sub.add_parser("version", aliases=["dfu_ver"],
                   help="Read the bootloader version (FW_VERSION) over DFU")

    # Make 'flash' the default sub-command so a bare image path works:
    #   python flash_firmware.py app_signed.bin
    known = {"list", "flash", "read", "erase", "leave", "version", "dfu_ver"}
    argv = list(sys.argv[1:])
    i = 0
    first_pos = None
    while i < len(argv):
        if argv[i] == "--serial":          # global option taking a value
            i += 2
            continue
        if argv[i].startswith("-"):
            i += 1
            continue
        first_pos = i
        break
    if first_pos is None:
        argv.append("flash")               # no args -> flash (will report missing image)
    elif argv[first_pos] not in known:
        argv.insert(first_pos, "flash")    # bare path -> 'flash <path>'

    args = parser.parse_args(argv)

    dispatch = {
        "list":    cmd_list,
        "flash":   cmd_flash,
        "read":    cmd_read,
        "erase":   cmd_erase,
        "leave":   cmd_leave,
        "version": cmd_version,
        "dfu_ver": cmd_version,
    }
    dispatch[args.command](args)


if __name__ == "__main__":
    main()
