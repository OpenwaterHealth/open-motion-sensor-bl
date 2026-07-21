/**
  ******************************************************************************
  * @file    anti_rollback.c
  * @brief   Persistent monotonic firmware anti-rollback floor (flash-backed).
  *
  * Storage: an append-only log of version entries in flash sector 5 of bank 1
  * (MEM_ANTIROLLBACK_BASE, 0x080A0000, 128 KB) — the first sector above the
  * application slot, i.e. the first address the DFU writable window cannot reach
  * (FLASH_END_ADDR in usbd_dfu_if.c), so a firmware update over USB DFU cannot
  * erase or overwrite it.
  *
  * It must also stay clear of whatever the APPLICATION puts in flash. The sensor
  * application owns 0x081A0000-0x081DFFFF for the camera FPGA bitstream and
  * sector 15 for its config, so bootloader-managed flash is kept contiguous at
  * the BOTTOM and grows upward, away from the application. See
  * Core/Inc/memory_map.h and issue #1.
  *
  * Note this erases/programs a sector in bank 1 while the bootloader executes
  * from bank 1 sector 0. That is already the norm here — SBSFU erases the
  * application slot (also bank 1) during every DFU download — so no
  * read-while-write accommodation is needed beyond what already exists.
  *
  * Each log entry is one STM32H7 flash word (32 bytes = 256 bits, the minimum
  * programmable unit, programmable once after erase):
  *     word[0] = version
  *     word[1] = ~version           (integrity check)
  *     word[2..7] = 0xFFFFFFFF       (unused / erased state)
  * A free entry reads all-0xFF; a valid entry has word[1] == ~word[0]. The floor
  * is the maximum version across all valid entries, so it only ever increases.
  *
  * Updating the floor appends a new entry to the first free slot — no erase in
  * the common case. The sector is erased only if it is full (after ~4096 distinct
  * version increases — effectively never) or contains stale/garbage data (e.g.
  * never provisioned to the erased state), in which case the new entry is written
  * at slot 0. The new version is >= every prior valid entry, so erasing loses no
  * floor information.
  ******************************************************************************
  */

#include "main.h"          /* HAL (flash) */
#include "anti_rollback.h"
#include "memory_map.h"    /* MEM_ANTIROLLBACK_* — the flash map is authoritative */

/* Flash sector 5 of bank 1 (flat sector 5). Two constraints, both load-bearing:
 *   1. MUST stay outside the DFU writable window (usbd_dfu_if.c:
 *      APP_FLASH_BASE .. FLASH_END_ADDR) so DFU cannot erase it.
 *   2. MUST NOT collide with application-owned flash — see memory_map.h. */
#define AR_SECTOR_BASE      MEM_ANTIROLLBACK_BASE   /* 0x080A0000 */
#define AR_SECTOR_SIZE      MEM_ANTIROLLBACK_SIZE   /* 128 KB */
#define AR_BANK             FLASH_BANK_1
#define AR_SECTOR_INDEX     5U                      /* sector 5 within bank 1 */

#define AR_ENTRY_SIZE       32U                /* one STM32H7 flash word (bytes) */
#define AR_WORDS_PER_ENTRY  (AR_ENTRY_SIZE / 4U)
#define AR_NUM_ENTRIES      (AR_SECTOR_SIZE / AR_ENTRY_SIZE)   /* 4096 */
#define AR_ERASED_U32       0xFFFFFFFFU

/**
  * @brief  Single pass over the log: report the floor (max valid version) and
  *         the index of the first free (erased) slot.
  * @param  p_floor    [out] highest valid version found, 0 if none.
  * @param  p_free_idx [out] first free slot index, or AR_NUM_ENTRIES if full.
  */
static void ar_scan(uint32_t *p_floor, uint32_t *p_free_idx)
{
  const volatile uint32_t *base = (const volatile uint32_t *)AR_SECTOR_BASE;
  uint32_t maxv = 0U;
  uint32_t free_idx = AR_NUM_ENTRIES;

  for (uint32_t i = 0U; i < AR_NUM_ENTRIES; i++)
  {
    const volatile uint32_t *e = &base[i * AR_WORDS_PER_ENTRY];
    uint32_t v   = e[0];
    uint32_t inv = e[1];

    if ((v == AR_ERASED_U32) && (inv == AR_ERASED_U32))
    {
      if (free_idx == AR_NUM_ENTRIES)
      {
        free_idx = i;                 /* first free slot */
      }
    }
    else if ((v != AR_ERASED_U32) && (inv == (uint32_t)(~v)))
    {
      if (v > maxv)
      {
        maxv = v;                     /* valid entry */
      }
    }
    /* else: stale/garbage entry -> ignored (neither free nor valid) */
  }

  *p_floor    = maxv;
  *p_free_idx = free_idx;
}

/**
  * @brief  Erase the floor sector (sector 5, bank 1).
  */
static void ar_erase_sector(void)
{
  FLASH_EraseInitTypeDef e = {0};
  uint32_t sector_error = 0U;

  e.TypeErase    = FLASH_TYPEERASE_SECTORS;
  e.Banks        = AR_BANK;
  e.Sector       = AR_SECTOR_INDEX;
  e.NbSectors    = 1U;
  e.VoltageRange = FLASH_VOLTAGE_RANGE_3;

  HAL_FLASH_Unlock();
  (void)HAL_FLASHEx_Erase(&e, &sector_error);
  HAL_FLASH_Lock();
}

/**
  * @brief  Program one log entry (one flash word) at slot @p idx.
  */
static void ar_program_entry(uint32_t idx, uint32_t version)
{
  static uint32_t buf[AR_WORDS_PER_ENTRY] __attribute__((aligned(8)));

  for (uint32_t i = 0U; i < AR_WORDS_PER_ENTRY; i++)
  {
    buf[i] = AR_ERASED_U32;
  }
  buf[0] = version;
  buf[1] = (uint32_t)(~version);

  HAL_FLASH_Unlock();
  (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                          AR_SECTOR_BASE + (idx * AR_ENTRY_SIZE),
                          (uint64_t)(uint32_t)buf);
  HAL_FLASH_Lock();
}

uint32_t AntiRollback_GetFloor(void)
{
  uint32_t floor;
  uint32_t free_idx;

  ar_scan(&floor, &free_idx);
  return floor;
}

void AntiRollback_UpdateFloor(uint32_t version)
{
  uint32_t floor;
  uint32_t free_idx;

  ar_scan(&floor, &free_idx);

  if (version <= floor)
  {
    return;                           /* floor already at/above this version */
  }

  if (free_idx >= AR_NUM_ENTRIES)
  {
    /* Log full (or stale/garbage): reclaim the sector. The new version is >=
     * every prior valid entry, so no floor information is lost. */
    ar_erase_sector();
    free_idx = 0U;
  }

  ar_program_entry(free_idx, version);
}
