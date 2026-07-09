# Cybersecurity Documentation for Premarket Submission (FDA 510(k))

**Device / Subsystem:** openMOTION Secure Bootloader & Secure Firmware Update (STM32H743 MCU subsystem)
**Manufacturer:** Openwater
**Document type:** Premarket cybersecurity documentation
**Status:** DRAFT — engineering input for 510(k) submission
**Version:** 0.2
**Date:** 2026-06-24

> **Scope & disclaimer.** This document provides the **engineering/technical** cybersecurity
> content that supports a 510(k) premarket submission for the device's embedded
> firmware-security subsystem (secure boot and secure firmware update). It is organized
> per the FDA guidance *Cybersecurity in Medical Devices: Quality System Considerations
> and Content of Premarket Submissions* (Sept 2023) and references **ANSI/AAMI SW96:2023**,
> **AAMI TIR57**, **IEC 81001-5-1**, and **NIST SP 800-30/-218**. It is **not** regulatory or
> legal advice. Device-level regulatory fields (intended use, classification, predicate,
> clinical claims) and the overall **security risk management file** must be completed and
> approved by Openwater Regulatory Affairs and Quality. Placeholders are marked `[[TODO: ...]]`.

---

## Table of contents

1. [Introduction and scope](#1-introduction-and-scope)
2. [Device description (security-relevant)](#2-device-description-security-relevant)
3. [Security risk management](#3-security-risk-management)
4. [Threat model](#4-threat-model)
5. [Security architecture](#5-security-architecture)
6. [Security controls](#6-security-controls)
7. [Software bill of materials (SBOM)](#7-software-bill-of-materials-sbom)
8. [Cybersecurity testing and verification](#8-cybersecurity-testing-and-verification)
9. [Vulnerability and postmarket management](#9-vulnerability-and-postmarket-management)
10. [Security labeling](#10-security-labeling)
11. [Unresolved anomalies](#11-unresolved-anomalies)
12. [Production hardening requirements](#12-production-hardening-requirements-critical)
13. [Appendix A — Security requirements traceability matrix](#appendix-a--security-requirements-traceability-matrix)
14. [Appendix B — Cryptographic details](#appendix-b--cryptographic-details)
15. [Appendix C — Glossary](#appendix-c--glossary)

---

## 1. Introduction and scope

### 1.1 Purpose
This document describes the cybersecurity design, risk management, controls, and verification
for the openMOTION **secure bootloader** and **secure firmware update** subsystem. This subsystem
establishes a hardware **Root of Trust** that ensures only firmware authored and signed by
Openwater can execute on the device, and that firmware updates are authenticated and integrity-checked.

### 1.2 Scope
**In scope:**
- The immutable secure bootloader (SBSFU/SECoreBin) executing on the STM32H743 MCU.
- Cryptographic verification of application firmware (authenticity + integrity).
- The secure firmware update path (USB DFU; signed image installation).
- Flash memory partitioning and access controls (read-only bootloader and user-config regions).
- Key provisioning and management for firmware signing/verification.

**Out of scope (covered in separate documents):**
- The application firmware's own clinical/functional cybersecurity controls. `[[TODO: reference]]`
- Host/companion software, cloud services, and network interfaces. `[[TODO: reference]]`
- Physical/electrical safety (IEC 60601). `[[TODO: reference]]`

### 1.3 Intended use / device context
`[[TODO: Regulatory — insert device intended use, indications, classification (e.g., Class II),
product code, predicate device(s), and whether the device is networked, multi-patient, or
life-sustaining. These drive the cybersecurity risk tier per FDA guidance.]]`

---

## 2. Device description (security-relevant)

### 2.1 Hardware platform
| Item | Detail |
|------|--------|
| MCU | STMicroelectronics STM32H743VIHx (Arm Cortex-M7) |
| Internal flash | 2 MB, dual-bank, 16 × 128 KB sectors |
| Security IP | Secure Engine (SE) call-gate isolation, MPU, Flash WRP/RDP/PCROP, option bytes |
| External interfaces | USB (DFU bootloader interface; application CDC), UART4 debug, I²C (I/O expander), USB hub |

### 2.2 Software components
| Component | Role | Mutability |
|-----------|------|-----------|
| SECoreBin (Secure Engine) | Crypto primitives + key custody inside protected region | Immutable (programmed at manufacture) |
| SBSFU bootloader | Root of Trust; verifies + launches application; secure update | Immutable (programmed at manufacture) |
| Application firmware | Device clinical/functional firmware | Field-updatable (signed) |

> The ST HAL, CMSIS, and USB Device Library components are baselined on the **STM32CubeH7
> firmware package V1.13.0**; the SBSFU/SECoreBin Root of Trust is ported from ST X-CUBE-SBSFU.
> Exact component versions are in the SBOM (§7).

### 2.3 Flash partition map
Defined in `Core/Inc/memory_map.h` (single source of truth):

```
Addr range              Size   Sct   Region              Access control
---------------------------------------------------------------------------
0x08000000-0x0801FFFF   128K   0     BOOTLOADER           read-only (immutable)
0x08020000-0x0809FFFF   512K   1-4   APP SLOT 1 (active)  updatable (signed) — DFU writable window
0x080A0000-0x081BFFFF  1408K   5-13  RESERVED             read-only via DFU
0x081C0000-0x081DFFFF   128K   14    ANTI-ROLLBACK FLOOR  read-only via DFU (bootloader-managed)
0x081E0000-0x081FFFFF   128K   15    USER CONFIG          read-only via DFU (application-managed)
```

The DFU update interface restricts erase/write to the **active application slot only**
(`0x08020000`–`0x0809FFFF`, the SBSFU `SLOT_ACTIVE_1` extent in `Linker/mapping_fwimg.ld`). The DFU
writable window is deliberately clamped to the slot so that **all DFU-writable flash is covered by
secure-boot slot verification** (no writable region escapes the `VerifyActiveSlot` check). The
bootloader sector (0), reserved sectors (5–13), the anti-rollback floor sector (14), and the
user-config sector (15) are **not erasable or writable** through the firmware-update (DFU) interface.
The anti-rollback floor sector is written only by the bootloader (during verified boot, see §5.6); the
user-config sector is owned by the application.

### 2.4 Cryptographic scheme
`SECBOOT_ECCDSA_WITH_AES128_CBC_SHA256`:
- **Authenticity:** ECDSA P-256 signature over the firmware image header (public key in device).
- **Integrity:** SHA-256 digest of the firmware binary, bound into the signed header.
- **Confidentiality (transport):** AES-128-CBC for the firmware payload during distribution.

---

## 3. Security risk management

### 3.1 Process
Security risk management is integrated with the product risk management process (ISO 14971)
and performed per **AAMI TIR57 / ANSI-AAMI SW96** using **NIST SP 800-30** methodology.
Security risks that can lead to patient harm are escalated to the safety risk file.
`[[TODO: Quality — reference the controlled Security Risk Management Plan, file ID, and approvals.]]`

### 3.2 Security objectives
| ID | Objective |
|----|-----------|
| SO-1 | Only firmware authentic to Openwater executes on the device (authenticity). |
| SO-2 | Firmware cannot be silently modified without detection (integrity). |
| SO-3 | Confidential firmware IP is protected in distribution (confidentiality). |
| SO-4 | The Root of Trust (bootloader, keys) is immutable and tamper-resistant. |
| SO-5 | A failed/forged update cannot brick the device or run unauthenticated code (resilience). |
| SO-6 | Field updates are controlled, authenticated, and auditable (updateability). |

### 3.3 Risk acceptance
`[[TODO: define residual-risk acceptance criteria and link to the security risk assessment
worksheet. Each threat in §4 maps to a control in §6 and a verification in §8 — see Appendix A.]]`

---

## 4. Threat model

Methodology: **STRIDE** over the data-flow and trust boundaries of the boot/update subsystem.

### 4.1 Assets
- A1: Application firmware authenticity/integrity (and patient safety that depends on it).
- A2: Firmware confidential IP.
- A3: Cryptographic keys (signing private key off-device; public + symmetric keys on-device).
- A4: Root of Trust (immutable bootloader / SECoreBin).
- A5: Device availability (must boot or safely enter recovery).

### 4.2 Trust boundaries & attack surfaces
| # | Surface | Description |
|---|---------|-------------|
| AS-1 | USB DFU interface | Firmware install endpoint exposed when no valid firmware present (or on request). |
| AS-2 | Physical flash / debug (SWD/JTAG) | Direct read/modify of flash or RAM via debugger. |
| AS-3 | Firmware distribution channel | Path by which signed images reach the device. |
| AS-4 | Application ↔ bootloader handoff | Control transfer and shared state at launch. |
| AS-5 | Signing infrastructure (off-device) | Where the ECDSA private key is held/used. |

### 4.3 STRIDE threats and mitigations
| ID | STRIDE | Threat | Mitigation (control) | Ref |
|----|--------|--------|----------------------|-----|
| T-1 | Tampering | Attacker replaces/patches application firmware in flash. | Secure boot verifies ECDSA header signature + SHA-256 FW tag every boot; rejects on mismatch. | C-1, C-2 |
| T-2 | Spoofing | Attacker installs forged firmware via DFU. | DFU image must carry a valid ECDSA signature over the header; unauthenticated images are not executed. | C-1, C-4 |
| T-3 | Tampering | Attacker modifies the bootloader or keys. | Bootloader + SECoreBin in read-only sector 0; (production) Flash WRP/RDP/PCROP and SE MPU isolation. | C-3, C-7 |
| T-4 | Information disclosure | Firmware IP extracted in transit. | AES-128-CBC encryption of the distributed payload. | C-5 |
| T-5 | Information disclosure | Keys/firmware read out via debug port. | (Production) RDP Level ≥ 1, DAP disable, PCROP on key region; SE call-gate isolation. | C-7 |
| T-6 | Denial of service | Corrupt/partial update bricks the device. | Update is verified before execution; invalid slot → safe recovery (DFU re-entry), never executes bad code. | C-2, C-6 |
| T-7 | Tampering | "Additional code beyond firmware" hidden in slot. | Bootloader verifies the unused slot region is empty before launch. | C-2 |
| T-8 | Elevation of privilege | Application calls protected Secure Engine services illegitimately. | SE call-gate validates caller region; (production) MPU privilege isolation. | C-7 |
| T-9 | Tampering | Rollback to a known-vulnerable signed firmware. | Signed-header firmware version (`FwVersion`) + **boot-time anti-rollback**: the bootloader refuses to launch any image whose verified version is below a persistent monotonic floor, and invalidates the downgrade so it cannot boot (§5.6). **Implemented & verified on STM32H743 target.** | C-8 |
| T-10 | Spoofing | Compromise of the off-device signing key. | HSM/controlled key custody, least-privilege signing, key rotation procedure. | C-9 |

### 4.4 Multi-patient / safety impact
`[[TODO: Risk — assess whether a single exploited unit can affect multiple patients or whether
a vulnerability is reproducible fleet-wide. FDA requires a multi-patient harm view for networked/
fleet devices. The Root of Trust limits fleet compromise to entities possessing the signing key.]]`

---

## 5. Security architecture

### 5.1 Architecture overview (Root of Trust)
On every reset the **immutable secure bootloader** executes first (Root of Trust). It initializes
the Secure Engine, then:
1. Detects an application image in the active slot.
2. Verifies the image **header signature** (ECDSA P-256) using the embedded public key.
3. Verifies the firmware **integrity** (SHA-256) against the value bound in the signed header.
4. Verifies no extraneous code exists beyond the firmware in the slot.
5. Only if all checks pass, transfers control to the application; otherwise it does not execute
   the image and enters the controlled update (DFU) path.

```
        ┌──────────────┐   verify (ECDSA + SHA-256)    ┌──────────────────┐
 RESET ─► Secure Boot   ├──────────────────────────────►  Application FW   │
        │ (Root of Trust)│   pass → launch               │  (active slot)   │
        └──────┬───────┘                                  └──────────────────┘
               │ fail / no valid FW
               ▼
        ┌──────────────┐   authenticated install (signed image only)
        │ USB DFU update │◄──────────────────────────────  Host tool (dfu-util / signed installer)
        └──────────────┘
```

### 5.2 Security architecture views (FDA-required)
- **Global system view:** §2.1–2.3 (hardware, software, partitions).
- **Updateability / patchability view:** §5.4 and §9 (how firmware is updated and patched).
- **Multi-patient harm view:** §4.4 `[[TODO]]`.
- **Security use case views:** boot-time verification (§5.1), field update (§5.4), key provisioning (§6 C-9).

### 5.3 Secure boot (detailed)
- Bootloader and Secure Engine reside in flash **sector 0 (0x08000000)**, marked read-only over the
  update interface and (in production) write-protected by hardware Option Bytes.
- The Secure Engine exposes cryptographic services through a **call-gate** that validates the caller
  originates from the trusted bootloader interface region; in production the SE RAM/ROM is isolated by MPU.
- Verification is **fail-closed**: any signature, hash, or layout check failure prevents execution of
  the candidate image.

### 5.4 Secure firmware update (detailed)
- Update images are produced by the controlled signing tool (`sign_firmware.py`): a 320-byte header
  (magic, version, sizes, SHA-256 FW tag, IV, **ECDSA-P256 signature**, image state, previous-header
  fingerprint) followed by the firmware body at the slot execution offset.
- The device exposes a **USB DFU** interface only when no valid firmware is present (or upon an
  authenticated in-application request — see §9). The DFU interface restricts writes/erases to the
  application slots; the bootloader sector and user-config sector are rejected.
- After download the device resets and re-runs full verification before executing the new image.

### 5.5 Cryptographic design
See Appendix B. Public ECDSA key and symmetric key are embedded in the Secure Engine binary at
manufacture; the **ECDSA private signing key never resides on the device**.

### 5.6 Anti-rollback (downgrade protection)
To prevent an attacker from installing an older, **validly-signed but known-vulnerable** firmware
version (threat T-9), the bootloader enforces a monotonic version floor:

- **Version source.** Each signed image carries a 16-bit `FwVersion` in its ECDSA-signed header,
  encoded from the release semantic version as `major×10000 + minor×100 + patch` (monotonic with
  semver ordering). The value is supplied by the controlled build/CI pipeline at signing time.
- **Persistent floor.** The bootloader stores the highest version ever launched in a dedicated flash
  sector (sector 14, `0x081C0000`) as an append-only log. This sector is **outside the DFU writable
  window**, so a firmware update cannot erase or lower it; it is non-volatile, so the floor survives
  power cycles and SWD reflashes of the application slot.
- **Boot-time enforcement (primary).** Enforcement occurs **after** the secure boot has authenticated
  the header signature and verified the firmware hash — i.e., the `FwVersion` used is
  signature-verified. If `FwVersion < floor`, the bootloader (a) records a version-rejected error,
  (b) **invalidates the image** (erases its header) so it can never execute — even after a power
  cycle, since the boot path performs no version check of its own — and (c) resets into the controlled
  DFU recovery path. If `FwVersion ≥ floor`, the floor is raised to that version and the image launches.
- **DFU-time check (secondary / fast-fail).** During download, the bootloader also compares the
  incoming image's version against the currently-installed version and rejects an obvious downgrade
  before committing, giving the operator immediate feedback. The boot-time floor remains the
  authoritative control.
- **Residual risk.** The floor can only be cleared by erasing sector 14, which requires debug/SWD
  access. Production units lock the debug port (RDP, §12), so the floor cannot be reset in the field.

This control is **fail-safe**: a flash-write failure leaves the floor unchanged (never lowered), and a
rejected downgrade results in safe DFU recovery rather than execution of vulnerable code.

---

## 6. Security controls

| ID | Control | Implements | Verification |
|----|---------|-----------|--------------|
| C-1 | ECDSA P-256 authentication of firmware header | SO-1, T-1/T-2 | §8 TC-AUTH-* |
| C-2 | SHA-256 integrity + empty-slot check | SO-2, T-1/T-6/T-7 | §8 TC-INTEG-* |
| C-3 | Immutable bootloader/SE in read-only sector 0 | SO-4, T-3 | §8 TC-IMMUT-* |
| C-4 | DFU restricted to app slots; signed-only install | SO-6, T-2 | §8 TC-DFU-* |
| C-5 | AES-128-CBC transport confidentiality | SO-3, T-4 | §8 TC-CONF-* |
| C-6 | Fail-closed verification; safe recovery on invalid FW | SO-5, T-6 | §8 TC-FAILCLOSED-* |
| C-7 | Hardware protections: RDP, WRP, PCROP, DAP, MPU/SE isolation | SO-4, T-3/T-5/T-8 | §8 TC-HWPROT-* / §12 |
| C-8 | Signed `FwVersion` + boot-time monotonic anti-rollback floor (§5.6) | SO-5, T-9 | §8 TC-ROLLBACK-* |
| C-9 | Off-device key custody (HSM), provisioning, rotation | A3, T-10 | §8 TC-KEY-* `[[TODO procedure]]` |
| C-10 | Protected user-config sector (no DFU erase/write) | data integrity | §8 TC-USERCFG-* |

---

## 7. Software bill of materials (SBOM)

The SBOM must be delivered in a machine-readable format (**SPDX** or **CycloneDX**) per FDA guidance.
The table below is the human-readable summary; `[[TODO: generate the machine-readable SBOM in CI and
attach it; include exact versions, suppliers, licenses, and known-vulnerability status (e.g., via NVD).]]`

| Component | Supplier | Version | Type | License | Notes |
|-----------|----------|---------|------|---------|-------|
| STM32 Secure Engine (SBSFU/SECoreBin) | STMicroelectronics (X-CUBE-SBSFU) | 2.8.0 (ported) | Middleware | ST SLA | Root of Trust |
| STM32 Cryptographic Library | STMicroelectronics | V3.1.1 (STM32H7) | Crypto lib | ST SLA | AES/ECDSA/SHA-256 |
| STM32H7 HAL / CMSIS | STMicroelectronics | STM32CubeH7 V1.13.0 (CMSIS Core v5.x) | HAL/driver | BSD-3 / Apache-2.0 | Sourced from STM32CubeH7 firmware package V1.13.0 |
| STM32 USB Device Library (DFU/CDC) | STMicroelectronics | STM32CubeH7 V1.13.0 | Middleware | ST SLA | Update + app comms; from STM32CubeH7 V1.13.0 |
| arm-none-eabi-gcc | Arm | 13.3.1 | Toolchain | GPL (toolchain) | Build only |
| Python `cryptography` | PyCA | ≥ 41.0 | Build/sign tool | Apache-2.0/BSD | Off-device signing |
| Python `pyusb` + libusb | PyUSB / libusb | 1.3.x / 1.0 | Build/host tool | BSD / LGPL-2.1 | Off-device flasher |

> Off-device build/sign/flash tools are listed because they form part of the secure-update **supply
> chain** even though they do not run on the device.

---

## 8. Cybersecurity testing and verification

FDA expects evidence across four areas: security-requirements testing, threat-mitigation testing,
vulnerability testing, and penetration testing.

### 8.1 Security requirements / threat-mitigation testing

> The bench-testable cases below were re-verified end-to-end against a clean build of the
> **`main` branch** on the STM32H743 target (2026-06-24): TC-AUTH-01, TC-AUTH-03, TC-INTEG-01/02,
> TC-DFU-01/02/03, TC-USERCFG-01, TC-FAILCLOSED-01, TC-IMMUT-01, and TC-ROLLBACK-01..04 — all Pass.
> Remaining `[[TODO]]` rows require the production hardware-protection build (§12) or process/key
> evidence.

| Test ID | Objective | Method | Result |
|---------|-----------|--------|--------|
| TC-AUTH-01 | Valid signed image boots | Sign with production key, install, observe execution | **Pass** (verified on STM32H743 target) |
| TC-AUTH-02 | Image with tampered body is rejected | Flip bytes after signing; attempt boot | **Pass** (signature/hash mismatch → not executed) `[[TODO: capture log]]` |
| TC-AUTH-03 | Image signed with wrong key is rejected | Sign with a freshly-generated (non-provisioned) ECDSA P-256 key; install via DFU | **Pass** (STM32H743): rejected at `VERIFY USER FW SIGNATURE`; not executed; enters DFU |
| TC-INTEG-01 | SHA-256 mismatch rejected | Valid signed header, flip one body byte (offset 0x800) without re-signing; install via DFU | **Pass**: integrity check fails at `VERIFY USER FW SIGNATURE`; not executed; enters DFU |
| TC-INTEG-02 | Extra code beyond firmware rejected | Boot a valid image, then write 0xAA bytes within the active slot beyond the firmware (0x08080000) via SWD; reset | **Pass**: `VerifyActiveSlot` detects extraneous slot content; rejected at `VERIFY USER FW SIGNATURE`; not executed; enters DFU. (Note: writes *outside* the slot end 0x0809FFFF are not part of the verified image — see §11 anomaly.) |
| TC-DFU-01 | DFU install of signed image succeeds | dfu-util + pure-Python flasher | **Pass** (both paths) |
| TC-DFU-02 | DFU write to bootloader sector rejected | `dfu-util` download targeting 0x08000000 | **Pass** (STM32H743): device rejects — `Last page at 0x0800001f is not writeable`; bootloader sector unmodified |
| TC-DFU-03 | DFU write outside the active slot rejected (AN-1 fix) | `dfu-util` download targeting 0x080A0000 (first address above the slot) | **Pass** (STM32H743): device rejects — `Last page at 0x080a001f is not writeable` (also verified at floor sector 0x081C0000 and user-config 0x081E0000) |
| TC-USERCFG-01 | DFU write/erase of user-config rejected | Target 0x081E0000 | **Pass** ("Last page … not writeable") |
| TC-FAILCLOSED-01 | No/invalid FW → safe recovery, no code exec | Empty slot | **Pass** (enters DFU; no unauthenticated execution) |
| TC-IMMUT-01 | Bootloader not modifiable via update path | SHA-256 of bootloader sector 0 (0x08000000, 128 KB) before/after a DFU write attempt to 0x08000000 | **Pass** (STM32H743): write rejected (`Last page … not writeable`); sector-0 hash identical before and after (`65b0bf97…256edd6`) — bootloader unchanged |
| TC-HWPROT-01 | RDP/WRP/PCROP/DAP effective | Read/modify attempts via SWD | `[[TODO: production build]]` |
| TC-ROLLBACK-01 | Older version rejected (boot-time floor) | Boot v1.8.0 (floor→10800); flash v1.7.0 (10700) to slot via SWD (floor sector untouched); reset | **Pass** (STM32H743): `ANTI-ROLLBACK: FW version 10700 below floor 10800 - launch refused`; image invalidated; enters DFU; not executed |
| TC-ROLLBACK-02 | Equal/higher version accepted, floor raised | Flash v1.9.0 (10900); reset | **Pass** (boots; floor raised to 10900) |
| TC-ROLLBACK-03 | Floor persists across app-slot reflash / SWD | Reflash app slot only (not floor sector); verify floor retained | **Pass** (floor 10800 read back on the TC-ROLLBACK-01 downgrade boot) |
| TC-ROLLBACK-04 | Floor sector not erasable via DFU | Target 0x081C0000 over DFU | **Pass** (outside DFU writable window; erase/write rejected) |

### 8.2 Vulnerability testing
`[[TODO: known-vulnerability scan of SBOM components against NVD/ICS-CERT; static analysis
(e.g., MISRA/compiler -Wall clean — current build is warning-clean except intentional ST
dev-mode protection reminders); fuzzing of the DFU/header parser.]]`

### 8.3 Penetration testing
`[[TODO: independent penetration test of the boot/update subsystem — fault injection, debug-port
attacks, signature-bypass attempts, update-path abuse. Provide report and remediation.]]`

---

## 9. Vulnerability and postmarket management

`[[TODO: Quality/Security — reference the controlled Cybersecurity Management Plan. Include:]]`
- **Monitoring:** subscribe to ST PSIRT, NVD, and component advisories for SBOM items.
- **Coordinated disclosure:** intake channel and SLA for externally reported vulnerabilities.
- **Patch/update mechanism:** the secure firmware update path (§5.4) is the supported remediation
  channel; signed updates can be deployed in the field via DFU.
- **Update authenticity in the field:** in-application DFU re-entry must itself be access-controlled
  so only authorized operators can place the device into update mode. `[[TODO: define trigger &
  authorization — e.g., authenticated host command + signed update.]]`
- **Patch cadence / end-of-support:** `[[TODO]]`.

---

## 10. Security labeling

Per FDA labeling expectations, provide to users/operators: `[[TODO: Regulatory to finalize]]`
- A description of the device's cybersecurity controls (secure boot, signed updates).
- Instructions for performing secure firmware updates and verifying success.
- A statement that only Openwater-signed firmware will run on the device.
- Guidance on physical security of the device and its update/host environment.
- A point of contact for reporting suspected vulnerabilities.
- SBOM availability to operators on request.

---

## 11. Unresolved anomalies

`[[TODO: link each item to the controlled defect system and complete the risk assessment.]]`

| ID | Anomaly | Risk assessment | Disposition |
|----|---------|-----------------|-------------|
| AN-1 | The DFU writable window (`0x08020000`–`0x081BFFFF`) was larger than the verified active slot (`0x08020000`–`0x0809FFFF`, `mapping_fwimg.ld`), so DFU could write flash *outside* the slot. | **Low** (never a verification bypass; out-of-slot content is not authenticated/executed). | **RESOLVED.** The DFU writable window is clamped to the active-slot end (`FLASH_END_ADDR = 0x080A0000`, DfuSe descriptor `01*128Ka,04*128Kg,11*128Ka`); writes outside the slot are now rejected (verified: TC-DFU-03). All DFU-writable flash is covered by slot verification. |

---

## 12. Production hardening requirements (CRITICAL)

> The development configuration disables the STM32 hardware security IP for debuggability
> (`SECBOOT_DISABLE_SECURITY_IPS`), which the build surfaces as intentional `#warning
> "SFU_*_PROTECT_DISABLED"` reminders. **A device shipped for clinical use MUST be built and
> provisioned with these protections ENABLED.** This is a release gate.

| Protection | Dev state | Production requirement |
|------------|-----------|------------------------|
| RDP (readout protection) | Off | **RDP Level ≥ 1** (Level 2 disables debug permanently — irreversible) |
| WRP (write protection) | Off | **WRP on bootloader + SE sectors** (immutability) |
| PCROP (code readout protection) | Off | **PCROP on key/SE region** |
| DAP / debug access | Open | **Disabled** in production |
| MPU / SE isolation | Off | **Enabled** (`SFU_MPU_PROTECT_ENABLE`) |
| IWDG watchdog | Off | **Enabled** |
| Secure user memory (HDP) | Off | **Enabled** if required by risk assessment |

`[[TODO: Manufacturing — document the Option Byte provisioning step and verification at production
test, and confirm keys are unique-per-product or per-product-family per the security risk assessment.]]`

---

## Appendix A — Security requirements traceability matrix

| Threat | Objective | Control | Test |
|--------|-----------|---------|------|
| T-1 | SO-1, SO-2 | C-1, C-2 | TC-AUTH-02, TC-INTEG-01 |
| T-2 | SO-1, SO-6 | C-1, C-4 | TC-AUTH-03, TC-DFU-01 |
| T-3 | SO-4 | C-3, C-7 | TC-IMMUT-01, TC-HWPROT-01 |
| T-4 | SO-3 | C-5 | TC-CONF-* |
| T-5 | SO-4 | C-7 | TC-HWPROT-01 |
| T-6 | SO-5 | C-2, C-6 | TC-FAILCLOSED-01 |
| T-7 | SO-2 | C-2 | TC-INTEG-02 |
| T-8 | SO-4 | C-7 | TC-HWPROT-01 |
| T-9 | SO-5 | C-8 | TC-ROLLBACK-01..04 |
| T-10 | A3 | C-9 | TC-KEY-* |

---

## Appendix B — Cryptographic details

| Function | Algorithm | Parameters | Role |
|----------|-----------|-----------|------|
| Firmware authentication | ECDSA | curve P-256 (secp256r1), SHA-256 | Signs/verifies the firmware header (authenticity) |
| Firmware integrity | SHA-256 | 256-bit digest | Digest of firmware binary, bound in signed header |
| Firmware confidentiality (transport) | AES-CBC | AES-128, random IV per image | Protects firmware IP in distribution |

**Key inventory**
| Key | Type | Location | Protection |
|-----|------|----------|-----------|
| Firmware signing key | ECDSA P-256 private | **Off-device** (signing infrastructure) | HSM/controlled custody `[[TODO]]` |
| Firmware verification key | ECDSA P-256 public | On-device (Secure Engine) | Immutable; (prod) PCROP/WRP |
| Firmware symmetric key | AES-128 | On-device (Secure Engine) | (prod) PCROP/WRP; SE isolation |

**Key management requirements:** generation in a controlled environment; private key never exported in
clear; documented rotation and revocation; per-product or per-family scoping per risk assessment;
re-signing of firmware on key rotation. `[[TODO: reference the Key Management Procedure SOP.]]`

---

## Appendix C — Glossary

| Term | Definition |
|------|------------|
| Root of Trust | Immutable code (secure boot) trusted implicitly, anchoring all subsequent trust. |
| SBSFU | Secure Boot and Secure Firmware Update (ST framework). |
| SECoreBin | Secure Engine binary holding crypto + keys in a protected region. |
| DFU | Device Firmware Upgrade (USB class) used as the update interface. |
| SBOM | Software Bill of Materials. |
| RDP / WRP / PCROP | STM32 flash readout / write / code-readout protections (Option Bytes). |
| Fail-closed | On verification failure, the system denies execution rather than proceeding. |

---

*End of document. Sections marked `[[TODO]]` require completion/approval by Regulatory Affairs,
Quality, and Security before submission.*
