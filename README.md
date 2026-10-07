# open-motion-sensor-bl

## Disclaimer

CAUTION - Investigational device. Limited by Federal (or United States) law to investigational use. The system described here has not been evaluated by the FDA and is not designed for the treatment or diagnosis of any disease. It is provided AS-IS, with no warranties. User assumes all liability and responsibility for identifying and mitigating risks associated with using this software.

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

- Public key: `py-tools/keys/ecdsa_public.pem` (committed).
  Fingerprint (SHA‑256, first 16 hex): `275e94117a64a528`.
- The ECDSA **private** key and the AES key are kept **out of git** (stored as
  CI secrets / offline). `se_key.s` — which embeds the AES key and public key —
  is generated at build time and is `.gitignore`d.

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
committed public key, then builds and publishes a release. **`SECOREBIN_AES_KEY`
must hold the sensor AES key** — a mismatched secret silently produces a
bootloader that rejects sensor-signed firmware:

```sh
base64 -w0 <sensor aes128.bin>   # -> set as the SECOREBIN_AES_KEY secret
```

## Signing an application

Sign the raw application `.bin` with the sensor keys before installing:

```sh
python py-tools/sign_firmware.py \
    --firmware    motion-sensor-fw.bin \
    --private-key <sensor ecdsa_private.pem> \
    --aes-key     <sensor aes128.bin> \
    --version     <N> \
    --output      motion-sensor-fw_signed.bin
```

The application must be linked to run at **`0x08020400`** (FLASH origin at the
slot + 0x400 header offset, with VTOR relocated there).

`--version` becomes the 16-bit `FwVersion` used by the monotonic anti-rollback
floor: a unit that has booted version *N* refuses any image `< N` until
re-flashed, so keep release versions increasing.

## Flashing

- **Bootloader** (ST-Link / OpenOCD): program `build/Release/openmotion-bl.hex`
  at `0x08000000` (with verify).
- **Signed app** (USB DFU): `dfu-util -a 0 -s 0x08020000 -D motion-sensor-fw_signed.bin`.
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
