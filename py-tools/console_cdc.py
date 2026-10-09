#!/usr/bin/env python3
"""
console_cdc.py — talk to a running Open-Motion console application over its USB CDC
command port, without the SDK. Only what the migration tooling needs: find the port,
read the firmware version, ask the application to reboot into DFU.

Packet framing is the firmware's (Core/Src/utils.c, mirrored by the SDK's
omotion/UartPacket.py):

    AA | id (BE16) | type | command | addr | reserved | len (BE16) | payload |
    CRC16 (BE16, CCITT-FALSE over everything after AA) | DD

A reply of type 0xEF (OW_ERROR) means the command was refused.
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:                               # pragma: no cover
    print("pyserial is required: pip install pyserial", file=sys.stderr)
    raise

CONSOLE_VID = 0x0483
CONSOLE_PID = 0xA53E                               # USB_DEVICE/App/usbd_desc.c USBD_PID_FS

OW_START_BYTE   = 0xAA
OW_END_BYTE     = 0xDD
OW_CMD          = 0xE2                             # command packet type
OW_ERROR        = 0xEF                             # refused
OW_CMD_PING     = 0x00
OW_CMD_VERSION  = 0x02
OW_CMD_DFU      = 0x0D                             # Core/Inc/common.h

HEADER_LEN = 9                                     # AA id2 type cmd addr res len2
TRAILER_LEN = 3                                    # crc2 DD


class ConsoleError(Exception):
    pass


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout)."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def build_packet(pkt_id: int, pkt_type: int, command: int,
                 addr: int = 0, reserved: int = 0, payload: bytes = b"") -> bytes:
    body = bytearray([OW_START_BYTE])
    body += pkt_id.to_bytes(2, "big")
    body += bytes([pkt_type & 0xFF, command & 0xFF, addr & 0xFF, reserved & 0xFF])
    body += len(payload).to_bytes(2, "big")
    body += payload
    body += crc16_ccitt(body[1:]).to_bytes(2, "big")
    body.append(OW_END_BYTE)
    return bytes(body)


def parse_packet(buf: bytes) -> dict:
    if len(buf) < HEADER_LEN + TRAILER_LEN or buf[0] != OW_START_BYTE or buf[-1] != OW_END_BYTE:
        raise ConsoleError(f"malformed reply ({len(buf)} bytes): {buf.hex()}")
    data_len = int.from_bytes(buf[7:9], "big")
    if len(buf) != HEADER_LEN + data_len + TRAILER_LEN:
        raise ConsoleError(f"reply length {len(buf)} does not match len field {data_len}")
    if crc16_ccitt(buf[1:HEADER_LEN + data_len]) != int.from_bytes(buf[-3:-1], "big"):
        raise ConsoleError("reply CRC mismatch")
    return {
        "id": int.from_bytes(buf[1:3], "big"), "type": buf[3], "command": buf[4],
        "addr": buf[5], "reserved": buf[6], "data": bytes(buf[HEADER_LEN:HEADER_LEN + data_len]),
    }


def find_console_ports() -> list:
    """Serial ports whose USB IDs are the console application's (0483:A53E)."""
    return [p.device for p in serial.tools.list_ports.comports()
            if getattr(p, "vid", None) == CONSOLE_VID and getattr(p, "pid", None) == CONSOLE_PID]


class ConsoleCdc:
    """Minimal synchronous command client. Use as a context manager."""

    def __init__(self, port: str, timeout: float = 3.0):
        self.port = port
        self.timeout = timeout
        self._ser = None
        self._id = 0

    def __enter__(self):
        # Baud rate is irrelevant on CDC but must be set; the SDK uses 921600.
        self._ser = serial.Serial(self.port, baudrate=921600, timeout=0.05)
        time.sleep(0.1)
        self._ser.reset_input_buffer()
        return self

    def __exit__(self, *_):
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    def _read_packet(self, deadline: float) -> dict:
        buf = bytearray()
        while time.monotonic() < deadline:
            chunk = self._ser.read(256)
            if chunk:
                buf += chunk
                # discard anything before the start byte (trace noise)
                while buf and buf[0] != OW_START_BYTE:
                    buf.pop(0)
                if len(buf) >= HEADER_LEN:
                    need = HEADER_LEN + int.from_bytes(buf[7:9], "big") + TRAILER_LEN
                    if len(buf) >= need:
                        return parse_packet(bytes(buf[:need]))
        raise ConsoleError(f"no reply from {self.port} within {self.timeout:.1f}s")

    def command(self, command: int, payload: bytes = b"", timeout: float = None) -> dict:
        self._id = (self._id % 0xFFFE) + 1
        pkt = build_packet(self._id, OW_CMD, command, payload=payload)
        self._ser.reset_input_buffer()
        self._ser.write(pkt)
        self._ser.flush()
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            rsp = self._read_packet(deadline)
            if rsp["id"] == self._id or rsp["command"] == command:
                return rsp

    def ping(self) -> bool:
        return self.command(OW_CMD_PING)["type"] != OW_ERROR

    def version(self) -> str:
        rsp = self.command(OW_CMD_VERSION)
        if rsp["type"] == OW_ERROR:
            raise ConsoleError("version command refused")
        d = rsp["data"].split(b"\x00", 1)[0]
        try:
            txt = d.decode("ascii")
            if txt and all(32 <= ord(c) < 127 for c in txt):
                return txt.strip()                      # firmware replies with a string, e.g. "1.8.1"
        except UnicodeDecodeError:
            pass
        return ".".join(str(b) for b in d[:3]) if len(d) >= 3 else d.hex()

    def enter_dfu(self) -> bool:
        """Ask the application to reboot into the bootloader's DFU mode.

        The firmware answers first and resets ~100 ms later from a timer callback,
        so a reply normally arrives. A missing reply is reported as success=False
        and the caller decides by watching for the DFU device.
        """
        try:
            rsp = self.command(OW_CMD_DFU)
        except ConsoleError:
            return False
        return rsp["type"] != OW_ERROR


def console_enter_dfu(port: str = None, timeout: float = 3.0) -> tuple:
    """Find the console (or use `port`), log its version, request DFU.

    Returns (port, app_version_or_None, acknowledged: bool).
    Raises ConsoleError if no console port is present or several are and none was chosen.
    """
    if port is None:
        ports = find_console_ports()
        if not ports:
            raise ConsoleError("no console application port (USB 0483:A53E) found")
        if len(ports) > 1:
            raise ConsoleError(f"several console ports found {ports}; pass --port")
        port = ports[0]
    with ConsoleCdc(port, timeout=timeout) as c:
        ver = None
        try:
            ver = c.version()
        except ConsoleError:
            pass
        ack = c.enter_dfu()
    return port, ver, ack


if __name__ == "__main__":   # small manual check: python console_cdc.py [PORT]
    p, v, a = console_enter_dfu(sys.argv[1] if len(sys.argv) > 1 else None)
    print(f"port {p}: app version {v or '?'}; DFU request {'acknowledged' if a else 'not acknowledged'}")
