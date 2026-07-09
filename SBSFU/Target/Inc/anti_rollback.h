/**
  ******************************************************************************
  * @file    anti_rollback.h
  * @brief   Persistent monotonic firmware anti-rollback floor.
  *
  * The floor is the highest firmware version ever launched after a successful
  * SBSFU signature verification. It is stored in a protected flash sector that
  * USB DFU cannot erase (see FLASH_END_ADDR in usbd_dfu_if.c), so a downgrade
  * image cannot reset it. The boot path refuses to launch any image whose
  * (signature-verified) FwVersion is below the floor. See anti_rollback.c for
  * the on-flash storage format.
  ******************************************************************************
  */

#ifndef ANTI_ROLLBACK_H
#define ANTI_ROLLBACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  Return the highest firmware version ever launched (the rollback
  *         floor), or 0 if none has been recorded yet.
  */
uint32_t AntiRollback_GetFloor(void);

/**
  * @brief  Raise the floor to @p version if it is strictly higher than the
  *         current floor; otherwise no-op. Best-effort: a flash failure leaves
  *         the floor unchanged, which is fail-safe (the floor is never lowered).
  * @note   Must only be called with a version read from a signature-verified
  *         image header.
  */
void AntiRollback_UpdateFloor(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif /* ANTI_ROLLBACK_H */
