/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "version.h"   /* FW_VERSION (CMake-generated git describe) */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "sfu_boot.h"   /* SBSFU secure boot service */
#include "usbd_dfu_if.h" /* DFU_ImageDownloadComplete() */
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Application -> bootloader "enter USB DFU" request.
 * The running application writes BL_FORCE_DFU_MAGIC into RTC backup register 7
 * and issues a system reset. RTC backup registers live in the always-on backup
 * domain and survive a software reset, so the bootloader can detect the request
 * on the next boot, skip launching the application, and enter the USB DFU
 * download path so a new signed image can be installed without a debugger.
 * Application side (added later in the demo app):
 *     HAL_PWR_EnableBkUpAccess();
 *     __HAL_RCC_RTC_ENABLE();
 *     RTC->BKP7R = BL_FORCE_DFU_MAGIC;
 *     NVIC_SystemReset();
 */
#define BL_FORCE_DFU_MAGIC      0xB007C0DEU   /* "BOOT CODE" — request marker, RTC->BKP7R */

/*
 * Failsafe boot counter (RTC->BKP6R).
 * The bootloader increments this on every boot attempt BEFORE launching the
 * application, and arms the IWDG just before the jump. The application must
 * clear it to 0 once it has booted successfully (see openmotion-console-fw).
 * If the application hangs it never clears the counter and never refreshes the
 * IWDG, so the watchdog resets the device and the count keeps rising across
 * attempts. After BL_BOOT_FAIL_MAX failed attempts the bootloader stops trying
 * to launch and falls through to USB DFU so a known-good image can be flashed.
 * A clean power cycle (no VBAT) clears the backup domain and resets the count.
 */
#define BL_BOOT_FAIL_MAX        3U            /* force DFU after this many failed boots */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

CRC_HandleTypeDef hcrc;

I2C_HandleTypeDef hi2c1;

IWDG_HandleTypeDef hiwdg1;

RNG_HandleTypeDef hrng;

RTC_HandleTypeDef hrtc;

UART_HandleTypeDef huart4;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_CRC_Init(void);
static void MX_RNG_Init(void);
static void MX_RTC_Init(void);
static void MX_UART4_Init(void);
static void MX_I2C1_Init(void);
static void MX_IWDG1_Init(void);
/* USER CODE BEGIN PFP */



/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  uint8_t force_dfu = 0U;   /* set when the application requested USB DFU (see RTC->BKP7R) */
  uint8_t boot_fail = 0U;   /* set when too many failed boot attempts forced DFU (see RTC->BKP6R) */
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /*
   * Application-requested USB DFU.
   * RTC backup registers reside in the always-on backup domain and are NOT
   * cleared by a software reset, so an application can hand control back to the
   * bootloader's DFU path by writing BL_FORCE_DFU_MAGIC to RTC->BKP7R and
   * resetting. Enable backup-domain write access (PWR DBP) and the RTC register
   * clock before touching the backup register. RTCEN/RTCSEL programmed by a
   * prior boot persist across the software reset, so the register reads back the
   * value the application stored.
   */
  /* (On STM32H7 the PWR peripheral is always clocked — no RCC enable needed.) */
  HAL_PWR_EnableBkUpAccess();   /* DBP = 1: unlock the backup domain */
  /* Ensure the RTC kernel clock (LSI) is running. LSI in RCC_CSR is cleared by a
   * system reset even though the backup-domain BDCR (RTCSEL/RTCEN) is not, so
   * re-enable it before accessing the RTC. */
  __HAL_RCC_LSI_ENABLE();
  { uint32_t to = 0U; while ((__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == 0U) && (++to < 0x00100000U)) { } }
  /* Select LSI as the RTC clock only if no source is selected yet. Writing
   * RTCSEL when it already holds a (non-zero) value would require a backup-domain
   * reset, which would erase the request, so we never do that here. */
  if ((RCC->BDCR & RCC_BDCR_RTCSEL) == 0U)
  {
    MODIFY_REG(RCC->BDCR, RCC_BDCR_RTCSEL, RCC_BDCR_RTCSEL_1);   /* RTCSEL = LSI */
  }
  __HAL_RCC_RTC_ENABLE();       /* enable RTC register / backup-register access */
  if (RTC->BKP7R == BL_FORCE_DFU_MAGIC)
  {
    RTC->BKP7R = 0U;            /* consume the request (one-shot) */
    force_dfu = 1U;
  }

  /*
   * Failsafe boot counter (RTC->BKP6R). Only evaluated when the application did
   * NOT explicitly request DFU above — a deliberate DFU request is not a boot
   * failure and must not advance the counter. The counter is incremented here,
   * before the IWDG is armed and before the application is launched; the
   * application clears it to 0 once it boots successfully. See BL_BOOT_FAIL_MAX.
   */
  if (force_dfu == 0U)
  {
    uint32_t boot_cnt = RTC->BKP6R;
    if (boot_cnt >= BL_BOOT_FAIL_MAX)
    {
      /* The last BL_BOOT_FAIL_MAX attempts all failed to clear the counter:
       * the installed firmware is not booting. Recover via USB DFU instead of
       * launching it again. Leave the counter as-is so the host/DFU tooling can
       * still observe that a boot-failure recovery occurred. */
      force_dfu = 1U;
      boot_fail = 1U;
    }
    else
    {
      RTC->BKP6R = boot_cnt + 1U;   /* record this boot attempt before the jump */
    }
  }

  /*
   * Arm the independent watchdog before launching the application.
   *
   * The IWDG runs from the LSI and, once started in software mode, keeps running
   * across the reset it triggers (only a power-on reset stops it). So on a retry
   * boot the bootloader is already running under a live IWDG: HAL_IWDG_Init()
   * below reloads the counter, giving the bootloader a fresh window for its
   * (un-refreshed) signature verification, and the application must take over
   * refreshing once launched. The DFU download loop also refreshes it (below).
   */
  MX_IWDG1_Init();

  /*
   * Run the SBSFU Secure Boot Service — skipped when the application explicitly
   * requested DFU mode (force_dfu), in which case we fall through to the USB DFU
   * download path below.
   *
   * This initializes the Secure Engine (SECoreBin), configures security,
   * and runs the SBSFU state machine:
   *  - If a valid signed firmware is found at SLOT_ACTIVE_1 (0x08020000),
   *    SBSFU verifies and launches it — this function never returns in that case.
   *  - If no valid firmware exists (SECBOOT_USE_NO_LOADER), the state
   *    machine exits gracefully and this function returns, allowing the USB DFU
   *    path below to accept a new signed image.
   *
   * SBSFU debug traces (sfu_com_trace.c) are printed via UART4 (blocking path).
   * SBSFU's DeInit disables the UART4 clock and deinits PD0/PD1 GPIO, so
   * MX_UART4_Init() must be called again below to restore the peripheral.
   */
  if (force_dfu == 0U)
  {
    SFU_BOOT_InitErrorTypeDef e_boot = SFU_BOOT_RunSecureBootService();
    (void)e_boot;  /* non-zero only on critical SE/security-IP init failure */
  }

  /*
   * Execution only reaches this point in the USB DFU download path: when a valid
   * signed firmware is present SFU_BOOT_RunSecureBootService() launches it and
   * never returns. SBSFU's DeInit() left UART4 and most GPIO de-initialised, so
   * re-initialise the peripherals the bootloader needs to enumerate as DFU.
   * GPIO must come first: the BSP_*_Enable() helpers below drive the IO-expander
   * and USB-hub reset lines via HAL_GPIO_WritePin() and require the pins to be
   * configured as outputs. DMA precedes UART4 because UART4's MspInit binds the
   * UART4 RX/TX DMA streams.
   */
  MX_GPIO_Init();
  MX_UART4_Init();

  /* USER CODE END SysInit */

  /* Initialize remaining configured peripherals */
  MX_CRC_Init();
  MX_RNG_Init();
  MX_RTC_Init();
  MX_I2C1_Init();
  
  /* USER CODE BEGIN 2 */
  if(force_dfu != 0U){
    PrintBootBanner(&huart4, FW_VERSION);
    
    if (boot_fail != 0U)
    {
      const char fail_note[] =
        "= [SBOOT] Reason: application failed to boot "
        "after repeated attempts (boot-counter recovery)\r\n\r\n";
      HAL_UART_Transmit(&huart4, (uint8_t *)fail_note, sizeof(fail_note) - 1, 250);
    }
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  // Init USB
  BSP_Set_IO_Expander_Enable();
  BSP_USB_Hub_Enable();
  MX_USB_DEVICE_Init();
  HAL_Delay(100);

  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* Refresh the IWDG: it may still be live from a prior firmware-hang reset,
     * and we armed it before reaching here. 500 ms loop << ~8 s timeout. */
    HAL_IWDG_Refresh(&hiwdg1);

    /* A new image was downloaded and the host has completed manifestation
     * (the DFU class is manifestation-tolerant, so it did NOT self-reset). */
    if (DFU_ImageDownloadComplete() != 0U)
    {
      if (DFU_IsRollback() != 0U)
      {
        /* Anti-rollback: the downloaded image is an older version than what was
         * installed. Destroy it so it can never boot (the boot path itself does
         * no version check), report, and stay in DFU so a valid (>=) image can
         * be flashed. Clear the completion latch so we handle this only once. */
        DFU_InvalidateImage();
        DFU_ClearDownloadState();

        const char rb[] =
          "\r\n= [SBOOT] Anti-rollback: rejected older firmware version. "
          "Image erased — flash a build with an equal or higher version.\r\n\r\n";
        HAL_UART_Transmit(&huart4, (uint8_t *)rb, sizeof(rb) - 1, 250);
      }
      else
      {
        /* Reboot so SBSFU verifies and launches the freshly flashed firmware.
         * The brief delay lets the final USB control transfer settle and the
         * host tool exit cleanly before the bus drops. */
        HAL_Delay(50);
        NVIC_SystemReset();
      }
    }

    HAL_GPIO_TogglePin(IND1_GPIO_Port, IND1_Pin);
    HAL_Delay(500);

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSI
                              |RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 30;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
  RCC_OscInitStruct.PLL.PLLR = 4;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x107075B0;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief IWDG1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG1_Init(void)
{

  /* USER CODE BEGIN IWDG1_Init 0 */

  /* USER CODE END IWDG1_Init 0 */

  /* USER CODE BEGIN IWDG1_Init 1 */

  /* USER CODE END IWDG1_Init 1 */
  /* Timeout = (Reload + 1) * Prescaler / LSI ~= 4096 * 64 / 32000 ~= 8.2 s.
   * This window must exceed BOTH the bootloader's signature-verification time
   * (it is not refreshed during SFU_BOOT_RunSecureBootService) AND the
   * application's time from launch to its first IWDG refresh. Widened from the
   * original ~2 s (Prescaler_16) so neither path trips the watchdog spuriously. */
  hiwdg1.Instance = IWDG1;
  hiwdg1.Init.Prescaler = IWDG_PRESCALER_64;
  hiwdg1.Init.Window = 4095;
  hiwdg1.Init.Reload = 4095;
  if (HAL_IWDG_Init(&hiwdg1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG1_Init 2 */

  /* USER CODE END IWDG1_Init 2 */

}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief UART4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART4_Init(void)
{

  /* USER CODE BEGIN UART4_Init 0 */

  /* USER CODE END UART4_Init 0 */

  /* USER CODE BEGIN UART4_Init 1 */

  /* USER CODE END UART4_Init 1 */
  huart4.Instance = UART4;
  huart4.Init.BaudRate = 115200;
  huart4.Init.WordLength = UART_WORDLENGTH_8B;
  huart4.Init.StopBits = UART_STOPBITS_1;
  huart4.Init.Parity = UART_PARITY_NONE;
  huart4.Init.Mode = UART_MODE_TX_RX;
  huart4.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart4.Init.OverSampling = UART_OVERSAMPLING_16;
  huart4.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart4.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart4.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart4, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart4, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART4_Init 2 */

  /* USER CODE END UART4_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */


/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}


#ifdef SFU_MPU_PROTECT_ENABLE

/**
  * @brief  CPU L1-Cache enable.
  * @param  None
  * @retval None
  */
static void CPU_CACHE_Enable(void)
{
  /* Enable I-Cache */
  SCB_EnableICache();

  /* Enable D-Cache */
  SCB_EnableDCache();
}
#endif /* SFU_MPU_PROTECT_ENABLE */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
