/**
  ******************************************************************************
  * @file    app_hw.h
  * @author  MCD Application Team
  * @brief   This file contains definitions for Secure Firmware Update hardware
  *          interface.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef APP_HW_H
#define APP_HW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Exported macros -----------------------------------------------------------*/

/**
  * Button abstraction.
  * The openmotion-bl hardware does not have a dedicated user button.
  * BUTTON_INIT() is a no-op; BUTTON_PUSHED() always returns 0 (never pressed).
  * This prevents the SBSFU local loader from being forced on every boot.
  */
#define BUTTON_INIT()       do {} while(0)
#define BUTTON_PUSHED()     (0U)
#define BRD_V0_Pin GPIO_PIN_9
#define BRD_V0_GPIO_Port GPIOE
#define BRD_V1_Pin GPIO_PIN_8
#define BRD_V1_GPIO_Port GPIOE
#define BRD_V2_Pin GPIO_PIN_7
#define BRD_V2_GPIO_Port GPIOE
#define IND2_Pin GPIO_PIN_4
#define IND2_GPIO_Port GPIOD
#define IND1_Pin GPIO_PIN_3
#define IND1_GPIO_Port GPIOA
#define IND3_Pin GPIO_PIN_5
#define IND3_GPIO_Port GPIOD
#define SYS_EN_Pin GPIO_PIN_9
#define SYS_EN_GPIO_Port GPIOB
#define DBG_RX_Pin GPIO_PIN_0
#define DBG_RX_GPIO_Port GPIOD
#define SCL_CFG_Pin GPIO_PIN_8
#define SCL_CFG_GPIO_Port GPIOA
#define DBG_TX_Pin GPIO_PIN_1
#define DBG_TX_GPIO_Port GPIOD
#define SDA_REM_Pin GPIO_PIN_9
#define SDA_REM_GPIO_Port GPIOC
#define IO_EXP_RSTN_Pin GPIO_PIN_2
#define IO_EXP_RSTN_GPIO_Port GPIOA
#define HUB_RESET_Pin GPIO_PIN_13
#define HUB_RESET_GPIO_Port GPIOC

/**
  * Status LED abstraction (maps to IND1 on PA3).
  * Provides BSP_LED_* shims so SBSFU sfu_boot.c and sfu_com_loader.c
  * compile and function without a full BSP driver.
  */
typedef uint32_t Led_TypeDef;

#define LED_GREEN           (0U)   /* Logical LED identifier used by SBSFU */

static void ConfigureGPOPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (GPIOx == GPIOA) __HAL_RCC_GPIOA_CLK_ENABLE();
  else if (GPIOx == GPIOB) __HAL_RCC_GPIOB_CLK_ENABLE();
  else if (GPIOx == GPIOC) __HAL_RCC_GPIOC_CLK_ENABLE();
  else if (GPIOx == GPIOD) __HAL_RCC_GPIOD_CLK_ENABLE();
  else if (GPIOx == GPIOE) __HAL_RCC_GPIOE_CLK_ENABLE();
  else if (GPIOx == GPIOF) __HAL_RCC_GPIOF_CLK_ENABLE();
  else if (GPIOx == GPIOG) __HAL_RCC_GPIOG_CLK_ENABLE();
  else if (GPIOx == GPIOH) __HAL_RCC_GPIOH_CLK_ENABLE();
  else if (GPIOx == GPIOI) __HAL_RCC_GPIOI_CLK_ENABLE();
  // De-initialize the REF_SEL pin
  HAL_GPIO_DeInit(GPIOx, GPIO_Pin);

  // Configure as output with no pull-up/down (Hi-Z)
  GPIO_InitStruct.Pin = GPIO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOx, &GPIO_InitStruct);
}

static inline int BSP_GPIO_Init(void)
{
    ConfigureGPOPin(SCL_CFG_GPIO_Port, SCL_CFG_Pin);
    ConfigureGPOPin(SDA_REM_GPIO_Port, SDA_REM_Pin);
    ConfigureGPOPin(IO_EXP_RSTN_GPIO_Port, IO_EXP_RSTN_Pin);
    ConfigureGPOPin(HUB_RESET_GPIO_Port, HUB_RESET_Pin);
    ConfigureGPOPin(IND1_GPIO_Port, IND1_Pin);
    ConfigureGPOPin(IND2_GPIO_Port, IND2_Pin);
    ConfigureGPOPin(IND3_GPIO_Port, IND3_Pin);
    
    HAL_GPIO_WritePin(IND1_GPIO_Port, IND1_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(IND2_GPIO_Port, IND2_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(IND3_GPIO_Port, IND3_Pin, GPIO_PIN_SET);
    
    HAL_GPIO_WritePin(SCL_CFG_GPIO_Port, SCL_CFG_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(SDA_REM_GPIO_Port, SDA_REM_Pin, GPIO_PIN_RESET);

    HAL_GPIO_WritePin(IO_EXP_RSTN_GPIO_Port, IO_EXP_RSTN_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(HUB_RESET_GPIO_Port, HUB_RESET_Pin, GPIO_PIN_RESET);
    
    return 0;
}

static inline int BSP_GPIO_DeInit(void)
{
    // De-initialize all GPIOs used by the board
    HAL_GPIO_DeInit(SCL_CFG_GPIO_Port, SCL_CFG_Pin);
    HAL_GPIO_DeInit(SDA_REM_GPIO_Port, SDA_REM_Pin);
    HAL_GPIO_DeInit(IO_EXP_RSTN_GPIO_Port, IO_EXP_RSTN_Pin);
    HAL_GPIO_DeInit(HUB_RESET_GPIO_Port, HUB_RESET_Pin);
    HAL_GPIO_DeInit(IND1_GPIO_Port, IND1_Pin);
    HAL_GPIO_DeInit(IND2_GPIO_Port, IND2_Pin);
    HAL_GPIO_DeInit(IND3_GPIO_Port, IND3_Pin);
    
    return 0;
}

static inline int BSP_USB_Hub_Enable(void)
{
    // Enable USB HUB
    HAL_GPIO_WritePin(HUB_RESET_GPIO_Port, HUB_RESET_Pin, GPIO_PIN_SET);
    return 0;
}

static inline int BSP_USB_Hub_Disable(void)
{
    // Disable USB HUB
    HAL_GPIO_WritePin(HUB_RESET_GPIO_Port, HUB_RESET_Pin, GPIO_PIN_RESET);
    return 0;
}

static inline int BSP_Set_IO_Expander_Enable(void)
{
    HAL_GPIO_WritePin(IO_EXP_RSTN_GPIO_Port, IO_EXP_RSTN_Pin, GPIO_PIN_SET);
    return 0;
}

static inline int BSP_Set_IO_Expander_Disable(void)
{
    HAL_GPIO_WritePin(IO_EXP_RSTN_GPIO_Port, IO_EXP_RSTN_Pin, GPIO_PIN_RESET);
    return 0;
}


static inline int BSP_LED_On(Led_TypeDef led)
{
    (void)led;
    HAL_GPIO_WritePin(IND3_GPIO_Port, IND3_Pin, GPIO_PIN_RESET); 
    return 0;
}

static inline int BSP_LED_Off(Led_TypeDef led)
{
    (void)led;
    HAL_GPIO_WritePin(IND3_GPIO_Port, IND3_Pin, GPIO_PIN_SET); 
    return 0;
}

static inline int BSP_LED_Toggle(Led_TypeDef led)
{
    (void)led;
    HAL_GPIO_TogglePin(IND3_GPIO_Port, IND3_Pin);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* APP_HW_H */

