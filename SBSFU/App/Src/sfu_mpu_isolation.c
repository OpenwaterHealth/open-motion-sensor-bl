/**
  ******************************************************************************
  * @file    sfu_mpu_isolation.c
  * @author  MCD Application Team
  * @brief   SFU MPU isolation primitives
  *          This file provides functions to manage the MPU isolation of the Secure Engine.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2017 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file in
  * the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */


/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "sfu_mpu_isolation.h"
#include "sfu_low_level_security.h"
#include "sfu_low_level_security_rss.h"
#if defined(__CC_ARM) || defined(__ARMCC_VERSION)
#include "mapping_sbsfu.h"
#elif defined (__ICCARM__) || defined(__GNUC__)
#include "mapping_export.h"
#endif /* __CC_ARM || __ARMCC_VERSION */

/* Private function prototypes -----------------------------------------------*/
#if defined(SFU_MPU_PROTECT_ENABLE) && defined(SFU_MPU_UNPRIV_BOOT)
static void SFU_SE_STACK_Clean(void);
#endif /* SFU_MPU_PROTECT_ENABLE && SFU_MPU_UNPRIV_BOOT */
#if defined(SFU_WIPE_SE_RAM_ON_EXIT) || defined(SFU_MPU_PROTECT_ENABLE)
static void SFU_SE_RAM_Erase(void);
#endif /* SFU_WIPE_SE_RAM_ON_EXIT || SFU_MPU_PROTECT_ENABLE */
#if defined(SFU_SECURE_USER_PROTECT_ENABLE)
/* Only used by the SB_SYSCALL_LAUNCH_APP handler when secure user memory is
   enabled; the development build launches the application directly. */
static void SFU_SecUserActivationInRam(uint32_t Address);
#endif /* SFU_SECURE_USER_PROTECT_ENABLE */

/* Functions Definition ------------------------------------------------------*/
#if defined(SFU_MPU_PROTECT_ENABLE) && defined(SFU_MPU_UNPRIV_BOOT)
/**
  * @brief Clean SE Stack.
  * @note  Only used in the unprivileged-boot model, where MSP has been moved to
  *        the top of SE RAM; cleans the SE stack from its base up to the current
  *        SP. In the privileged-boot model use SFU_SE_RAM_Erase() instead.
  */
static void SFU_SE_STACK_Clean(void)
{
  uint32_t sp_base = SE_REGION_RAM_START;
  uint32_t sp_top = SE_REGION_RAM_STACK_TOP;
  uint32_t sp = __get_MSP();
  if ((sp > sp_base) && (sp <= sp_top))
  {
    while (sp_base < sp)
    {
      *(__IO uint32_t *)sp_base = 0U;
      sp_base += 4U;
    }
  }
  else
  {
    /*  error trigger a reset */
    HAL_NVIC_SystemReset();
  }
}
#endif /* SFU_MPU_PROTECT_ENABLE && SFU_MPU_UNPRIV_BOOT */

/**
  * @brief Zeroize the entire Secure Engine RAM region before control leaves the
  *        trusted bootloader.
  * @note  Wipes [SE_REGION_RAM_START, SE_REGION_RAM_END] (SE stack + SE static
  *        data: the AES firmware-key copy, the ECC public-key buffer, the
  *        AES/SHA contexts). This is the durable, app-independent protection for
  *        the RAM-resident key material: after this, nothing sensitive remains
  *        in SE RAM when the (privileged) application or the USB-DFU path runs.
  *
  *        Unlike SFU_SE_STACK_Clean(), this does NOT depend on MSP pointing into
  *        SE RAM, so it is safe in the privileged-boot model where MSP is the
  *        main SB stack. The SVC handler frame lives on that main stack (outside
  *        SE RAM), so wiping SE RAM here never corrupts the live stack.
  */
#if defined(SFU_WIPE_SE_RAM_ON_EXIT) || defined(SFU_MPU_PROTECT_ENABLE)
static void SFU_SE_RAM_Erase(void)
{
  __IO uint32_t *p   = (__IO uint32_t *)SE_REGION_RAM_START;
  uint32_t       end = SE_REGION_RAM_END;

  while ((uint32_t)p <= end)
  {
    *p = 0U;
    p++;
  }
  __DSB();
}
#endif /* SFU_WIPE_SE_RAM_ON_EXIT || SFU_MPU_PROTECT_ENABLE */

/**
  * @brief Prepare to leave the trusted bootloader (app-privileged model).
  * @note  Called on BOTH exit paths: the app launch (SFU_IMG_LaunchActiveImg)
  *        and the USB-DFU fallback return to main().
  *        - If SFU_WIPE_SE_RAM_ON_EXIT: zeroize the SE key RAM (AES firmware-key
  *          copy, ECC public-key buffer, AES/SHA contexts, SE stack). This is
  *          the durable, app-independent protection and it closes the DFU
  *          UPLOAD/read exfil path for RAM-resident key material.
  *        - If SFU_MPU_PROTECT_ENABLE: additionally disable the MPU and re-enable
  *          ITCM so the privileged app / DFU code runs on a flat memory map
  *          (leaving the isolation regions engaged, PRIVDEFENA=0, would fault it).
  *        No-op when neither is compiled in. NOT used in the unprivileged-boot
  *        model (SFU_MPU_UNPRIV_BOOT), where the MPU must stay engaged.
  */
void SFU_MPU_DisengageForExit(void)
{
#if defined(SFU_WIPE_SE_RAM_ON_EXIT) || defined(SFU_MPU_PROTECT_ENABLE)
  /* Wipe SE key RAM (the durable protection in the app-privileged model). */
  SFU_SE_RAM_Erase();
#endif /* SFU_WIPE_SE_RAM_ON_EXIT || SFU_MPU_PROTECT_ENABLE */
#if defined(SFU_MPU_PROTECT_ENABLE)
  HAL_MPU_Disable();
  SCB->ITCMCR |= SCB_ITCMCR_EN_Msk;   /* undo the ITCM disable from SetProtectionMPU */
  __DSB();
  __ISB();
#endif /* SFU_MPU_PROTECT_ENABLE */
}

/**
  * @brief This function copy secure user memory activation code from Flash to RAM
  *        then starts  its execution.
  * @note  As soon as the secure user memory is activated this secured area is no longer accessible
  *        Thus, activation should be performed from outside this area
  * @param Address of the code to jump into
  * @retval SFU_ErrorStatus SFU_SUCCESS if successful, SFU_ERROR otherwise.
  */
#if defined(SFU_SECURE_USER_PROTECT_ENABLE)
static void SFU_SecUserActivationInRam(uint32_t Address)
{
  uint32_t *psrc;
  uint32_t *psrc_end;
  uint32_t *pdest;
  uint32_t result = 0U;

  /* Copy code from Flash to RAM */
  psrc = (uint32_t *)SB_HDP_REGION_ROM_START;
  psrc_end = (uint32_t *)SB_HDP_REGION_ROM_END;
  pdest = (uint32_t *)SB_HDP_CODE_REGION_RAM_START;
  while (psrc < psrc_end)
  {
    *pdest = *psrc;
    pdest++;
    psrc++;
  }

#if defined(SFU_MPU_PROTECT_ENABLE)
  /* Change MPU configuration to set read only property to this RAM area */
  if (SFU_LL_SECU_SetProtectionMPU_SecUser(MPU_INSTRUCTION_ACCESS_DISABLE) != SFU_SUCCESS)
  {
    HAL_NVIC_SystemReset();
  }
#endif /* (SFU_MPU_PROTECT_ENABLE) */

  /* Verify thecopy code from Flash to RAM */
  psrc = (uint32_t *)(SB_HDP_REGION_ROM_START);
  psrc_end = (uint32_t *)SB_HDP_REGION_ROM_END;
  pdest = (uint32_t *)SB_HDP_CODE_REGION_RAM_START;
  while (psrc < psrc_end)
  {
    result |= (*pdest ^ *psrc);
    pdest++;
    psrc++;
  }

  if (result == 0U)
  {
#if defined(SFU_MPU_PROTECT_ENABLE)
    /* Change MPU configuration to set execution property to this RAM area */
    if (SFU_LL_SECU_SetProtectionMPU_SecUser(MPU_INSTRUCTION_ACCESS_ENABLE) != SFU_SUCCESS)
    {
      HAL_NVIC_SystemReset();
    }
#endif /* (SFU_MPU_PROTECT_ENABLE) */

    /* Execute code in RAM to activate secure user memory */
    SFU_LL_SECU_ActivateSecUser(Address);
  }
  else
  {
    HAL_NVIC_SystemReset();
  }
}
#endif /* SFU_SECURE_USER_PROTECT_ENABLE */


/**
  * @brief This is the Supervisor calls handler.
  * @param args SVC arguments
  * @retval void
  * @note Installed in startup_stm32xxxx.s
  *
  * This handler handles 2 requests:
  * \li Secure Engine SVC: run a Secure Engine privileged operation provided as a parameter
  * \li (see @ref SE_FunctionIDTypeDef)
  * \li Internal SB_SFU SVC: run a SB_SFU privileged operation provided as a parameter (see @ref SFU_MPU_PrivilegedOpId)
  */
void MPU_SVC_Handler(uint32_t *args)
{
  /* in PSP (args) contains caller context as follow *
    * args[0] :R0
    * args[1] :R1
    * args[2] :R2
    * args[3] :R3
    * args[4] :R12
    * args[5] :LR
    * args[6] :PC to return after exception
    * args[7] :xPSR
  */
  /*  read code OP of instruction that generate SVC interrupt
   *  bottom 8-bits of the SVC instruction code OP are SVC value  */
  uint8_t code = ((uint8_t *)args[6])[-2];

  switch (code)
  {
    case 0:
      /* A Secure Engine service is called */
      SE_SVC_Handler(args);
      break;
    case 1:
      /* Internal SB_SFU privileged service */
      SFU_MPU_SVC_Handler(args);
      break;
    default:
      /* Force a reset */
      HAL_NVIC_SystemReset();
      break;
  }
}

/**
  * @brief This function triggers a SB_SFU Privileged Operation requested with SB_SysCall
  * @param args arguments
  *             The first argument is the identifier of the requested operation.
  * @retval void
  */
void SFU_MPU_SVC_Handler(uint32_t *args)
{
  switch (args[0])
  {
    case SB_SYSCALL_LAUNCH_APP:
      /* Wipe the SE key RAM before control leaves the trusted bootloader.
       * NOTE: in this port's app-privileged model the app is NOT launched
       * through this handler (SFU_IMG_LaunchActiveImg uses the direct jump), so
       * this case is exercised only in the unprivileged model / standalone
       * loader. The wipe references below are guarded to match whichever helper
       * is actually compiled in. */
#if defined(SFU_MPU_PROTECT_ENABLE) && defined(SFU_MPU_UNPRIV_BOOT)
      /* Unprivileged-boot model: MSP was moved into SE RAM, clean up to SP. */
      SFU_SE_STACK_Clean();
#elif defined(SFU_WIPE_SE_RAM_ON_EXIT) || defined(SFU_MPU_PROTECT_ENABLE)
      /* MSP is the main SB stack (outside SE RAM): full SE-RAM wipe is safe. */
      SFU_SE_RAM_Erase();
#endif /* wipe helper selection */
#if defined(SFU_MPU_PROTECT_ENABLE)
      SCB_InvalidateICache();
      SCB_DisableICache();
      SCB_CleanDCache();
      SCB_DisableDCache();
#endif /* SFU_MPU_PROTECT_ENABLE */
#if defined(SFU_SECURE_USER_PROTECT_ENABLE)
      /* Production: activate the secure user memory from RAM, then jump. */
      SFU_SecUserActivationInRam(args[1]);
#else
      /* Unprivileged-boot / full-isolation model: the MPU stays engaged across
       * the jump so the application runs unprivileged and reaches SE services
       * only through this call gate. launch_application() reads the reset
       * handler from args[1] (the active-slot vector table) and exception-
       * returns to it in Thread mode; the application re-initialises the FPU and
       * VTOR in its SystemInit.
       *
       * NOTE: In the privileged / app-privileged model used by this port
       * (SFU_MPU_UNPRIV_BOOT undefined) the application is NOT launched through
       * here — SFU_IMG_LaunchActiveImg() disengages MPU isolation via
       * SFU_MPU_DisengageForExit() and uses the direct jump instead, which
       * handles the interrupt/CONTROL/FPU state this board's app requires. This
       * SVC handler path is therefore only exercised in the unprivileged model
       * (and by the standalone loader). */
      launch_application(args[1], (uint32_t)jump_to_function);
#endif /* SFU_SECURE_USER_PROTECT_ENABLE */
      break;
    case SB_SYSCALL_RESET:
      HAL_NVIC_SystemReset();
      break;
    case SB_SYSCALL_MPU_CONFIG:
#if defined(SFU_MPU_PROTECT_ENABLE)
      /* Privileged mode required for MPU re-configuration */
      (void)SFU_LL_SECU_SetProtectionMPU(SFU_SECOND_CONFIGURATION);
#endif /* SFU_MPU_PROTECT_ENABLE */
      break;
    case SB_SYSCALL_DMA_CONFIG:
#if defined(SFU_DMA_PROTECT_ENABLE)
      /* Privileged mode required for DMA re-configuration (clocks access required privilege mode) */
      (void)SFU_LL_SECU_SetProtectionDMA();
#endif /* SFU_DMA_PROTECT_ENABLE */
      break;
    case SB_SYSCALL_DAP_CONFIG:
#if defined(SFU_DAP_PROTECT_ENABLE)
      /* Privileged mode required for DAP re-configuration (clock access required privilege mode) */
      (void)SFU_LL_SECU_SetProtectionDAP();
#endif /* SFU_DAP_PROTECT_ENABLE */
      break;
    case SB_SYSCALL_TAMPER_CONFIG:
#if defined(SFU_TAMPER_PROTECT_ENABLE)
      /* Privileged mode required for TAMPER re-configuration (clock access required privilege mode) */
      (void)SFU_LL_SECU_SetProtectionANTI_TAMPER();
#endif /* SFU_DAP_PROTECT_ENABLE */
      break;
    case SB_SYSCALL_SYSTICK_SUSPEND:
      HAL_SuspendTick();
      break;
    default:
      /* Force a reset */
      HAL_NVIC_SystemReset();
      break;
  }
}

/**
  * @brief This functions triggers a SB_SFU system call (supervisor call): request privileged operation
  * @param syscall The identifier of the operation to be called (see @ref SFU_MPU_PrivilegedOpId)
  * @param arguments arguments of the privileged operation
  * @retval void
  */
void SFU_MPU_SysCall(uint32_t syscall, ...)
{
  /*
    * You cannot directly change to privileged mode from unprivileged mode without going through an exception, for
    * example an SVC.
    * Handled by @ref MPU_SVC_Handler() and finally @ref SFU_MPU_SVC_Handler()
    */
  __ASM volatile("SVC #1");   /* 1 is the hard-coded value to indicate a SB_SFU syscall */
}

/**
  * @brief This is a helper function to determine if we are currently running in non-privileged mode or not
  * @param void
  * @retval 0 if we are in privileged mode, 1 if we are in non-privileged mode
  */
uint32_t SFU_MPU_IsUnprivileged(void)
{
  return ((__get_IPSR() == 0U) && ((__get_CONTROL() & 1U) == 1U));
}

/**
  * @brief This is a helper function to enter the unprivileged level for software execution
  * @param void
  * @retval void
  */
void SFU_MPU_EnterUnprivilegedMode(void)
{
  __set_PSP(__get_MSP()); /* set up Process Stack Pointer to current stack pointer */
  __set_MSP(SE_REGION_RAM_STACK_TOP); /* change main stack to point on privileged stack */
  __set_CONTROL(__get_CONTROL() | 3U); /* bit 0 = 1: unprivileged      bit 1=1: stack=PSP */
  __ISB();
}

