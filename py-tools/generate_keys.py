#!/usr/bin/env python3
"""
generate_keys.py  —  Generate a fresh ECC P-256 key pair + AES-128 key for SBSFU.

Outputs (in py-tools/keys/):
  ecdsa_private.pem   — ECDSA private key (keep SECRET, never commit)
  ecdsa_public.pem    — ECDSA public key  (can be shared / committed)
  pub_key_x.bin       — raw 32-byte P-256 X coordinate (big-endian)
  pub_key_y.bin       — raw 32-byte P-256 Y coordinate (big-endian)
  aes128.bin          — raw 16-byte AES-128 symmetric key

After generating keys this script automatically calls gen_se_key_s.py to
rewrite  SECoreBin/Startup/se_key.s.  You must then rebuild SECoreBin and
the full bootloader binary so the new public key is embedded.

Usage
-----
  python generate_keys.py [--force]

  --force   Overwrite existing keys.  Without this flag the script refuses to
            overwrite existing key material to prevent accidental key loss.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import serialization
except ImportError:
    sys.exit(
        "ERROR: 'cryptography' package not found.\n"
        "Activate the venv and run:  pip install -r requirements.txt"
    )

# Locate repository root (two levels up from py-tools/)
REPO_ROOT  = Path(__file__).resolve().parent.parent
KEYS_DIR   = Path(__file__).resolve().parent / "keys"
SE_KEY_S   = REPO_ROOT / "SECoreBin" / "Startup" / "se_key.s"


def generate(force: bool = False) -> None:
    KEYS_DIR.mkdir(parents=True, exist_ok=True)

    priv_pem  = KEYS_DIR / "ecdsa_private.pem"
    pub_pem   = KEYS_DIR / "ecdsa_public.pem"
    pub_x_bin = KEYS_DIR / "pub_key_x.bin"
    pub_y_bin = KEYS_DIR / "pub_key_y.bin"
    aes_bin   = KEYS_DIR / "aes128.bin"

    # Guard against accidental overwrite
    existing = [p for p in [priv_pem, aes_bin] if p.exists()]
    if existing and not force:
        sys.exit(
            f"ERROR: Key files already exist: {[str(p) for p in existing]}\n"
            "Use --force to overwrite (this invalidates all previously signed firmware)."
        )

    # -----------------------------------------------------------------------
    # 1. Generate ECC P-256 key pair
    # -----------------------------------------------------------------------
    private_key = ec.generate_private_key(ec.SECP256R1())
    public_key  = private_key.public_key()
    pub_numbers = public_key.public_numbers()

    x_bytes = pub_numbers.x.to_bytes(32, "big")
    y_bytes = pub_numbers.y.to_bytes(32, "big")

    priv_pem.write_bytes(
        private_key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    pub_pem.write_bytes(
        public_key.public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        )
    )
    pub_x_bin.write_bytes(x_bytes)
    pub_y_bin.write_bytes(y_bytes)

    print(f"[generate_keys] ECC P-256 private key : {priv_pem}")
    print(f"[generate_keys] ECC P-256 public key  : {pub_pem}")
    print(f"[generate_keys] Public key X (raw)    : {pub_x_bin}")
    print(f"[generate_keys] Public key Y (raw)    : {pub_y_bin}")

    # -----------------------------------------------------------------------
    # 2. Generate AES-128 key (16 random bytes)
    # -----------------------------------------------------------------------
    aes_key = os.urandom(16)
    aes_bin.write_bytes(aes_key)
    print(f"[generate_keys] AES-128 key            : {aes_bin}")

    # -----------------------------------------------------------------------
    # 3. Regenerate se_key.s with the new keys
    # -----------------------------------------------------------------------
    # Import locally so gen_se_key_s.py can also be used standalone
    sys.path.insert(0, str(Path(__file__).parent))
    from gen_se_key_s import generate_se_key_s

    se_key_s_content = generate_se_key_s(aes_key, x_bytes, y_bytes)
    SE_KEY_S.write_text(se_key_s_content, encoding="ascii")
    print(f"[generate_keys] Updated se_key.s       : {SE_KEY_S}")

    # -----------------------------------------------------------------------
    # 4. Write .gitignore for the keys directory
    # -----------------------------------------------------------------------
    gitignore = KEYS_DIR / ".gitignore"
    if not gitignore.exists():
        gitignore.write_text(
            "# Never commit private key or AES key material\n"
            "ecdsa_private.pem\n"
            "aes128.bin\n"
            "*.bin\n",
            encoding="ascii",
        )

    print()
    print("=" * 60)
    print("Keys generated successfully.")
    print()
    print("NEXT STEPS:")
    print("  1. Rebuild SECoreBin and the full bootloader so the new")
    print("     public key is embedded in the flash image.")
    print("  2. Use sign_firmware.py to sign your application binary.")
    print("  3. Upload the signed binary via USB DFU.")
    print()
    print("WARNING: Keep ecdsa_private.pem and aes128.bin SECRET.")
    print("         Never commit them to version control.")
    print("=" * 60)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate SBSFU ECC + AES keys.")
    parser.add_argument(
        "--force", action="store_true",
        help="Overwrite existing key files.",
    )
    args = parser.parse_args()
    generate(force=args.force)


if __name__ == "__main__":
    main()
