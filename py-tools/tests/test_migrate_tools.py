"""Host-side unit tests for the migration tooling (no hardware).

    python -m pytest py-tools/tests -q      or      python py-tools/tests/test_migrate_tools.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import console_cdc                                      # noqa: E402
import stm32dfu                                         # noqa: E402
import migrate                                          # noqa: E402


class PacketFraming(unittest.TestCase):
    # Vectors produced with the SDK's omotion.utils.util_crc16 / UartPacket (2026-10-08).
    def test_dfu_packet_matches_sdk(self):
        self.assertEqual(console_cdc.build_packet(1, console_cdc.OW_CMD, console_cdc.OW_CMD_DFU).hex(),
                         "aa0001e20d00000000a85ddd")
        self.assertEqual(console_cdc.build_packet(0x1234, console_cdc.OW_CMD, console_cdc.OW_CMD_DFU).hex(),
                         "aa1234e20d000000003a65dd")

    def test_crc_ccitt_false_check_value(self):
        self.assertEqual(console_cdc.crc16_ccitt(b"123456789"), 0x29B1)

    def test_parse_roundtrip_and_error_type(self):
        pkt = console_cdc.build_packet(7, console_cdc.OW_ERROR, console_cdc.OW_CMD_DFU, payload=b"\x01\x02")
        p = console_cdc.parse_packet(pkt)
        self.assertEqual((p["id"], p["type"], p["command"], p["data"]), (7, 0xEF, 0x0D, b"\x01\x02"))
        with self.assertRaises(console_cdc.ConsoleError):
            console_cdc.parse_packet(pkt[:-2] + b"\x00\xdd")      # bad CRC


class ModeClassification(unittest.TestCase):
    ROM = ["@Internal Flash  /0x08000000/16*128Kg",
           "@Option Bytes  /0x5200201C/01*128 e",
           "@OTP Memory /0x1FF0F000/01*1024 e",
           "@Device Feature/0xFFFF0000/01*004 e"]
    BL = ["@Internal Flash/0x08000000/01*128Ka,04*128Kg,11*128Ka"]

    def test_rom(self):
        self.assertEqual(stm32dfu.classify_alt_names(self.ROM), "rom")

    def test_bootloader(self):
        self.assertEqual(stm32dfu.classify_alt_names(self.BL), "bootloader")

    def test_unknown(self):
        self.assertEqual(stm32dfu.classify_alt_names([]), "unknown")
        self.assertEqual(stm32dfu.classify_alt_names(["@Internal Flash/0x08000000/16*128Kg"]), "unknown")
        self.assertEqual(stm32dfu.classify_alt_names(["junk", "more junk"]), "unknown")


class VersionParsing(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(migrate.parse_bl_version("1.0.0"), (1, 0, 0))
        self.assertEqual(migrate.parse_bl_version("1.2.0-rc.1"), (1, 2, 0))
        self.assertEqual(migrate.parse_bl_version("v1.2.0-3-gabcdef-dirty"), (1, 2, 0))
        self.assertIsNone(migrate.parse_bl_version("abcdef1"))
        self.assertTrue(migrate.parse_bl_version("1.0.0") < (1, 2, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
