#!/usr/bin/env python3
"""
sensor_usb.py — talk to a running Open-Motion sensor-module application over its USB
command interface, without the SDK. The sensor is a composite USB device (0483:5A5A):
interface 0 carries commands as bulk transfers (SDK omotion/CommInterface.py), 1 and 2
are the histogram and IMU streams. Same packet framing as the console (console_cdc.py).

Only what the migration tooling needs: find modules, read the firmware version, ask the
application to reboot into DFU.
"""
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:                               # pragma: no cover
    print("pyusb is required: pip install pyusb", file=sys.stderr)
    raise

from console_cdc import (OW_CMD, OW_ERROR, OW_CMD_PING, OW_CMD_VERSION, OW_CMD_DFU, OW_END_BYTE,
                         OW_START_BYTE, HEADER_LEN, TRAILER_LEN, build_packet, parse_packet, ConsoleError)
from stm32dfu import _find_libusb

SENSOR_VID = 0x0483
SENSOR_PID = 0x5A5A                                # USB/Core/Src/usbd_desc.c USBD_PID
COMM_INTERFACE = 0                                 # MotionComposite: comm = interface 0


class SensorError(ConsoleError):
    pass


def _serial(dev):
    try:
        return usb.util.get_string(dev, dev.iSerialNumber) if dev.iSerialNumber else None
    except Exception:
        return None


def find_sensor_devices():
    """[(serial, bus, address)] for every sensor-module application on the bus."""
    backend = _find_libusb()
    out = []
    for d in usb.core.find(idVendor=SENSOR_VID, idProduct=SENSOR_PID, find_all=True, backend=backend) or []:
        out.append((_serial(d), d.bus, d.address))
    return out


class SensorUsb:
    """Minimal synchronous command client over bulk USB. Use as a context manager."""

    def __init__(self, serial=None, timeout=3.0):
        self.serial = serial
        self.timeout = timeout
        self._dev = None
        self._ep_in = None
        self._ep_out = None
        self._id = 0

    def __enter__(self):
        backend = _find_libusb()
        devs = list(usb.core.find(idVendor=SENSOR_VID, idProduct=SENSOR_PID, find_all=True, backend=backend) or [])
        if self.serial:
            devs = [d for d in devs if _serial(d) == self.serial]
        if not devs:
            raise SensorError("no sensor-module application (USB 0483:5A5A) found"
                              + (f" with serial {self.serial!r}" if self.serial else ""))
        if len(devs) > 1:
            raise SensorError(f"{len(devs)} sensor modules found; pass --serial")
        dev = devs[0]
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass                                    # already configured by the OS
        if sys.platform != "win32":
            try:
                if dev.is_kernel_driver_active(COMM_INTERFACE):
                    dev.detach_kernel_driver(COMM_INTERFACE)
            except Exception:
                pass
        usb.util.claim_interface(dev, COMM_INTERFACE)
        intf = dev.get_active_configuration()[(COMM_INTERFACE, 0)]
        self._ep_out = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self._ep_in = usb.util.find_descriptor(
            intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        if self._ep_out is None or self._ep_in is None:
            usb.util.release_interface(dev, COMM_INTERFACE)
            raise SensorError("sensor command interface has no bulk IN/OUT endpoint pair")
        self._dev = dev
        self._drain()
        return self

    def __exit__(self, *_):
        if self._dev is not None:
            try:
                usb.util.release_interface(self._dev, COMM_INTERFACE)
                usb.util.dispose_resources(self._dev)
            except Exception:
                pass
            self._dev = None

    def _drain(self):
        for _ in range(8):
            try:
                self._dev.read(self._ep_in.bEndpointAddress, 512, timeout=20)
            except usb.core.USBError:
                break

    def command(self, command, payload=b"", timeout=None):
        self._id = (self._id % 0xFFFE) + 1
        pkt = build_packet(self._id, OW_CMD, command, payload=payload)
        self._dev.write(self._ep_out.bEndpointAddress, pkt, timeout=1000)
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        buf = bytearray()
        while time.monotonic() < deadline:
            try:
                chunk = self._dev.read(self._ep_in.bEndpointAddress, 512, timeout=100)
            except usb.core.USBError as e:
                if getattr(e, "errno", None) in (110, 10060) or "timed out" in str(e).lower():
                    continue
                raise SensorError(f"USB read failed: {e}")
            if chunk:
                buf += bytes(chunk)
                while buf and buf[0] != OW_START_BYTE:
                    buf.pop(0)
                if len(buf) >= HEADER_LEN:
                    need = HEADER_LEN + int.from_bytes(buf[7:9], "big") + TRAILER_LEN
                    if len(buf) >= need:
                        rsp = parse_packet(bytes(buf[:need]))
                        if rsp["id"] == self._id or rsp["command"] == command:
                            return rsp
                        buf = buf[need:]
        raise SensorError(f"no reply from the sensor within {self.timeout:.1f}s")

    def ping(self):
        return self.command(OW_CMD_PING)["type"] != OW_ERROR

    def version(self):
        rsp = self.command(OW_CMD_VERSION)
        if rsp["type"] == OW_ERROR:
            raise SensorError("version command refused")
        d = rsp["data"].split(b"\x00", 1)[0]
        try:
            txt = d.decode("ascii")
            if txt and all(32 <= ord(c) < 127 for c in txt):
                return txt.strip()
        except UnicodeDecodeError:
            pass
        return ".".join(str(b) for b in d[:3]) if len(d) >= 3 else d.hex()

    def enter_dfu(self):
        """Ask the application to reboot into DFU. The firmware answers, then resets from
        the TIM15 callback; a missing reply is reported as False and the caller watches
        for the DFU device."""
        try:
            rsp = self.command(OW_CMD_DFU)
        except ConsoleError:
            return False
        return rsp["type"] != OW_ERROR


def sensor_enter_dfu(serial=None, timeout=3.0):
    """Returns (serial, app_version_or_None, acknowledged)."""
    with SensorUsb(serial=serial, timeout=timeout) as s:
        sn = _serial(s._dev)
        ver = None
        try:
            ver = s.version()
        except ConsoleError:
            pass
        ack = s.enter_dfu()
    return sn, ver, ack


if __name__ == "__main__":
    print(find_sensor_devices())
    print(sensor_enter_dfu(sys.argv[1] if len(sys.argv) > 1 else None))
