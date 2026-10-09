#!/usr/bin/env python3
"""
migrate.py — move a fielded Open-Motion console to bootloader 1.2.0 over USB.

Implements the `enter-dfu` and `migrate` sub-commands of flash_firmware.py
(see PLAN-console-migration.md in the Open-Motion workspace).

Starting states handled by `migrate`:

  bootloader 1.0.x (RDP 0, old key)   erase slot -> flash the signed UPDATER image
                                      (old key, FwVersion 1.8.99) -> the updater
                                      rewrites sector 0 with bootloader 1.2.0 and
                                      resets into its DFU -> flash the signed APP
                                      (new key) -> application boots.
  bootloader 1.2.x                    flash the signed APP only.
  STM32 ROM loader (bare-metal unit)  needs --production: erase sectors 0-5, write
                                      the production image (bootloader + app) at
                                      0x08000000, leave DFU. Untested on hardware
                                      until a bare-metal unit is available (T4).

Every image is verified on the host before anything is erased.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from stm32dfu import STM32DFU, DFUError                      # noqa: E402
from verify_firmware import ImageVerificationError, verify_image, DEFAULT_PUBLIC_KEY  # noqa: E402
import console_cdc                                           # noqa: E402
import sensor_usb                                            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def _default_legacy_key():
    """keys/ecdsa_public_legacy_<fielded bootloader tag>.pem of this repository (console
    1.0.0 or sensor 1.1.0); the file name records which bootloader trusted it."""
    import glob
    cands = sorted(glob.glob(os.path.join(HERE, "keys", "ecdsa_public_legacy*.pem")))
    return cands[-1] if cands else os.path.join(HERE, "keys", "ecdsa_public_legacy.pem")


DEFAULT_LEGACY_PUBLIC_KEY = _default_legacy_key()


# ---- application-side device (console: CDC serial port; sensor: bulk USB) ---------------

def _find_app(product, port=None, serial=None):
    """Return (kind, handle) for the one running application on the bus, or (None, None)."""
    if product in ("auto", "console"):
        ports = [port] if port else console_cdc.find_console_ports()
        if ports:
            return "console", ports[0]
    if product in ("auto", "sensor"):
        devs = sensor_usb.find_sensor_devices()
        if serial:
            devs = [d for d in devs if d[0] == serial]
        if devs:
            return "sensor", devs[0][0]
    return None, None


def _app_enter_dfu(product, port=None, serial=None):
    """Ask the running application to reboot into DFU. Returns (label, version, acked)."""
    kind, handle = _find_app(product, port, serial)
    if kind == "console":
        p, ver, ack = console_cdc.console_enter_dfu(handle)
        return f"console on {p}", ver, ack
    if kind == "sensor":
        sn, ver, ack = sensor_usb.sensor_enter_dfu(handle)
        return f"sensor module {sn or ''}".strip(), ver, ack
    raise console_cdc.ConsoleError("no console (0483:A53E) or sensor (0483:5A5A) application found")


def _wait_app(product, timeout, serial=None):
    """Wait for an application to enumerate; returns (kind, handle) or (None, None)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        kind, handle = _find_app(product, None, serial)
        if kind:
            return kind, handle
        time.sleep(0.5)
    return None, None


def _app_version(kind, handle):
    try:
        if kind == "console":
            with console_cdc.ConsoleCdc(handle) as c:
                return c.version()
        if kind == "sensor":
            with sensor_usb.SensorUsb(serial=handle) as s:
                return s.version()
    except Exception:
        pass
    return "unknown"

SLOT_ADDR      = 0x08020000
BOOTLOADER_LEN = 0x00020000           # sector 0
FLASH_BASE     = 0x08000000
ROM_ERASE_END  = 0x080C0000           # sectors 0..5: bootloader, slot, floor. Never sector 15 (user config).

T_DFU_APPEAR     = 20.0               # app -> bootloader DFU after OW_CMD_DFU
T_UPDATER_CYCLE  = 150.0              # 1.0.0 verifies updater, updater rewrites sector 0, 1.2.0 first boot applies
                                      # option bytes (reset) and enumerates DFU
T_APP_APPEAR     = 90.0               # bootloader verifies the app, launches it, CDC enumerates
T_POWER_CYCLE    = 300.0              # ROM path on a sensor module: operator power-cycles the unit by hand

_VER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)")


def _say(msg=""):
    print(msg, flush=True)


def _progress(done, total, msg=""):
    if msg:
        print(f"\r  {msg:<48}", end="", flush=True)
        return
    pct = int(done * 100 / total) if total else 100
    print(f"\r  [{'#' * (pct // 4):<25}] {pct:3d}%  {done // 1024}/{total // 1024} KB", end="", flush=True)
    if done >= total:
        print()


def parse_bl_version(s: str):
    """'1.2.0-rc.1-3-gabc-dirty' -> (1, 2, 0); None if unparsable."""
    m = _VER_RE.match(s.strip())
    return tuple(int(x) for x in m.groups()) if m else None


def _read(path, what):
    if not path:
        raise SystemExit(f"Error: --{what} is required for this path")
    if not os.path.isfile(path):
        raise SystemExit(f"Error: {what} file not found: {path}")
    with open(path, "rb") as f:
        return f.read()


def _verify(image, pem_path, what):
    with open(pem_path, "rb") as f:
        pem = f.read()
    try:
        info = verify_image(image, pem)
    except ImageVerificationError as e:
        raise SystemExit(f"Error: {what} rejected on the host, nothing was flashed: {e}")
    return info


def _ensure_dfu_device(args):
    """Return True when a DFU device is present, entering DFU from the app if needed."""
    if STM32DFU.wait_for_device(0.5, present=True, serial=args.serial):
        return True
    if getattr(args, "skip_enter_dfu", False):
        return False
    try:
        label, ver, ack = _app_enter_dfu(args.product, getattr(args, "port", None), getattr(args, "app_serial", None))
    except console_cdc.ConsoleError as e:
        _say(f"  no DFU device and no application: {e}")
        return False
    _say(f"  {label}, application {ver or 'version unknown'}: DFU request "
         f"{'acknowledged' if ack else 'sent (no reply)'}")
    _say(f"  waiting for the bootloader DFU device (up to {T_DFU_APPEAR:.0f}s)...")
    return STM32DFU.wait_for_device(T_DFU_APPEAR, present=True, serial=args.serial)


# ── enter-dfu ─────────────────────────────────────────────────────────────────

def cmd_enter_dfu(args):
    if STM32DFU.wait_for_device(0.5, present=True, serial=args.serial):
        _say("A DFU device is already present.")
    else:
        try:
            label, ver, ack = _app_enter_dfu(args.product, args.port, args.app_serial)
        except console_cdc.ConsoleError as e:
            raise SystemExit(f"Error: {e}")
        _say(f"{label[0].upper() + label[1:]}, application {ver or 'version unknown'}: DFU request "
             f"{'acknowledged' if ack else 'sent (no reply)'}.")
        _say(f"Waiting for the DFU device (up to {args.timeout:.0f}s)...")
        if not STM32DFU.wait_for_device(args.timeout, present=True, serial=args.serial):
            raise SystemExit("Error: no DFU device appeared.")
    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        mode, alts = dfu.detect_mode()
        _say(f"DFU mode   : {mode}")
        for a in alts:
            _say(f"  alt       : {a}")
        if mode == "bootloader":
            _say(f"Bootloader : {dfu.read_version()}")
        elif mode == "rom":
            _say("Bootloader : none (STM32 ROM loader; bare-metal unit)")


# ── migrate ───────────────────────────────────────────────────────────────────

def _preflight(args):
    updater = _read(args.updater, "updater")
    signed  = _read(args.signed, "signed")
    production = None
    if args.production:
        production = _read(args.production, "production")

    _say("Host pre-flight")
    u = _verify(updater, args.legacy_public_key, "updater image")
    _say(f"  updater    : {os.path.basename(args.updater)}  FwVersion {u.version_str}  "
         f"(verified against the 1.0.0 public key)")
    s = _verify(signed, args.public_key, "signed application")
    _say(f"  application: {os.path.basename(args.signed)}  FwVersion {s.version_str}  "
         f"(verified against the 1.2.0 public key)")
    if production is not None:
        if len(production) <= BOOTLOADER_LEN:
            raise SystemExit("Error: production image is not longer than the bootloader sector")
        if production[BOOTLOADER_LEN:BOOTLOADER_LEN + len(signed)] != signed:
            raise SystemExit("Error: production image does not contain the given signed application at 0x08020000")
        bl = production[:BOOTLOADER_LEN].rstrip(b"\xff")
        sp, rv = int.from_bytes(bl[0:4], "little"), int.from_bytes(bl[4:8], "little")
        if not (0x20000000 <= sp <= 0x20020000 or 0x24000000 <= sp <= 0x24080000) or not (FLASH_BASE < rv < FLASH_BASE + BOOTLOADER_LEN):
            raise SystemExit(f"Error: production image bootloader part looks wrong (sp={sp:#x} reset={rv:#x})")
        if updater.find(bl) < 0:
            raise SystemExit("Error: the updater does not embed the same bootloader as the production image")
        _say(f"  production : {os.path.basename(args.production)}  bootloader {len(bl)} B, "
             f"same bootloader embedded in the updater, same application")
    return updater, signed, production, u, s


def _path_bootloader_10x(dfu, args, updater, signed, ver_str):
    _say(f"\nStep 1/3  bootloader {ver_str}: install the updater (old key)")
    _say("  erasing the application slot (1.0.0 does not clean stale tails)...")
    dfu.erase_all(progress_cb=_progress)
    _say("  programming the updater...")
    t0 = time.monotonic()
    dfu.download(SLOT_ADDR, updater, progress_cb=_progress, pre_erased=True)
    _say(f"  downloaded in {time.monotonic() - t0:.1f}s; the unit resets and runs the updater.")
    _say("  DO NOT POWER OFF OR UNPLUG. IND1+IND2 on = sector 0 is being rewritten.")

    _say(f"\nStep 2/3  waiting for the new bootloader's DFU (up to {T_UPDATER_CYCLE:.0f}s)...")
    STM32DFU.wait_for_device(15.0, present=False, serial=args.serial)
    if not STM32DFU.wait_for_device(T_UPDATER_CYCLE, present=True, serial=args.serial):
        if _find_app(args.product)[0]:
            raise SystemExit("Error: the unit came back as an application, not in DFU. The old "
                             "bootloader may have refused the updater (check UART4). Nothing in sector 0 changed.")
        raise SystemExit("Error: no DFU device after the updater ran. If all three LEDs blink, sector 0 "
                         "programming failed: bench recovery (SWD) required. Otherwise check UART4.")
    return True


def cmd_migrate(args):
    updater, signed, production, u_info, s_info = _preflight(args)

    _say("\nDevice")
    if not _ensure_dfu_device(args):
        raise SystemExit("Error: no DFU device. Connect exactly one unit (application or DFU) and retry.")

    with STM32DFU() as dfu:
        dfu.connect(serial=args.serial)
        mode, alts = dfu.detect_mode()
        _say(f"  DFU mode   : {mode}")
        if mode == "unknown":
            for a in alts:
                _say(f"    alt      : {a}")
            raise SystemExit("Error: unrecognised DFU device; refusing to guess a flash layout.")

        if mode == "rom":
            if production is None:
                raise SystemExit("Error: bare-metal unit (ROM loader) needs --production <bootloader+app image>.")
            if not args.yes:
                _say("  This writes the production image at 0x08000000 (sectors 0-5 erased). Re-run with --yes.")
                raise SystemExit(2)
            _say("\nStep 1/1  ROM loader: write the production image (bootloader + application)")
            dfu.set_rom_mode()
            dfu.erase_range(FLASH_BASE, ROM_ERASE_END, progress_cb=_progress)
            dfu.download(FLASH_BASE, production, progress_cb=_progress, pre_erased=True)
            # The zero-length DNLOAD that ends download() is the DfuSe manifestation; the
            # ROM loader jumps to the image right there (seen on the bench 2026-10-08), so
            # the device is normally gone before this explicit leave. Keep it for a ROM
            # that waits for one, tolerate its absence.
            try:
                dfu.leave_dfu()
            except Exception:
                pass
            _say("  written; the ROM loader starts the new bootloader, which applies its option bytes and launches the application.")
            did_updater = False
        else:
            ver_str = dfu.read_version()
            ver = parse_bl_version(ver_str)
            _say(f"  bootloader : {ver_str}")
            if ver is None:
                raise SystemExit(f"Error: cannot parse bootloader version {ver_str!r}")
            if ver < (1, 2, 0):
                if not args.yes:
                    _say("\n  This replaces the bootloader in sector 0. Mains power, no USB unplug, one unit on the bus.")
                    _say("  Re-run with --yes to proceed.")
                    raise SystemExit(2)
                _path_bootloader_10x(dfu, args, updater, signed, ver_str)
                did_updater = True
            else:
                _say(f"\n  bootloader already {ver_str}; installing the application only")
                did_updater = False

    if mode != "rom":
        step = "3/3" if did_updater else "1/1"
        with STM32DFU() as dfu:
            dfu.connect(serial=args.serial)
            ver_str = dfu.read_version()
            ver = parse_bl_version(ver_str)
            _say(f"\nStep {step}  bootloader {ver_str}: install the application (new key)")
            if ver is None or ver < (1, 2, 0):
                raise SystemExit(f"Error: bootloader reports {ver_str!r}, not 1.2.x. The updater did not replace "
                                 "sector 0 (it refuses when option bytes are not RDP0/no WRP/no PCROP). "
                                 "Check UART4 output; the unit is still usable with old-key images.")
            try:
                dfu.download(SLOT_ADDR, signed, progress_cb=_progress)
            except DFUError as e:
                raise SystemExit(f"Error: the bootloader refused the download: {e}. "
                                 "Its DFU checks the signed header before writing: wrong key, or FwVersion "
                                 "below the header still in the slot. An updater older than the self-erasing "
                                 "build leaves its 1.8.99 header there; the slot must then be cleared on the bench.")
            _say("  downloaded; the bootloader verifies the image and launches it.")

    _say(f"\nWaiting for the application (up to {T_APP_APPEAR:.0f}s)...")
    kind, handle = _wait_app(args.product, T_APP_APPEAR, getattr(args, "app_serial", None))
    if kind is None and mode == "rom" and not STM32DFU.wait_for_device(0.5, present=True, serial=args.serial):
        # Flash is complete, but the unit is gone from USB: the STM32 ROM loader's jump into
        # a freshly written image hangs on the sensor modules until a real power cycle
        # (openmotion-sensor-fw CLAUDE.md "jump-to-app hang"; seen on the bench 2026-10-08).
        _say("  The unit left USB without re-enumerating. The image is written; the ROM loader's jump\n"
             "  into it is known to hang on sensor modules. POWER-CYCLE THE UNIT NOW (console power\n"
             "  cycles both sensors). The new bootloader then starts cold and launches the application.")
        _say(f"  waiting for the application after the power cycle (up to {T_POWER_CYCLE:.0f}s)...")
        kind, handle = _wait_app(args.product, T_POWER_CYCLE, getattr(args, "app_serial", None))
    if kind is None:
        raise SystemExit("Error: the application did not enumerate. Check UART4; a DFU device still present "
                         "means the bootloader refused the image.")
    time.sleep(1.0)
    app_ver = _app_version(kind, handle)
    where = f"console on {handle}" if kind == "console" else f"sensor module {handle or ''}".strip()
    _say(f"Done. {where[0].upper() + where[1:]}, application {app_ver}"
         + (f", bootloader 1.2.x (updater {u_info.version_str})" if did_updater else "") + ".")


def add_subcommands(sub):
    def product_args(sp):
        sp.add_argument("--product", choices=("auto", "console", "sensor"), default="auto",
                        help="Which application to look for on USB (default: whichever is present)")
        sp.add_argument("--port", default=None, help="Console COM port (default: the single 0483:A53E port)")
        sp.add_argument("--app-serial", default=None, help="Sensor module USB serial when several are attached")

    p = sub.add_parser("enter-dfu", help="Ask a running console/sensor application to reboot into DFU and report the bootloader")
    product_args(p)
    p.add_argument("--timeout", type=float, default=T_DFU_APPEAR, help="Seconds to wait for the DFU device")
    p.set_defaults(func=cmd_enter_dfu)

    m = sub.add_parser("migrate", help="Old bootloader -> 1.2.0 migration: updater, then the signed application")
    product_args(m)
    m.add_argument("--updater", required=True, help="Signed updater image (old key), e.g. open-motion-console-bl-updater-1.8.99-bl1.2.0-rc.1-signed.bin")
    m.add_argument("--signed", required=True, help="Signed application image for bootloader 1.2.0 (new key)")
    m.add_argument("--production", default=None, help="Production image (bootloader+app) for bare-metal units in the ROM loader")
    m.add_argument("--legacy-public-key", default=DEFAULT_LEGACY_PUBLIC_KEY, help="Public key bootloader 1.0.0 trusts")
    m.add_argument("--public-key", default=str(DEFAULT_PUBLIC_KEY), help="Public key bootloader 1.2.0 trusts")
    m.add_argument("--skip-enter-dfu", action="store_true", help="Do not look for a console application; require a DFU device")
    m.add_argument("--yes", action="store_true", help="Proceed with the irreversible steps without the extra prompt")
    m.set_defaults(func=cmd_migrate)
