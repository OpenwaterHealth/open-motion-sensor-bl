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
 *   sectors 1-4   0x08020000  512K  APP SLOT 1 (active)   <-- this file
 *   sectors 5-8   0x080A0000  512K  APP SLOT 2 (reserved/future dual-slot)
 *   sectors 9-14  0x08120000  768K  reserved (future use)
 *   sector 15     0x081E0000  128K  user config (read-only) */

/* Active slot #1: sectors 1-4 = 4 x 128KB = 512KB */
__ICFEDIT_SLOT_Active_1_header__ = 0x08020000;
__ICFEDIT_SLOT_Active_1_start__  = 0x08020000;
__ICFEDIT_SLOT_Active_1_end__    = 0x0809FFFF;

/* Slots 2 and 3 are not managed by SBSFU in this single-image configuration.
 * APP SLOT 2 (0x080A0000, 512KB) is physically reserved (see memory_map.h) for a
 * future dual-slot / A-B update scheme; enable it here and bump
 * SFU_NB_MAX_ACTIVE_IMAGE when that is implemented. */
__ICFEDIT_SLOT_Active_2_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_2_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_2_end__    = 0x00000000;
__ICFEDIT_SLOT_Active_3_header__ = 0x00000000;
__ICFEDIT_SLOT_Active_3_start__  = 0x00000000;
__ICFEDIT_SLOT_Active_3_end__    = 0x00000000;
