#!/usr/bin/env python3
"""
verify_firmware.py  —  Check a signed SBSFU image on the host BEFORE it is flashed.

The bootloader has a single application slot: a DFU download erases the installed
image first and SBSFU only verifies the new one on the next boot. A bad file is
therefore refused too late to keep the old firmware — the device stays in USB DFU
until a good image is installed. This tool runs the same checks the bootloader
will run, on the file, so a bad image is caught while the installed one is intact.

Only the ECDSA *public* key is needed: the active slot holds the firmware in
clear (see sign_firmware.py, Step 3), so FwTag can be recomputed from the file.

Checks, in the order the bootloader applies them:
    1. "SFU1" magic and ProtocolVersion
    2. ECDSA-P256/SHA-256 signature over the 128 authenticated header bytes
    3. FwVersion is valid and not below --min-version (anti-rollback)
    4. Full-image header (no partial update) and FwSize fits the slot
    5. SHA-256 of the firmware body equals FwTag
    6. Nothing but 0x00/0xFF after the firmware body (SBSFU VerifySlot)

This is a pre-flight for the operator, not a security boundary: the bootloader
still verifies every image at boot and that check cannot be skipped.

Usage
-----
    python verify_firmware.py signed_app.bin
    python verify_firmware.py signed_app.bin --min-version 1.8.1
    python verify_firmware.py signed_app.bin --public-key keys/ecdsa_public.pem
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

# The header layout is owned by sign_firmware.py; import it so the two cannot drift.
from sign_firmware import (
    HEADER_AUTH_LEN,
    HEADER_SIGN_LEN,
    IMAGE_OFFSET,
    PROTOCOL_VERSION,
    SFUMAGIC,
    parse_version_arg,
    unpack_semver,
)

# Active slot size. MUST match SLOT_ACTIVE_1 in Linker/mapping_fwimg.ld
# (0x08020000-0x0809FFFF on the sensor bootloader).
SLOT_SIZE = 0x00080000

# Same struct as sign_firmware.py, Step 4.
_AUTH_HEADER_FMT = "<4sHHIII32s32s16s28s"


class ImageVerificationError(Exception):
    """The image would be rejected by the bootloader."""


# FwVersion is compared as the raw uint16, exactly as the bootloader does (plain
# unsigned `<`); the dotted form is for display only (sign_firmware.unpack_semver).
@dataclass(frozen=True)
class ImageInfo:
    fw_version: int      # raw uint16 FwVersion from the signed header
    fw_size: int         # firmware body size in bytes
    fw_tag: bytes        # SHA-256 of the firmware body

    @property
    def version_str(self) -> str:
        """'10801 (1.8.1)' — raw value first, since that is what the floor stores."""
        return "{} ({}.{}.{})".format(self.fw_version, *unpack_semver(self.fw_version))


def read_header_version(header: bytes) -> int:
    """
    FwVersion from the start of a slot/image, or 0 if there is no "SFU1" header.
    Not authenticated — use it only to read what is currently installed.
    """
    if len(header) < 8 or header[:4] != SFUMAGIC:
        return 0
    return struct.unpack_from("<H", header, 6)[0]


def verify_image(
    image: bytes,
    public_key_pem: bytes,
    slot_size: int = SLOT_SIZE,
    min_version: int = 0,
) -> ImageInfo:
    """
    Verify a signed image the way the bootloader will. Raises
    ImageVerificationError naming the first check that fails.
    """
    if len(image) < IMAGE_OFFSET:
        raise ImageVerificationError(
            f"file is {len(image)} bytes, shorter than the 0x{IMAGE_OFFSET:X}-byte header region"
        )

    auth_header = image[:HEADER_AUTH_LEN]
    (magic, protocol, fw_version, fw_size, partial_offset, partial_size,
     fw_tag, partial_tag, _iv, _reserved) = struct.unpack(_AUTH_HEADER_FMT, auth_header)

    if magic != SFUMAGIC:
        raise ImageVerificationError(
            "no 'SFU1' magic: this is not a signed image (raw application .bin?)"
        )
    if protocol != PROTOCOL_VERSION:
        raise ImageVerificationError(f"unsupported ProtocolVersion {protocol}")

    # Signature first: no other header field is trusted until this passes.
    public_key = serialization.load_pem_public_key(public_key_pem)
    raw_sig = image[HEADER_AUTH_LEN:HEADER_AUTH_LEN + HEADER_SIGN_LEN]
    der_sig = encode_dss_signature(
        int.from_bytes(raw_sig[:32], "big"), int.from_bytes(raw_sig[32:], "big")
    )
    try:
        public_key.verify(der_sig, auth_header, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise ImageVerificationError(
            "header signature is invalid: the header was modified, or the image "
            "was signed with a different key than this bootloader trusts"
        ) from None

    if fw_version == 0:
        raise ImageVerificationError("FwVersion 0 is invalid")
    if fw_version < min_version:
        raise ImageVerificationError(
            f"FwVersion {fw_version} is older than {min_version}: the bootloader "
            "refuses downgrades"
        )

    if partial_offset != 0 or partial_size != 0 or partial_tag != fw_tag:
        raise ImageVerificationError("header describes a partial image; only full images are supported")
    if fw_size == 0 or fw_size > slot_size - IMAGE_OFFSET:
        raise ImageVerificationError(
            f"FwSize {fw_size} does not fit the {slot_size // 1024} KB application slot"
        )
    body_end = IMAGE_OFFSET + fw_size
    if len(image) < body_end:
        raise ImageVerificationError(
            f"file is truncated: header declares {fw_size} firmware bytes, "
            f"file holds {len(image) - IMAGE_OFFSET}"
        )

    if hashlib.sha256(image[IMAGE_OFFSET:body_end]).digest() != fw_tag:
        raise ImageVerificationError(
            "firmware body does not match FwTag: the file is corrupted or was modified after signing"
        )

    # VerifySlot (sfu_fwimg_common.c) accepts only 0x00 or 0xFF beyond the body.
    if image[body_end:].strip(b"\x00\xFF"):
        raise ImageVerificationError("unexpected data after the firmware body")

    return ImageInfo(fw_version=fw_version, fw_size=fw_size, fw_tag=fw_tag)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

DEFAULT_PUBLIC_KEY = Path(__file__).parent / "keys" / "ecdsa_public.pem"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify a signed SBSFU image before flashing it."
    )
    parser.add_argument("image", help="Signed .bin (from sign_firmware.py).")
    parser.add_argument(
        "--public-key",
        default=str(DEFAULT_PUBLIC_KEY),
        help="ECDSA P-256 public key PEM the bootloader was built with "
             "(default: py-tools/keys/ecdsa_public.pem).",
    )
    parser.add_argument(
        "--slot-size",
        type=lambda x: int(x, 0),
        default=SLOT_SIZE,
        help=f"Application slot size in bytes (default: 0x{SLOT_SIZE:X}).",
    )
    parser.add_argument(
        "--min-version",
        type=parse_version_arg,
        default=0,
        help="Reject an image older than this version, e.g. the installed one "
             "('1.8.1' or the raw FwVersion 10801).",
    )
    args = parser.parse_args()

    image = Path(args.image).read_bytes()
    public_key_pem = Path(args.public_key).read_bytes()

    try:
        info = verify_image(image, public_key_pem, args.slot_size, args.min_version)
    except ImageVerificationError as e:
        sys.exit(f"[verify_firmware] REJECTED: {e}")

    print(f"[verify_firmware] OK: {args.image}")
    print(f"[verify_firmware] FwVersion    : {info.version_str}")
    print(f"[verify_firmware] FwSize       : {info.fw_size} bytes")
    print(f"[verify_firmware] FwTag        : {info.fw_tag.hex()}")


if __name__ == "__main__":
    main()
