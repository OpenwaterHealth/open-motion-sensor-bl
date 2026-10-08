#!/usr/bin/env python3
"""
sign_firmware.py  —  Create an SBSFU-compatible signed firmware image.

Crypto scheme: SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256 (header format)
  • A 320-byte header is prepended containing metadata, the SHA-256 of the
    firmware (FwTag), an AES-CBC IV field, and an ECDSA-P256/SHA-256 signature
    over the first 128 authenticated bytes of the header.
  • The firmware body is written IN CLEAR (see Step 3 below): the bootloader
    authenticates the active slot by SHA-256 only. No AES key is needed to sign.

Signing backends
  • --kms-key      Google Cloud KMS asymmetric key version (HSM, non-exportable).
                   Production path. Credentials come from Application Default
                   Credentials (in CI: google-github-actions/auth via Workload
                   Identity Federation). The signature is verified locally against
                   the KMS public key before the image is written.
  • --private-key  Local ECDSA P-256 PEM. Debug builds and bench test keys only.

Output image layout (written to SLOT_ACTIVE_1 starting at 0x08020000 via DFU):

    Offset 0x000  [  4 B]  SFUMagic      "SFU1"
    Offset 0x004  [  2 B]  ProtocolVersion  0x0001
    Offset 0x006  [  2 B]  FwVersion     major*10000 + minor*100 + patch, see pack_semver()
    Offset 0x008  [  4 B]  FwSize        size of encrypted firmware (bytes)
    Offset 0x00C  [  4 B]  PartialFwOffset  0
    Offset 0x010  [  4 B]  PartialFwSize    0
    Offset 0x014  [ 32 B]  FwTag         SHA-256(plaintext firmware)
    Offset 0x034  [ 32 B]  PartialFwTag  = FwTag (full image)
    Offset 0x054  [ 16 B]  InitVector    random AES-CBC IV
    Offset 0x064  [ 28 B]  Reserved      0x00...
    ─── end of authenticated region (128 bytes = 0x80) ──────────────────────
    Offset 0x080  [ 64 B]  HeaderSignature  ECDSA-P256(SHA-256(bytes 0..127))
    Offset 0x0C0  [ 96 B]  FwImageState  3 × 32 bytes of 0xFF (VALID marker)
    Offset 0x140  [ 32 B]  PrevHeaderFingerprint  0x00... (first install)
    ─── end of header (320 bytes = 0x140) ───────────────────────────────────
    Offset 0x140  [FwSize] Encrypted firmware (AES-128-CBC)

The resulting binary is installed into the active slot over USB DFU:

    python py-tools/flash_firmware.py <output>.bin

Usage
-----
    # Production (CI): sign with the HSM-held key in Google Cloud KMS
    python sign_firmware.py \\
        --firmware   path/to/app.bin \\
        --kms-key    projects/<project>/locations/<loc>/keyRings/<ring>/cryptoKeys/<key>/cryptoKeyVersions/<n> \\
        --version    1.0.0 \\
        --output     signed_app.bin

    # Debug / bench: sign with a local test key
    python sign_firmware.py \\
        --firmware     path/to/app.bin \\
        --private-key  py-tools/keys/ecdsa_private.pem \\
        --version      1.0.0 \\
        --output       signed_app.bin
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit(
        "ERROR: 'cryptography' package not found.\n"
        "Activate the venv and run:  pip install -r requirements.txt"
    )

# ---------------------------------------------------------------------------
# SBSFU header constants
# ---------------------------------------------------------------------------
SFUMAGIC          = b"SFU1"
PROTOCOL_VERSION  = 1
HEADER_AUTH_LEN   = 128    # bytes signed by ECDSA
HEADER_SIGN_LEN   = 64     # ECDSA P-256 raw R||S
HEADER_STATE_LEN  = 96     # 3 × 32 bytes, initially 0xFF
HEADER_FP_LEN     = 32     # PrevHeaderFingerprint, 0x00 for first install
HEADER_TOTAL_LEN  = HEADER_AUTH_LEN + HEADER_SIGN_LEN + HEADER_STATE_LEN + HEADER_FP_LEN
# = 320 bytes (0x140)

# Offset, from the slot start, at which SBSFU expects the (encrypted) firmware
# binary. This MUST match SFU_IMG_IMAGE_OFFSET in
# SBSFU/App/Inc/sfu_fwimg_regions.h. On Cortex-M7 the firmware vector table is
# aligned to 0x400 (1024) — larger than the 0x140 header — so the slot image is
#   [header 0x000..0x13F] [pad 0x140..0x3FF = 0xFF] [encrypted FW @ 0x400].
# When the image is flashed directly into the active slot (DFU / ST-Link, i.e.
# the SECBOOT_USE_NO_LOADER configuration) the firmware must already sit at this
# offset; there is no SBSFU install step to relocate it.
IMAGE_OFFSET      = 0x400   # = SFU_IMG_IMAGE_OFFSET (1024 bytes)

AES_BLOCK         = 16     # AES block size in bytes
FLASH_WORD        = 32     # STM32H7 flash word (min programmable unit)

# ---------------------------------------------------------------------------
# FwVersion (anti-rollback) semver <-> uint16 encoding
# ---------------------------------------------------------------------------
# The signed header's 16-bit FwVersion is the decimal encoding
#
#   FwVersion = major * 10000 + minor * 100 + patch
#
# which is what the firmware release workflows have signed every released image
# with (1.8.1 -> 10801). It must never change: fielded units carry a monotonic
# anti-rollback floor in this encoding, and an image signed under a different
# scheme would compare below the floor and be refused as a downgrade.
#
# minor and patch are limited to 0..99 so the integer orders exactly like
# (major, minor, patch); the 16-bit field then caps the range at 6.55.35.
# 0.0.0 encodes to 0, which SBSFU reserves for "no firmware", so the minimum
# valid release is 0.0.1. The bootloader compares the raw integer with a plain
# unsigned `<` and needs no knowledge of this scheme.
FWVER_MAJOR_MULT  = 10000
FWVER_MINOR_MULT  = 100
FWVER_MINOR_MAX   = 99
FWVER_PATCH_MAX   = 99
FWVER_MAX         = 0xFFFF   # 6.55.35


def pack_semver(major: int, minor: int, patch: int) -> int:
    """Encode semver (major, minor, patch) as the 16-bit FwVersion field.

    Ranges: minor 0-99, patch 0-99, and the result must fit 16 bits (max
    6.55.35). 0.0.0 is invalid. The result orders exactly like the tuple.
    """
    if major < 0:
        raise ValueError(f"major must be >= 0, got {major}")
    if not (0 <= minor <= FWVER_MINOR_MAX):
        raise ValueError(f"minor must be 0..{FWVER_MINOR_MAX}, got {minor}")
    if not (0 <= patch <= FWVER_PATCH_MAX):
        raise ValueError(f"patch must be 0..{FWVER_PATCH_MAX}, got {patch}")
    packed = major * FWVER_MAJOR_MULT + minor * FWVER_MINOR_MULT + patch
    if packed == 0:
        raise ValueError("0.0.0 is not a valid FwVersion (minimum is 0.0.1)")
    if packed > FWVER_MAX:
        raise ValueError(
            f"{major}.{minor}.{patch} does not fit the 16-bit FwVersion (max 6.55.35)"
        )
    return packed


def unpack_semver(fw_version: int) -> tuple[int, int, int]:
    """Inverse of pack_semver — return (major, minor, patch)."""
    if not (0 <= fw_version <= FWVER_MAX):
        raise ValueError(f"FwVersion must fit in 16 bits, got {fw_version}")
    major = fw_version // FWVER_MAJOR_MULT
    minor = (fw_version // FWVER_MINOR_MULT) % 100
    patch = fw_version % 100
    return major, minor, patch

def parse_version_arg(value: str) -> int:
    """Parse the --version CLI argument.

    Accepts either a dotted semver 'MAJOR.MINOR.PATCH' (preferred) or a raw
    decimal/hex integer that is already packed. Returns the packed uint16.
    """
    s = value.strip()
    if "." in s:
        parts = s.split(".")
        if len(parts) != 3:
            raise ValueError(
                f"semver must be MAJOR.MINOR.PATCH (got '{value}')"
            )
        try:
            major, minor, patch = (int(p) for p in parts)
        except ValueError as exc:
            raise ValueError(f"semver components must be integers: '{value}'") from exc
        return pack_semver(major, minor, patch)
    # Raw integer (decimal or 0x-prefixed hex)
    try:
        packed = int(s, 0)
    except ValueError as exc:
        raise ValueError(f"--version must be 'MAJOR.MINOR.PATCH' or an integer, got '{value}'") from exc
    if not (1 <= packed <= 0xFFFF):
        raise ValueError(f"raw FwVersion must be in 1..65535, got {packed}")
    return packed


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _pad_to_multiple(data: bytes, multiple: int, pad_byte: int = 0xFF) -> bytes:
    """Pad data to the next multiple of `multiple` bytes."""
    remainder = len(data) % multiple
    if remainder == 0:
        return data
    return data + bytes([pad_byte] * (multiple - remainder))


def _aes128_cbc_encrypt(key: bytes, iv: bytes, plaintext: bytes) -> bytes:
    """Encrypt plaintext with AES-128-CBC. Plaintext must be block-aligned."""
    assert len(key) == 16
    assert len(iv) == AES_BLOCK
    assert len(plaintext) % AES_BLOCK == 0
    cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
    enc = cipher.encryptor()
    return enc.update(plaintext) + enc.finalize()


def _der_to_raw_rs(der_sig: bytes) -> bytes:
    """DER ECDSA signature -> 64-byte raw R (32 B big-endian) || S (32 B big-endian)."""
    r, s = decode_dss_signature(der_sig)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


class LocalPemSigner:
    """ECDSA-P256/SHA-256 with a local private key PEM (Debug builds, bench test keys)."""

    name = "local PEM"

    def __init__(self, private_key_pem: bytes):
        self._priv = serialization.load_pem_private_key(private_key_pem, password=None)
        if not isinstance(self._priv, ec.EllipticCurvePrivateKey) or \
                not isinstance(self._priv.curve, ec.SECP256R1):
            raise ValueError("--private-key must be an ECDSA P-256 (secp256r1) private key")

    def public_key_pem(self) -> bytes:
        return self._priv.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)

    def sign_raw(self, data: bytes) -> bytes:
        return _der_to_raw_rs(self._priv.sign(data, ec.ECDSA(hashes.SHA256())))


class KmsSigner:
    """
    ECDSA-P256/SHA-256 with a Google Cloud KMS asymmetric key version.

    `key_version` is the full resource name
      projects/<p>/locations/<l>/keyRings/<r>/cryptoKeys/<k>/cryptoKeyVersions/<n>
    The private key never leaves the HSM; KMS signs the SHA-256 digest we send.
    Authentication is Application Default Credentials (gcloud auth
    application-default login, or Workload Identity Federation in CI).
    """

    name = "Google Cloud KMS"

    def __init__(self, key_version: str):
        try:
            from google.cloud import kms  # type: ignore
        except ImportError:
            sys.exit(
                "ERROR: 'google-cloud-kms' package not found (needed for --kms-key).\n"
                "Run:  pip install -r py-tools/requirements.txt"
            )
        if "/cryptoKeyVersions/" not in key_version:
            raise ValueError(
                "--kms-key must name a crypto key VERSION "
                "(.../cryptoKeys/<key>/cryptoKeyVersions/<n>), got: " + key_version)
        self._client = kms.KeyManagementServiceClient()
        self._key_version = key_version
        # Only GetPublicKey and AsymmetricSign: roles/cloudkms.signerVerifier (and
        # publicKeyViewer) grant viewPublicKey but NOT cryptoKeyVersions.get, so no
        # GetCryptoKeyVersion call here. GetPublicKey already reports algorithm and
        # protection level, and fails for a version that is not ENABLED.
        pub = self._client.get_public_key(request={"name": key_version})
        algo = kms.CryptoKeyVersion.CryptoKeyVersionAlgorithm
        if pub.algorithm != algo.EC_SIGN_P256_SHA256:
            raise ValueError(f"KMS key version algorithm is {pub.algorithm.name}, "
                             "expected EC_SIGN_P256_SHA256")
        self.protection_level = pub.protection_level.name
        self._pub_pem = pub.pem.encode()

    def public_key_pem(self) -> bytes:
        return self._pub_pem

    def sign_raw(self, data: bytes) -> bytes:
        digest = hashlib.sha256(data).digest()
        resp = self._client.asymmetric_sign(
            request={"name": self._key_version, "digest": {"sha256": digest}})
        return _der_to_raw_rs(resp.signature)


def _verify_raw_rs(public_key_pem: bytes, data: bytes, raw_sig: bytes) -> None:
    """Raise if raw R||S is not a valid ECDSA-P256/SHA-256 signature of data."""
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
    pub = serialization.load_pem_public_key(public_key_pem)
    der = encode_dss_signature(int.from_bytes(raw_sig[:32], "big"),
                               int.from_bytes(raw_sig[32:], "big"))
    try:
        pub.verify(der, data, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise RuntimeError("signature does not verify against the signer's public key")


def public_key_fingerprint(public_key_pem: bytes) -> str:
    """SHA-256 over the DER SubjectPublicKeyInfo, hex. Matches
    `openssl pkey -pubin -outform DER | openssl dgst -sha256`."""
    pub = serialization.load_pem_public_key(public_key_pem)
    der = pub.public_bytes(serialization.Encoding.DER,
                           serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(der).hexdigest()


# ---------------------------------------------------------------------------
# Main signing function
# ---------------------------------------------------------------------------

def sign_firmware(
    firmware_bin: bytes,
    private_key_pem: bytes | None = None,
    aes_key: bytes | None = None,
    fw_version: int = 1,
    signer=None,
) -> bytes:
    """
    Sign a raw firmware binary for SBSFU.

    Parameters
    ----------
    firmware_bin    : raw application .bin (starts at application load address)
    private_key_pem : PEM-encoded ECDSA P-256 private key (local signing);
                      ignored when `signer` is given
    aes_key         : optional 16-byte AES-128 key. Accepted for compatibility
                      and validated if given; NOT used (body is stored in clear).
    fw_version      : 16-bit packed FwVersion (see pack_semver);
                      must be in [1, 0xFFFF] — 0 (0.0.0) is invalid.
    signer          : object with sign_raw(data)->64 B and public_key_pem()->PEM
                      (LocalPemSigner or KmsSigner). Takes precedence.

    Returns
    -------
    bytes — complete signed image: SBSFU header (320 B) + padding + firmware
    """

    if aes_key is not None and len(aes_key) != 16:
        raise ValueError(f"AES key must be 16 bytes, got {len(aes_key)}")
    if not (1 <= fw_version <= 0xFFFF):
        raise ValueError(
            f"fw_version must be in 1..65535 (0 is invalid); got {fw_version}"
        )

    if signer is None:
        if private_key_pem is None:
            raise ValueError("either `signer` or `private_key_pem` is required")
        signer = LocalPemSigner(private_key_pem)

    # ------------------------------------------------------------------
    # Step 1: Pad plaintext to a multiple of FLASH_WORD (32 bytes).
    #         AES-CBC requires a multiple of 16; 32 satisfies both.
    # ------------------------------------------------------------------
    plaintext = _pad_to_multiple(firmware_bin, FLASH_WORD, 0xFF)

    # ------------------------------------------------------------------
    # Step 2: Compute SHA-256 of the padded plaintext → FwTag
    # ------------------------------------------------------------------
    fw_tag = hashlib.sha256(plaintext).digest()   # 32 bytes

    # ------------------------------------------------------------------
    # Step 3: Init Vector for the header's AES-CBC InitVector field.
    #
    # IMPORTANT — the SBSFU *active slot* always stores the firmware IN CLEAR.
    # At boot, SE_CRYPTO_AuthenticateFW_* (for SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256)
    # only computes SHA-256 over the slot contents and compares it to FwTag; it does
    # NOT decrypt. AES-CBC decryption happens only in the install/download path
    # (SE_CRYPTO_Decrypt_*), which writes the clear firmware into the active slot.
    # Since this tool targets direct flashing (DFU / ST-Link) into the active slot
    # (SECBOOT_USE_NO_LOADER), the firmware body below is emitted IN CLEAR so that
    # SHA-256(slot) == FwTag == SHA-256(plaintext).
    #
    # The InitVector field is still part of the (ECDSA-signed) AES-CBC header
    # format, so a random IV is generated to populate it; it is unused at boot.
    iv         = os.urandom(AES_BLOCK)            # 16 bytes (header field only)
    fw_size    = len(plaintext)

    # ------------------------------------------------------------------
    # Step 4: Build the 128-byte authenticated header part
    # ------------------------------------------------------------------
    # struct layout (little-endian):
    #   4s  SFUMagic
    #   H   ProtocolVersion
    #   H   FwVersion
    #   I   FwSize
    #   I   PartialFwOffset
    #   I   PartialFwSize
    #   32s FwTag
    #   32s PartialFwTag   (== FwTag for full image)
    #   16s InitVector
    #   28s Reserved
    auth_header = struct.pack(
        "<4sHHIII32s32s16s28s",
        SFUMAGIC,
        PROTOCOL_VERSION,
        fw_version,
        fw_size,
        0,          # PartialFwOffset
        0,          # PartialFwSize
        fw_tag,
        fw_tag,     # PartialFwTag == FwTag for a full image
        iv,
        b"\x00" * 28,  # Reserved
    )
    assert len(auth_header) == HEADER_AUTH_LEN, \
        f"Auth header size mismatch: {len(auth_header)} != {HEADER_AUTH_LEN}"

    # ------------------------------------------------------------------
    # Step 5: ECDSA-P256/SHA-256 sign the 128-byte authenticated header
    # ------------------------------------------------------------------
    signature = signer.sign_raw(auth_header)   # 64 bytes R||S
    assert len(signature) == HEADER_SIGN_LEN
    # Independent check: the signature must verify with the signer's own public
    # key. Catches a misconfigured KMS key or a corrupt response before anything
    # is written.
    _verify_raw_rs(signer.public_key_pem(), auth_header, signature)

    # ------------------------------------------------------------------
    # Step 6: Assemble the full 320-byte header
    #   [128 bytes authenticated] [64 bytes signature]
    #   [96 bytes FwImageState = 0xFF] [32 bytes PrevFingerprint = 0x00]
    # ------------------------------------------------------------------
    fw_image_state       = b"\xFF" * HEADER_STATE_LEN   # VALID marker
    prev_fingerprint     = b"\x00" * HEADER_FP_LEN      # first install

    header = auth_header + signature + fw_image_state + prev_fingerprint
    assert len(header) == HEADER_TOTAL_LEN, \
        f"Header size mismatch: {len(header)} != {HEADER_TOTAL_LEN}"

    # Pad the header region out to SFU_IMG_IMAGE_OFFSET so the firmware body
    # lands at the offset SBSFU verifies/executes from when the image is flashed
    # directly into the active slot (NO_LOADER). Pad with 0xFF (erased-flash state).
    assert IMAGE_OFFSET >= HEADER_TOTAL_LEN, \
        f"IMAGE_OFFSET (0x{IMAGE_OFFSET:X}) must be >= header size (0x{HEADER_TOTAL_LEN:X})"
    pad = b"\xFF" * (IMAGE_OFFSET - HEADER_TOTAL_LEN)

    # Clear firmware body (see Step 3) — the active slot holds plaintext.
    return header + pad + plaintext


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Sign a firmware binary for the SBSFU active slot "
                    "(ECCDSA_WITH_AES128_CBC_SHA256 header format, clear body)."
    )
    parser.add_argument("--firmware",    required=True, help="Input raw .bin file.")
    key_grp = parser.add_mutually_exclusive_group()
    key_grp.add_argument(
        "--kms-key",
        help="Google Cloud KMS crypto key VERSION resource name "
             "(projects/.../cryptoKeys/<key>/cryptoKeyVersions/<n>). Production signing; "
             "uses Application Default Credentials.",
    )
    key_grp.add_argument(
        "--private-key",
        help="Local ECDSA P-256 private key PEM. Debug/bench test keys only "
             "(default when --kms-key is absent: py-tools/keys/ecdsa_private.pem).",
    )
    parser.add_argument(
        "--aes-key",
        default=None,
        help="Raw 16-byte AES-128 key. Optional and unused for signing (the slot image "
             "body is stored in clear); accepted for backward compatibility.",
    )
    parser.add_argument(
        "--version", type=str, default="0.0.1",
        help="Firmware version, dotted semver 'MAJOR.MINOR.PATCH' "
             "(encoded as major*10000 + minor*100 + patch; max 6.55.35; 0.0.0 invalid). "
             "A raw integer (1..65535) is also accepted. Default: 0.0.1.",
    )
    parser.add_argument(
        "--output",
        help="Output signed image file (default: <firmware>_signed.bin).",
    )
    args = parser.parse_args()

    fw_path  = Path(args.firmware)
    out_path = Path(args.output) if args.output else fw_path.with_stem(fw_path.stem + "_signed")

    firmware_bin = fw_path.read_bytes()
    aes_key = Path(args.aes_key).read_bytes() if args.aes_key else None

    if args.kms_key:
        signer = KmsSigner(args.kms_key)
        key_desc = f"{args.kms_key} ({signer.protection_level})"
    else:
        pem_path = Path(args.private_key) if args.private_key \
            else Path(__file__).parent / "keys" / "ecdsa_private.pem"
        if not pem_path.is_file():
            parser.error(f"private key not found: {pem_path} (use --kms-key for production signing)")
        signer = LocalPemSigner(pem_path.read_bytes())
        key_desc = str(pem_path)

    try:
        fw_version = parse_version_arg(args.version)
    except ValueError as exc:
        parser.error(str(exc))
    major, minor, patch = unpack_semver(fw_version)

    print(f"[sign_firmware] Input        : {fw_path}  ({len(firmware_bin):,} bytes)")
    print(f"[sign_firmware] FW version   : {major}.{minor}.{patch}  "
          f"(FwVersion = {fw_version} / 0x{fw_version:04X})")
    print(f"[sign_firmware] Signer       : {signer.name}  {key_desc}")
    print(f"[sign_firmware] Public key   : sha256:{public_key_fingerprint(signer.public_key_pem())}")

    signed = sign_firmware(
        firmware_bin = firmware_bin,
        aes_key      = aes_key,
        fw_version   = fw_version,
        signer       = signer,
    )

    out_path.write_bytes(signed)

    header_size = HEADER_TOTAL_LEN
    fw_size     = len(signed) - IMAGE_OFFSET
    print(f"[sign_firmware] Header       : {header_size} bytes  (auth={HEADER_AUTH_LEN}, "
          f"sig={HEADER_SIGN_LEN}, state={HEADER_STATE_LEN}, fp={HEADER_FP_LEN})")
    print(f"[sign_firmware] FW @ offset  : 0x{IMAGE_OFFSET:X} (SFU_IMG_IMAGE_OFFSET; "
          f"0x{IMAGE_OFFSET - HEADER_TOTAL_LEN:X} bytes of 0xFF padding after header)")
    print(f"[sign_firmware] FW body      : {fw_size:,} bytes (clear, SHA-256 in header)")
    print(f"[sign_firmware] Total output : {len(signed):,} bytes")
    print(f"[sign_firmware] Output       : {out_path}")
    print()
    print("Check, then install over USB DFU:")
    print(f"  python py-tools/verify_firmware.py \"{out_path}\"")
    print(f"  python py-tools/flash_firmware.py  \"{out_path}\"")


if __name__ == "__main__":
    main()
