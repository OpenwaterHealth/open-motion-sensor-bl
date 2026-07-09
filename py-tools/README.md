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
| dfu-util | ≥ 0.9 | install application via USB DFU (option A) |
| Python | ≥ 3.9 | key generation, signing, pure-Python DFU flasher |
| pyusb | ≥ 1.3 | only for the pure-Python flasher (`flash_firmware.py`) |

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

## 2. Generate keys  *(once per device family)*

```sh
# From repo root, with the .venv active:
python py-tools/generate_keys.py          # add --force to overwrite existing keys
```

What it does:
- Generates an **ECDSA P-256** key pair and a **16-byte AES-128** key.
- Writes to `py-tools/keys/`:
  - `ecdsa_private.pem` — **keep secret, never commit**
  - `ecdsa_public.pem`, `pub_key_x.bin`, `pub_key_y.bin`
  - `aes128.bin` — **keep secret, never commit**
- Rewrites `SECoreBin/Startup/se_key.s` with the public key + AES key embedded
  as ARM MOVW/MOVT instructions (commit this file).

> Regenerating keys invalidates all previously signed firmware. **Rebuild the
> bootloader (step 3)** afterwards so the new public key is embedded in SECoreBin.

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
dfu-util -l        # or:  python py-tools/flash_firmware.py list
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

```sh
python py-tools/sign_firmware.py \
    --firmware    path/to/your_app.bin \
    --private-key py-tools/keys/ecdsa_private.pem \
    --aes-key     py-tools/keys/aes128.bin \
    --version     1 \
    --output      your_app_signed.bin
```

| Option | Default | Description |
|--------|---------|-------------|
| `--firmware` | *(required)* | Raw `.bin` built for `0x08020400` (step 5) |
| `--private-key` | `py-tools/keys/ecdsa_private.pem` | ECDSA P-256 private key |
| `--aes-key` | `py-tools/keys/aes128.bin` | Raw 16-byte AES-128 key |
| `--version` | `1` | 16-bit firmware version (1–65535) for anti-rollback — see [Firmware versioning & anti-rollback](#firmware-versioning--anti-rollback) |
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
ECDSA-signed region). `--version` sets it as a raw integer (1–65535); because it
is signed, it cannot be altered without re-signing.

**Encoding convention — `MMmmpp`.** Encode the release semantic version as a
single integer:

```
FwVersion = major*10000 + minor*100 + patch
```

| Semver | `--version` |
|--------|-------------|
| 1.0.0  | `10000` |
| 1.8.0  | `10800` |
| 1.9.3  | `10903` |
| 2.0.0  | `20000` |

This keeps the integer **monotonic** with semver ordering. Limits: `minor` and
`patch` are each `0–99`, and `major ≤ 6` (the packed value must fit 16 bits,
`≤ 65535`). Pre-release suffixes are **not** encoded — `1.8.0-rc.1`, `1.8.0-dev.2`
and `1.8.0` all map to `10800`.

The release/CI build derives it from the git tag, e.g.:

```sh
VER="${TAG%%-*}"                          # 1.8.0-rc.1 -> 1.8.0
IFS=. read -r MAJ MIN PAT <<< "$VER"
FWVER=$(( MAJ*10000 + MIN*100 + PAT ))     # -> 10800
python py-tools/sign_firmware.py --firmware app.bin --version "$FWVER" --output app_signed.bin
```

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

### Option A — dfu-util

```sh
dfu-util -D your_app_signed.bin -a 0 -s 0x08020000:leave
```
*(The `Error during download get_status` after `:leave` is benign — the device
detaches/resets immediately. `Invalid DFU suffix` is also expected.)*

### Option B — pure-Python flasher (no dfu-util)

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
writes the image to `0x08020000`, and resets — the equivalent of the dfu-util
command above.

> **Windows / pyusb driver:** the "STM32 BOOTLOADER" device must be bound to a
> WinUSB/libusb driver (use [Zadig](https://zadig.akeo.ie/)), or `stm32dfu.py`
> will fall back to the libusb-1.0.dll bundled with STM32CubeProgrammer (see
> `_CUBEPROG_LIBUSB_PATHS` in `stm32dfu.py`).

### Option C — direct ST-Link (no DFU)

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
0x08020000-0x0809FFFF   512K   1-4   APP SLOT 1 (active) read/erase/write
  0x08020000    └ signed header (0x400)
  0x08020400    └ application firmware (execution address)
0x080A0000-0x0811FFFF   512K   5-8   APP SLOT 2 (spare)  read/erase/write
0x08120000-0x081DFFFF   768K   9-14  RESERVED (future)   read/erase/write
0x081E0000-0x081FFFFF   128K   15    USER CONFIG         read-only
0x08200000  End of flash
```

> The bootloader sector and the USER CONFIG sector cannot be erased or written
> over DFU (the bootloader may still *read* user config). SLOT 2 is reserved for
> a future dual-slot / A-B update scheme; the bootloader currently boots SLOT 1.

---

## Key files reference

```
py-tools/
  generate_keys.py     Generate ECC P-256 + AES-128 keys, update se_key.s
  gen_se_key_s.py      Low-level: raw key bytes -> ARM MOVW/MOVT asm
  sign_firmware.py     Sign + format an application image for the active slot
  flash_firmware.py    Pure-Python USB DFU installer (uses stm32dfu.py)
  stm32dfu.py          Pure-Python STM32 DfuSe protocol (pyusb)
  requirements.txt     cryptography, pyusb
  keys/
    ecdsa_private.pem  PRIVATE — never commit
    ecdsa_public.pem, pub_key_x.bin, pub_key_y.bin
    aes128.bin         PRIVATE — never commit

SECoreBin/Startup/
  se_key.s             Auto-generated by generate_keys.py — commit this
```

---

## Quick reference

```sh
# one-time
python py-tools/generate_keys.py
cmake --preset Debug && cmake --build build/Debug --target all -j 10
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "init; reset halt; program build/Debug/openmotion-bl.hex verify; reset run; exit"

# per application build
#   (1) link app at 0x08020400 + VTOR 0x08020400, then build your_app.bin
python py-tools/sign_firmware.py --firmware your_app.bin --version 1 --output your_app_signed.bin
python py-tools/flash_firmware.py your_app_signed.bin        # or: dfu-util -D your_app_signed.bin -a 0 -s 0x08020000:leave
```
