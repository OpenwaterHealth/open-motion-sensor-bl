"""
stm32dfu.py — Pure-Python USB DFU class for STM32 custom bootloaders.

Implements the STM32 DFU sub-protocol (AN3156) over pyusb:
  - Sector erase + block download (flash write)
  - Block upload (flash read)
  - Download verification (read-back compare)
  - Device reset / leave-DFU

Requirements:
    pip install pyusb

Windows driver note:
    pyusb needs the device to be bound to a WinUSB or libusb-win32 driver.
    Use Zadig (https://zadig.akeo.ie/) to switch the "STM32 BOOTLOADER"
    device from the STM32CubeProgrammer driver to WinUSB.
    Alternatively, point _find_libusb() at the DLL that ships with
    STM32CubeProgrammer (see _CUBEPROG_LIBUSB_PATHS below).
"""

import struct
import time
import sys
import os

try:
    import usb.core
    import usb.util
    import usb.backend.libusb1
except ImportError as e:
    raise ImportError("pyusb is required: pip install pyusb") from e


# ── DFU state / status constants ────────────────────────────────────────────

class DFUState:
    APP_IDLE              = 0
    APP_DETACH            = 1
    DFU_IDLE              = 2
    DFU_DNLOAD_SYNC       = 3
    DFU_DNLOAD_BUSY       = 4
    DFU_DNLOAD_IDLE       = 5
    DFU_MANIFEST_SYNC     = 6
    DFU_MANIFEST          = 7
    DFU_MANIFEST_WAIT_RST = 8
    DFU_UPLOAD_IDLE       = 9
    DFU_ERROR             = 10

    NAMES = {
        0: "appIDLE",           1: "appDETACH",
        2: "dfuIDLE",           3: "dfuDNLOAD-SYNC",
        4: "dfuDNLOAD-BUSY",    5: "dfuDNLOAD-IDLE",
        6: "dfuMANIFEST-SYNC",  7: "dfuMANIFEST",
        8: "dfuMANIFEST-WAIT-RESET",
        9: "dfuUPLOAD-IDLE",   10: "dfuERROR",
    }

    @classmethod
    def name(cls, state):
        return cls.NAMES.get(state, f"UNKNOWN({state})")


class DFUStatus:
    OK                = 0x00
    ERROR_TARGET      = 0x01
    ERROR_FILE        = 0x02
    ERROR_WRITE       = 0x03
    ERROR_ERASE       = 0x04
    ERROR_CHECK_ERASED= 0x05
    ERROR_PROG        = 0x06
    ERROR_VERIFY      = 0x07
    ERROR_ADDRESS     = 0x08
    ERROR_NOTDONE     = 0x09
    ERROR_FIRMWARE    = 0x0A
    ERROR_VENDOR      = 0x0B
    ERROR_USB         = 0x0C
    ERROR_POR         = 0x0D
    ERROR_UNKNOWN     = 0x0E
    ERROR_STALLEDPKT  = 0x0F

    NAMES = {
        0x00: "OK",            0x01: "errTARGET",  0x02: "errFILE",
        0x03: "errWRITE",      0x04: "errERASE",   0x05: "errCHECK_ERASED",
        0x06: "errPROG",       0x07: "errVERIFY",  0x08: "errADDRESS",
        0x09: "errNOTDONE",    0x0A: "errFIRMWARE",0x0B: "errVENDOR",
        0x0C: "errUSB",        0x0D: "errPOR",     0x0E: "errUNKNOWN",
        0x0F: "errSTALLEDPKT",
    }

    @classmethod
    def name(cls, status):
        return cls.NAMES.get(status, f"UNKNOWN({status:#04x})")


class DFUError(Exception):
    pass


# ── libusb backend discovery ─────────────────────────────────────────────────

_CUBEPROG_LIBUSB_PATHS = [
    r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\libusb-1.0.dll",
    r"C:\Program Files (x86)\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\libusb-1.0.dll",
]

def _find_libusb():
    """Return a libusb-1.0 backend, probing CubeProgrammer's bundled DLL on Windows."""
    if sys.platform == "win32":
        for path in _CUBEPROG_LIBUSB_PATHS:
            if os.path.exists(path):
                return usb.backend.libusb1.get_backend(find_library=lambda _x: path)
    return usb.backend.libusb1.get_backend()


# ── Main class ───────────────────────────────────────────────────────────────

class STM32DFU:
    """
    USB DFU interface for STM32 custom bootloaders.

    Typical usage::

        with STM32DFU() as dfu:
            dfu.connect()
            dfu.download(0x08040000, open("app.bin","rb").read())

    The XFER_SIZE must match ``USBD_DFU_XFER_SIZE`` compiled into the
    bootloader firmware (default 2048 for this project).
    """

    STM32_VID = 0x0483
    STM32_PID = 0xDF11

    # DFU class request codes
    _REQ_DETACH    = 0
    _REQ_DNLOAD    = 1
    _REQ_UPLOAD    = 2
    _REQ_GETSTATUS = 3
    _REQ_CLRSTATUS = 4
    _REQ_GETSTATE  = 5
    _REQ_ABORT     = 6

    # STM32 special commands (wBlockNum=0 DNLOAD payload)
    _CMD_GETCOMMANDS      = 0x00
    _CMD_SETADDRESSPOINTER= 0x21
    _CMD_ERASE            = 0x41

    # bmRequestType constants
    _H2D = 0x21   # Class | Interface | Host→Device
    _D2H = 0xA1   # Class | Interface | Device→Host

    # STM32H743 flash geometry
    FLASH_SECTOR_SIZE    = 128 * 1024   # 128 KB per sector, both banks

    # Writable region exposed by the openmotion-bl secure bootloader DFU
    # interface (usbd_dfu_if.c APP_FLASH_BASE .. FLASH_END_ADDR; see
    # Core/Inc/memory_map.h). Clamped to the SBSFU active slot only: the
    # bootloader (sector 0), reserved sectors, the anti-rollback floor and the
    # user-config sector are all read-only over DFU.
    APP_FLASH_START = 0x08020000   # SBSFU active slot start (signed image goes here)
    APP_FLASH_END   = 0x080A0000   # exclusive end of writable region (active-slot end + 1)

    # Virtual UPLOAD address that returns the bootloader version string
    # (usbd_dfu_if.c DFU_VERSION_VIRT_ADDR / DFU_VERSION_READ_LEN).
    DFU_VERSION_ADDR = 0xFFFFFF00
    DFU_VERSION_LEN  = 64

    def __init__(self, vid=None, pid=None, xfer_size=1024):
        """
        Args:
            vid:        USB Vendor ID  (default 0x0483)
            pid:        USB Product ID (default 0xDF11)
            xfer_size:  DFU transfer size — must be <= USBD_DFU_XFER_SIZE in the
                        bootloader firmware (1024 for openmotion-bl). This is a
                        fallback; connect() auto-detects wTransferSize from the
                        device's DFU functional descriptor.
        """
        self.vid       = vid or self.STM32_VID
        self.pid       = pid or self.STM32_PID
        self.xfer_size = xfer_size
        self._dev      = None
        self._iface    = 0

    # ── context manager ──────────────────────────────────────────────────────

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.disconnect()

    # ── device discovery ─────────────────────────────────────────────────────

    @classmethod
    def list_devices(cls):
        """
        Return a list of dicts describing connected DFU devices::

            [{"vid": 0x0483, "pid": 0xDF11, "serial": "316C347D3437",
              "bus": 1, "address": 3, "product": "STM32 DownLoad Firmware Update"}]
        """
        backend = _find_libusb()
        found = usb.core.find(idVendor=cls.STM32_VID, idProduct=cls.STM32_PID,
                              find_all=True, backend=backend)
        result = []
        for dev in (found or []):
            def _str(idx):
                try:
                    return usb.util.get_string(dev, idx) if idx else None
                except Exception:
                    return None
            result.append({
                "vid":     dev.idVendor,
                "pid":     dev.idProduct,
                "serial":  _str(dev.iSerialNumber),
                "product": _str(dev.iProduct),
                "bus":     dev.bus,
                "address": dev.address,
            })
        return result

    # ── connect / disconnect ─────────────────────────────────────────────────

    def connect(self, serial=None):
        """
        Open the first matching DFU device.

        Args:
            serial: Optional USB serial-number string to select a specific
                    device when multiple are attached.

        Raises:
            DFUError: if no matching device is found.
        """
        backend = _find_libusb()

        if serial:
            devs = list(usb.core.find(
                idVendor=self.vid, idProduct=self.pid,
                find_all=True, backend=backend) or [])
            dev = None
            for d in devs:
                try:
                    if usb.util.get_string(d, d.iSerialNumber) == serial:
                        dev = d
                        break
                except Exception:
                    pass
            if dev is None:
                raise DFUError(f"No DFU device with serial {serial!r} found")
        else:
            dev = usb.core.find(idVendor=self.vid, idProduct=self.pid,
                                backend=backend)
            if dev is None:
                raise DFUError(
                    f"No DFU device found (VID={self.vid:#06x} PID={self.pid:#06x})")

        dev.set_configuration()

        if sys.platform != "win32":
            try:
                if dev.is_kernel_driver_active(self._iface):
                    dev.detach_kernel_driver(self._iface)
            except Exception:
                pass

        usb.util.claim_interface(dev, self._iface)
        self._dev = dev

        # Auto-detect the DFU transfer size from the functional descriptor so we
        # never send blocks larger than the device accepts (openmotion-bl = 1024).
        detected = self._read_dfu_transfer_size(dev)
        if detected:
            self.xfer_size = detected

        # Recover from any leftover error state
        self._ensure_idle()

    @staticmethod
    def _read_dfu_transfer_size(dev):
        """Parse wTransferSize (offset 5-6) from the DFU functional descriptor
        (bDescriptorType 0x21) in the active configuration. Returns None on
        failure, in which case the constructor default is kept."""
        try:
            cfg = dev.get_active_configuration()
            for intf in cfg:
                extra = bytes(getattr(intf, "extra_descriptors", b"") or b"")
                i = 0
                while i + 2 <= len(extra):
                    blen = extra[i]
                    if blen < 2 or i + blen > len(extra):
                        break
                    if extra[i + 1] == 0x21 and blen >= 7:   # DFU functional descriptor
                        return extra[i + 5] | (extra[i + 6] << 8)
                    i += blen
        except Exception:
            pass
        return None

    def disconnect(self):
        """Release the USB interface."""
        if self._dev is not None:
            try:
                usb.util.release_interface(self._dev, self._iface)
                usb.util.dispose_resources(self._dev)
            except Exception:
                pass
            self._dev = None

    # ── low-level DFU requests ───────────────────────────────────────────────

    def get_status(self):
        """
        DFU_GETSTATUS → (bStatus, bwPollTimeout_ms, bState, iString)
        """
        data = self._dev.ctrl_transfer(
            self._D2H, self._REQ_GETSTATUS, 0, self._iface, 6)
        status      = data[0]
        poll_ms     = data[1] | (data[2] << 8) | (data[3] << 16)
        state       = data[4]
        istring     = data[5]
        return status, poll_ms, state, istring

    def get_state(self):
        """DFU_GETSTATE → state byte."""
        data = self._dev.ctrl_transfer(
            self._D2H, self._REQ_GETSTATE, 0, self._iface, 1)
        return data[0]

    def clear_status(self):
        """DFU_CLRSTATUS — clears error, returns to dfuIDLE."""
        self._dev.ctrl_transfer(
            self._H2D, self._REQ_CLRSTATUS, 0, self._iface, None)

    def abort(self):
        """DFU_ABORT — abort current op, return to dfuIDLE."""
        self._dev.ctrl_transfer(
            self._H2D, self._REQ_ABORT, 0, self._iface, None)

    # ── STM32 special commands ───────────────────────────────────────────────

    def set_address(self, address):
        """Send STM32 Set Address Pointer command."""
        payload = bytes([self._CMD_SETADDRESSPOINTER]) + struct.pack("<I", address)
        self._dnload(0, payload)
        self._poll_idle()

    def erase_sector(self, address):
        """
        Erase the 128 KB flash sector whose start address is `address`.
        `address` must be sector-aligned (multiple of 128 KB).
        """
        if address % self.FLASH_SECTOR_SIZE:
            # round down to sector start
            address = (address // self.FLASH_SECTOR_SIZE) * self.FLASH_SECTOR_SIZE
        payload = bytes([self._CMD_ERASE]) + struct.pack("<I", address)
        self._dnload(0, payload)
        self._poll_idle(timeout_s=30.0)   # erase can take up to ~500 ms per sector

    def erase_all(self, progress_cb=None):
        """
        Erase all writable application-slot sectors (0x08020000–0x081FFFFF).

        Sector 0 (0x08000000–0x0801FFFF) holds the read-only bootloader and is
        intentionally skipped.

        Args:
            progress_cb: Optional callable(done: int, total: int, msg: str="").
                         ``done`` counts sectors completed; ``total`` is the
                         total number of sectors to erase.
        """
        sector = self.APP_FLASH_START
        sectors = []
        while sector < self.APP_FLASH_END:
            sectors.append(sector)
            sector += self.FLASH_SECTOR_SIZE

        total = len(sectors)
        for i, sec in enumerate(sectors):
            msg = f"Erasing sector {i+1}/{total} @ {sec:#010x}"
            if progress_cb:
                progress_cb(i, total, msg)
            else:
                print(f"  {msg}")
            self.erase_sector(sec)

        if progress_cb:
            progress_cb(total, total)
        else:
            print(f"  Erase complete — {total} sectors ({total * self.FLASH_SECTOR_SIZE // 1024} KB) erased.")

    # ── high-level operations ────────────────────────────────────────────────

    def download(self, address, data, progress_cb=None):
        """
        Erase affected flash sectors and write `data` starting at `address`.

        Args:
            address:     Target flash address. For the secure bootloader this is
                         the SBSFU active-slot start, 0x08020000 (APP_FLASH_START),
                         and `data` must be a SIGNED image (sign_firmware.py).
            data:        Bytes-like firmware image (signed .bin for secure boot).
            progress_cb: Optional callable(done: int, total: int, msg: str="").
                         Called after each erased sector and each written block.

        After the download completes the device issues NVIC_SystemReset(); the
        bootloader then verifies the signature and launches the app if valid.
        """
        data  = bytes(data)
        total = len(data)
        if total == 0:
            raise ValueError("Empty firmware image")

        if address < self.APP_FLASH_START or (address + total) > self.APP_FLASH_END:
            raise DFUError(
                f"Target range {address:#010x}..{address + total:#010x} is outside the "
                f"bootloader's writable region {self.APP_FLASH_START:#010x}.."
                f"{self.APP_FLASH_END:#010x} (sector 0 holds the read-only bootloader).")

        # ── erase affected sectors ──────────────────────────────────────────
        sector_start = (address // self.FLASH_SECTOR_SIZE) * self.FLASH_SECTOR_SIZE
        end_address  = address + total
        sectors = []
        s = sector_start
        while s < end_address:
            sectors.append(s)
            s += self.FLASH_SECTOR_SIZE

        for i, sec in enumerate(sectors):
            msg = f"Erasing sector {i+1}/{len(sectors)} @ {sec:#010x}"
            if progress_cb:
                progress_cb(0, total, msg)
            self.erase_sector(sec)

        # ── set address pointer ─────────────────────────────────────────────
        self.set_address(address)

        # ── write blocks ────────────────────────────────────────────────────
        block_num = 2       # DFU data blocks start at wBlockNum=2
        offset    = 0

        while offset < total:
            chunk = data[offset : offset + self.xfer_size]

            # Pad to 32-byte flash word boundary (STM32H7 requirement)
            remainder = len(chunk) % 32
            if remainder:
                chunk = chunk + b'\xff' * (32 - remainder)

            self._dnload(block_num, chunk)
            self._poll_idle()

            offset    += self.xfer_size
            block_num += 1

            if progress_cb:
                progress_cb(min(offset, total), total)

        # ── trigger manifestation (zero-length DNLOAD) ──────────────────────
        # This sends the device into MANIFEST → NVIC_SystemReset()
        self._dnload(0, b'')
        try:
            self.get_status()   # may time out / fail as device resets
        except Exception:
            pass

    def upload(self, address, length):
        """
        Read `length` bytes from device flash starting at `address`.

        Returns:
            bytes
        """
        self.set_address(address)
        # DfuSe leaves us in dfuDNLOAD-IDLE after Set Address Pointer; UPLOAD is
        # only served from dfuIDLE/dfuUPLOAD-IDLE, so abort back to dfuIDLE first.
        self.abort()

        result    = bytearray()
        block_num = 2
        remaining = length

        while remaining > 0:
            chunk_len = min(remaining, self.xfer_size)
            chunk = bytes(self._dev.ctrl_transfer(
                self._D2H, self._REQ_UPLOAD, block_num, self._iface, chunk_len))
            result.extend(chunk)
            remaining -= chunk_len
            block_num += 1

        self.abort()    # return to dfuIDLE without resetting
        return bytes(result)

    def read_version(self):
        """
        Read the bootloader version string (FW_VERSION) via the virtual DFU
        version address. Returns the decoded string with null padding stripped.
        """
        raw = self.upload(self.DFU_VERSION_ADDR, self.DFU_VERSION_LEN)
        return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace").strip()

    def verify(self, address, data, progress_cb=None):
        """
        Read back the programmed region and compare with `data`.

        Returns:
            True if every byte matches.

        Raises:
            DFUError: on the first mismatch (includes offset and values).
        """
        data     = bytes(data)
        readback = self.upload(address, len(data))

        for i, (expected, actual) in enumerate(zip(data, readback)):
            if expected != actual:
                raise DFUError(
                    f"Verify mismatch at {address + i:#010x}: "
                    f"expected {expected:#04x}, got {actual:#04x}")

        return True

    def leave_dfu(self):
        """
        Reset the device without downloading firmware.
        The bootloader will verify and launch the app if a valid signed image
        exists in the active slot.
        """
        self.set_address(self.APP_FLASH_START)
        self._dnload(0, b'')
        try:
            self.get_status()
        except Exception:
            pass

    # ── internal helpers ─────────────────────────────────────────────────────

    def _dnload(self, block_num, data):
        self._dev.ctrl_transfer(
            self._H2D, self._REQ_DNLOAD, block_num, self._iface, bytes(data))

    def _poll_idle(self, timeout_s=10.0):
        """
        Poll GETSTATUS until the state is dfuDNLOAD-IDLE or dfuIDLE.
        Respects the bwPollTimeout from each status response.
        """
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            status, poll_ms, state, _ = self.get_status()
            if status != DFUStatus.OK:
                raise DFUError(
                    f"Device error: {DFUStatus.name(status)} "
                    f"in state {DFUState.name(state)}")
            if state in (DFUState.DFU_DNLOAD_IDLE, DFUState.DFU_IDLE):
                return
            if state == DFUState.DFU_ERROR:
                raise DFUError(
                    f"dfuERROR: {DFUStatus.name(status)}")
            time.sleep(max(poll_ms / 1000.0, 0.001))
        raise TimeoutError(f"Timed out after {timeout_s:.0f}s waiting for dfuIDLE")

    def _ensure_idle(self):
        """Recover to dfuIDLE regardless of current state."""
        try:
            _, _, state, _ = self.get_status()
        except Exception:
            return
        if state == DFUState.DFU_ERROR:
            self.clear_status()
        elif state not in (DFUState.DFU_IDLE, DFUState.DFU_DNLOAD_IDLE,
                           DFUState.DFU_UPLOAD_IDLE):
            self.abort()
