/*
*****************************************************************************
**  File        : mapping_fwimg.ld
**  Abstract    : Firmware image slot mapping for single-image SBSFU.
**                Active slot starts at sector 1 (0x08020000).
*****************************************************************************
*/

/* Slots must be aligned on sector size (128KB on STM32H7).
 * See Core/Inc/memory_map.h for the full flash partition map. Layout:
 *   sector 0      0x08000000  128K  bootloader
 *   sectors 1-4   0x08020000  512K  APP SLOT (active)     <-- this file
 *   sector 5      0x080A0000  128K  anti-rollback floor (bootloader)
 *   sectors 6-12  0x080C0000  896K  reserved (future use)
 *   sectors 13-14 0x081A0000  256K  application-owned (sensor: FPGA bitstream)
 *   sector 15     0x081E0000  128K  user config (read-only) */

/* Active slot #1: sectors 1-4 = 4 x 128KB = 512KB */
__ICFEDIT_SLOT_Active_1_header__ = 0x08020000;
__ICFEDIT_SLOT_Active_1_start__  = 0x08020000;
__ICFEDIT_SLOT_Active_1_end__    = 0x0809FFFF;

/* Slots 2 and 3 are unused: this is a single-image configuration
 * (SFU_NB_MAX_ACTIVE_IMAGE == 1) and dual-slot / A-B updating was deliberately
 * removed so that all openmotion bootloaders share one layout. The address space
 * that a second slot would have occupied is plain reserved flash — see
 * Core/Inc/memory_map.h. Do not reinstate it without revisiting that map. */
__ICFEDIT_SLOT_Active_2_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_2_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_2_end__    = 0x00000000;
__ICFEDIT_SLOT_Active_3_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_3_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_3_end__    = 0x00000000;
