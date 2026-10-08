# openmotion-bl — Secure Boot: Build, Sign & Flash Guide

Target: **STM32H743VIHx**
Crypto scheme: `SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`
Bootloader slot: `0x08000000` (128 KB)
Application slot: `0x08020000` (1920 KB, 15 × 128 KB sectors)
Application **runs from**: `0x08020400` (slot + `SFU_IMG_IMAGE_OFFSET`)

> The bootloader authenticates the application before every boot: the firmware
> header is ECDSA-P256 signed and the firmware body is checked with SHA-256.
> A bare-metal application must be **relinked to `0x08020400`** and **signed**
> before it can be installed.

---

## Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| CMake | ≥ 3.22 | |
| Ninja | any | |
| arm-none-eabi-gcc | 13.3.1 | tested |
| OpenOCD | any | flash bootloader via ST-Link |
| Python | ≥ 3.9 | key generation, signing, USB DFU flasher |
| pyusb | ≥ 1.3 | USB backend for the flasher (`flash_firmware.py`) |

> Application images are installed with the repo's own `flash_firmware.py`.
> Neither `dfu-util` nor the STM32CubeProgrammer CLI is used or required.

---

## 1. Python environment

Run once from the repository root:

```sh
cd py-tools
python -m venv .venv

# Windows
.venv\Scripts\activate
# Linux / macOS
source .venv/bin/activate

pip install -r requirements.txt   # cryptography, pyusb
```

---

## 2. Keys

### Production: Google Cloud KMS

The production signing key for each product is an ECDSA P-256 key in Google
Cloud KMS (project `openwater-cloud`, key ring `openmotion-firmware`,
`us-central1`, HSM protection level, non-exportable). Nobody holds the private
key; CI signs through Workload Identity Federation. Only the public half is
needed here, committed as `py-tools/keys/ecdsa_public.pem`:

```sh
# needs roles/cloudkms.publicKeyViewer and:  gcloud auth application-default login
python py-tools/export_public_key.py \
    --kms-key projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/<product>-fw-signing/cryptoKeyVersions/<n>
# or only confirm that the committed PEM matches the KMS key:
python py-tools/export_public_key.py --kms-key ... --check
```

Replacing the committed public key is a coordinated bootloader release: every
unit must receive the new bootloader before firmware signed with the new key
boots on it. Setup, rotation and audit: Open-Motion workspace
`RUNBOOK-kms-signing-setup.md`.

### Local test keys (Debug / bench only)

```sh
python py-tools/generate_keys.py          # add --force to overwrite existing keys
```

- Generates an **ECDSA P-256** key pair and a **16-byte AES-128** key into
  `py-tools/keys/`: `ecdsa_private.pem` and `aes128.bin` (**never commit**),
  `ecdsa_public.pem`, `pub_key_x.bin`, `pub_key_y.bin`.
- Rewrites `SECoreBin/Startup/se_key.s` (`.gitignore`d) with the public key and
  AES key embedded as ARM MOVW/MOVT instructions.

A bootloader built from local test keys accepts only images signed with that
test private key. **Do not commit a test `ecdsa_public.pem`**: CI builds
SECoreBin from the committed PEM, and a test key there ships a bootloader that
rejects every production image.

### AES key

SECoreBin embeds an AES-128 key because the selected crypto scheme
(`SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`) defines one, but slot images are
stored in clear and authenticated by signature, so the AES key protects nothing
and is not needed to sign. CI takes it from the `SECOREBIN_AES_KEY` secret.

---

## 3. Build the SECoreBin + bootloader

Two-step CMake build: **SECoreBin** (the Secure Engine binary) is compiled first
and embedded into the SBSFU bootloader via `.incbin`. The preset handles both.

```sh
# Configure (Debug = SBSFU UART traces on UART4 @ 115200; Release = quiet)
cmake --preset Debug

# Build (SECoreBin then openmotion-bl)
cmake --build build/Debug --target all -j 10
```

| Output | Description |
|--------|-------------|
| `build/Debug/openmotion-bl.hex` | Bootloader (SBSFU + embedded SECoreBin) — flash this |
| `build/Debug/openmotion-bl.bin` | Raw binary |
| `build/Debug/SECoreBin/SECoreBin.bin` | SE Core binary (embedded automatically) |

---

## 4. Flash the bootloader  *(ST-Link)*

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program build/Debug/openmotion-bl.hex verify; reset run; exit"
```

This writes the bootloader to `0x08000000`. With an empty application slot the
bootloader boots, finds no valid firmware, and **enters USB DFU download mode**
(LED blinks, `0483:df11` enumerates). Confirm:

```sh
python py-tools/flash_firmware.py list
```

UART4 (PD1 TX / PD0 RX, 115200 8N1) shows:

```
= [SBOOT] STATE: CHECK USER FW STATUS
	  No valid FW found - entering USB DFU download mode
```

---

## 5. Prepare a bare-metal application for secure boot

A normal CubeMX/bare-metal app links to `0x08000000` and boots directly. To run
under the bootloader it must live in the active slot **at `0x08020400`** (the
slot starts at `0x08020000`; the first `0x400` bytes hold the signed header).
Two edits:

**5a. Linker script** — set the FLASH region origin/length:

```ld
/* STM32H743XX_FLASH.ld */
FLASH (rx) : ORIGIN = 0x08020400, LENGTH = 511K
```
*(App slot 1 is 512K at 0x08020000; the app runs at 0x08020400, so the usable
length is 512K − 0x400 = 511K. See Core/Inc/memory_map.h.)*

**5b. Vector table relocation (VTOR)** — in `Core/Src/system_stm32h7xx.c`:

```c
#define USER_VECT_TAB_ADDRESS                  /* uncomment to enable relocation */
...
#define VECT_TAB_BASE_ADDRESS   0x08020400U    /* was FLASH_BANK1_BASE */
```

Then build the application normally to produce `your_app.bin`. Verify the vector
table landed correctly:

```sh
arm-none-eabi-objdump -h build/Debug/your_app.elf | grep isr_vector
#   0 .isr_vector  ...  08020400  08020400  ...
```

> Nothing else is required — clocks, peripherals, UART, etc. are configured by
> the application as usual. The bootloader hands off with interrupts enabled,
> exactly like a normal reset.

---

## 6. Sign the application

Production (CI, Google Cloud KMS; needs `roles/cloudkms.signerVerifier`):

```sh
python py-tools/sign_firmware.py \
    --firmware path/to/your_app.bin \
    --kms-key  projects/openwater-cloud/locations/us-central1/keyRings/openmotion-firmware/cryptoKeys/<product>-fw-signing/cryptoKeyVersions/<n> \
    --version  1.0.0 \
    --output   your_app_signed.bin
```

Bench (Debug bootloader built from local test keys):

```sh
python py-tools/sign_firmware.py \
    --firmware    path/to/your_app.bin \
    --private-key py-tools/keys/ecdsa_private.pem \
    --version     1.0.0 \
    --output      your_app_signed.bin
```

| Option | Default | Description |
|--------|---------|-------------|
| `--firmware` | *(required)* | Raw `.bin` built for `0x08020400` (step 5) |
| `--kms-key` | — | Google Cloud KMS crypto key **version** resource name; uses Application Default Credentials. Mutually exclusive with `--private-key` |
| `--private-key` | `py-tools/keys/ecdsa_private.pem` | Local ECDSA P-256 private key (test keys only) |
| `--aes-key` | — | Optional and unused for signing (the body is stored in clear); accepted for compatibility |
| `--version` | `0.0.1` | Firmware version, dotted semver `MAJOR.MINOR.PATCH`, encoded as `major*10000 + minor*100 + patch` (a raw uint16 is also accepted) — see [Firmware versioning & anti-rollback](#firmware-versioning--anti-rollback) |
| `--output` | `<firmware>_signed.bin` | Output path |

### Signed-image layout (what gets flashed to the slot)

```
Offset 0x000 [320 B]  Header  (SFU1 magic, version, sizes, SHA-256 FW tag,
                               IV, 64-B ECDSA signature, image-state, fingerprint)
Offset 0x140 [704 B]  0xFF padding  (header region padded up to SFU_IMG_IMAGE_OFFSET)
Offset 0x400 [FwSize] Firmware body  (CLEAR — see note)
```

> **Note (single-slot / NO_LOADER):** the active slot stores the firmware **in
> clear**. The bootloader's boot-time check is SHA-256 only (it does not decrypt);
> AES-CBC decryption belongs to the OTA install path, which is not used here. The
> header is still ECDSA-signed, so the image is authenticated. `sign_firmware.py`
> emits the clear body at offset `0x400` automatically.

### Firmware versioning & anti-rollback

The signed header carries a **16-bit `FwVersion`** (offset `0x006`, inside the
ECDSA-signed region). `--version` sets it; because it is signed, it cannot be
altered without re-signing.

**Encoding — decimal (`FwVersion = major*10000 + minor*100 + patch`).**
`sign_firmware.py` encodes a dotted `MAJOR.MINOR.PATCH` argument as a plain
decimal number, the same scheme the firmware release workflows use:

| Semver | `--version` | FwVersion |
|--------|-------------|-----------|
| 0.0.1  | `0.0.1` | 1 |
| 1.0.0  | `1.0.0` | 10000 |
| 1.8.1  | `1.8.1` | 10801 (`0x2A31`) |
| 1.8.2  | `1.8.2` | 10802 (`0x2A32`) |
| 6.55.35 | `6.55.35` | 65535 (`0xFFFF`, the maximum) |

`minor` and `patch` are limited to 0–99, so the integer orders exactly like
`(major, minor, patch)` and the bootloader's anti-rollback compare is a plain
unsigned `<`. The 16-bit field caps the range at `6.55.35`. **`0.0.0` is
invalid** (encodes to `0`, which SBSFU reserves for "no firmware"); the minimum
valid release is `0.0.1`. Pre-release suffixes are **not** encoded —
`1.8.2-rc.1`, `1.8.2-dev.4` and `1.8.2` all map to `10802`.

> **Do not change this encoding.** Fielded units store their anti-rollback floor
> in it; an image signed under any other scheme compares below the floor and is
> refused as a downgrade (the bootloader then erases it). Changing it is a
> coordinated image-format change (see rule 8 in the repository `CLAUDE.md`).

The release/CI build passes the git tag directly:

```sh
VER="${TAG%%-*}"                          # 1.8.2-rc.1 -> 1.8.2
python py-tools/sign_firmware.py --firmware app.bin --version "$VER" --output app_signed.bin
```

> A raw integer (decimal or `0x`-prefixed hex) in `1..65535` is also accepted
> by `--version` for advanced use.
**Anti-rollback (downgrade protection).** The bootloader keeps a persistent,
monotonic **version floor** — the highest `FwVersion` it has ever launched —
stored in a flash sector that the DFU update path cannot erase. After verifying
an image's signature, it compares the (now-trusted) `FwVersion` to the floor:

- `FwVersion ≥ floor` → launch the app, and raise the floor to this version.
- `FwVersion < floor` → **reject**: the image is invalidated so it can never boot
  (even after a power cycle), and the device drops to USB DFU:

  ```
  = [SBOOT] Anti-rollback: rejected older firmware version
  ```

Practical effect: you may re-flash the **same** version or install a **higher**
one, but never a lower one. To recover from a bad release, ship a build whose
version is **≥** the current floor (bump the patch if needed). The floor resets
only on a full-chip erase via debugger, which production RDP locks out.

> This is the **application** firmware version. It is distinct from the
> **bootloader's own** version string (git `describe`) shown on the boot banner
> and read back with `flash_firmware.py version` — see
> [Bootloader version](#bootloader-version).

---

## 7. Install the application firmware

The bootloader must be in **USB DFU mode** (empty/invalid slot — see step 4; to
re-enter DFU on a programmed device, erase the slot header and reset, or just
flash a new image which replaces the old one). The image is written to
`0x08020000`; the device then resets, verifies the signature, and boots the app.

### Check the image first

There is one application slot, so a download erases the installed firmware
before the bootloader has verified the new one. An image it rejects at boot
leaves the device in DFU mode with no application until a good image is
flashed. Run the same checks on the file beforehand, while the installed
firmware is still intact:

```sh
python py-tools/verify_firmware.py your_app_signed.bin
python py-tools/verify_firmware.py your_app_signed.bin --min-version 1.8.1   # also refuse a downgrade
```

It needs only the public key (`keys/ecdsa_public.pem` by default; pass
`--public-key` for a bootloader built with local test keys) and checks the
header signature, `FwTag`, size and trailing data. `flash_firmware.py` runs
it automatically before touching the device.

### Install over USB DFU (`flash_firmware.py`)

```sh
python py-tools/flash_firmware.py your_app_signed.bin

# helpers:
python py-tools/flash_firmware.py list                 # list DFU devices
python py-tools/flash_firmware.py read 0x08020000 320  # dump the signed header
python py-tools/flash_firmware.py version              # read the bootloader version
python py-tools/flash_firmware.py leave                # reset device into the app
```

`flash_firmware.py` uses `stm32dfu.py` (pure-Python DfuSe over pyusb). It
auto-detects the device's DFU transfer size, erases the affected slot sector(s),
writes the image to `0x08020000`, and resets.

Before erasing anything it verifies the image (see "Check the image first") and
reads the installed version from the slot header, and stops if the image would
be rejected or is a downgrade. `--allow-unverified` skips this for bench
negative-path tests only; the bootloader's own verification is unaffected.

> **Windows / pyusb driver:** the "STM32 BOOTLOADER" device must be bound to a
> WinUSB/libusb driver (use [Zadig](https://zadig.akeo.ie/)), or `stm32dfu.py`
> will fall back to the libusb-1.0.dll bundled with STM32CubeProgrammer (see
> `_CUBEPROG_LIBUSB_PATHS` in `stm32dfu.py`). Only the DLL is borrowed; the
> CubeProgrammer CLI itself is not used.

> The bootloader speaks standard DfuSe (AN3156), so generic tools such as
> `dfu-util` can talk to it, but they are not part of this workflow: they skip
> the host-side image check above, so a bad image erases the installed
> firmware before it is refused at boot.

### Alternative — direct ST-Link (no DFU)

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program your_app_signed.bin 0x08020000 verify; reset run; exit"
```

### Success (UART4 @ 115200)

```
= [SBOOT] STATE: VERIFY USER FW SIGNATURE
= [SBOOT] STATE: EXECUTE USER FIRMWARE
<your application output>
```

### Bootloader version

The **bootloader** has its own version string — the git `describe` of the
`openmotion-bl` repo, generated into `version.h` by CMake at configure time
(`FW_VERSION`; e.g. `1.4.0` for a tagged build, or `fd1546a-dirty` for an
untagged/dirty tree). It is independent of the application `FwVersion` above.

It is reported in two places:

- **Boot banner** (UART4 @ 115200), on every boot:

  ```
  = [SBOOT] Bootloader version: 1.4.0
  ```

- **Over DFU** (no UART needed), via a read-only query:

  ```sh
  python py-tools/flash_firmware.py version    # alias: dfu_ver  ->  1.4.0
  ```

  Internally this UPLOADs from the virtual address `0xFFFFFF00`, which the
  bootloader intercepts to return the string; no flash is read or written.

---

## Flash memory map

Defined in `Core/Inc/memory_map.h` (single source of truth). 2 MB flash, 16 × 128 KB sectors:

```
Addr range              Size   Sct   Region            DFU access
-------------------------------------------------------------------
0x08000000-0x0801FFFF   128K   0     BOOTLOADER         read-only
  0x08000000  ISR vectors
  0x08000400  SE CallGate + SECoreBin
  0x08008A00  SBSFU code
0x08020000-0x0809FFFF   512K   1-4   APP SLOT (active)   read/erase/write
  0x08020000    └ signed header (0x400)
  0x08020400    └ application firmware (execution address)
0x080A0000-0x080BFFFF   128K   5     ANTI-ROLLBACK FLOOR read-only
0x080C0000-0x0819FFFF   896K   6-12  RESERVED (future)   read-only
0x081A0000-0x081DFFFF   256K   13-14 APPLICATION-OWNED   read-only
0x081E0000-0x081FFFFF   128K   15    USER CONFIG         read-only
0x08200000  End of flash
```

> Only the APP SLOT can be erased or written over DFU; everything else is
> read-only through that path (the bootloader may still *read* user config).
> There is no second slot — SBSFU is single-image and dual-slot / A-B updating
> was deliberately removed so all openmotion bootloaders share one layout.
> APPLICATION-OWNED is written by the application, never the bootloader; on the
> sensor it holds the camera FPGA bitstream. See `Core/Inc/memory_map.h`.

---

## Key files reference

```
py-tools/
  generate_keys.py     Generate LOCAL TEST ECC P-256 + AES-128 keys, update se_key.s
  export_public_key.py Fetch the production public key from Google Cloud KMS (or --check it)
  gen_se_key_s.py      Low-level: raw key bytes -> ARM MOVW/MOVT asm
  sign_firmware.py     Sign + format an application image (--kms-key or local --private-key)
  verify_firmware.py   Check a signed image on the host before flashing it
  flash_firmware.py    Pure-Python USB DFU installer (uses stm32dfu.py)
  stm32dfu.py          Pure-Python STM32 DfuSe protocol (pyusb)
  requirements.txt     cryptography, pyusb, google-cloud-kms
  keys/
    ecdsa_private.pem  local TEST key only — PRIVATE, never commit (production key is in KMS)
    ecdsa_public.pem   committed; public half of the KMS production key
    pub_key_x.bin, pub_key_y.bin
    aes128.bin         PRIVATE — never commit

SECoreBin/Startup/
  se_key.s             Generated (generate_keys.py locally, CI from the secret) — .gitignored
```

---

## Quick reference

```sh
# one-time, bench only: LOCAL TEST keys (the production public key comes from KMS, see §2)
python py-tools/generate_keys.py
cmake --preset Debug && cmake --build build/Debug --target all -j 10
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program build/Debug/openmotion-bl.hex verify; reset run; exit"

# per application build
#   (1) link app at 0x08020400 + VTOR 0x08020400, then build your_app.bin
python py-tools/sign_firmware.py --firmware your_app.bin --version 1 --output your_app_signed.bin
python py-tools/flash_firmware.py your_app_signed.bin
```
