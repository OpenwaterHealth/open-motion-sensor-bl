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
  *  0x080A0000-0x0811FFFF    512K   5-8   APP SLOT 2 (spare)  read/erase/write
  *  0x08120000-0x081DFFFF    768K   9-14  RESERVED (future)   read/erase/write
  *  0x081E0000-0x081FFFFF    128K   15    USER CONFIG         read-only
  *  ---------------------------------------------------------------------------
  *                          2048K
  *
  *  Notes:
  *   - The BOOTLOADER sector and the USER CONFIG sector are NOT writable or
  *     erasable through the DFU interface. The bootloader may READ user config.
  *   - The bootloader currently boots APP SLOT 1. SLOT 2 is reserved for a
  *     future dual-slot / A-B update scheme and is not yet referenced by SBSFU.
  *   - Inside a slot the first @ref MEM_APP_IMAGE_OFFSET bytes hold the signed
  *     image header; the application is linked to run at SLOT_START + that offset.
  *   - RESERVED is unallocated flash kept free for future features.
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

/* ── Reserved for future use (sectors 9-14, 768 KB) ───────────────────────── */
#define MEM_RESERVED_BASE         (0x08120000UL)
#define MEM_RESERVED_SIZE         (0x000C0000UL)   /* 768 KB                      */
#define MEM_RESERVED_END          (MEM_RESERVED_BASE + MEM_RESERVED_SIZE)

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

/* ── DFU writable window (everything except bootloader + user config) ─────── */
/* The DFU interface accepts erase/write only within [BASE, END). Both the
   bootloader sector (below BASE) and the user-config sector (at END) are
   excluded, making them effectively read-only over DFU. */
#define MEM_DFU_WRITABLE_BASE     (MEM_SLOT1_BASE)        /* 0x08020000 */
#define MEM_DFU_WRITABLE_END      (MEM_USER_CONFIG_BASE)  /* 0x081E0000 (exclusive) */

#endif /* MEMORY_MAP_H */
