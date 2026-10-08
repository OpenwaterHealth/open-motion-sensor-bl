#!/usr/bin/env python3
"""
export_public_key.py  —  Fetch the firmware-signing PUBLIC key from Google Cloud KMS.

The production signing key lives in Cloud KMS (HSM, non-exportable). The
bootloader needs only the public half, compiled into SECoreBin from
py-tools/keys/ecdsa_public.pem. This tool writes that file from the KMS key
version and prints the fingerprint that goes into the release notes and the
Key Management Procedure.

Needs roles/cloudkms.publicKeyViewer on the key (or key ring) and Application
Default Credentials:  gcloud auth application-default login

Usage
-----
    # Write py-tools/keys/ecdsa_public.pem from KMS and print the fingerprint
    python py-tools/export_public_key.py \\
        --kms-key projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/console-fw-signing/cryptoKeyVersions/1

    # Only check that the committed PEM matches the KMS key (exit 1 if not)
    python py-tools/export_public_key.py --kms-key ... --check

Changing the committed public key is a coordinated bootloader release: every
unit must receive the new bootloader before firmware signed with the new key
will boot on it.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
except ImportError:
    sys.exit("ERROR: 'cryptography' package not found. Run: pip install -r py-tools/requirements.txt")

try:
    from google.cloud import kms  # type: ignore
except ImportError:
    sys.exit("ERROR: 'google-cloud-kms' package not found. Run: pip install -r py-tools/requirements.txt")

DEFAULT_OUT = Path(__file__).parent / "keys" / "ecdsa_public.pem"


def spki_fingerprint(pem: bytes) -> str:
    pub = serialization.load_pem_public_key(pem)
    der = pub.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return hashlib.sha256(der).hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser(description="Export the signing public key from Google Cloud KMS.")
    ap.add_argument("--kms-key", required=True,
                    help="Crypto key VERSION resource name (.../cryptoKeys/<key>/cryptoKeyVersions/<n>).")
    ap.add_argument("--output", type=Path, default=DEFAULT_OUT,
                    help=f"Where to write the PEM (default: {DEFAULT_OUT}).")
    ap.add_argument("--check", action="store_true",
                    help="Do not write; exit 1 if --output differs from the KMS public key.")
    args = ap.parse_args()

    if "/cryptoKeyVersions/" not in args.kms_key:
        ap.error("--kms-key must name a key VERSION (.../cryptoKeyVersions/<n>)")

    client = kms.KeyManagementServiceClient()
    # GetPublicKey only: roles/cloudkms.publicKeyViewer grants viewPublicKey, not
    # cryptoKeyVersions.get, so GetCryptoKeyVersion would be denied. The response
    # carries algorithm and protection level, and fails for a non-ENABLED version.
    resp = client.get_public_key(request={"name": args.kms_key})
    if resp.algorithm != kms.CryptoKeyVersion.CryptoKeyVersionAlgorithm.EC_SIGN_P256_SHA256:
        sys.exit(f"ERROR: key algorithm is {resp.algorithm.name}, expected EC_SIGN_P256_SHA256")
    pem = resp.pem.encode()

    pub = serialization.load_pem_public_key(pem)
    if not isinstance(pub, ec.EllipticCurvePublicKey) or not isinstance(pub.curve, ec.SECP256R1):
        sys.exit("ERROR: KMS returned a key that is not EC P-256")
    n = pub.public_numbers()

    print(f"[export_public_key] Key version : {args.kms_key}")
    print(f"[export_public_key] Level       : {resp.protection_level.name}")
    print(f"[export_public_key] Fingerprint : sha256:{spki_fingerprint(pem)}  (DER SubjectPublicKeyInfo)")
    print(f"[export_public_key] X           : {n.x.to_bytes(32, 'big').hex()}")
    print(f"[export_public_key] Y           : {n.y.to_bytes(32, 'big').hex()}")

    if args.check:
        if not args.output.is_file():
            sys.exit(f"ERROR: {args.output} does not exist")
        committed = args.output.read_bytes()
        if spki_fingerprint(committed) != spki_fingerprint(pem):
            sys.exit(f"ERROR: {args.output} (sha256:{spki_fingerprint(committed)}) does not match the KMS key")
        print(f"[export_public_key] OK: {args.output} matches the KMS key")
        return

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(pem)
    print(f"[export_public_key] Wrote       : {args.output}")
    print()
    print("Next: rebuild the bootloader (CI regenerates se_key.s from this PEM) and release it.")
    print("      Record the fingerprint above in the release notes and the Key Management Procedure.")


if __name__ == "__main__":
    main()
