/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_dfu_if.c
  * @brief          : Usb device for Download Firmware Update.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbd_dfu_if.h"

/* USER CODE BEGIN INCLUDE */
#include "main.h"
#include "version.h"   /* FW_VERSION (CMake-generated git describe) */
#include "se_def_metadata.h"          /* SE_FwRawHeaderTypeDef, SE_FW_HEADER_TOT_LEN */
#include "se_interface_bootloader.h"  /* SE_VerifyHeaderSignature */
#include "sfu_fwimg_regions.h"        /* SFU_IMG_IMAGE_OFFSET */
#include <string.h>
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* Set once a DFU image has actually been programmed this session. Gates the
 * post-manifestation reboot so we never reset spuriously at power-on (where
 * manif_state already reads DFU_MANIFEST_COMPLETE). */
static volatile uint8_t s_dfu_image_written = 0U;

/* Anti-rollback (DFU-time enforcement). Before the first erase wipes the active
 * slot, we latch the version of the currently-installed image; after the new
 * image is downloaded we compare. A downgrade is rejected and the image is
 * destroyed so it can never boot. The floor is the currently-installed version
 * (0 if the slot has no valid "SFU1" header). NOTE: this guards the DFU update
 * path only; a direct SWD/debugger reflash bypasses it, so production units must
 * keep the debug port locked (RDP). */
static uint8_t  s_cur_ver_captured  = 0U;   /* 1 once s_current_fw_version is latched */
static uint16_t s_current_fw_version = 0U;  /* installed FwVersion at DFU entry (the floor) */

/* Set when the host writes to DFU_RESET_VIRT_ADDR (see MEM_If_Write_FS). The
 * bootloader main loop polls DFU_ResetRequested() and performs the reset. */
static volatile uint8_t s_dfu_reset_requested = 0U;

/* Header check before the installed image is destroyed.
 *
 * There is one application slot, so a DFU download has to erase the installed
 * image before the new one is complete, and SBSFU only verifies the new image at
 * the next boot. Without a check here a corrupt, wrongly signed or downgraded
 * file costs the device its working firmware before it is refused (observed on
 * the bench, 2026-10-06). So erase requests are not executed when they arrive:
 * they are recorded in s_pending_erase_mask, and the first block the host
 * writes — which in the DfuSe sequence is the signed header at the slot start —
 * is checked first (magic, ECDSA signature through the Secure Engine, FwVersion
 * not below the installed one, FwSize fits the slot). Only then is the header
 * sector erased and the block programmed; every other pending sector is erased
 * when the first block for it arrives. A refused header fails the DNLOAD, and
 * the installed image is still intact.
 *
 * Each DNLOAD therefore carries at most one sector erase, exactly as before, so
 * the host-side timing (dfu-util, STM32CubeProgrammer, stm32dfu.py) is
 * unchanged and no host protocol change is needed. */
static uint8_t  s_header_accepted    = 0U;   /* 1 once this download's header passed the check */
static uint32_t s_pending_erase_mask = 0U;   /* bit n: erase of sector n requested, not yet done */
static SE_FwRawHeaderTypeDef s_dfu_header;   /* copy of the received header (must be in SBSFU RAM for the SE) */
static uint32_t s_next_write_addr    = 0U;   /* where the next data block will land (0 = none written yet) */

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
  * @brief Usb device.
  * @{
  */

/** @defgroup USBD_DFU
  * @brief Usb DFU device module.
  * @{
  */

/** @defgroup USBD_DFU_Private_TypesDefinitions
  * @brief Private types.
  * @{
  */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Defines
  * @brief Private defines.
  * @{
  */

/* STM32H743: 2MB flash, dual-bank, 128KB sectors. See Core/Inc/memory_map.h.
 * DfuSe sector type letters encode access (bit0=read, bit1=erase, bit2=write):
 *   'a' = 1 = read-only,  'g' = 7 = read+erase+write.
 *   sector 0        bootloader               read-only  -> 01*128Ka
 *   sectors 1-4     active app slot          r/w/erase  -> 04*128Kg
 *   sectors 5-15    reserved/floor/config    read-only  -> 11*128Ka
 * The DFU writable window is clamped to the SBSFU active slot
 * (SLOT_ACTIVE_1: 0x08020000-0x0809FFFF, see Linker/mapping_fwimg.ld) so that
 * ALL DFU-writable flash is covered by secure-boot slot verification. Everything
 * else — bootloader (sector 0), the anti-rollback floor (sector 5, 0x080A0000)
 * and user config (sector 15, 0x081E0000) — is read-only over DFU. */
#define FLASH_DESC_STR      "@Internal Flash/0x08000000/01*128Ka,04*128Kg,11*128Ka"

/* DFU writable window: the active application slot only. The bootloader (below
 * APP_FLASH_BASE) and everything at/above FLASH_END_ADDR (reserved flash, the
 * anti-rollback floor, and the user-config sector) are excluded and cannot be
 * erased or written over DFU. FLASH_END_ADDR is the active-slot end + 1
 * (SLOT_ACTIVE_1_END = 0x0809FFFF in Linker/mapping_fwimg.ld). */
#define APP_FLASH_BASE      0x08020000UL  /* MEM_DFU_WRITABLE_BASE (active slot start) */
#define FLASH_END_ADDR      0x080A0000UL  /* MEM_DFU_WRITABLE_END (active slot end + 1, exclusive) */
#define DFU_SECTOR_SIZE     0x00020000UL  /* 128KB per sector */
#ifndef FLASH_BANK2_BASE
#define FLASH_BANK2_BASE    0x08100000UL  /* start of bank 2 */
#endif

/* USER CODE BEGIN PRIVATE_DEFINES */

/* Virtual DFU UPLOAD address (outside flash). A host that points the DfuSe
 * address pointer here and uploads receives the bootloader version string
 * (FW_VERSION from version.h), null-padded to DFU_VERSION_READ_LEN bytes.
 * This is a read-only query: nothing is written and no flash is touched. */
#define DFU_VERSION_VIRT_ADDR   0xFFFFFF00U
#define DFU_VERSION_READ_LEN    64U

/* Virtual DFU DNLOAD address (outside flash). A host that points the DfuSe
 * address pointer here and downloads any payload requests a device reset —
 * the clean way to leave DFU mode without flashing (e.g. the SDK aborting an
 * update after its pre-flight downgrade check). No flash is touched: the
 * request is only latched here and the bootloader main loop performs the
 * reset after the host's final GETSTATUS handshake completes. SBSFU fully
 * re-verifies the slot on the way back up, so this can never launch an
 * unverified image. */
#define DFU_RESET_VIRT_ADDR     0xFFFFFF08U

/* USER CODE END PRIVATE_DEFINES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Macros
  * @brief Private macros.
  * @{
  */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_Variables
  * @brief Private variables.
  * @{
  */

/* USER CODE BEGIN PRIVATE_VARIABLES */

/* USER CODE END PRIVATE_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Exported_Variables
  * @brief Public variables.
  * @{
  */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_DFU_Private_FunctionPrototypes
  * @brief Private functions declaration.
  * @{
  */

static uint16_t MEM_If_Init_FS(void);
static uint16_t MEM_If_Erase_FS(uint32_t Add);
static uint16_t MEM_If_Write_FS(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint8_t *MEM_If_Read_FS(uint8_t *src, uint8_t *dest, uint32_t Len);
static uint16_t MEM_If_DeInit_FS(void);
static uint16_t MEM_If_GetStatus_FS(uint32_t Add, uint8_t Cmd, uint8_t *buffer);
static uint16_t dfu_read_slot_version(void);

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */
static uint32_t dfu_sector_index(uint32_t Add);
static uint16_t dfu_erase_sector(uint32_t Add);
static uint8_t  dfu_check_header(const SE_FwRawHeaderTypeDef *p_hdr);
static void     dfu_latch_installed_version(void);
/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
  * @}
  */

#if defined ( __ICCARM__ ) /* IAR Compiler */
  #pragma data_alignment=4
#endif
__ALIGN_BEGIN USBD_DFU_MediaTypeDef USBD_DFU_fops_FS __ALIGN_END =
{
   (uint8_t*)FLASH_DESC_STR,
    MEM_If_Init_FS,
    MEM_If_DeInit_FS,
    MEM_If_Erase_FS,
    MEM_If_Write_FS,
    MEM_If_Read_FS,
    MEM_If_GetStatus_FS
};

/* Private functions ---------------------------------------------------------*/
/**
  * @brief  Memory initialization routine.
  * @retval USBD_OK if operation is successful, MAL_FAIL else.
  */
uint16_t MEM_If_Init_FS(void)
{
  /* USER CODE BEGIN 0 */
  /* New session: every download starts with an unchecked header. */
  s_header_accepted    = 0U;
  s_pending_erase_mask = 0U;
  s_next_write_addr    = 0U;
  return (USBD_OK);
  /* USER CODE END 0 */
}

/**
  * @brief  De-Initializes Memory
  * @retval USBD_OK if operation is successful, MAL_FAIL else
  */
uint16_t MEM_If_DeInit_FS(void)
{
  /* USER CODE BEGIN 1 */
  return (USBD_OK);
  /* USER CODE END 1 */
}

/**
  * @brief  Erase sector.
  * @param  Add: Address of sector to be erased.
  * @retval USBD_OK if operation is successful, USBD_FAIL else.
  */
uint16_t MEM_If_Erase_FS(uint32_t Add)
{
  /* USER CODE BEGIN 2 */

  /* Reject erase of the bootloader sector (sector 0, 0x08000000-0x0801FFFF) */
  if (Add < APP_FLASH_BASE || Add >= FLASH_END_ADDR)
  {
    return (USBD_FAIL);
  }

  /* Anti-rollback: latch the currently-installed version BEFORE any erase wipes
   * the active-slot header. Done once per DFU session, on the first erase (the
   * slot is still intact at this point regardless of which sector erases first). */
  dfu_latch_installed_version();

  /* A new erase of the header sector starts a new download: its header has to
   * pass the check again before anything is erased. */
  if (dfu_sector_index(Add) == dfu_sector_index(APP_FLASH_BASE))
  {
    s_header_accepted = 0U;
  }
  s_next_write_addr = 0U;                    /* a download starts with erase requests */

  /* Deferred until the header has been checked (see s_header_accepted); the
   * sector is erased when its first block is written. */
  if (s_header_accepted == 0U)
  {
    s_pending_erase_mask |= (1UL << dfu_sector_index(Add));
    return (USBD_OK);
  }

  return dfu_erase_sector(Add);

  /* USER CODE END 2 */
}

/**
  * @brief  Memory write routine.
  * @param  src:  Buffer containing data to program.
  * @param  dest: Target flash address.
  * @param  Len:  Number of bytes to write.
  * @retval USBD_OK if operation is successful, USBD_FAIL else.
  * @note   STM32H7 requires 256-bit (32-byte) aligned flash word writes.
  *         Partial final words are padded with 0xFF.
  */
uint16_t MEM_If_Write_FS(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  /* USER CODE BEGIN 3 */

  uint32_t addr = (uint32_t)dest;

  /* Virtual reset request: latch and ACK without touching flash. The main
   * loop resets after the host's status handshake completes. Deliberately
   * does NOT set s_dfu_image_written — no image state changes here. */
  if (addr == DFU_RESET_VIRT_ADDR)
  {
    s_dfu_reset_requested = 1U;
    return (USBD_OK);
  }

  /* Protect bootloader */
  if (addr < APP_FLASH_BASE || (addr + Len) > FLASH_END_ADDR)
  {
    return (USBD_FAIL);
  }

  /* First block of a download: it must be the signed header at the slot start,
   * and it must pass the check before any sector is erased. */
  if (s_header_accepted == 0U)
  {
    if ((addr != APP_FLASH_BASE) || (Len < SE_FW_HEADER_TOT_LEN))
    {
      return (USBD_FAIL);
    }
    dfu_latch_installed_version();            /* in case the host wrote without erasing */
    memcpy(&s_dfu_header, src, sizeof(s_dfu_header));
    if (dfu_check_header(&s_dfu_header) != 0U)
    {
      return (USBD_FAIL);                     /* refused: the installed image is untouched */
    }
    s_header_accepted = 1U;
  }

  /* Erase deferred for this sector? Do it now, before the first block lands. */
  if ((s_pending_erase_mask & (1UL << dfu_sector_index(addr))) != 0U)
  {
    if (dfu_erase_sector(addr & ~(DFU_SECTOR_SIZE - 1UL)) != USBD_OK)
    {
      return (USBD_FAIL);
    }
  }

  /* STM32H7 flash word = 256 bits = 32 bytes; data buffer must be 4-byte aligned */
  static __attribute__((aligned(4))) uint8_t padded[32];
  uint32_t remaining = Len;

  HAL_FLASH_Unlock();

  while (remaining > 0)
  {
    uint32_t chunk = (remaining >= 32U) ? 32U : remaining;
    memcpy(padded, src, chunk);
    if (chunk < 32U)
    {
      memset(padded + chunk, 0xFF, 32U - chunk);
    }

    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr,
                          (uint64_t)(uint32_t)padded) != HAL_OK)
    {
      HAL_FLASH_Lock();
      return (USBD_FAIL);
    }

    src       += chunk;
    addr      += 32U;
    remaining -= chunk;
  }

  HAL_FLASH_Lock();

  s_next_write_addr   = addr;  /* already advanced past this block */
  s_dfu_image_written = 1U;   /* a new image was programmed: arm post-manifest reboot */

  return (USBD_OK);

  /* USER CODE END 3 */
}

/**
  * @brief  Memory read routine.
  * @param  src: Pointer to the source buffer. Address to be written to.
  * @param  dest: Pointer to the destination buffer.
  * @param  Len: Number of data to be read (in bytes).
  * @retval Pointer to the physical address where data should be read.
  */
uint8_t *MEM_If_Read_FS(uint8_t *src, uint8_t *dest, uint32_t Len)
{
  /* Return a valid address to avoid HardFault */
  /* USER CODE BEGIN 4 */

  /* DFU "get version" command: an UPLOAD from the virtual address returns the
   * bootloader version string instead of reading flash. Reply with FW_VERSION,
   * null-padded to the requested length so the host can strip the padding. */
  if ((uint32_t)src == DFU_VERSION_VIRT_ADDR)
  {
    const char *ver = FW_VERSION;
    uint32_t ver_len = (uint32_t)strlen(ver);
    if (ver_len > DFU_VERSION_READ_LEN) { ver_len = DFU_VERSION_READ_LEN; }
    if (ver_len > Len)                  { ver_len = Len; }
    memset(dest, 0, Len);
    memcpy(dest, ver, ver_len);
    return dest;
  }

  /* Bound DFU UPLOAD/read to the same window as erase/write: the active
   * application slot only. Without this, a host could point the DfuSe address
   * pointer at any address and UPLOAD-read it, exfiltrating the bootloader and
   * the SE key material (se_key.s AES key in the first sector), the anti-rollback
   * floor, the user-config sector, or RAM. Reject anything outside
   * [APP_FLASH_BASE, FLASH_END_ADDR); the read range must also fit entirely
   * inside the window. The comparison against (FLASH_END_ADDR - addr) is written
   * to avoid the unsigned overflow that (addr + Len) would risk.
   *
   * A refused read answers with zeros rather than NULL: on a NULL return the
   * DFU class (usbd_dfu.c, DFU_Upload) writes the DFU_ERROR_STALLEDPKT status
   * code into dev_state, a value that is not a DFU state, and the device then
   * answers every further request with a STALL until it is power-cycled
   * (observed on the bench, 2026-10-06). Zeros leak nothing and keep the
   * state machine sane. */
  {
    uint32_t addr = (uint32_t)src;

    if ((addr < APP_FLASH_BASE) ||
        (addr >= FLASH_END_ADDR) ||
        (Len  > (FLASH_END_ADDR - addr)))
    {
      memset(dest, 0, Len);
      return dest;
    }
  }

  memcpy(dest, src, Len);
  return dest;
  /* USER CODE END 4 */
}

/**
  * @brief  Get status routine
  * @param  Add: Address to be read from
  * @param  Cmd: Number of data to be read (in bytes)
  * @param  buffer: used for returning the time necessary for a program or an erase operation
  * @retval USBD_OK if operation is successful
  */
uint16_t MEM_If_GetStatus_FS(uint32_t Add, uint8_t Cmd, uint8_t *buffer)
{
  /* USER CODE BEGIN 5 */
  UNUSED(Add);

  uint32_t timeout_ms;
  switch (Cmd)
  {
    /* The DFU class asks for the poll interval first and performs the Erase()/
     * Write() afterwards, inside the GETSTATUS reply (EP0_TxReady). The interval
     * must therefore cover the work the next callback will do, or the host polls
     * while the sector erase is still running and the transfer fails. */
    case DFU_MEDIA_PROGRAM:
    {
      /* Add is the DfuSe address pointer, not the block address: the next
       * block lands where the previous write stopped. */
      uint32_t next = (s_next_write_addr != 0U) ? s_next_write_addr : Add;
      if ((next >= APP_FLASH_BASE) && (next < FLASH_END_ADDR) &&
          ((s_pending_erase_mask & (1UL << dfu_sector_index(next))) != 0U))
      {
        timeout_ms = 4010U;  /* this block carries a deferred sector erase */
      }
      else
      {
        timeout_ms = 10U;    /* 10 ms per write block */
      }
      break;
    }

    case DFU_MEDIA_ERASE:
      /* Add is the address pointer here too, not the sector being erased, so
       * only the session state can tell whether the erase will be deferred. */
      if (s_header_accepted == 0U)
      {
        timeout_ms = 10U;    /* will be deferred: nothing is erased yet (see MEM_If_Erase_FS) */
      }
      else
      {
        timeout_ms = 4000U;  /* 4 s per 128 KB sector on STM32H743 */
      }
      break;

    default:
      timeout_ms = 0U;
      break;
  }

  /* wPollTimeout: 24-bit little-endian poll interval in ms */
  buffer[1] = (uint8_t)(timeout_ms & 0xFFU);
  buffer[2] = (uint8_t)((timeout_ms >> 8U) & 0xFFU);
  buffer[3] = (uint8_t)((timeout_ms >> 16U) & 0xFFU);

  return (USBD_OK);
  /* USER CODE END 5 */
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @brief  Flat sector index (0..15) of a flash address, across both banks.
  */
static uint32_t dfu_sector_index(uint32_t Add)
{
  return (Add - FLASH_BASE) / DFU_SECTOR_SIZE;
}

/**
  * @brief  Latch the installed FwVersion once per DFU session, while the slot
  *         header is still intact. Harmless if the slot is already empty.
  */
static void dfu_latch_installed_version(void)
{
  if (s_cur_ver_captured == 0U)
  {
    s_cur_ver_captured   = 1U;
    s_current_fw_version = dfu_read_slot_version();
  }
}

/**
  * @brief  Check a received signed header before the installed image is erased.
  * @note   The signature check runs in the Secure Engine, which holds the ECDSA
  *         public key; the header copy must live in SBSFU RAM for the SE to
  *         accept it. FwVersion is only compared once the signature is good.
  * @retval 0 if the header is acceptable, 1 if the download must be refused.
  */
static uint8_t dfu_check_header(const SE_FwRawHeaderTypeDef *p_hdr)
{
  SE_StatusTypeDef se_status = SE_KO;
  const uint8_t   *magic     = (const uint8_t *)p_hdr->SFUMagic;

  if ((magic[0] != 'S') || (magic[1] != 'F') || (magic[2] != 'U') || (magic[3] != '1'))
  {
    return 1U;
  }

  if (SE_VerifyHeaderSignature(&se_status, (SE_FwRawHeaderTypeDef *)p_hdr) != SE_SUCCESS)
  {
    return 1U;
  }

  /* Fields below are now authenticated. */
  if ((p_hdr->FwVersion == 0U) || (p_hdr->FwVersion < s_current_fw_version))
  {
    return 1U;    /* downgrade (same rule as DFU_IsRollback, applied before the erase) */
  }

  if ((p_hdr->FwSize == 0U) ||
      (p_hdr->FwSize > (FLASH_END_ADDR - APP_FLASH_BASE - SFU_IMG_IMAGE_OFFSET)))
  {
    return 1U;    /* does not fit the slot */
  }

  return 0U;
}

/**
  * @brief  Erase one application-slot sector now.
  */
static uint16_t dfu_erase_sector(uint32_t Add)
{
  FLASH_EraseInitTypeDef eraseInit = {0};
  uint32_t sectorError = 0;

  eraseInit.TypeErase    = FLASH_TYPEERASE_SECTORS;
  eraseInit.NbSectors    = 1;
  eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;

  if (Add < FLASH_BANK2_BASE)
  {
    eraseInit.Banks  = FLASH_BANK_1;
    eraseInit.Sector = (uint32_t)((Add - FLASH_BASE) / DFU_SECTOR_SIZE);
  }
  else
  {
    eraseInit.Banks  = FLASH_BANK_2;
    eraseInit.Sector = (uint32_t)((Add - FLASH_BANK2_BASE) / DFU_SECTOR_SIZE);
  }

  HAL_FLASH_Unlock();
  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&eraseInit, &sectorError);
  HAL_FLASH_Lock();

  if (status == HAL_OK)
  {
    /* A new application image is being installed: clear the failsafe boot
     * counter (RTC->BKP6R) so the freshly flashed firmware gets a clean set of
     * boot attempts. Without this, if we entered DFU because the counter hit
     * BL_BOOT_FAIL_MAX, it would still be at the limit after the download and
     * the bootloader would refuse to launch the new image (re-entering DFU and
     * causing the host's manifest get_status to fail) until a power cycle.
     * Backup-domain write access was already enabled in main(); re-assert DBP
     * defensively as this is a one-shot, low-cost operation. */
    HAL_PWR_EnableBkUpAccess();
    RTC->BKP6R = 0U;
    __DSB();

    s_pending_erase_mask &= ~(1UL << dfu_sector_index(Add));
  }

  return (status == HAL_OK) ? USBD_OK : USBD_FAIL;
}


/**
  * @brief  Reports whether a DFU download has completed and the host has
  *         finished the manifestation phase (device back in dfuIDLE).
  * @note   The DFU class is configured manifestation-tolerant, so it no longer
  *         self-resets in DFU_Leave(). The bootloader main loop polls this and
  *         issues the reboot into the freshly programmed image once the host has
  *         cleanly read final status — which is what removes dfu-util's
  *         "Error during download get_status".
  * @retval 1 once an image was written AND manifestation is complete, else 0.
  */
uint8_t DFU_ImageDownloadComplete(void)
{
  if (s_dfu_image_written == 0U)
  {
    return 0U;
  }

  const USBD_DFU_HandleTypeDef *hdfu =
      (const USBD_DFU_HandleTypeDef *)hUsbDeviceFS.pClassDataCmsit[hUsbDeviceFS.classId];
  if (hdfu == NULL)
  {
    return 0U;
  }

  /* Manifestation is finished once DFU_Leave() has run, which sets manif_state
   * to COMPLETE. The host's terminal dev_state varies: dfu-util in DfuSe mode
   * reads status once (seeing dfuMANIFEST) and stops, leaving us in
   * MANIFEST_SYNC; a host that polls through to the end leaves us in dfuIDLE.
   * Accept either. The s_dfu_image_written gate keeps this from matching the
   * power-on default (IDLE + COMPLETE), and the write phase never sits in
   * MANIFEST_SYNC/IDLE (it cycles through the DNLOAD_* states), so neither can
   * trigger a premature reboot. */
  if (hdfu->manif_state != DFU_MANIFEST_COMPLETE)
  {
    return 0U;
  }

  return ((hdfu->dev_state == DFU_STATE_MANIFEST_SYNC) ||
          (hdfu->dev_state == DFU_STATE_IDLE)) ? 1U : 0U;
}

/**
  * @brief  Read the FwVersion from the SBSFU header at the active slot start.
  * @note   Header layout (see py-tools/sign_firmware.py): "SFU1" magic at 0x000,
  *         ProtocolVersion at 0x004, FwVersion (u16, little-endian) at 0x006.
  * @retval The installed FwVersion, or 0 if there is no valid "SFU1" header.
  */
static uint16_t dfu_read_slot_version(void)
{
  const uint8_t *hdr = (const uint8_t *)APP_FLASH_BASE;

  if ((hdr[0] != 'S') || (hdr[1] != 'F') || (hdr[2] != 'U') || (hdr[3] != '1'))
  {
    return 0U;   /* no valid installed image -> no rollback floor */
  }
  return (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
}

/**
  * @brief  Anti-rollback check on the just-downloaded image.
  * @note   Compares the new image's FwVersion (read from its freshly written
  *         header) against the version that was installed at DFU entry. The
  *         version field is part of the ECDSA-signed header region, so an
  *         attacker cannot forge a higher value on a genuinely-signed old image;
  *         a forged-high but unsigned image is still rejected by SBSFU at boot.
  * @retval 1 if the new image is a downgrade (must be refused), else 0.
  */
uint8_t DFU_IsRollback(void)
{
  uint16_t new_ver = dfu_read_slot_version();

  if (new_ver == 0U)
  {
    /* No valid header written: not a "rollback" — SBSFU's signature check will
     * reject it at boot anyway. */
    return 0U;
  }
  return (new_ver < s_current_fw_version) ? 1U : 0U;
}

/**
  * @brief  Destroy the image in the active slot by erasing its header sector.
  * @note   Used to make a rejected downgrade unbootable: without a valid header
  *         SBSFU refuses to launch it, so it can never run — even after a power
  *         cycle (the boot path performs no version check of its own).
  */
void DFU_InvalidateImage(void)
{
  (void)dfu_erase_sector(APP_FLASH_BASE);
}

/**
  * @brief  Clear the post-download state so the main loop stops acting on a
  *         completed download (used after a rollback has been handled).
  */
void DFU_ClearDownloadState(void)
{
  s_dfu_image_written = 0U;
}

/**
  * @brief  Reports whether the host requested a device reset via a DNLOAD to
  *         DFU_RESET_VIRT_ADDR. Polled by the bootloader main loop, which
  *         performs the actual NVIC_SystemReset (after a short delay so the
  *         host's final USB status transaction completes).
  * @retval 1 if a reset was requested, else 0.
  */
uint8_t DFU_ResetRequested(void)
{
  return s_dfu_reset_requested;
}

/**
  * @brief  Erase slot sectors beyond the just-downloaded image that are not clean.
  * @note   A host only erases the sectors its file covers. If the previous image
  *         was larger, its tail survives in the sectors above the new image and
  *         SBSFU's VerifySlot (which accepts only 0x00/0xFF beyond the image)
  *         would refuse the new image at boot. Runs from the main loop, after
  *         manifestation, so the per-sector erase time does not sit inside a
  *         USB control transfer. The sector containing the image end was erased
  *         by the download itself and is left alone.
  */
void DFU_CleanSlotTail(void)
{
  const uint8_t *hdr = (const uint8_t *)APP_FLASH_BASE;
  uint32_t fw_size;
  uint32_t image_end;
  uint32_t sector;

  if ((hdr[0] != 'S') || (hdr[1] != 'F') || (hdr[2] != 'U') || (hdr[3] != '1'))
  {
    return;                                   /* no image: nothing to protect */
  }
  fw_size   = *(const uint32_t *)(APP_FLASH_BASE + 8U);     /* FwSize at 0x008 */
  image_end = APP_FLASH_BASE + SFU_IMG_IMAGE_OFFSET + fw_size;
  if ((fw_size == 0U) || (image_end > FLASH_END_ADDR))
  {
    return;                                   /* SBSFU will reject it anyway */
  }

  for (sector = dfu_sector_index(image_end - 1U) + 1U;
       sector < dfu_sector_index(FLASH_END_ADDR);
       sector++)
  {
    const uint32_t *p   = (const uint32_t *)(FLASH_BASE + (sector * DFU_SECTOR_SIZE));
    const uint32_t *end = p + (DFU_SECTOR_SIZE / 4U);
    uint8_t dirty = 0U;

    for (; p < end; p++)
    {
      if ((*p != 0x00000000U) && (*p != 0xFFFFFFFFU))
      {
        dirty = 1U;
        break;
      }
    }
    if (dirty != 0U)
    {
      (void)dfu_erase_sector(FLASH_BASE + (sector * DFU_SECTOR_SIZE));
    }
  }
}

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @}
  */

/**
  * @}
  */

