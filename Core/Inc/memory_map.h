/**
  ******************************************************************************
  * @file    memory_map.h
  * @brief   STM32H743VI (2 MB internal flash) partition map for the openmotion
  *          secure bootloader.
  *
  *          This is the single human-readable source of truth for the flash
  *          layout. The actual linker partitioning lives in:
  *            - STM32H743XX_FLASH.ld / Linker/mapping_sbsfu.ld  (bootloader image)
  *            - Linker/mapping_fwimg.ld                          (firmware slots)
  *            - USB_DEVICE/App/usbd_dfu_if.c                     (DFU writable region)
  *          Keep those in sync with the values below.
  ******************************************************************************
  *
  *  STM32H743 internal flash: 2 MB = 2 banks x 1 MB, 16 sectors x 128 KB.
  *
  *   Addr range              Size   Sct   Region            Access (via DFU)
  *  ---------------------------------------------------------------------------
  *  0x08000000-0x0801FFFF    128K   0     BOOTLOADER         read-only
  *  0x08020000-0x0809FFFF    512K   1-4   APP SLOT 1 (active) read/erase/write
  *  0x080A0000-0x0811FFFF    512K   5-8   APP SLOT 2 (spare)  read-only
  *  0x08120000-0x0813FFFF    128K   9     ANTI-ROLLBACK FLOOR read-only
  *  0x08140000-0x0819FFFF    384K   10-12 RESERVED (future)   read-only
  *  0x081A0000-0x081DFFFF    256K   13-14 APPLICATION-OWNED   read-only
  *  0x081E0000-0x081FFFFF    128K   15    USER CONFIG         read-only
  *  ---------------------------------------------------------------------------
  *                          2048K
  *
  *  Notes:
  *   - Only APP SLOT 1 is writable or erasable through the DFU interface; the
  *     window is deliberately clamped to it so that ALL DFU-writable flash is
  *     covered by secure-boot slot verification. Everything else is read-only
  *     over DFU. The bootloader may READ user config.
  *   - The bootloader currently boots APP SLOT 1. SLOT 2 is reserved for a
  *     future dual-slot / A-B update scheme and is not yet referenced by SBSFU.
  *   - Inside a slot the first @ref MEM_APP_IMAGE_OFFSET bytes hold the signed
  *     image header; the application is linked to run at SLOT_START + that offset.
  *   - ANTI-ROLLBACK FLOOR is bootloader-managed (SBSFU/Target/Src/anti_rollback.c).
  *     It is erased and rewritten by the bootloader, so nothing else may live in
  *     that sector.
  *   - APPLICATION-OWNED is flash the *application* writes and the bootloader must
  *     never touch. On the sensor module this holds the camera FPGA bitstream
  *     (~160 KB at 0x081A0000, spanning sectors 13-14), which the application
  *     streams to the CrossLink on every boot. Read-only here means read-only via
  *     DFU; the application manages it through its own path.
  *   - RESERVED is unallocated flash kept free for future features. Anything the
  *     BOOTLOADER claims from it must be taken from the BOTTOM (sector 10 up), so
  *     it grows away from application-owned flash rather than into it.
  *
  ******************************************************************************
  */

#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

/* ── Flash device geometry ────────────────────────────────────────────────── */
#define MEM_FLASH_BASE            (0x08000000UL)
#define MEM_FLASH_SIZE            (0x00200000UL)   /* 2 MB                        */
#define MEM_FLASH_END             (MEM_FLASH_BASE + MEM_FLASH_SIZE) /* exclusive  */
#define MEM_SECTOR_SIZE           (0x00020000UL)   /* 128 KB per sector           */

/* ── Bootloader (sector 0, read-only) ─────────────────────────────────────── */
#define MEM_BOOTLOADER_BASE       (0x08000000UL)
#define MEM_BOOTLOADER_SIZE       (0x00020000UL)   /* 128 KB                      */
#define MEM_BOOTLOADER_END        (MEM_BOOTLOADER_BASE + MEM_BOOTLOADER_SIZE)

/* ── Application slot 1 — active (sectors 1-4, 512 KB) ─────────────────────── */
#define MEM_SLOT1_BASE            (0x08020000UL)
#define MEM_SLOT1_SIZE            (0x00080000UL)   /* 512 KB                      */
#define MEM_SLOT1_END             (MEM_SLOT1_BASE + MEM_SLOT1_SIZE)

/* ── Application slot 2 — spare/future (sectors 5-8, 512 KB) ───────────────── */
#define MEM_SLOT2_BASE            (0x080A0000UL)
#define MEM_SLOT2_SIZE            (0x00080000UL)   /* 512 KB                      */
#define MEM_SLOT2_END             (MEM_SLOT2_BASE + MEM_SLOT2_SIZE)

/* ── Anti-rollback floor (sector 9, bootloader-managed) ───────────────────── */
/* Append-only monotonic firmware-version log. Erased and rewritten by the
   bootloader (SBSFU/Target/Src/anti_rollback.c), so it must not overlap anything
   the application owns. Kept at the BOTTOM of the old reserved band for exactly
   that reason — the top of it belongs to the application (see below). */
#define MEM_ANTIROLLBACK_BASE     (0x08120000UL)
#define MEM_ANTIROLLBACK_SIZE     (0x00020000UL)   /* 128 KB                      */
#define MEM_ANTIROLLBACK_END      (MEM_ANTIROLLBACK_BASE + MEM_ANTIROLLBACK_SIZE)

/* ── Reserved for future use (sectors 10-12, 384 KB) ──────────────────────── */
#define MEM_RESERVED_BASE         (0x08140000UL)
#define MEM_RESERVED_SIZE         (0x00060000UL)   /* 384 KB                      */
#define MEM_RESERVED_END          (MEM_RESERVED_BASE + MEM_RESERVED_SIZE)

/* ── Application-owned (sectors 13-14, 256 KB) ────────────────────────────── */
/* Written by the APPLICATION, never by the bootloader. On the sensor module this
   is the camera FPGA bitstream at 0x081A0000 (~160 KB, spanning both sectors).
   Placing bootloader data here corrupts it — see issue #1. */
#define MEM_APP_OWNED_BASE        (0x081A0000UL)
#define MEM_APP_OWNED_SIZE        (0x00040000UL)   /* 256 KB                      */
#define MEM_APP_OWNED_END         (MEM_APP_OWNED_BASE + MEM_APP_OWNED_SIZE)

/* ── User configuration (sector 15, read-only, not erasable via DFU) ──────── */
#define MEM_USER_CONFIG_BASE      (0x081E0000UL)
#define MEM_USER_CONFIG_SIZE      (0x00020000UL)   /* 128 KB                      */
#define MEM_USER_CONFIG_END       (MEM_USER_CONFIG_BASE + MEM_USER_CONFIG_SIZE)

/* ── Signed-image layout inside a slot ────────────────────────────────────── */
/* Offset from a slot's base to the firmware body / execution address. Must match
   SFU_IMG_IMAGE_OFFSET in SBSFU/App/Inc/sfu_fwimg_regions.h. The first
   MEM_APP_IMAGE_OFFSET bytes of the slot hold the signed header. */
#define MEM_APP_IMAGE_OFFSET      (0x00000400UL)   /* 1 KB (Cortex-M7 vector align) */

/* Address the active application is linked to / executes from. */
#define MEM_APP_RUN_ADDRESS       (MEM_SLOT1_BASE + MEM_APP_IMAGE_OFFSET) /* 0x08020400 */

/* ── DFU writable window (the active application slot ONLY) ───────────────── */
/* The DFU interface accepts erase/write only within [BASE, END) — the active
   slot and nothing else, so that all DFU-writable flash is covered by secure-boot
   slot verification. Everything below BASE (the bootloader) and everything at or
   above END (slot 2, the anti-rollback floor, reserved, application-owned, and
   user config) is read-only over DFU.

   These must match APP_FLASH_BASE / FLASH_END_ADDR in USB_DEVICE/App/usbd_dfu_if.c,
   which is where the bound is actually enforced. MEM_DFU_WRITABLE_END previously
   read 0x081E0000 here while the enforced value was 0x080A0000; the enforced
   (narrower) value is correct and this is now aligned to it. */
#define MEM_DFU_WRITABLE_BASE     (MEM_SLOT1_BASE)  /* 0x08020000 */
#define MEM_DFU_WRITABLE_END      (MEM_SLOT1_END)   /* 0x080A0000 (exclusive) */

#endif /* MEMORY_MAP_H */
