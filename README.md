# open-motion-sensor-bl

Secure boot + secure firmware update (SBSFU) bootloader for the **OpenMotion
sensor module** (STM32H743). On reset it verifies the application image in the
active slot against an on-chip public key and launches it only if the signature
and integrity checks pass; if no valid image is present it drops to USB DFU so a
signed image can be installed.

This is the sensor-board sibling of `open-motion-console-bl`: the same
secure-boot core, but a different board bring-up and a **distinct signing key**.

## Overview

- **MCU:** STM32H743, single active application slot.
- **Authentication:** ECDSA P‑256 signature over the SHA‑256 of the image
  metadata, plus full-image SHA‑256 integrity
  (scheme `SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`).
- **Recovery:** USB DFU (OTG_FS). Also supports application-requested DFU (via an
  RTC backup-register magic + reset) and a boot-failure failsafe.
- **Extra protections:** the Secure Engine key RAM is zeroized before control
  leaves the bootloader; a persistent monotonic anti-rollback version floor; and
  DFU UPLOAD/read is bounded to the application slot.

## Flash layout

| Region | Address | Notes |
|---|---|---|
| Bootloader | `0x08000000` (sector 0) | this image |
| Application slot | `0x08020000`–`0x0809FFFF` (512 KB) | DFU-writable; app vectors at `0x08020400` |
| Anti-rollback floor | `0x080A0000` (sector 5) | bootloader-managed; erased/rewritten by this image |
| Reserved | `0x080C0000`–`0x0819FFFF` (896 KB) | unallocated |
| Application-owned | `0x081A0000`–`0x081DFFFF` (256 KB) | sensor camera FPGA bitstream — **never written by the bootloader** |
| User config | `0x081E0000` (sector 15) | application-managed |

Everything outside the application slot is read-only over DFU. Full map with the
reasoning behind each boundary: [`Core/Inc/memory_map.h`](Core/Inc/memory_map.h).

The map is ordered by owner: bootloader-managed flash at the bottom (sectors 0-5,
contiguous with the bootloader), application-managed flash at the top (13-15), and
the unallocated run in between. Anything the bootloader claims in future must be
taken from sector 6 upward, so it grows away from application-owned flash rather
than into it — the top of that band holds the sensor's camera FPGA bitstream.

There is no second slot: SBSFU is single-image (`SFU_NB_MAX_ACTIVE_IMAGE == 1`)
and dual-slot / A-B updating was deliberately removed so that all openmotion
bootloaders share one layout.

> **Note:** this is the one place the sensor and console bootloader flash maps
> intentionally differ. `open-motion-console-bl` keeps its floor at `0x081C0000`,
> which is free on that board (no FPGA bitstream), and moving it would reset the
> stored floor on any already-converted console.

## Sensor-board bring-up (differs from the console board)

The bootloader enumerates over **USB full-speed (OTG_FS)** by keeping the board's
USB mux at its FS default:

- `USB_MUX` (PC12) → **LOW** — routes the USB connector to the full-speed path.
- `USB_RESET` (PA15) → **LOW** — holds the external ULPI high-speed PHY in reset.
  The bootloader uses the STM32 embedded FS PHY; the *application* drives the mux
  to OTG_HS/ULPI for its high-speed classes.
- **Clock:** external HSE crystal → 480 MHz (VOS SCALE0); HSI48 supplies the
  OTG_FS 48 MHz kernel clock.
- **Debug trace:** UART4 on PD0/PD1, 115200 8N1.
- **Status LED:** `ERROR_LED` (PC14). Note: `IND1` (PA3) is a ULPI data pin on
  this board and is deliberately not touched.

## Keys

The bootloader embeds the **sensor** signing public key. Applications must be
signed with the matching sensor private key or they are rejected at boot.

- Public key: `py-tools/keys/ecdsa_public.pem` (committed). It is the public half
  of the Google Cloud KMS key `projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/sensor-fw-signing` (version 1).
  Fingerprint (SHA-256 of the DER SubjectPublicKeyInfo): `55f6ef94ef9c76ad62f798a44b7d945096a83f0a7910600d57a1e8f3b88a8c7b`.
  Confirm with `python py-tools/export_public_key.py --kms-key projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/sensor-fw-signing/cryptoKeyVersions/1 --check`.
- The ECDSA **private** key exists only inside the KMS HSM (non-exportable). It is
  never downloaded, never stored in CI, and not needed to build this bootloader.
  Firmware CI signs through Workload Identity Federation; see the `OpenwaterHealth/OpenWater-KMS` repository, `docs/RUNBOOK-kms-signing-setup.md`.
- The AES-128 key is kept out of git (CI secret `SECOREBIN_AES_KEY`). The crypto
  scheme embeds it in SECoreBin, but slot images are stored in clear and
  authenticated by signature, so it protects nothing. `se_key.s`, which embeds
  the AES key and the public key, is generated at build time and is `.gitignore`d.

The sensor key set is independent of the console key set; do not cross them.

## Building

Requires the Arm GNU toolchain, CMake, and Ninja. `se_key.s` must be generated
from the key material before the first build:

```sh
python py-tools/gen_se_key_s.py \
    --aes-key   <sensor aes128.bin>   \
    --pub-key-x <sensor pub_key_x.bin> \
    --pub-key-y <sensor pub_key_y.bin> \
    --output    SECoreBin/Startup/se_key.s

cmake --preset Release
cmake --build build/Release
```

Outputs: `build/Release/openmotion-bl.{elf,hex,bin}`.

### CI

`.github/workflows/build-firmware.yml` regenerates `se_key.s` from the
`SECOREBIN_AES_KEY` repository secret (base64 of the 16 raw AES bytes) plus the
committed public key, builds, and on a tag:

- uploads `openmotion-bl.{bin,hex,elf}` and `SHA256SUMS` to the private bucket
  `gs://openwater-firmware-artifacts/<repo>/<tag>/` through Workload Identity
  Federation (no stored Google credential; accepted for tag refs only);
- creates a GitHub Release carrying the notes (build SHA, trusted-key fingerprint,
  binary SHA-256, bucket path) and the SBOM. **Binaries are not release assets.**

Release-config builds exist only in the bucket. Debug builds (branches, `*-dev.*`
tags) are also kept as workflow artifacts for developers. The AES secret:

```sh
base64 -w0 <sensor aes128.bin>   # -> set as the SECOREBIN_AES_KEY secret
```

## Signing an application

Production images are signed in CI with the sensor key in Google Cloud KMS; no
private key is available locally. For bench work against a Debug bootloader
built from a local **test** key pair (`py-tools/generate_keys.py`):

```sh
python py-tools/sign_firmware.py \
    --firmware    motion-sensor-fw.bin \
    --private-key py-tools/keys/ecdsa_private.pem \
    --version     <MAJOR.MINOR.PATCH> \
    --output      motion-sensor-fw_signed.bin
python py-tools/verify_firmware.py motion-sensor-fw_signed.bin
```

With KMS signing rights (`roles/cloudkms.signerVerifier`; normally CI only):

```sh
python py-tools/sign_firmware.py --firmware motion-sensor-fw.bin \
    --kms-key projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/sensor-fw-signing/cryptoKeyVersions/1 \
    --version <MAJOR.MINOR.PATCH> --output motion-sensor-fw_signed.bin
```

The application must be linked to run at **`0x08020400`** (FLASH origin at the
slot + 0x400 header offset, with VTOR relocated there).

`--version` is a dotted semver encoded as `major*10000 + minor*100 + patch` into
the signed header's 16-bit `FwVersion` (minor and patch 0–99, maximum 6.55.35,
`0.0.0` invalid). The monotonic anti-rollback floor compares this value: a unit
that has booted version *N* refuses any image `< N` until re-flashed, so keep
release versions increasing. See `py-tools/README.md` §"Firmware versioning &
anti-rollback" for the full encoding table.

## Flashing

- **Bootloader** (ST-Link / OpenOCD): program `build/Release/openmotion-bl.hex`
  at `0x08000000` (with verify).
- **Signed app** (USB DFU): `python py-tools/flash_firmware.py motion-sensor-fw_signed.bin`
  (verifies the image on the host, then writes it to `0x08020000`; `dfu-util`
  and the STM32CubeProgrammer CLI are not used).
- **Production image:** bootloader + signed app merged into a single image and
  flashed at `0x08000000`.

## Security configuration status

The protections are selected by the CMake preset (`SBSFU_ENABLE_PROTECTIONS`,
see `CMakeLists.txt` and the security block in `SBSFU/App/Inc/app_sfu.h`):

| Preset | Protections | Debug probe |
|---|---|---|
| `Debug` | Development mode (`SECBOOT_DISABLE_SECURITY_IPS`): option bytes untouched | Usable |
| `Release` | Applied by the bootloader at its first boot: **WRP** on the bootloader sector, **RDP level 1**, **PCROP** on the Secure Engine key region, **DAP** lock (SWD pins become inputs), **DMA** protection | Not usable; RDP level 1 is reversed only by a mass erase |

Firmware signature verification, the SE key-RAM wipe, the DFU read bounds and
the pre-erase header check are active in both presets. **RDP level 2**
(`SFU_FINAL_SECURE_LOCK_ENABLE`) is the production end state but is not yet
enabled anywhere: it is permanent on the part, so it is a deliberate step
recorded under tracker T2, not a build option.

> **Caution:** a `Release` build programs the option bytes on the first boot of
> whatever board it is flashed to. Keep a full flash backup (including the
> user-config sector) of a bench unit before flashing it, and power-cycle with
> the probe disconnected afterwards — at RDP level 1 the core cannot execute
> from flash while a debugger is attached. The protected build has been
> bench-tested on the console only; the sensor needs its own pass.
